/* @file
 * @brief Bluetooth ASCS
 */
/*
 * Copyright (c) 2020 Intel Corporation
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/check.h>

#include <zephyr/device.h>
#include <zephyr/init.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include "zephyr/bluetooth/iso.h"
#include <zephyr/bluetooth/audio/audio.h>
#include <zephyr/bluetooth/audio/pacs.h>

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bt_ascs, CONFIG_BT_ASCS_LOG_LEVEL);

#include "common/bt_str.h"
#include "common/assert.h"

#include "../host/hci_core.h"
#include "../host/conn_internal.h"

#include "audio_internal.h"
#include "audio_iso.h"
#include "endpoint.h"
#include "unicast_server.h"
#include "pacs_internal.h"
#include "cap_internal.h"

#define MAX_ASES_SESSIONS CONFIG_BT_MAX_CONN * \
				(CONFIG_BT_ASCS_ASE_SNK_COUNT + \
				 CONFIG_BT_ASCS_ASE_SRC_COUNT)

BUILD_ASSERT(CONFIG_BT_ASCS_MAX_ACTIVE_ASES <= MAX(MAX_ASES_SESSIONS,
						   CONFIG_BT_ISO_MAX_CHAN),
	     "Max active ASEs are set to more than actual number of ASEs or ISOs");

#if defined(CONFIG_BT_AUDIO_UNICAST_SERVER)

#define ASE_ID(_ase) ase->ep.status.id
#define ASE_DIR(_id) \
	(_id > CONFIG_BT_ASCS_ASE_SNK_COUNT ? BT_AUDIO_DIR_SOURCE : BT_AUDIO_DIR_SINK)
#define ASE_UUID(_id) \
	(_id > CONFIG_BT_ASCS_ASE_SNK_COUNT ? BT_UUID_ASCS_ASE_SRC : BT_UUID_ASCS_ASE_SNK)
#define ASE_COUNT (CONFIG_BT_ASCS_ASE_SNK_COUNT + CONFIG_BT_ASCS_ASE_SRC_COUNT)

struct bt_ascs_ase {
	struct bt_ascs *ascs;
	struct bt_audio_ep ep;
	const struct bt_gatt_attr *attr;
	sys_snode_t node;
};

struct bt_ascs {
	struct bt_conn *conn;
	sys_slist_t ases;
};

K_MEM_SLAB_DEFINE(ase_slab, sizeof(struct bt_ascs_ase),
		  CONFIG_BT_ASCS_MAX_ACTIVE_ASES,
		  __alignof__(struct bt_ascs_ase));

static struct bt_ascs sessions[CONFIG_BT_MAX_CONN];
NET_BUF_SIMPLE_DEFINE_STATIC(ase_buf, CONFIG_BT_L2CAP_TX_MTU);
static struct bt_audio_stream *enabling[CONFIG_BT_ISO_MAX_CHAN];

static int control_point_notify(struct bt_conn *conn, const void *data, uint16_t len);
static int ascs_ep_get_status(struct bt_audio_ep *ep,
			      struct net_buf_simple *buf);

static bool is_valid_ase_id(uint8_t ase_id)
{
	return IN_RANGE(ase_id, 1, ASE_COUNT);
}

static void bt_ascs_ase_return_to_slab(struct bt_ascs_ase *ase)
{
	__ASSERT(ase && ase->ascs, "Non-existing ASE or ASCS");

	LOG_DBG("Returning ase %p to slab", ase);

	sys_slist_find_and_remove(&ase->ascs->ases, &ase->node);
	k_mem_slab_free(&ase_slab, (void **)&ase);
}

static struct bt_ascs_ase *bt_ascs_ase_get_from_slab(void)
{
	struct bt_ascs_ase *ase = NULL;

	if (k_mem_slab_alloc(&ase_slab, (void **)&ase, K_NO_WAIT) < 0) {
		LOG_DBG("Could not get ASE from slab, out of memory");
	}

	return ase;
}

static void ase_status_changed(struct bt_audio_ep *ep, uint8_t old_state,
			       uint8_t state)
{
	struct bt_ascs_ase *ase = CONTAINER_OF(ep, struct bt_ascs_ase, ep);
	struct bt_conn *conn = ase->ascs->conn;

	LOG_DBG("ase %p, ep %p", ase, ep);

	if (conn != NULL && conn->state == BT_CONN_CONNECTED) {
		ascs_ep_get_status(ep, &ase_buf);

		bt_gatt_notify(conn, ase->attr, ase_buf.data, ase_buf.len);
	}
}

static int ascs_stream_disconnect(struct bt_audio_stream *stream)
{
	/* Stop listening */
	for (size_t i = 0; i < ARRAY_SIZE(enabling); i++) {
		if (enabling[i] == stream) {
			enabling[i] = NULL;
			break;
		}
	}

	return bt_audio_stream_disconnect(stream);
}

void ascs_ep_set_state(struct bt_audio_ep *ep, uint8_t state)
{
	struct bt_audio_stream *stream;
	bool state_changed;
	uint8_t old_state;

	if (!ep) {
		return;
	}

	/* TODO: Verify state changes */

	old_state = ep->status.state;
	ep->status.state = state;
	state_changed = old_state != state;

	LOG_DBG("ep %p id 0x%02x %s -> %s", ep, ep->status.id, bt_audio_ep_state_str(old_state),
		bt_audio_ep_state_str(state));

	/* Notify clients*/
	ase_status_changed(ep, old_state, state);

	if (ep->stream == NULL) {
		return;
	}

	stream = ep->stream;

	if (stream->ops != NULL) {
		const struct bt_audio_stream_ops *ops = stream->ops;

		switch (state) {
		case BT_AUDIO_EP_STATE_IDLE:
			if (ops->released != NULL) {
				ops->released(stream);
			}
			struct bt_ascs_ase *ase = CONTAINER_OF(ep, struct bt_ascs_ase, ep);

			/* Return the ase to slab */
			bt_ascs_ase_return_to_slab(ase);

			break;
		case BT_AUDIO_EP_STATE_CODEC_CONFIGURED:
			switch (old_state) {
			case BT_AUDIO_EP_STATE_IDLE:
			case BT_AUDIO_EP_STATE_CODEC_CONFIGURED:
			case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
			case BT_AUDIO_EP_STATE_RELEASING:
				break;
			default:
				BT_ASSERT_MSG(false,
					      "Invalid state transition: %s -> %s",
					      bt_audio_ep_state_str(old_state),
					      bt_audio_ep_state_str(ep->status.state));
				return;
			}

			if (ops->configured != NULL) {
				ops->configured(stream, &ep->qos_pref);
			}

			break;
		case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
			/* QoS configured have different allowed states
			 * depending on the endpoint type
			 */
			if (ep->dir == BT_AUDIO_DIR_SOURCE) {
				switch (old_state) {
				case BT_AUDIO_EP_STATE_CODEC_CONFIGURED:
				case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
				case BT_AUDIO_EP_STATE_DISABLING:
					break;
				default:
					BT_ASSERT_MSG(false,
						      "Invalid state transition: %s -> %s",
						      bt_audio_ep_state_str(old_state),
						      bt_audio_ep_state_str(ep->status.state));
					return;
				}
			} else {
				switch (old_state) {
				case BT_AUDIO_EP_STATE_CODEC_CONFIGURED:
				case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
				case BT_AUDIO_EP_STATE_ENABLING:
				case BT_AUDIO_EP_STATE_STREAMING:
					break;
				default:
					BT_ASSERT_MSG(false,
						      "Invalid state transition: %s -> %s",
						      bt_audio_ep_state_str(old_state),
						      bt_audio_ep_state_str(ep->status.state));
					return;
				}
			}

			if (ops->qos_set != NULL) {
				ops->qos_set(stream);
			}

			break;
		case BT_AUDIO_EP_STATE_ENABLING:
			switch (old_state) {
			case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
			case BT_AUDIO_EP_STATE_ENABLING:
				break;
			default:
				BT_ASSERT_MSG(false,
					      "Invalid state transition: %s -> %s",
					      bt_audio_ep_state_str(old_state),
					      bt_audio_ep_state_str(ep->status.state));
				return;
			}

			if (state_changed && ops->enabled != NULL) {
				ops->enabled(stream);
			} else if (!state_changed && ops->metadata_updated) {
				ops->metadata_updated(stream);
			}

			/* SINK ASEs can autonomously go into the streaming state if
			 * the CIS is connected
			 */
			if (ep->dir == BT_AUDIO_DIR_SINK &&
			    ep->iso != NULL &&
			    ep->iso->chan.state == BT_ISO_STATE_CONNECTED) {
				ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_STREAMING);
			}

			break;
		case BT_AUDIO_EP_STATE_STREAMING:
			switch (old_state) {
			case BT_AUDIO_EP_STATE_ENABLING:
			case BT_AUDIO_EP_STATE_STREAMING:
				break;
			default:
				BT_ASSERT_MSG(false,
					      "Invalid state transition: %s -> %s",
					      bt_audio_ep_state_str(old_state),
					      bt_audio_ep_state_str(ep->status.state));
				return;
			}

			if (state_changed && ops->started != NULL) {
				ops->started(stream);
			} else if (!state_changed && ops->metadata_updated) {
				ops->metadata_updated(stream);
			}

			break;
		case BT_AUDIO_EP_STATE_DISABLING:
			if (ep->dir == BT_AUDIO_DIR_SOURCE) {
				switch (old_state) {
				case BT_AUDIO_EP_STATE_ENABLING:
				case BT_AUDIO_EP_STATE_STREAMING:
					ep->receiver_ready = false;
					break;
				default:
					BT_ASSERT_MSG(false,
						      "Invalid state transition: %s -> %s",
						      bt_audio_ep_state_str(old_state),
						      bt_audio_ep_state_str(ep->status.state));
					return;
				}
			} else {
				/* Sinks cannot go into the disabling state */
				BT_ASSERT_MSG(false,
					      "Invalid state transition: %s -> %s",
					      bt_audio_ep_state_str(old_state),
					      bt_audio_ep_state_str(ep->status.state));
				return;
			}

			if (ops->disabled != NULL) {
				ops->disabled(stream);
			}

			break;
		case BT_AUDIO_EP_STATE_RELEASING:
			switch (old_state) {
			case BT_AUDIO_EP_STATE_CODEC_CONFIGURED:
			case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
			case BT_AUDIO_EP_STATE_ENABLING:
			case BT_AUDIO_EP_STATE_STREAMING:
				ep->receiver_ready = false;
				break;
			case BT_AUDIO_EP_STATE_DISABLING:
				if (ep->dir == BT_AUDIO_DIR_SOURCE) {
					break;
				} /* else fall through for sink */

				/* fall through */
			default:
				BT_ASSERT_MSG(false,
					      "Invalid state transition: %s -> %s",
					      bt_audio_ep_state_str(old_state),
					      bt_audio_ep_state_str(ep->status.state));
				return;
			}

			if (ep->iso == NULL ||
			    ep->iso->chan.state == BT_ISO_STATE_DISCONNECTED) {
				if (ep->iso != NULL) {
					bt_audio_iso_unbind_ep(ep->iso, ep);
				}

				bt_audio_stream_detach(stream);
				ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_IDLE);
			} else {
				/* Either the client or the server may disconnect the
				 * CISes when entering the releasing state.
				 */
				const int err = ascs_stream_disconnect(stream);

				if (err != 0) {
					LOG_ERR("Failed to disconnect stream %p: %d",
						stream, err);
				}
			}

			break;
		default:
			LOG_ERR("Invalid state: %u", state);
			break;
		}
	}
}

