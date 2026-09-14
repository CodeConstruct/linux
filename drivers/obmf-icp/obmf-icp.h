// SPDX-License-Identifier: GPL-2.0-only
/*
 * Open Boot Management Framework: Interface Consolidation Protocol (OBMF-ICP)
 * support.
 *
 * Core OBMF-ICP definitions: messaging support, bus type, channel device/driver
 * implementation
 *
 * Copyright (c) 2026 Code Construct
 */

#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/types.h>
#include <linux/usb.h>

/* Base protocol constants */
#define OBMF_ICP_MTU 0x216
#define OBMF_ICP_REV 0
#define OBMF_ICP_CHANNEL_INDEX_DISCOVERY 1
#define OBMF_ICP_CHANNEL_TYPE_DISCOVERY 0
#define OBMF_ICP_CHANNEL_TYPE_MMIO 1
#define OBMF_ICP_CHANNEL_TYPE_GPIO 2
#define OBMF_ICP_CHANNEL_TYPE_SERIAL 3

#define OBMF_ICP_MSG_HDR_STATUS_FIELD		GENMASK(6, 0)
#define OBMF_ICP_MSG_HDR_RQRESP_FIELD		BIT(7)
#define OBMF_ICP_MSG_HDR_RQRESP_VALUE_REQUEST	0
#define OBMF_ICP_MSG_HDR_RQRESP_VALUE_RESPONSE	1

/* Base message format */
struct obmf_icp_msg_hdr {
	u8 channel;
	u8 status_rq;
	__le16 size;
} __packed;

struct obmf_icp_msg {
	struct obmf_icp_msg_hdr hdr;
	unsigned char payload[];
} __packed;

/* Generic status codes, 0x00 - 0x3f. May be augmented with channel-specific
 * status codes 0x40 - 0x7f.
 */
enum obmf_icp_msg_status {
	OBMF_ICP_MSG_STS_SUCCESS = 0x00,
	OBMF_ICP_MSG_STS_CMD_PARAM_INVALID = 0x01,
	OBMF_ICP_MSG_STS_REQ_TIMEOUT = 0x02,
	OBMF_ICP_MSG_STS_CHAN_NOT_READY = 0x03,
	OBMF_ICP_MSG_STS_CHAN_PERM_ERR = 0x04,
	OBMF_ICP_MSG_STS_CHAN_INVALID = 0x05,
	OBMF_ICP_MSG_STS_REQ_SIZE_INVALID = 0x06,
};

static inline void obmf_icp_msg_get_status_rq(struct obmf_icp_msg_hdr *hdr,
					      u8 *status, u8 *rq)
{
	u8 status_rq = hdr->status_rq;

	if (status)
		*status = FIELD_GET(OBMF_ICP_MSG_HDR_STATUS_FIELD, status_rq);
	if (rq)
		*rq = FIELD_GET(OBMF_ICP_MSG_HDR_RQRESP_FIELD, status_rq);
}

/* discovery */
struct obmf_icp_ch0_channel_common {
	u8	type;
	u8	idx;
	u8	name[16];
	u8	role;
	u8	health_status;
	u8	control;
	u8	channel_specific_status;
	u8	channel_specific_control;
	__le16	max_req_payload_size;
	__le16	max_resp_payload_size;
	u8	rsvd;
	__le32	config_size;
} __packed;

#define OBMF_ICP_CH0_CONFIG_CHANNEL_SPECIFIC_OFFSET 0x20

/* OBMF-ICP implementation */
struct obmf_icp;
struct obmf_icp_tx;
struct obmf_icp_transport;

struct obmf_icp_transport_ops {
	/* Initialise a transmit context, @tx. The context is long-running,
	 * typically around for the life of a channel.
	 *
	 * Transport implementations will typically use this to allocate any
	 * transport-specific tx facilities, and may store in
	 * tx->transport_data.
	 *
	 * Optional.
	 */
	int (*tx_init)(struct obmf_icp_transport *transport,
		       struct obmf_icp_tx *tx);

	/* Finalise a transmit context @tx. Typically the inverse of ->tx_init
	 *
	 * Optional.
	 */
	void (*tx_fini)(struct obmf_icp_transport *transport,
			struct obmf_icp_tx *tx);

	/* Submit a tx context for device transmit.
	 *
	 * Implementations should send the tx->buf data (of tx->len) length,
	 * and set tx->status to reflect any transport errors, then
	 * indicate completion via obmf_icp_transport_tx_complete().
	 */
	int (*tx_submit)(struct obmf_icp *icp, struct obmf_icp_tx *tx);