static void ascs_codec_data_add(struct net_buf_simple *buf, const char *prefix,
				uint8_t num, struct bt_codec_data *data)
{
	struct bt_ascs_codec_config *cc;
	int i;

	for (i = 0; i < num; i++) {
		struct bt_data *d = &data[i].data;

		LOG_DBG("#%u: %s type 0x%02x len %u", i, prefix, d->type, d->data_len);
		LOG_HEXDUMP_DBG(d->data, d->data_len, prefix);

		cc = net_buf_simple_add(buf, sizeof(*cc));
		cc->len = d->data_len + sizeof(cc->type);
		cc->type = d->type;
		net_buf_simple_add_mem(buf, d->data, d->data_len);
	}
}

static void ascs_ep_get_status_config(struct bt_audio_ep *ep,
				      struct net_buf_simple *buf)
{
	struct bt_ascs_ase_status_config *cfg;
	struct bt_codec_qos_pref *pref = &ep->qos_pref;

	cfg = net_buf_simple_add(buf, sizeof(*cfg));
	cfg->framing = pref->unframed_supported ? BT_ASCS_QOS_FRAMING_UNFRAMED
						: BT_ASCS_QOS_FRAMING_FRAMED;
	cfg->phy = pref->phy;
	cfg->rtn = pref->rtn;
	cfg->latency = sys_cpu_to_le16(pref->latency);
	sys_put_le24(pref->pd_min, cfg->pd_min);
	sys_put_le24(pref->pd_max, cfg->pd_max);
	sys_put_le24(pref->pref_pd_min, cfg->prefer_pd_min);
	sys_put_le24(pref->pref_pd_max, cfg->prefer_pd_max);
	cfg->codec.id = ep->codec.id;
	cfg->codec.cid = sys_cpu_to_le16(ep->codec.cid);
	cfg->codec.vid = sys_cpu_to_le16(ep->codec.vid);

	LOG_DBG("dir %s unframed_supported 0x%02x phy 0x%02x rtn %u "
	       "latency %u pd_min %u pd_max %u codec 0x%02x",
	       bt_audio_dir_str(ep->dir), pref->unframed_supported, pref->phy,
	       pref->rtn, pref->latency, pref->pd_min, pref->pd_max,
	       ep->stream->codec->id);

	cfg->cc_len = buf->len;
	ascs_codec_data_add(buf, "data", ep->codec.data_count, ep->codec.data);
	cfg->cc_len = buf->len - cfg->cc_len;
}

static void ascs_ep_get_status_qos(struct bt_audio_ep *ep,
				   struct net_buf_simple *buf)
{
	struct bt_ascs_ase_status_qos *qos;

	qos = net_buf_simple_add(buf, sizeof(*qos));
	qos->cig_id = ep->cig_id;
	qos->cis_id = ep->cis_id;
	sys_put_le24(ep->stream->qos->interval, qos->interval);
	qos->framing = ep->stream->qos->framing;
	qos->phy = ep->stream->qos->phy;
	qos->sdu = sys_cpu_to_le16(ep->stream->qos->sdu);
	qos->rtn = ep->stream->qos->rtn;
	qos->latency = sys_cpu_to_le16(ep->stream->qos->latency);
	sys_put_le24(ep->stream->qos->pd, qos->pd);

	LOG_DBG("dir %s codec 0x%02x interval %u framing 0x%02x phy 0x%02x "
	       "rtn %u latency %u pd %u",
	       bt_audio_dir_str(ep->dir), ep->stream->codec->id,
	       ep->stream->qos->interval, ep->stream->qos->framing,
	       ep->stream->qos->phy, ep->stream->qos->rtn,
	       ep->stream->qos->latency, ep->stream->qos->pd);
}

static void ascs_ep_get_status_enable(struct bt_audio_ep *ep,
				      struct net_buf_simple *buf)
{
	struct bt_ascs_ase_status_enable *enable;

	enable = net_buf_simple_add(buf, sizeof(*enable));
	enable->cig_id = ep->cig_id;
	enable->cis_id = ep->cis_id;

	enable->metadata_len = buf->len;
	ascs_codec_data_add(buf, "meta", ep->codec.meta_count, ep->codec.meta);
	enable->metadata_len = buf->len - enable->metadata_len;

	LOG_DBG("dir %s cig 0x%02x cis 0x%02x",
		bt_audio_dir_str(ep->dir), ep->cig_id, ep->cis_id);
}

static int ascs_ep_get_status_idle(uint8_t ase_id, struct net_buf_simple *buf)
{
	struct bt_ascs_ase_status *status;

	if (!buf || ase_id > ASE_COUNT) {
		return -EINVAL;
	}

	net_buf_simple_reset(buf);

	status = net_buf_simple_add(buf, sizeof(*status));
	status->id = ase_id;
	status->state = BT_AUDIO_EP_STATE_IDLE;

	LOG_DBG("id 0x%02x state %s", ase_id,
		bt_audio_ep_state_str(status->state));

	return 0;
}

static int ascs_ep_get_status(struct bt_audio_ep *ep,
			      struct net_buf_simple *buf)
{
	struct bt_ascs_ase_status *status;

	if (!ep || !buf) {
		return -EINVAL;
	}

	LOG_DBG("ep %p id 0x%02x state %s", ep, ep->status.id,
		bt_audio_ep_state_str(ep->status.state));

	/* Reset if buffer before using */
	net_buf_simple_reset(buf);

	status = net_buf_simple_add_mem(buf, &ep->status,
					sizeof(ep->status));

	switch (ep->status.state) {
	case BT_AUDIO_EP_STATE_IDLE:
	/* Fallthrough */
	case BT_AUDIO_EP_STATE_RELEASING:
		break;
	case BT_AUDIO_EP_STATE_CODEC_CONFIGURED:
		ascs_ep_get_status_config(ep, buf);
		break;
	case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
		ascs_ep_get_status_qos(ep, buf);
		break;
	case BT_AUDIO_EP_STATE_ENABLING:
	/* Fallthrough */
	case BT_AUDIO_EP_STATE_STREAMING:
	/* Fallthrough */
	case BT_AUDIO_EP_STATE_DISABLING:
		ascs_ep_get_status_enable(ep, buf);
		break;
	default:
		LOG_ERR("Invalid Endpoint state");
		break;
	}

	return 0;
}

static int ascs_iso_accept(const struct bt_iso_accept_info *info,
				      struct bt_iso_chan **iso_chan)
{
	LOG_DBG("acl %p", info->acl);

	for (size_t i = 0U; i < ARRAY_SIZE(enabling); i++) {
		struct bt_audio_stream *c = enabling[i];

		if (c != NULL && c->ep->cig_id == info->cig_id && c->ep->cis_id == info->cis_id) {
			*iso_chan = &enabling[i]->ep->iso->chan;
			enabling[i] = NULL;

			LOG_DBG("iso_chan %p", *iso_chan);

			return 0;
		}
	}

	LOG_ERR("No channel listening");

	return -EPERM;
}

static int ascs_iso_listen(struct bt_audio_stream *stream)
{
	struct bt_audio_stream **free_stream = NULL;
	static struct bt_iso_server iso_server = {
		.sec_level = BT_SECURITY_L2,
		.accept = ascs_iso_accept,
	};
	static bool server;
	int err;

	LOG_DBG("stream %p conn %p", stream, stream->conn);

	if (server) {
		goto done;
	}

	err = bt_iso_server_register(&iso_server);
	if (err) {
		LOG_ERR("bt_iso_server_register: %d", err);
		return err;
	}

	server = true;

done:
	for (size_t i = 0U; i < ARRAY_SIZE(enabling); i++) {
		if (enabling[i] == stream) {
			return 0;
		}

		if (enabling[i] == NULL && free_stream == NULL) {
			free_stream = &enabling[i];
		}
	}

	if (free_stream != NULL) {
		*free_stream = stream;

		return 0;
	}

	LOG_ERR("Unable to listen: no slot left");

	return -ENOSPC;
}

static void ascs_iso_recv(struct bt_iso_chan *chan,
			  const struct bt_iso_recv_info *info,
			  struct net_buf *buf)
{
	struct bt_audio_iso *iso = CONTAINER_OF(chan, struct bt_audio_iso, chan);
	const struct bt_audio_stream_ops *ops;
	struct bt_audio_stream *stream;
	struct bt_audio_ep *ep;

	ep = iso->rx.ep;
	if (ep == NULL) {
		/* In the case that the CIS has been setup as bidirectional, and
		 * only one of the directions have an ASE configured yet,
		 * we should only care about valid ISO packets when doing this
		 * check. The reason is that some controllers send HCI ISO data
		 * packets to the host, even if no SDU was sent on the remote
		 * side. This basically means that empty PDUs are sent to the
		 * host as HCI ISO data packets, which we should just ignore
		 */
		if ((info->flags & BT_ISO_FLAGS_VALID) != 0) {
			LOG_ERR("iso %p not bound with ep", chan);
		}

		return;
	}

	if (IS_ENABLED(CONFIG_BT_AUDIO_DEBUG_STREAM_DATA) &&
	    ep->status.state != BT_AUDIO_EP_STATE_STREAMING) {
		LOG_DBG("ep %p is not in the streaming state: %s",
		       ep, bt_audio_ep_state_str(ep->status.state));
		return;
	}

	stream = ep->stream;
	if (stream == NULL) {
		LOG_ERR("No stream for ep %p", ep);
		return;
	}

	ops = stream->ops;

	if (IS_ENABLED(CONFIG_BT_AUDIO_DEBUG_STREAM_DATA)) {
		LOG_DBG("stream %p ep %p len %zu", stream, stream->ep, net_buf_frags_len(buf));
	}

	if (ops != NULL && ops->recv != NULL) {
		ops->recv(stream, info, buf);
	} else {
		LOG_WRN("No callback for recv set");
	}
}

static void ascs_iso_sent(struct bt_iso_chan *chan)
{
	struct bt_audio_iso *iso = CONTAINER_OF(chan, struct bt_audio_iso, chan);
	const struct bt_audio_stream_ops *ops;
	struct bt_audio_stream *stream;
	struct bt_audio_ep *ep;

	ep = iso->tx.ep;
	if (ep == NULL) {
		LOG_ERR("iso %p not bound with ep", chan);
		return;
	}

	stream = ep->stream;
	if (stream == NULL) {
		LOG_ERR("No stream for ep %p", ep);
		return;
	}

	ops = stream->ops;

	if (IS_ENABLED(CONFIG_BT_AUDIO_DEBUG_STREAM_DATA)) {
		LOG_DBG("stream %p ep %p", stream, stream->ep);
	}

	if (ops != NULL && ops->sent != NULL) {
		ops->sent(stream);
	}
}

static void ascs_ep_iso_connected(struct bt_audio_ep *ep)
{
	struct bt_audio_stream *stream;

	if (ep->status.state != BT_AUDIO_EP_STATE_ENABLING) {
		LOG_DBG("ep %p not in enabling state: %s",
		       ep, bt_audio_ep_state_str(ep->status.state));
		return;
	}

	stream = ep->stream;
	if (stream == NULL) {
		LOG_ERR("No stream for ep %p", ep);
		return;
	}

	if (ep->dir == BT_AUDIO_DIR_SINK && ep->receiver_ready) {
		/* Source ASEs shall be ISO connected first, and then receive
		 * the receiver start ready command to enter the streaming
		 * state
		 */
		ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_STREAMING);
	}
}

static void ascs_iso_connected(struct bt_iso_chan *chan)
{
	struct bt_audio_iso *iso = CONTAINER_OF(chan, struct bt_audio_iso, chan);

	if (iso->rx.ep == NULL && iso->tx.ep == NULL) {
		LOG_ERR("iso %p not bound with ep", chan);
		return;
	}

	if (iso->rx.ep != NULL) {
		ascs_ep_iso_connected(iso->rx.ep);
	}

	if (iso->tx.ep != NULL) {
		ascs_ep_iso_connected(iso->tx.ep);
	}
}

static void ascs_ep_iso_disconnected(struct bt_audio_ep *ep, uint8_t reason)
{
	const struct bt_audio_stream_ops *ops;
	struct bt_audio_stream *stream;
	int err;

	stream = ep->stream;
	if (stream == NULL) {
		LOG_ERR("No stream for ep %p", ep);
		return;
	}

	ops = stream->ops;

	LOG_DBG("stream %p ep %p reason 0x%02x", stream, stream->ep, reason);

	if (ep->status.state == BT_AUDIO_EP_STATE_ENABLING &&
	    reason == BT_HCI_ERR_CONN_FAIL_TO_ESTAB) {
		LOG_DBG("Waiting for retry");

		err = ascs_iso_listen(stream);
		if (err != 0) {
			LOG_ERR("Could not make stream listen: %d", err);
		}

		return;
	}

	if (ops != NULL && ops->stopped != NULL) {
		ops->stopped(stream);
	} else {
		LOG_WRN("No callback for stopped set");
	}

	if (ep->status.state == BT_AUDIO_EP_STATE_RELEASING) {
		bt_audio_iso_unbind_ep(ep->iso, ep);
		bt_audio_stream_detach(stream);
		ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_IDLE);
	} else {
		/* The ASE state machine goes into different states from this operation
		 * based on whether it is a source or a sink ASE.
		 */
		if (ep->status.state == BT_AUDIO_EP_STATE_STREAMING ||
		    ep->status.state == BT_AUDIO_EP_STATE_ENABLING) {
			if (ep->dir == BT_AUDIO_DIR_SOURCE) {
				ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_DISABLING);
			} else {
				ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_QOS_CONFIGURED);
			}
		}
		err = ascs_iso_listen(stream);
		if (err != 0) {
			LOG_ERR("Could not make stream listen: %d", err);
		}
	}
}

static void ascs_iso_disconnected(struct bt_iso_chan *chan, uint8_t reason)
{
	struct bt_audio_iso *iso = CONTAINER_OF(chan, struct bt_audio_iso, chan);

	if (iso->rx.ep == NULL && iso->tx.ep == NULL) {
		LOG_ERR("iso %p not bound with ep", chan);
		return;
	}

	if (iso->rx.ep != NULL) {
		ascs_ep_iso_disconnected(iso->rx.ep, reason);
	}

	if (iso->tx.ep != NULL) {
		ascs_ep_iso_disconnected(iso->tx.ep, reason);
	}
}

static struct bt_iso_chan_ops ascs_iso_ops = {
	.recv = ascs_iso_recv,
	.sent = ascs_iso_sent,
	.connected = ascs_iso_connected,
	.disconnected = ascs_iso_disconnected,
};

static void ascs_ase_cfg_changed(const struct bt_gatt_attr *attr,
				 uint16_t value)
{
	LOG_DBG("attr %p value 0x%04x", attr, value);
}

NET_BUF_SIMPLE_DEFINE_STATIC(rsp_buf, CONFIG_BT_L2CAP_TX_MTU);

static void ascs_cp_rsp_alloc(uint8_t op)
{
	struct bt_ascs_cp_rsp *rsp;

	rsp = net_buf_simple_add(&rsp_buf, sizeof(*rsp));
	rsp->op = op;
	rsp->num_ase = 0;
}

/* Add response to an opcode/ASE ID */
static void ascs_cp_rsp_add(uint8_t id, uint8_t op, uint8_t code,
			    uint8_t reason)
{
	struct bt_ascs_cp_rsp *rsp = (void *)rsp_buf.__buf;
	struct bt_ascs_cp_ase_rsp *ase_rsp;

	LOG_DBG("id 0x%02x op %s (0x%02x) code %s (0x%02x) reason %s (0x%02x)", id,
		bt_ascs_op_str(op), op, bt_ascs_rsp_str(code), code, bt_ascs_reason_str(reason),
		reason);

	/* Allocate response if buffer is empty */
	if (!rsp_buf.len) {
		ascs_cp_rsp_alloc(op);
	}

	if (rsp->num_ase == 0xff) {
		return;
	}

	switch (code) {
	/* If the Response_Code value is 0x01 or 0x02, Number_of_ASEs shall be
	 * set to 0xFF.
	 */
	case BT_ASCS_RSP_NOT_SUPPORTED:
	case BT_ASCS_RSP_TRUNCATED:
		rsp->num_ase = 0xff;
		break;
	default:
		rsp->num_ase++;
		break;
	}

	ase_rsp = net_buf_simple_add(&rsp_buf, sizeof(*ase_rsp));
	ase_rsp->id = id;
	ase_rsp->code = code;
	ase_rsp->reason = reason;
}

static void ascs_cp_rsp_add_errno(uint8_t id, uint8_t op, int err,
				  uint8_t reason)
{
	LOG_DBG("id %u op %u err %d reason %u", id, op, err, reason);

	switch (err) {
	case -ENOBUFS:
	case -ENOMEM:
		return ascs_cp_rsp_add(id, op, BT_ASCS_RSP_NO_MEM,
				       BT_ASCS_REASON_NONE);
	case -EINVAL:
		switch (op) {
		case BT_ASCS_CONFIG_OP:
		/* Fallthrough */
		case BT_ASCS_QOS_OP:
			return ascs_cp_rsp_add(id, op,
					       BT_ASCS_RSP_CONF_INVALID,
					       reason);
		case BT_ASCS_ENABLE_OP:
		/* Fallthrough */
		case BT_ASCS_METADATA_OP:
			return ascs_cp_rsp_add(id, op,
					       BT_ASCS_RSP_METADATA_INVALID,
					       reason);
		default:
			return ascs_cp_rsp_add(id, op, BT_ASCS_RSP_UNSPECIFIED,
					       BT_ASCS_REASON_NONE);
		}
	case -ENOTSUP:
		switch (op) {
		case BT_ASCS_CONFIG_OP:
		/* Fallthrough */
		case BT_ASCS_QOS_OP:
			return ascs_cp_rsp_add(id, op,
					       BT_ASCS_RSP_CONF_UNSUPPORTED,
					       reason);
		case BT_ASCS_ENABLE_OP:
		/* Fallthrough */
		case BT_ASCS_METADATA_OP:
			return ascs_cp_rsp_add(id, op,
					       BT_ASCS_RSP_METADATA_UNSUPPORTED,
					       reason);
		default:
			return ascs_cp_rsp_add(id, op,
					       BT_ASCS_RSP_NOT_SUPPORTED,
					       BT_ASCS_REASON_NONE);
		}
	case -EBADMSG:
		return ascs_cp_rsp_add(id, op, BT_ASCS_RSP_INVALID_ASE_STATE,
					       BT_ASCS_REASON_NONE);
	case -EACCES:
		switch (op) {
		case BT_ASCS_METADATA_OP:
			return ascs_cp_rsp_add(id, op,
					       BT_ASCS_RSP_METADATA_REJECTED,
					       reason);
		default:
			return ascs_cp_rsp_add(id, op, BT_ASCS_RSP_UNSPECIFIED,
					       BT_ASCS_REASON_NONE);
		}
	default:
		return ascs_cp_rsp_add(id, op, BT_ASCS_RSP_UNSPECIFIED,
				       BT_ASCS_REASON_NONE);
	}
}

static void ascs_cp_rsp_success(uint8_t id, uint8_t op)
{
	ascs_cp_rsp_add(id, op, BT_ASCS_RSP_SUCCESS, BT_ASCS_REASON_NONE);
}

static void ase_release(struct bt_ascs_ase *ase)
{
	int err;

	LOG_DBG("ase %p state %s", ase, bt_audio_ep_state_str(ase->ep.status.state));

	if (ase->ep.status.state == BT_AUDIO_EP_STATE_RELEASING) {
		/* already releasing */
		return;
	}

	if (unicast_server_cb != NULL && unicast_server_cb->release != NULL) {
		err = unicast_server_cb->release(ase->ep.stream);
	} else {
		err = -ENOTSUP;
	}

	if (err) {
		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_RELEASE_OP, err,
				      BT_ASCS_REASON_NONE);
		return;
	}

	ascs_ep_set_state(&ase->ep, BT_AUDIO_EP_STATE_RELEASING);

	ascs_cp_rsp_success(ASE_ID(ase), BT_ASCS_RELEASE_OP);
}