	/* Cancel a tx context
	 *
	 * Implementations do not need to trigger a completion on
	 * cancel.
	 */
	void (*tx_cancel)(struct obmf_icp *icp, struct obmf_icp_tx *tx);

	/* Submit an incoming receive, for received data into icp->in_buf.
	 *
	 * On receive, the transport impemlementation must call
	 * obmf_icp_transport_rx_complete() to indicate receive status and
	 * length. Once the core is ready for further
	 * received data, it will call ->rx_submit() again.
	 *
	 * Will be called from atomic contexts; implementations must not sleep.
	 *
	 * The receive context is global to the icp device; only one receive
	 * is submitted any any time.
	 */
	int (*rx_submit)(struct obmf_icp *icp);

	/* Cancel any ongoing submitted receive. */
	int (*rx_cancel)(struct obmf_icp_transport *transport);
};

struct obmf_icp_transport {
	struct obmf_icp *icp;
	const struct obmf_icp_transport_ops *ops;
};

/* transport calls in atomic context */
void obmf_icp_transport_rx_complete(struct obmf_icp *icp, int status,
				    size_t len);

/* transport updates tx->status, calls in atomic context */
void obmf_icp_transport_tx_complete(struct obmf_icp *icp,
				    struct obmf_icp_tx *tx);

void obmf_icp_transport_disconnect(struct obmf_icp_transport *ts);

struct obmf_icp {
	struct device dev;
	unsigned int idx;

	/* this includes channel 0, so channels are indexed by their
	 * on-the-wire channel index in this array
	 */
	unsigned int n_channels;
	struct obmf_icp_channel_device **channels __counted_by(n_channels);

	struct obmf_icp_channel_device *ch0;

	struct obmf_icp_transport *transport;

	/* todo: pool, buffers handed off to channel's rx */
	u8 in_buf[OBMF_ICP_MTU];

	spinlock_t lock;
};

int obmf_icp_bus_register(struct obmf_icp *icp);
void obmf_icp_bus_unregister(struct obmf_icp *icp);

struct obmf_icp_requester;
struct obmf_icp_responder;

/* channel / device types */
struct obmf_icp_channel_device {
	struct device dev;

	u8 idx, type;
	u32 cfg_ptr;
	/* channel-specific size, 0 representing only common config */
	u32 cfg_size;
	u16 max_req_payload_size;
	u16 max_resp_payload_size;

	bool enabled;

	struct obmf_icp *icp;
	void *chan_priv;

	/* access via obmf_icp_channel_get_requester() */
	struct obmf_icp_requester *req;
	/* access via obmf_icp_channel_get_responder() */
	struct obmf_icp_responder *rsp;
};

/* context for request / response transmit */
struct obmf_icp_tx {
	u8 buf[OBMF_ICP_MTU];
	size_t len;
	int status;
	struct obmf_icp *icp;
	void *transport_data;
	struct completion completion;
};

/* context for request / response receive */
struct obmf_icp_rx {
	struct obmf_icp_channel_device *chan;
	u8 buf[OBMF_ICP_MTU];
	size_t len;
	int status;
	struct completion completion;
};

/* requester context, with facility to transmit then receive */
struct obmf_icp_requester {
	struct mutex mutex;
	struct obmf_icp_tx tx;
	struct obmf_icp_rx rx;
	void *transport_data;
};

/* Callback for incoming messages to a responder.
 *
 * The responder's RX data is populated from the incoming message, but
 * will be overwritten after msg_fn completes. Implementations that use
 * the message data outside the scope of the function will need to take a
 * copy.
 *
 * Implementations will typically prepare their response into the responder
 * tx. The TX will be initiated after msg_fn completion, using the
 * prepared message.
 *
 * On a non-zero return, the return value will be used to construct an
 * error response, with no payload, using the provided status code. On
 * zero return, the message is transmitted as-is, so much have been
 * prepared by the implementation.
 *
 * Called in workqueue context; implementations may sleep.
 */
typedef u8 (*obmf_icp_responder_msg_fn)(
	struct obmf_icp_channel_device *chan,
	struct obmf_icp_responder *rsp);

struct obmf_icp_responder {
	struct obmf_icp_channel_device *chan;
	struct obmf_icp_rx rx;
	struct obmf_icp_tx tx;
	atomic_t in_use;
	struct work_struct msg_work;
	obmf_icp_responder_msg_fn msg_fn;
};

struct obmf_icp_channel_device_id {
	unsigned int match_flags;
	u8 type;
};

#define OBMF_ICP_DEV_MATCH_TYPE 0x1