static void ase_disable(struct bt_ascs_ase *ase)
{
	struct bt_audio_stream *stream;
	struct bt_audio_ep *ep;
	int err;

	LOG_DBG("ase %p", ase);

	ep = &ase->ep;

	switch (ep->status.state) {
	/* Valid only if ASE_State field = 0x03 (Enabling) */
	case BT_AUDIO_EP_STATE_ENABLING:
	 /* or 0x04 (Streaming) */
	case BT_AUDIO_EP_STATE_STREAMING:
		break;
	default:
		LOG_WRN("Invalid operation in state: %s", bt_audio_ep_state_str(ep->status.state));
		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_DISABLE_OP,
				      -EBADMSG, BT_ASCS_REASON_NONE);
		return;
	}

	stream = ep->stream;

	if (unicast_server_cb != NULL && unicast_server_cb->disable != NULL) {
		err = unicast_server_cb->disable(stream);
	} else {
		err = -ENOTSUP;
	}

	if (err) {
		LOG_ERR("Disable failed: %d", err);
		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_DISABLE_OP,
				      err, BT_ASCS_REASON_NONE);
		return;
	}

	/* The ASE state machine goes into different states from this operation
	 * based on whether it is a source or a sink ASE.
	 */
	if (ep->dir == BT_AUDIO_DIR_SOURCE) {
		ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_DISABLING);
	} else {
		ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_QOS_CONFIGURED);
	}

	ascs_cp_rsp_success(ASE_ID(ase), BT_ASCS_DISABLE_OP);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_ascs *session = &sessions[bt_conn_index(conn)];

	if (session->conn == NULL) {
		return;
	}

	sys_snode_t *ase_node, *s;

	SYS_SLIST_FOR_EACH_NODE_SAFE(&session->ases, ase_node, s) {
		struct bt_audio_stream *stream;
		struct bt_ascs_ase *ase;

		ase = CONTAINER_OF(ase_node, struct bt_ascs_ase, node);
		stream = ase->ep.stream;

		if (ase->ep.status.state != BT_AUDIO_EP_STATE_IDLE) {
			/* ase_process will handle the final state transition into idle
			 * state, where the ase finally will be deallocated
			 */
			ase_release(ase);

			if (stream != NULL) {
				const struct bt_audio_stream_ops *ops;

				/* Notify upper layer */
				ops = stream->ops;
				if (ops != NULL && ops->released != NULL) {
					ops->released(stream);
				} else {
					LOG_WRN("No callback for released set");
				}
			}
		}

		if (stream != NULL && stream->conn != NULL) {
			bt_conn_unref(stream->conn);
			stream->conn = NULL;
		}
	}

	bt_conn_unref(session->conn);
	session->conn = NULL;
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.disconnected = disconnected,
};

struct audio_iso_find_params {
	struct bt_conn *acl;
	uint8_t cig_id;
	uint8_t cis_id;
};

static bool audio_iso_find_func(struct bt_audio_iso *iso, void *user_data)
{
	struct audio_iso_find_params *params = user_data;
	const struct bt_audio_ep *ep;

	if (iso->rx.ep != NULL) {
		ep = iso->rx.ep;
	} else if (iso->tx.ep != NULL) {
		ep = iso->tx.ep;
	} else {
		return false;
	}

	return ep->stream->conn == params->acl &&
	       ep->cig_id == params->cig_id &&
	       ep->cis_id == params->cis_id;
}

static struct bt_audio_iso *audio_iso_get_or_new(struct bt_ascs *ascs,
						 uint8_t cig_id,
						 uint8_t cis_id)
{
	struct bt_audio_iso *iso;
	struct audio_iso_find_params params = {
		.acl = ascs->conn,
		.cig_id = cig_id,
		.cis_id = cis_id,
	};

	iso = bt_audio_iso_find(audio_iso_find_func, &params);
	if (iso) {
		return iso;
	}

	iso = bt_audio_iso_new();
	if (!iso) {
		return NULL;
	}

	bt_audio_iso_init(iso, &ascs_iso_ops);

	return iso;
}

static void ase_stream_add(struct bt_ascs *ascs, struct bt_ascs_ase *ase,
			   struct bt_audio_stream *stream)
{
	LOG_DBG("ase %p stream %p", ase, stream);
	ase->ep.stream = stream;
	if (stream->conn == NULL) {
		stream->conn = bt_conn_ref(ascs->conn);
	}
	stream->ep = &ase->ep;
}

static struct bt_ascs *ascs_get(struct bt_conn *conn)
{
	struct bt_ascs *session = &sessions[bt_conn_index(conn)];

	if (session->conn == NULL) {
		session->conn = bt_conn_ref(conn);
	}

	return session;
}

static uint8_t ase_attr_cb(const struct bt_gatt_attr *attr, uint16_t handle,
			   void *user_data)
{
	struct bt_ascs_ase *ase = user_data;

	if (ase->ep.status.id == POINTER_TO_UINT(BT_AUDIO_CHRC_USER_DATA(attr))) {
		ase->attr = attr;

		return BT_GATT_ITER_STOP;
	}

	return BT_GATT_ITER_CONTINUE;
}

void ascs_ep_init(struct bt_audio_ep *ep, uint8_t id)
{
	LOG_DBG("ep %p id 0x%02x", ep, id);

	(void)memset(ep, 0, sizeof(*ep));
	ep->status.id = id;
	ep->dir = ASE_DIR(id);
}

static void ase_init(struct bt_ascs_ase *ase, uint8_t id)
{
	ascs_ep_init(&ase->ep, id);

	/* Lookup ASE characteristic */
	bt_gatt_foreach_attr_type(0x0001, 0xffff, ASE_UUID(id), NULL, 0, ase_attr_cb, ase);

	__ASSERT(ase->attr, "ASE characteristic not found\n");
}

static struct bt_ascs_ase *ase_new(struct bt_ascs *ascs, uint8_t id)
{
	struct bt_ascs_ase *ase;

	if (!id || id > ASE_COUNT) {
		return NULL;
	}

	ase = bt_ascs_ase_get_from_slab();
	if (!ase) {
		return NULL;
	}

	sys_slist_append(&ascs->ases, &ase->node);

	ase_init(ase, id);
	ase->ascs = ascs;

	return ase;
}

static struct bt_ascs_ase *ase_find(struct bt_ascs *ascs, uint8_t id)
{
	sys_snode_t *ase_node;

	SYS_SLIST_FOR_EACH_NODE(&ascs->ases, ase_node) {
		struct bt_ascs_ase *ase = CONTAINER_OF(ase_node, struct bt_ascs_ase, node);

		if (ase->ep.status.id == id) {
			return ase;
		}
	}

	return NULL;
}

static struct bt_ascs_ase *ase_get(struct bt_ascs *ascs, uint8_t id)
{
	struct bt_ascs_ase *ase;

	ase = ase_find(ascs, id);
	if (ase) {
		return ase;
	}

	return ase_new(ascs, id);
}

static ssize_t ascs_ase_read(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, void *buf,
			     uint16_t len, uint16_t offset)
{
	struct bt_ascs *ascs = ascs_get(conn);
	struct bt_ascs_ase *ase;
	uint8_t ase_id;

	LOG_DBG("conn %p attr %p buf %p len %u offset %u", conn, attr, buf, len, offset);

	ase_id = POINTER_TO_UINT(BT_AUDIO_CHRC_USER_DATA(attr));

	if (ase_id > ASE_COUNT) {
		LOG_ERR("Unable to get ASE, id out of range");
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	ase = ase_find(ascs, ase_id);

	/* If NULL, we haven't assigned an ASE, this also means that we are currently in IDLE */
	if (!ase) {
		ascs_ep_get_status_idle(ase_id, &ase_buf);
	} else {
		ascs_ep_get_status(&ase->ep, &ase_buf);
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, ase_buf.data,
				 ase_buf.len);
}

static void ascs_cp_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	LOG_DBG("attr %p value 0x%04x", attr, value);
}

static bool ascs_codec_config_store(struct bt_data *data, void *user_data)
{
	struct bt_codec *codec = user_data;
	struct bt_codec_data *cdata;

	if (codec->data_count >= ARRAY_SIZE(codec->data)) {
		LOG_ERR("No slot available for Codec Config");
		return false;
	}

	cdata = &codec->data[codec->data_count];

	if (data->data_len > sizeof(cdata->value)) {
		LOG_ERR("Not enough space for Codec Config: %u > %zu", data->data_len,
			sizeof(cdata->value));
		return false;
	}

	LOG_DBG("#%u type 0x%02x len %u", codec->data_count, data->type, data->data_len);

	cdata->data.type = data->type;
	cdata->data.data_len = data->data_len;

	/* Deep copy data contents */
	cdata->data.data = cdata->value;
	(void)memcpy(cdata->value, data->data, data->data_len);

	LOG_HEXDUMP_DBG(cdata->value, data->data_len, "data");

	codec->data_count++;

	return true;
}

struct codec_lookup_id_data {
	uint8_t id;
	struct bt_codec *codec;
};

static bool codec_lookup_id(const struct bt_pacs_cap *cap, void *user_data)
{
	struct codec_lookup_id_data *data = user_data;

	if (cap->codec->id == data->id) {
		data->codec = cap->codec;

		return false;
	}

	return true;
}

static int ascs_ep_set_codec(struct bt_audio_ep *ep, uint8_t id, uint16_t cid,
			     uint16_t vid, struct net_buf_simple *buf,
			     uint8_t len, struct bt_codec *codec)
{
	struct net_buf_simple ad;
	struct codec_lookup_id_data lookup_data = {
		.id = id,
	};

	if (ep == NULL && codec == NULL) {
		return -EINVAL;
	}

	LOG_DBG("ep %p dir %s codec id 0x%02x cid 0x%04x vid 0x%04x len %u",
		ep, bt_audio_dir_str(ep->dir), id, cid, vid, len);

	bt_pacs_cap_foreach(ep->dir, codec_lookup_id, &lookup_data);

	if (lookup_data.codec == NULL) {
		LOG_DBG("Codec with id %u for dir %s is not supported by our capabilities",
			id, bt_audio_dir_str(ep->dir));

		return -ENOENT;
	}

	if (codec == NULL) {
		codec = &ep->codec;
	}

	codec->id = id;
	codec->cid = cid;
	codec->vid = vid;
	codec->data_count = 0;
	codec->path_id = lookup_data.codec->path_id;

	if (len == 0 || buf == NULL) {
		return 0;
	}

	net_buf_simple_init_with_data(&ad, net_buf_simple_pull_mem(buf, len),
				      len);

	/* Parse LTV entries */
	bt_data_parse(&ad, ascs_codec_config_store, codec);

	/* Check if all entries could be parsed */
	if (ad.len) {
		LOG_ERR("Unable to parse Codec Config: len %u", ad.len);
		(void)memset(codec, 0, sizeof(*codec));

		return -EINVAL;
	}

	return 0;
}

static int ase_config(struct bt_ascs *ascs, struct bt_ascs_ase *ase,
		      const struct bt_ascs_config *cfg,
		      struct net_buf_simple *buf)
{
	struct bt_audio_stream *stream;
	struct bt_codec codec;
	int err;

	LOG_DBG("ase %p latency 0x%02x phy 0x%02x codec 0x%02x "
		"cid 0x%04x vid 0x%04x codec config len 0x%02x", ase,
		cfg->latency, cfg->phy, cfg->codec.id, cfg->codec.cid,
		cfg->codec.vid, cfg->cc_len);

	if (cfg->latency < BT_ASCS_CONFIG_LATENCY_LOW ||
	    cfg->latency > BT_ASCS_CONFIG_LATENCY_HIGH) {
		LOG_WRN("Invalid latency: 0x%02x", cfg->latency);
		ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_CONFIG_OP,
				BT_ASCS_RSP_CONF_INVALID,
				BT_ASCS_REASON_LATENCY);
		return 0;
	}

	if (cfg->phy < BT_ASCS_CONFIG_PHY_LE_1M ||
	    cfg->phy > BT_ASCS_CONFIG_PHY_LE_CODED) {
		LOG_WRN("Invalid PHY: 0x%02x", cfg->phy);
		ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_CONFIG_OP,
				BT_ASCS_RSP_CONF_INVALID, BT_ASCS_REASON_PHY);
		return 0;
	}

	switch (ase->ep.status.state) {
	/* Valid only if ASE_State field = 0x00 (Idle) */
	case BT_AUDIO_EP_STATE_IDLE:
	 /* or 0x01 (Codec Configured) */
	case BT_AUDIO_EP_STATE_CODEC_CONFIGURED:
	 /* or 0x02 (QoS Configured) */
	case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
		break;
	default:
		LOG_WRN("Invalid operation in state: %s",
			bt_audio_ep_state_str(ase->ep.status.state));
		ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_CONFIG_OP,
				BT_ASCS_RSP_INVALID_ASE_STATE, 0x00);
		return 0;
	}

	/* Store current codec configuration to be able to restore it
	 * in case of error.
	 */
	(void)memcpy(&codec, &ase->ep.codec, sizeof(codec));

	if (ascs_ep_set_codec(&ase->ep, cfg->codec.id,
			      sys_le16_to_cpu(cfg->codec.cid),
			      sys_le16_to_cpu(cfg->codec.vid),
			      buf, cfg->cc_len, &ase->ep.codec)) {
		(void)memcpy(&ase->ep.codec, &codec, sizeof(codec));
		ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_CONFIG_OP,
				BT_ASCS_RSP_CONF_INVALID,
				BT_ASCS_REASON_CODEC_DATA);
		return 0;
	}

	if (ase->ep.stream != NULL) {
		if (unicast_server_cb != NULL &&
		    unicast_server_cb->reconfig != NULL) {
			err = unicast_server_cb->reconfig(ase->ep.stream,
							  ase->ep.dir,
							  &ase->ep.codec,
							  &ase->ep.qos_pref);
		} else {
			err = -ENOTSUP;
		}

		if (err != 0) {
			uint8_t reason = BT_ASCS_REASON_CODEC_DATA;

			LOG_ERR("Reconfig failed: %d", err);

			(void)memcpy(&ase->ep.codec, &codec, sizeof(codec));
			ascs_cp_rsp_add_errno(ASE_ID(ase),
					      BT_ASCS_CONFIG_OP,
					      err, reason);
			return 0;
		}

		stream = ase->ep.stream;
	} else {
		stream = NULL;
		if (unicast_server_cb != NULL &&
		    unicast_server_cb->config != NULL) {
			err = unicast_server_cb->config(ascs->conn, &ase->ep,
							ase->ep.dir,
							&ase->ep.codec, &stream,
							&ase->ep.qos_pref);
		} else {
			err = -ENOTSUP;
		}

		if (err != 0 || stream == NULL) {
			LOG_ERR("Config failed, err: %d, stream %p", err, stream);

			(void)memcpy(&ase->ep.codec, &codec, sizeof(codec));
			ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_CONFIG_OP,
					BT_ASCS_RSP_CONF_REJECTED,
					BT_ASCS_REASON_CODEC_DATA);

			return err;
		}

		ase_stream_add(ascs, ase, stream);
	}

	ascs_cp_rsp_success(ASE_ID(ase), BT_ASCS_CONFIG_OP);

	/* TODO: bt_audio_stream_attach duplicates some of the
	 * ase_stream_add. Should be cleaned up.
	 */
	bt_audio_stream_attach(ascs->conn, stream, &ase->ep,
			       &ase->ep.codec);

	ascs_ep_set_state(&ase->ep, BT_AUDIO_EP_STATE_CODEC_CONFIGURED);

	return 0;
}

int bt_ascs_config_ase(struct bt_conn *conn, struct bt_audio_stream *stream, struct bt_codec *codec,
		       const struct bt_codec_qos_pref *qos_pref)
{
	int err;
	struct bt_ascs *ascs;
	struct bt_ascs_ase *ase;
	struct bt_audio_ep *ep;

	CHECKIF(conn == NULL || stream == NULL || codec == NULL || qos_pref == NULL) {
		LOG_DBG("NULL value(s) supplied)");
		return -EINVAL;
	}

	ascs = ascs_get(conn);
	ep = stream->ep;

	if (stream->ep != NULL) {
		LOG_DBG("Stream already configured for conn %p", stream->conn);
		return -EALREADY;
	}

	/* Get a free ASE or NULL if all ASE instances are aready in use */
	for (int i = 1; i <= ASE_COUNT; i++) {
		ase = ase_find(ascs, i);

		if (ase == NULL) {
			ase = ase_new(ascs, i);
			break;
		}
	}

	if (ase == NULL) {
		LOG_WRN("No free ASE found.");
		return -ENOTSUP;
	}

	ep = &ase->ep;

	if (ep->status.state != BT_AUDIO_EP_STATE_IDLE) {
		LOG_ERR("Invalid state: %s", bt_audio_ep_state_str(ep->status.state));
		return -EBADMSG;
	}

	err = ascs_ep_set_codec(ep, codec->id, sys_le16_to_cpu(codec->cid),
				sys_le16_to_cpu(codec->vid), NULL, 0, &ep->codec);
	if (err) {
		return err;
	}

	ep->qos_pref = *qos_pref;

	bt_audio_stream_attach(conn, stream, ep, &ep->codec);

	ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_CODEC_CONFIGURED);

	return 0;
}