#define OBMF_ICP_DEV_ID_TYPE(t) { \
	     .match_flags = OBMF_ICP_DEV_MATCH_TYPE, \
	     .type = (t), \
	}

struct obmf_icp_channel_driver {
	struct device_driver drv;
	const struct obmf_icp_channel_device_id *id_table;

	int (*probe)(struct obmf_icp_channel_device *dev);
	void (*remove)(struct obmf_icp_channel_device *dev);
};

int obmf_icp_channel_driver_register(struct obmf_icp_channel_driver *drv);
void obmf_icp_channel_driver_unregister(struct obmf_icp_channel_driver *drv);

#define module_obmf_icp_channel_driver(__obmf_icp_channel_driver) \
		module_driver(__obmf_icp_channel_driver, \
			      obmf_icp_channel_driver_register, \
			      obmf_icp_channel_driver_unregister)

static inline void *
ombf_icp_get_drvdata(struct obmf_icp_channel_device *dev)
{
	return dev_get_drvdata(&dev->dev);
}

static inline void
obmf_icp_set_drvdata(struct obmf_icp_channel_device *dev, void *data)
{
	dev_set_drvdata(&dev->dev, data);
}

int obmf_icp_channel_init_requester(struct obmf_icp_channel_device *chan);
int obmf_icp_channel_init_responder(struct obmf_icp_channel_device *chan,
				    obmf_icp_responder_msg_fn msg_fn);

int obmf_icp_requester_txn(struct obmf_icp *icp,
			   struct obmf_icp_requester *req);

struct obmf_icp_requester *
obmf_icp_channel_get_requester(struct obmf_icp_channel_device *chan);

void obmf_icp_channel_put_requester(struct obmf_icp_requester *req);

void *obmf_icp_format_request(struct obmf_icp_channel_device *chan,
			      struct obmf_icp_requester *req,
			      size_t payload_len);

/* returns a payload pointer, of len *lenp */
void *obmf_icp_get_request(struct obmf_icp_responder *rsp,
			    u8 *statusp, size_t *lenp);


#define obmf_icp_format_request_typed(c, r, ty) \
({ \
	void *__p = obmf_icp_format_request((c), (r), sizeof(ty)); \
	(ty *)__p; \
})

#define obmf_icp_format_request_flex(c, r, ty, fam, count) \
({ \
	const size_t __c = (count); \
	const size_t __size = struct_size_t(ty, fam, __c); \
	void *__p = obmf_icp_format_request((c), (r), __size); \
	(ty *)__p; \
})

void *obmf_icp_get_response(struct obmf_icp_requester *req,
			    u8 *statusp, size_t *lenp);

void *obmf_icp_format_response(struct obmf_icp_channel_device *chan,
			      struct obmf_icp_responder *rsp,
			      u8 status, size_t payload_len);

void obmf_icp_format_response_error(struct obmf_icp_channel_device *chan,
				    struct obmf_icp_responder *rsp,
				    u8 status);

int obmf_icp_responder_tx(struct obmf_icp *icp,
			  struct obmf_icp_responder *rsp);

#define obmf_icp_format_response_typed(c, r, st, ty) \
({ \
	void *__p = obmf_icp_format_response((c), (r), (st), sizeof(ty)); \
	(ty *)__p; \
})

#define obmf_icp_format_response_flex(c, r, st, ty, fam, count) \
({ \
	const size_t __c = (count); \
	const size_t __size = struct_size_t(ty, fam, __c); \
	void *__p = obmf_icp_format_response((c), (r), (st), __size); \
	(ty *)__p; \
})

/* configuration facility for channel drivers */
int obmf_icp_channel_config_read(struct obmf_icp_channel_device *chan, u32 addr,
				 u16 size, void *data);
int obmf_icp_channel_config_write(struct obmf_icp_channel_device *chan, u32 addr,
				  u16 size, const void *data);

int obmf_icp_channel_enable(struct obmf_icp_channel_device *chan);
int obmf_icp_channel_disable(struct obmf_icp_channel_device *chan);

/* MMIO */
struct obmf_icp_channel_mmio;
int obmf_icp_mmio_channel_init(struct obmf_icp_channel_device *chan,
			       struct obmf_icp_channel_mmio **mmio);
int obmf_icp_mmio_ch0_init(struct obmf_icp_channel_device **chanp,
			   struct obmf_icp_channel_mmio **mmiop);
int obmf_icp_mmio_short_read(struct obmf_icp_channel_device *mmio,
			     u32 addr, u16 len, void *data);
int obmf_icp_mmio_short_write(struct obmf_icp_channel_device *mmio,
			      u32 addr, u16 len, const void *data);