static ssize_t ascs_config(struct bt_ascs *ascs, struct net_buf_simple *buf)
{
	const struct bt_ascs_config_op *req;
	const struct bt_ascs_config *cfg;
	int i;

	if (buf->len < sizeof(*req)) {
		LOG_WRN("Malformed ASE Config");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	req = net_buf_simple_pull_mem(buf, sizeof(*req));

	LOG_DBG("num_ases %u", req->num_ases);

	if (req->num_ases < 1) {
		LOG_WRN("Number_of_ASEs parameter value is less than 1");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else if (buf->len < req->num_ases * sizeof(*cfg)) {
		LOG_WRN("Malformed ASE Config: len %u < %zu", buf->len,
			req->num_ases * sizeof(*cfg));
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	for (i = 0; i < req->num_ases; i++) {
		struct bt_ascs_ase *ase;
		int err;

		if (buf->len < sizeof(*cfg)) {
			LOG_WRN("Malformed ASE Config: len %u < %zu", buf->len, sizeof(*cfg));
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		cfg = net_buf_simple_pull_mem(buf, sizeof(*cfg));

		if (buf->len < cfg->cc_len) {
			LOG_WRN("Malformed ASE Codec Config len %u != %u", buf->len, cfg->cc_len);
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		LOG_DBG("ase 0x%02x cc_len %u", cfg->ase, cfg->cc_len);

		if (!cfg->ase || cfg->ase > ASE_COUNT) {
			LOG_WRN("Invalid ASE ID: %u", cfg->ase);
			ascs_cp_rsp_add(cfg->ase, BT_ASCS_CONFIG_OP,
					BT_ASCS_RSP_INVALID_ASE, 0x00);
			continue;
		} else {
			ase = ase_get(ascs, cfg->ase);
		}

		if (!ase) {
			ascs_cp_rsp_add(cfg->ase, BT_ASCS_CONFIG_OP,
					BT_ASCS_RSP_NO_MEM, 0x00);
			LOG_WRN("No free ASE found for config ASE ID 0x%02x", cfg->ase);
			continue;
		}

		err = ase_config(ascs, ase, cfg, buf);
		if (err != 0) {
			LOG_WRN("Malformed ASE Config");
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}
	}

	return buf->size;
}

void bt_ascs_foreach_ep(struct bt_conn *conn, bt_audio_ep_func_t func, void *user_data)
{
	sys_snode_t *ase_node;
	struct bt_ascs *ascs = ascs_get(conn);

	SYS_SLIST_FOR_EACH_NODE(&ascs->ases, ase_node) {
		struct bt_ascs_ase *ase = CONTAINER_OF(ase_node, struct bt_ascs_ase, node);

		func(&ase->ep, user_data);
	}
}

static int ase_stream_qos(struct bt_audio_stream *stream,
			  struct bt_codec_qos *qos,
			  struct bt_ascs *ascs,
			  uint8_t cig_id,
			  uint8_t cis_id)
{
	struct bt_audio_ep *ep;

	if (stream == NULL || stream->ep == NULL || qos == NULL) {
		LOG_DBG("Invalid input stream, ep or qos pointers");
		return -EINVAL;
	}

	LOG_DBG("stream %p ep %p qos %p", stream, stream->ep, qos);

	ep = stream->ep;

	switch (ep->status.state) {
	/* Valid only if ASE_State field = 0x01 (Codec Configured) */
	case BT_AUDIO_EP_STATE_CODEC_CONFIGURED:
	/* or 0x02 (QoS Configured) */
	case BT_AUDIO_EP_STATE_QOS_CONFIGURED:
		break;
	default:
		LOG_WRN("Invalid operation in state: %s", bt_audio_ep_state_str(ep->status.state));
		return -EBADMSG;
	}

	if (!bt_audio_valid_qos(qos)) {
		return -EINVAL;
	}

	if (!bt_audio_valid_stream_qos(stream, qos)) {
		return -EINVAL;
	}

	if (unicast_server_cb != NULL && unicast_server_cb->qos != NULL) {
		int err;

		err = unicast_server_cb->qos(stream, qos);
		if (err != 0) {
			LOG_DBG("Application returned error: %d", err);
			return err;
		}
	}

	/* QoS->QoS transition. Unbind ISO if CIG/CIS changed. */
	if (ep->iso != NULL && (ep->cig_id != cig_id || ep->cis_id != cis_id)) {
		bt_audio_iso_unbind_ep(ep->iso, ep);
	}

	if (ep->iso == NULL) {
		struct bt_audio_iso *iso;

		iso = audio_iso_get_or_new(ascs, cig_id, cis_id);
		if (iso == NULL) {
			LOG_ERR("Could not allocate audio_iso");
			return -ENOMEM;
		}

		if (bt_audio_iso_get_ep(false, iso, ep->dir) != NULL) {
			LOG_ERR("iso %p already in use in dir %s",
			       &iso->chan, bt_audio_dir_str(ep->dir));
			bt_audio_iso_unref(iso);
			return -EALREADY;
		}

		bt_audio_iso_bind_ep(iso, ep);
		bt_audio_iso_unref(iso);
	}

	stream->qos = qos;

	/* We setup the data path here, as this is the earliest where
	 * we have the ISO <-> EP coupling completed (due to setting
	 * the CIS ID in the QoS procedure).
	 */
	if (ep->dir == BT_AUDIO_DIR_SINK) {
		bt_audio_codec_to_iso_path(&ep->iso->rx.path, stream->codec);
	} else {
		bt_audio_codec_to_iso_path(&ep->iso->tx.path, stream->codec);
	}

	ep->cig_id = cig_id;
	ep->cis_id = cis_id;

	ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_QOS_CONFIGURED);

	ascs_iso_listen(stream);

	return 0;
}

static void ase_qos(struct bt_ascs_ase *ase, const struct bt_ascs_qos *qos)
{
	struct bt_audio_ep *ep = &ase->ep;
	struct bt_audio_stream *stream = ep->stream;
	struct bt_codec_qos *cqos = &ep->qos;
	const uint8_t cig_id = qos->cig;
	const uint8_t cis_id = qos->cis;
	int err;

	cqos->interval = sys_get_le24(qos->interval);
	cqos->framing = qos->framing;
	cqos->phy = qos->phy;
	cqos->sdu = sys_le16_to_cpu(qos->sdu);
	cqos->rtn = qos->rtn;
	cqos->latency = sys_le16_to_cpu(qos->latency);
	cqos->pd = sys_get_le24(qos->pd);

	LOG_DBG("ase %p cig 0x%02x cis 0x%02x interval %u framing 0x%02x "
	       "phy 0x%02x sdu %u rtn %u latency %u pd %u", ase, qos->cig,
	       qos->cis, cqos->interval, cqos->framing, cqos->phy, cqos->sdu,
	       cqos->rtn, cqos->latency, cqos->pd);

	err = ase_stream_qos(stream, cqos, ase->ascs, cig_id, cis_id);
	if (err) {
		uint8_t reason = BT_ASCS_REASON_NONE;

		LOG_ERR("QoS failed: err %d", err);

		if (err == -ENOTSUP) {
			if (cqos->interval == 0) {
				reason = BT_ASCS_REASON_INTERVAL;
			} else if (cqos->framing == 0xff) {
				reason = BT_ASCS_REASON_FRAMING;
			} else if (cqos->phy == 0) {
				reason = BT_ASCS_REASON_PHY;
			} else if (cqos->sdu == 0xffff) {
				reason = BT_ASCS_REASON_SDU;
			} else if (cqos->latency == 0) {
				reason = BT_ASCS_REASON_LATENCY;
			} else if (cqos->pd == 0) {
				reason = BT_ASCS_REASON_PD;
			}
		} else if (err == -EALREADY) {
			reason = BT_ASCS_REASON_CIS;
			/* FIXME: Ugly workaround to send Response_Code
			 *        0x09 = Invalid Configuration Parameter Value
			 */
			err = -EINVAL;
		}

		memset(cqos, 0, sizeof(*cqos));

		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_QOS_OP,
				      err, reason);
		return;
	}

	ascs_cp_rsp_success(ASE_ID(ase), BT_ASCS_QOS_OP);
}

static ssize_t ascs_qos(struct bt_ascs *ascs, struct net_buf_simple *buf)
{
	const struct bt_ascs_qos_op *req;
	const struct bt_ascs_qos *qos;
	int i;

	if (buf->len < sizeof(*req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	req = net_buf_simple_pull_mem(buf, sizeof(*req));

	LOG_DBG("num_ases %u", req->num_ases);

	if (req->num_ases < 1) {
		LOG_WRN("Number_of_ASEs parameter value is less than 1");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else if (buf->len < req->num_ases * sizeof(*qos)) {
		LOG_WRN("Malformed ASE QoS: len %u < %zu", buf->len, req->num_ases * sizeof(*qos));
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	for (i = 0; i < req->num_ases; i++) {
		struct bt_ascs_ase *ase;

		qos = net_buf_simple_pull_mem(buf, sizeof(*qos));

		LOG_DBG("ase 0x%02x", qos->ase);

		if (!is_valid_ase_id(qos->ase)) {
			ascs_cp_rsp_add(qos->ase, BT_ASCS_QOS_OP,
					BT_ASCS_RSP_INVALID_ASE, 0x00);
			LOG_WRN("Unknown ase 0x%02x", qos->ase);
			continue;
		}

		ase = ase_find(ascs, qos->ase);
		if (!ase) {
			LOG_DBG("Invalid operation for idle ASE");
			ascs_cp_rsp_add(qos->ase, BT_ASCS_QOS_OP,
					BT_ASCS_RSP_INVALID_ASE_STATE, 0x00);
			continue;
		}

		ase_qos(ase, qos);
	}

	return buf->size;
}

static bool ascs_codec_store_metadata(struct bt_data *data, void *user_data)
{
	struct bt_codec *codec = user_data;
	struct bt_codec_data *meta;

	meta = &codec->meta[codec->meta_count];
	meta->data.type = data->type;
	meta->data.data_len = data->data_len;

	/* Deep copy data contents */
	meta->data.data = meta->value;
	(void)memcpy(meta->value, data->data, data->data_len);

	LOG_DBG("#%zu: data: %s", codec->meta_count, bt_hex(meta->value, data->data_len));

	codec->meta_count++;

	return true;
}

struct ascs_parse_result {
	int err;
	size_t count;
	const struct bt_audio_ep *ep;
};

static bool ascs_parse_metadata(struct bt_data *data, void *user_data)
{
	struct ascs_parse_result *result = user_data;
	const struct bt_audio_ep *ep = result->ep;
	const uint8_t data_len = data->data_len;
	const uint8_t data_type = data->type;
	const uint8_t *data_value = data->data;

	result->count++;

	LOG_DBG("#%u type 0x%02x len %u", result->count, data_type, data_len);

	if (result->count > CONFIG_BT_CODEC_MAX_METADATA_COUNT) {
		LOG_ERR("Not enough buffers for Codec Config Metadata: %zu > %zu", result->count,
			CONFIG_BT_CODEC_MAX_METADATA_LEN);
		result->err = -ENOMEM;

		return false;
	}

	if (data_len > CONFIG_BT_CODEC_MAX_METADATA_LEN) {
		LOG_ERR("Not enough space for Codec Config Metadata: %u > %zu", data->data_len,
			CONFIG_BT_CODEC_MAX_METADATA_LEN);
		result->err = -ENOMEM;

		return false;
	}

	/* The CAP acceptor shall not accept metadata with
	 * unsupported stream context.
	 */
	if (IS_ENABLED(CONFIG_BT_CAP_ACCEPTOR)) {
		if (data_type == BT_AUDIO_METADATA_TYPE_STREAM_CONTEXT) {
			const uint16_t context = sys_get_le16(data_value);

			if (!bt_pacs_context_available(ep->dir, context)) {
				LOG_WRN("Context 0x%04x is unavailable", context);

				result->err = -EACCES;

				return false;
			}
		} else if (data_type == BT_AUDIO_METADATA_TYPE_CCID_LIST) {
			/* Verify that the CCID is a known CCID on the
			 * writing device
			 */
			for (uint8_t i = 0; i < data_len; i++) {
				const uint8_t ccid = data_value[i];

				if (!bt_cap_acceptor_ccid_exist(ep->stream->conn,
								ccid)) {
					LOG_WRN("CCID %u is unknown", ccid);

					/* TBD:
					 * Should we reject the Metadata?
					 *
					 * Should unknown CCIDs trigger a
					 * discovery procedure for TBS or MCS?
					 *
					 * Or should we just accept as is, and
					 * then let the application decide?
					 */
				}
			}
		}
	}

	return true;
}

static int ascs_verify_metadata(const struct net_buf_simple *buf,
				struct bt_audio_ep *ep)
{
	struct ascs_parse_result result = {
		.count = 0U,
		.err = 0,
		.ep = ep
	};
	struct net_buf_simple meta_ltv;

	/* Clone the buf to avoid pulling data from the original buffer */
	net_buf_simple_clone(buf, &meta_ltv);

	/* Parse LTV entries */
	bt_data_parse(&meta_ltv, ascs_parse_metadata, &result);

	/* Check if all entries could be parsed */
	if (meta_ltv.len != 0) {
		LOG_ERR("Unable to parse Metadata: len %u", meta_ltv.len);

		if (meta_ltv.len > 2) {
			/* Value of the Metadata Type field in error */
			return meta_ltv.data[2];
		}

		return -EINVAL;
	}

	return result.err;
}

static int ascs_ep_set_metadata(struct bt_audio_ep *ep, struct net_buf_simple *buf, uint8_t len,
				struct bt_codec *codec)
{
	struct net_buf_simple meta_ltv;
	int err;

	if (ep == NULL && codec == NULL) {
		return -EINVAL;
	}

	LOG_DBG("ep %p len %u codec %p", ep, len, codec);

	if (len == 0) {
		(void)memset(codec->meta, 0, sizeof(codec->meta));
		return 0;
	}

	if (codec == NULL) {
		codec = &ep->codec;
	}

	/* Extract metadata LTV for this specific endpoint */
	net_buf_simple_init_with_data(&meta_ltv,
				      net_buf_simple_pull_mem(buf, len),
				      len);

	err = ascs_verify_metadata(&meta_ltv, ep);
	if (err != 0) {
		return err;
	}

	/* reset cached metadata */
	ep->codec.meta_count = 0;

	/* store data contents */
	bt_data_parse(&meta_ltv, ascs_codec_store_metadata, codec);

	return 0;
}

static int ase_metadata(struct bt_ascs_ase *ase, uint8_t op,
			struct bt_ascs_metadata *meta,
			struct net_buf_simple *buf)
{
	struct bt_codec_data metadata_backup[CONFIG_BT_CODEC_MAX_DATA_COUNT];
	struct bt_audio_stream *stream;
	struct bt_audio_ep *ep;
	uint8_t state;
	int err;

	LOG_DBG("ase %p meta->len %u", ase, meta->len);

	ep = &ase->ep;
	state = ep->status.state;

	switch (state) {
	/* Valid for an ASE only if ASE_State field = 0x03 (Enabling) */
	case BT_AUDIO_EP_STATE_ENABLING:
	/* or 0x04 (Streaming) */
	case BT_AUDIO_EP_STATE_STREAMING:
		break;
	default:
		LOG_WRN("Invalid operation in state: %s", bt_audio_ep_state_str(state));
		err = -EBADMSG;
		ascs_cp_rsp_add_errno(ASE_ID(ase), op, err,
				      buf->len ? *buf->data : 0x00);
		return err;
	}

	if (!meta->len) {
		goto done;
	}

	/* Backup existing metadata */
	(void)memcpy(metadata_backup, ep->codec.meta, sizeof(metadata_backup));
	err = ascs_ep_set_metadata(ep, buf, meta->len, &ep->codec);
	if (err) {
		if (err < 0) {
			ascs_cp_rsp_add_errno(ASE_ID(ase), op, err, 0x00);
		} else {
			ascs_cp_rsp_add(ASE_ID(ase), op,
					BT_ASCS_RSP_METADATA_INVALID, err);
		}
		return 0;
	}

	stream = ep->stream;
	if (unicast_server_cb != NULL && unicast_server_cb->metadata != NULL) {
		err = unicast_server_cb->metadata(stream, ep->codec.meta,
						  ep->codec.meta_count);
	} else {
		err = -ENOTSUP;
	}

	if (err) {
		/* Restore backup */
		(void)memcpy(ep->codec.meta, metadata_backup,
			     sizeof(metadata_backup));

		LOG_ERR("Metadata failed: %d", err);
		ascs_cp_rsp_add_errno(ASE_ID(ase), op, err,
				      buf->len ? *buf->data : 0x00);
		return err;
	}

	/* Set the state to the same state to trigger the notifications */
	ascs_ep_set_state(ep, ep->status.state);
done:
	ascs_cp_rsp_success(ASE_ID(ase), op);

	return 0;
}

static int ase_enable(struct bt_ascs_ase *ase, struct bt_ascs_metadata *meta,
		      struct net_buf_simple *buf)
{
	struct bt_audio_stream *stream;
	struct bt_audio_ep *ep;
	int err;

	LOG_DBG("ase %p buf->len %u", ase, buf->len);

	ep = &ase->ep;

	/* Valid for an ASE only if ASE_State field = 0x02 (QoS Configured) */
	if (ep->status.state != BT_AUDIO_EP_STATE_QOS_CONFIGURED) {
		err = -EBADMSG;
		LOG_WRN("Invalid operation in state: %s", bt_audio_ep_state_str(ep->status.state));
		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_ENABLE_OP, err,
				      BT_ASCS_REASON_NONE);
		return err;
	}

	err = ascs_ep_set_metadata(ep, buf, meta->len, &ep->codec);
	if (err) {
		if (err < 0) {
			ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_ENABLE_OP,
					      err, 0x00);
		} else {
			ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_ENABLE_OP,
					BT_ASCS_RSP_METADATA_INVALID, err);
		}
		return 0;
	}

	stream = ep->stream;
	if (unicast_server_cb != NULL && unicast_server_cb->enable != NULL) {
		err = unicast_server_cb->enable(stream, ep->codec.meta,
						ep->codec.meta_count);
	} else {
		err = -ENOTSUP;
	}

	if (err) {
		LOG_ERR("Enable rejected: %d", err);
		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_ENABLE_OP, err,
				      BT_ASCS_REASON_NONE);
		return -EFAULT;
	}

	ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_ENABLING);

	ascs_cp_rsp_success(ASE_ID(ase), BT_ASCS_ENABLE_OP);

	return 0;
}

static ssize_t ascs_enable(struct bt_ascs *ascs, struct net_buf_simple *buf)
{
	const struct bt_ascs_enable_op *req;
	struct bt_ascs_metadata *meta;
	int i;

	if (buf->len < sizeof(*req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	req = net_buf_simple_pull_mem(buf, sizeof(*req));

	LOG_DBG("num_ases %u", req->num_ases);

	if (req->num_ases < 1) {
		LOG_WRN("Number_of_ASEs parameter value is less than 1");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else if (buf->len < req->num_ases * sizeof(*meta)) {
		LOG_WRN("Malformed ASE Metadata: len %u < %zu", buf->len,
			req->num_ases * sizeof(*meta));
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	for (i = 0; i < req->num_ases; i++) {
		struct bt_ascs_ase *ase;

		meta = net_buf_simple_pull_mem(buf, sizeof(*meta));

		LOG_DBG("ase 0x%02x meta->len %u", meta->ase, meta->len);

		if (buf->len < meta->len) {
			LOG_WRN("Malformed ASE Enable Metadata len %u != %u", buf->len, meta->len);
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		if (!is_valid_ase_id(meta->ase)) {
			ascs_cp_rsp_add(meta->ase, BT_ASCS_ENABLE_OP,
					BT_ASCS_RSP_INVALID_ASE, 0x00);
			LOG_WRN("Unknown ase 0x%02x", meta->ase);
			continue;
		}

		ase = ase_find(ascs, meta->ase);
		if (!ase) {
			LOG_DBG("Invalid operation for idle ASE");
			ascs_cp_rsp_add(meta->ase, BT_ASCS_ENABLE_OP,
					BT_ASCS_RSP_INVALID_ASE_STATE, 0x00);
			continue;
		}

		ase_enable(ase, meta, buf);
	}

	return buf->size;
}

static void ase_start(struct bt_ascs_ase *ase)
{
	struct bt_audio_ep *ep;
	int err;

	LOG_DBG("ase %p", ase);

	ep = &ase->ep;

	/* Valid for an ASE only if ASE_State field = 0x02 (QoS Configured) */
	if (ep->status.state != BT_AUDIO_EP_STATE_ENABLING) {
		LOG_WRN("Invalid operation in state: %s", bt_audio_ep_state_str(ep->status.state));
		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_START_OP, -EBADMSG,
				      BT_ASCS_REASON_NONE);
		return;
	}

	/* If the ASE_ID  written by the client represents a Sink ASE, the
	 * server shall not accept the Receiver Start Ready operation for that
	 * ASE. The server shall send a notification of the ASE Control Point
	 * characteristic to the client, and the server shall set the
	 * Response_Code value for that ASE to 0x05 (Invalid ASE direction).
	 */
	if (ep->dir == BT_AUDIO_DIR_SINK) {
		LOG_WRN("Start failed: invalid operation for Sink");
		ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_START_OP,
				BT_ASCS_RSP_INVALID_DIR, BT_ASCS_REASON_NONE);
		return;
	} else if (ep->iso->chan.state != BT_ISO_STATE_CONNECTED) {
		/* An ASE may not go into the streaming state unless the CIS
		 * is connected
		 */
		LOG_WRN("Start failed: CIS not connected: %u",
			ep->iso->chan.state);
		ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_START_OP,
				BT_ASCS_RSP_INVALID_ASE_STATE,
				BT_ASCS_REASON_NONE);
		return;
	}

	if (unicast_server_cb != NULL && unicast_server_cb->start != NULL) {
		err = unicast_server_cb->start(ep->stream);
	} else {
		err = -ENOTSUP;
	}

	if (err) {
		LOG_ERR("Start failed: %d", err);
		ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_START_OP, err,
				BT_ASCS_REASON_NONE);

		return;
	}

	ep->receiver_ready = true;

	ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_STREAMING);

	ascs_cp_rsp_success(ASE_ID(ase), BT_ASCS_START_OP);
}

static ssize_t ascs_start(struct bt_ascs *ascs, struct net_buf_simple *buf)
{
	const struct bt_ascs_start_op *req;
	int i;

	if (buf->len < sizeof(*req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	req = net_buf_simple_pull_mem(buf, sizeof(*req));

	LOG_DBG("num_ases %u", req->num_ases);

	if (req->num_ases < 1) {
		LOG_WRN("Number_of_ASEs parameter value is less than 1");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else if (buf->len < req->num_ases) {
		LOG_WRN("Malformed ASE Start: len %u < %u", buf->len, req->num_ases);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	for (i = 0; i < req->num_ases; i++) {
		struct bt_ascs_ase *ase;
		uint8_t id;

		id = net_buf_simple_pull_u8(buf);

		LOG_DBG("ase 0x%02x", id);

		if (!is_valid_ase_id(id)) {
			ascs_cp_rsp_add(id, BT_ASCS_START_OP,
					BT_ASCS_RSP_INVALID_ASE, 0x00);
			LOG_WRN("Unknown ase 0x%02x", id);
			continue;
		}

		ase = ase_find(ascs, id);
		if (!ase) {
			LOG_DBG("Invalid operation for idle ASE");
			ascs_cp_rsp_add(id, BT_ASCS_START_OP,
					BT_ASCS_RSP_INVALID_ASE_STATE, 0x00);
			continue;
		}

		ase_start(ase);
	}

	return buf->size;
}

static ssize_t ascs_disable(struct bt_ascs *ascs, struct net_buf_simple *buf)
{
	const struct bt_ascs_disable_op *req;
	int i;

	if (buf->len < sizeof(*req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	req = net_buf_simple_pull_mem(buf, sizeof(*req));

	LOG_DBG("num_ases %u", req->num_ases);

	if (req->num_ases < 1) {
		LOG_WRN("Number_of_ASEs parameter value is less than 1");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else if (buf->len < req->num_ases) {
		LOG_WRN("Malformed ASE Disable: len %u < %u", buf->len, req->num_ases);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	for (i = 0; i < req->num_ases; i++) {
		struct bt_ascs_ase *ase;
		uint8_t id;

		id = net_buf_simple_pull_u8(buf);

		LOG_DBG("ase 0x%02x", id);

		if (!is_valid_ase_id(id)) {
			ascs_cp_rsp_add(id, BT_ASCS_DISABLE_OP,
					BT_ASCS_RSP_INVALID_ASE, 0x00);
			LOG_WRN("Unknown ase 0x%02x", id);
			continue;
		}

		ase = ase_find(ascs, id);
		if (!ase) {
			LOG_DBG("Invalid operation for idle ASE");
			ascs_cp_rsp_add(id, BT_ASCS_DISABLE_OP,
					BT_ASCS_RSP_INVALID_ASE_STATE, 0x00);
			continue;
		}

		ase_disable(ase);
	}

	return buf->size;
}

static void ase_stop(struct bt_ascs_ase *ase)
{
	struct bt_audio_stream *stream;
	struct bt_audio_ep *ep;
	int err;

	LOG_DBG("ase %p", ase);

	ep = &ase->ep;

	/* If the ASE_ID  written by the client represents a Sink ASE, the
	 * server shall not accept the Receiver Stop Ready operation for that
	 * ASE. The server shall send a notification of the ASE Control Point
	 * characteristic to the client, and the server shall set the
	 * Response_Code value for that ASE to 0x05 (Invalid ASE direction).
	 */
	if (ase->ep.dir == BT_AUDIO_DIR_SINK) {
		LOG_WRN("Stop failed: invalid operation for Sink");
		ascs_cp_rsp_add(ASE_ID(ase), BT_ASCS_STOP_OP,
				BT_ASCS_RSP_INVALID_DIR, BT_ASCS_REASON_NONE);
		return;
	}

	if (ep->status.state != BT_AUDIO_EP_STATE_DISABLING) {
		LOG_WRN("Invalid operation in state: %s", bt_audio_ep_state_str(ep->status.state));
		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_STOP_OP, -EBADMSG,
				      BT_ASCS_REASON_NONE);
		return;
	}

	stream = ep->stream;
	if (unicast_server_cb != NULL && unicast_server_cb->stop != NULL) {
		err = unicast_server_cb->stop(stream);
	} else {
		err = -ENOTSUP;
	}

	if (err) {
		LOG_ERR("Stop failed: %d", err);
		ascs_cp_rsp_add_errno(ASE_ID(ase), BT_ASCS_STOP_OP, err,
				      BT_ASCS_REASON_NONE);
		return;
	}

	/* If the Receiver Stop Ready operation has completed successfully the
	 * Unicast Client or the Unicast Server may terminate a CIS established
	 * for that ASE by following the Connected Isochronous Stream Terminate
	 * procedure defined in Volume 3, Part C, Section 9.3.15.
	 */
	err = ascs_stream_disconnect(stream);
	if (err != -ENOTCONN && err != 0) {
		LOG_ERR("Could not disconnect the CIS: %d", err);
		return;
	}

	ascs_ep_set_state(ep, BT_AUDIO_EP_STATE_QOS_CONFIGURED);
	err = ascs_iso_listen(stream);
	if (err != 0) {
		LOG_ERR("Could not make stream listen: %d", err);
		return;
	}

	ascs_cp_rsp_success(ASE_ID(ase), BT_ASCS_STOP_OP);
}

static ssize_t ascs_stop(struct bt_ascs *ascs, struct net_buf_simple *buf)
{
	const struct bt_ascs_start_op *req;
	int i;

	if (buf->len < sizeof(*req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	req = net_buf_simple_pull_mem(buf, sizeof(*req));

	LOG_DBG("num_ases %u", req->num_ases);

	if (req->num_ases < 1) {
		LOG_WRN("Number_of_ASEs parameter value is less than 1");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else if (buf->len < req->num_ases) {
		LOG_WRN("Malformed ASE Start: len %u < %u", buf->len, req->num_ases);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	for (i = 0; i < req->num_ases; i++) {
		struct bt_ascs_ase *ase;
		uint8_t id;

		id = net_buf_simple_pull_u8(buf);

		LOG_DBG("ase 0x%02x", id);

		if (!is_valid_ase_id(id)) {
			ascs_cp_rsp_add(id, BT_ASCS_STOP_OP,
					BT_ASCS_RSP_INVALID_ASE, 0x00);
			LOG_WRN("Unknown ase 0x%02x", id);
			continue;
		}

		ase = ase_find(ascs, id);
		if (!ase) {
			LOG_DBG("Invalid operation for idle ASE");
			ascs_cp_rsp_add(id, BT_ASCS_STOP_OP,
					BT_ASCS_RSP_INVALID_ASE_STATE, 0x00);
			continue;
		}

		ase_stop(ase);
	}

	return buf->size;
}

static ssize_t ascs_metadata(struct bt_ascs *ascs, struct net_buf_simple *buf)
{
	const struct bt_ascs_metadata_op *req;
	struct bt_ascs_metadata *meta;
	int i;

	if (buf->len < sizeof(*req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	req = net_buf_simple_pull_mem(buf, sizeof(*req));

	LOG_DBG("num_ases %u", req->num_ases);

	if (req->num_ases < 1) {
		LOG_WRN("Number_of_ASEs parameter value is less than 1");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else if (buf->len < req->num_ases * sizeof(*meta)) {
		LOG_WRN("Malformed ASE Metadata: len %u < %zu", buf->len,
			req->num_ases * sizeof(*meta));
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	for (i = 0; i < req->num_ases; i++) {
		struct bt_ascs_ase *ase;

		meta = net_buf_simple_pull_mem(buf, sizeof(*meta));

		if (buf->len < meta->len) {
			LOG_WRN("Malformed ASE Metadata: len %u < %u", buf->len, meta->len);
			return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		}

		LOG_DBG("ase 0x%02x meta->len %u", meta->ase, meta->len);

		if (!is_valid_ase_id(meta->ase)) {
			ascs_cp_rsp_add(meta->ase, BT_ASCS_METADATA_OP,
					BT_ASCS_RSP_INVALID_ASE, 0x00);
			LOG_WRN("Unknown ase 0x%02x", meta->ase);
			continue;
		}

		ase = ase_find(ascs, meta->ase);
		if (!ase) {
			LOG_DBG("Invalid operation for idle ASE");
			ascs_cp_rsp_add(meta->ase, BT_ASCS_METADATA_OP,
					BT_ASCS_RSP_INVALID_ASE_STATE, 0x00);
			continue;
		}

		ase_metadata(ase, BT_ASCS_METADATA_OP, meta, buf);
	}

	return buf->size;
}

static ssize_t ascs_release(struct bt_ascs *ascs, struct net_buf_simple *buf)
{
	const struct bt_ascs_release_op *req;
	int i;

	if (buf->len < sizeof(*req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	req = net_buf_simple_pull_mem(buf, sizeof(*req));

	LOG_DBG("num_ases %u", req->num_ases);

	if (req->num_ases < 1) {
		LOG_WRN("Number_of_ASEs parameter value is less than 1");
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	} else if (buf->len < req->num_ases) {
		LOG_WRN("Malformed ASE Release: len %u < %u", buf->len, req->num_ases);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	for (i = 0; i < req->num_ases; i++) {
		uint8_t id;
		struct bt_ascs_ase *ase;

		id = net_buf_simple_pull_u8(buf);

		LOG_DBG("ase 0x%02x", id);

		if (!is_valid_ase_id(id)) {
			ascs_cp_rsp_add(id, BT_ASCS_RELEASE_OP,
					BT_ASCS_RSP_INVALID_ASE, 0x00);
			LOG_WRN("Unknown ase 0x%02x", id);
			continue;
		}

		ase = ase_find(ascs, id);
		if (!ase) {
			LOG_DBG("Invalid operation for idle ASE");
			ascs_cp_rsp_add(id, BT_ASCS_RELEASE_OP,
					BT_ASCS_RSP_INVALID_ASE_STATE, 0x00);
			continue;
		}

		if (ase->ep.status.state == BT_AUDIO_EP_STATE_IDLE ||
		    ase->ep.status.state == BT_AUDIO_EP_STATE_RELEASING) {
			LOG_WRN("Invalid operation in state: %s",
				bt_audio_ep_state_str(ase->ep.status.state));
			ascs_cp_rsp_add(id, BT_ASCS_RELEASE_OP,
					BT_ASCS_RSP_INVALID_ASE_STATE, BT_ASCS_REASON_NONE);
			continue;
		}

		ase_release(ase);
	}

	return buf->size;
}

static ssize_t ascs_cp_write(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr, const void *data,
			     uint16_t len, uint16_t offset, uint8_t flags)
{
	struct bt_ascs *ascs = ascs_get(conn);
	const struct bt_ascs_ase_cp *req;
	struct net_buf_simple buf;
	ssize_t ret;

	if (offset) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len < sizeof(*req)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	net_buf_simple_init_with_data(&buf, (void *) data, len);

	req = net_buf_simple_pull_mem(&buf, sizeof(*req));

	LOG_DBG("conn %p attr %p buf %p len %u op %s (0x%02x)", conn, attr, data, len,
		bt_ascs_op_str(req->op), req->op);

	/* Reset/empty response buffer before using it again */
	net_buf_simple_reset(&rsp_buf);

	switch (req->op) {
	case BT_ASCS_CONFIG_OP:
		ret = ascs_config(ascs, &buf);
		break;
	case BT_ASCS_QOS_OP:
		ret = ascs_qos(ascs, &buf);
		break;
	case BT_ASCS_ENABLE_OP:
		ret = ascs_enable(ascs, &buf);
		break;
	case BT_ASCS_START_OP:
		ret = ascs_start(ascs, &buf);
		break;
	case BT_ASCS_DISABLE_OP:
		ret = ascs_disable(ascs, &buf);
		break;
	case BT_ASCS_STOP_OP:
		ret = ascs_stop(ascs, &buf);
		break;
	case BT_ASCS_METADATA_OP:
		ret = ascs_metadata(ascs, &buf);
		break;
	case BT_ASCS_RELEASE_OP:
		ret = ascs_release(ascs, &buf);
		break;
	default:
		ascs_cp_rsp_add(0x00, req->op, BT_ASCS_RSP_NOT_SUPPORTED, 0);
		LOG_DBG("Unknown opcode");
		goto respond;
	}

	if (ret == BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN)) {
		ascs_cp_rsp_add(0, req->op, BT_ASCS_RSP_TRUNCATED,
				BT_ASCS_REASON_NONE);
	}

respond:
	control_point_notify(ascs->conn, rsp_buf.data, rsp_buf.len);

	return len;
}

#define BT_ASCS_ASE_DEFINE(_uuid, _id) \
	BT_AUDIO_CHRC(_uuid, \
		      BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, \
		      BT_GATT_PERM_READ_ENCRYPT, \
		      ascs_ase_read, NULL, UINT_TO_POINTER(_id)), \
	BT_AUDIO_CCC(ascs_ase_cfg_changed)
#define BT_ASCS_ASE_SNK_DEFINE(_n, ...) BT_ASCS_ASE_DEFINE(BT_UUID_ASCS_ASE_SNK, (_n) + 1)
#define BT_ASCS_ASE_SRC_DEFINE(_n, ...) BT_ASCS_ASE_DEFINE(BT_UUID_ASCS_ASE_SRC, (_n) + 1 + \
							   CONFIG_BT_ASCS_ASE_SNK_COUNT)

BT_GATT_SERVICE_DEFINE(ascs_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_ASCS),
	BT_AUDIO_CHRC(BT_UUID_ASCS_ASE_CP,
		      BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
		      BT_GATT_PERM_WRITE_ENCRYPT,
		      NULL, ascs_cp_write, NULL),
	BT_AUDIO_CCC(ascs_cp_cfg_changed),
#if CONFIG_BT_ASCS_ASE_SNK_COUNT > 0
	LISTIFY(CONFIG_BT_ASCS_ASE_SNK_COUNT, BT_ASCS_ASE_SNK_DEFINE, (,)),
#endif /* CONFIG_BT_ASCS_ASE_SNK_COUNT > 0 */
#if CONFIG_BT_ASCS_ASE_SRC_COUNT > 0
	LISTIFY(CONFIG_BT_ASCS_ASE_SRC_COUNT, BT_ASCS_ASE_SRC_DEFINE, (,)),
#endif /* CONFIG_BT_ASCS_ASE_SRC_COUNT > 0 */
);

static int control_point_notify(struct bt_conn *conn, const void *data, uint16_t len)
{
	return bt_gatt_notify_uuid(conn, BT_UUID_ASCS_ASE_CP, ascs_svc.attrs, data, len);
}

#endif /* BT_AUDIO_UNICAST_SERVER */
