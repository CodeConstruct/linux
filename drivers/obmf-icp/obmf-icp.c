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
#include <linux/container_of.h>
#include <linux/device/devres.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/usb.h>

#include "obmf-icp.h"

static struct device *obmf_icp_dev(struct obmf_icp *icp)
{
	return &icp->dev;
}

#define to_obmf_icp_dev(devp) \
	container_of(devp, struct obmf_icp_channel_device, dev)
#define to_obmf_icp_drv(drvp) \
	container_of_const(drvp, struct obmf_icp_channel_driver, drv)

static int obmf_icp_bus_match_one(struct obmf_icp_channel_device *dev,
				  const struct obmf_icp_channel_device_id *id)
{
	if (id->match_flags & OBMF_ICP_DEV_MATCH_TYPE) {
		if (id->type != dev->type)
			return 0;
	}

	return 1;
}

static int obmf_icp_bus_match(struct device *dev,
			      const struct device_driver *drv)
{
	const struct obmf_icp_channel_driver *chan_drv = to_obmf_icp_drv(drv);
	struct obmf_icp_channel_device *chan_dev = to_obmf_icp_dev(dev);
	const struct obmf_icp_channel_device_id *id;

	if (!chan_drv->id_table)
		return 0;

	for (id = chan_drv->id_table; id->match_flags; id++) {
		if (obmf_icp_bus_match_one(chan_dev, id))
			return 1;
	}

	return 0;
}

static int obmf_icp_channel_device_probe(struct device *dev)
{
	struct obmf_icp_channel_driver *chan_drv = to_obmf_icp_drv(dev->driver);
	struct obmf_icp_channel_device *chan_dev = to_obmf_icp_dev(dev);

	if (!chan_drv->probe)
		return 0;

	return chan_drv->probe(chan_dev);
}

static void obmf_icp_channel_device_remove(struct device *dev)
{
	struct obmf_icp_channel_driver *chan_drv = to_obmf_icp_drv(dev->driver);
	struct obmf_icp_channel_device *chan_dev = to_obmf_icp_dev(dev);

	if (chan_drv->remove)
		chan_drv->remove(chan_dev);
}

static const struct bus_type obmf_icp_bus_type;

int obmf_icp_channel_driver_register(struct obmf_icp_channel_driver *drv)
{
	if (!drv)
		return -EINVAL;

	if (!drv->id_table)
		return -EINVAL;

	drv->drv.bus = &obmf_icp_bus_type;

	return driver_register(&drv->drv);
}

void obmf_icp_channel_driver_unregister(struct obmf_icp_channel_driver *drv)
{
	driver_unregister(&drv->drv);
}

static const struct bus_type obmf_icp_bus_type = {
	.name = "obmf-icp",
	.match = obmf_icp_bus_match,
	.probe = obmf_icp_channel_device_probe,
	.remove = obmf_icp_channel_device_remove,
};

static void obmf_icp_channel_device_release(struct device *dev)
{
	struct obmf_icp_channel_device *chan = to_obmf_icp_dev(dev);

	kfree(chan);
}

int obmf_icp_channel_init_requester(struct obmf_icp_channel_device *chan)
{
	struct obmf_icp_transport *ts = chan->icp->transport;
	struct obmf_icp_requester *req;
	int rc;

	req = devm_kmalloc(&chan->dev, sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	init_completion(&req->tx.completion);
	init_completion(&req->rx.completion);
	mutex_init(&req->mutex);

	if (ts->ops->tx_init) {
		rc = ts->ops->tx_init(ts, &req->tx);
		if (rc) {
			dev_err(&chan->dev, "tx init failed: %d\n", rc);
			devm_kfree(&chan->dev, req);
			return rc;
		}
	}

	req->rx.chan = chan;
	chan->req = req;

	return 0;
}

struct obmf_icp_requester *
obmf_icp_channel_get_requester(struct obmf_icp_channel_device *chan)
{
	if (!chan->enabled)
		return NULL;

	if (!chan->req)
		return NULL;

	mutex_lock(&chan->req->mutex);

	return chan->req;
}

void obmf_icp_channel_put_requester(struct obmf_icp_requester *req)
{
	mutex_unlock(&req->mutex);
}

/* len is total message length, including ICP header. returns a payload
 * pointer, of @payload_len bytes
 */
static void *obmf_icp_format_tx(struct obmf_icp_channel_device *chan,
				struct obmf_icp_tx *tx, u8 status_rq,
				size_t payload_len)
{
	struct obmf_icp_msg_hdr *hdr;

	if (payload_len > U16_MAX)
		return ERR_PTR(-EMSGSIZE);

	if (payload_len + sizeof(*hdr) > sizeof(tx->buf))
		return ERR_PTR(-EMSGSIZE);

	memset(tx->buf, 0, payload_len + sizeof(*hdr));
	hdr = (void *)tx->buf;

	hdr->channel = chan->idx;
	hdr->status_rq = status_rq;
	hdr->size = cpu_to_le16(payload_len);
	tx->len = payload_len + sizeof(*hdr);

	return tx->buf + sizeof(*hdr);
}

/* returns a payload pointer, of len *lenp */
static void *obmf_icp_get_rx(struct obmf_icp_rx *rx, u8 *statusp, size_t *lenp)
{
	struct obmf_icp_msg_hdr *hdr = (void *)rx->buf;
	size_t len = rx->len;

	if (len < sizeof(*hdr)) {
		/* we will have checked for a valid message on receive,
		 * so the caller would not have completed the rx.
		 */
		pr_err("invalid length %zd, corrupt requester?\n", len);
		return ERR_PTR(-EIO);
	}

	if (lenp)
		*lenp = len - sizeof(*hdr);

	if (statusp)
		*statusp = FIELD_GET(OBMF_ICP_MSG_HDR_STATUS_FIELD,
				     hdr->status_rq);

	return rx->buf + sizeof(*hdr);
}

void *obmf_icp_format_request(struct obmf_icp_channel_device *chan,
			      struct obmf_icp_requester *req,
			      size_t payload_len)
{
	u8 status_rq = FIELD_PREP(OBMF_ICP_MSG_HDR_RQRESP_FIELD,
				  OBMF_ICP_MSG_HDR_RQRESP_VALUE_REQUEST);

	return obmf_icp_format_tx(chan, &req->tx, status_rq, payload_len);
}

void *obmf_icp_get_response(struct obmf_icp_requester *req,
			    u8 *statusp, size_t *lenp)
{
	return obmf_icp_get_rx(&req->rx, statusp, lenp);
}

static struct obmf_icp_channel_device *
obmf_icp_channel_init(struct obmf_icp *icp, u8 idx)
{
	struct obmf_icp_channel_device *chan;

	chan = kzalloc_obj(*chan, GFP_KERNEL);
	if (!chan)
		return NULL;

	chan->icp = icp;
	chan->idx = idx;
	chan->dev.parent = &icp->dev;
	chan->dev.bus = &obmf_icp_bus_type;
	chan->dev.release = obmf_icp_channel_device_release;
	chan->enabled = false;
	dev_set_name(&chan->dev, "%d:%02x", icp->idx, idx);
	device_initialize(&chan->dev);

	return chan;
}

static void __obmf_icp_handle_rx(struct obmf_icp *icp, size_t len)
{
	struct obmf_icp_channel_device *chan;
	struct device *dev = &icp->dev;
	struct obmf_icp_msg *msg;
	bool rq;

	msg = (struct obmf_icp_msg *)icp->in_buf;
	rq = FIELD_GET(OBMF_ICP_MSG_HDR_RQRESP_FIELD, msg->hdr.status_rq)
		== OBMF_ICP_MSG_HDR_RQRESP_VALUE_REQUEST;

	u8 chan_idx = msg->hdr.channel;
	if (chan_idx > icp->n_channels) {
		dev_err(dev, "RX for invalid channel %d\n", chan_idx);
		return;
	}

	chan = icp->channels[chan_idx];
	if (!chan) {
		dev_err(dev, "invalid rx for missing channel %d\n",
			chan_idx);
		return;
	}

	if (!chan->enabled) {
		dev_err(dev, "invalid rx for disabled channel %d\n",
			chan_idx);
		return;
	}

	if (rq) {
		dev_err(dev,
			"responder not implemented, channel %d\n",
			chan_idx);
		return;
	} else {
		struct obmf_icp_requester *req = chan->req;

		if (!req) {
			dev_err(dev,
				"invalid response for non-requester channel %d\n",
				chan_idx);
			return;
		}
		req->rx.len = len;
		memcpy(req->rx.buf, icp->in_buf, req->rx.len);
		complete(&req->rx.completion);
	}
}

void obmf_icp_transport_rx_complete(struct obmf_icp *icp, int status,
				    size_t len)
{
	struct device *dev = &icp->dev;
	int rc;

	dev_dbg(dev, "%s: status %d len %zd\n", __func__, status, len);

	if (status) {
		/* todo */
		return;
	}

	if (len < sizeof(struct obmf_icp_msg)) {
		dev_err(dev, "short (%zd) receive?\n", len);
		return;
	}

	scoped_guard(spinlock_irq, &icp->lock) {
		__obmf_icp_handle_rx(icp, len);
	}

	rc = icp->transport->ops->rx_submit(icp);
	if (rc) {
		dev_err(&icp->dev, "can't prepare for receive: %d\n", rc);
		// TODO: handling; aborting the current transaction is
		// not sufficient
	}
}

/* TODO: channel-based helper perhaps? check enabled and present */
int obmf_icp_requester_txn(struct obmf_icp *icp,
			   struct obmf_icp_requester *req)
{
	struct obmf_icp_tx *tx = &req->tx;
	struct obmf_icp_rx *rx = &req->rx;
	int rc;

	reinit_completion(&rx->completion);
	reinit_completion(&tx->completion);

	rc = icp->transport->ops->tx_submit(icp, tx);
	if (rc)
		return rc;

	rc = wait_for_completion_interruptible(&tx->completion);
	if (rc) {
		if (icp->transport->ops->tx_cancel)
			icp->transport->ops->tx_cancel(icp, tx);
		return -EPROTO;
	}

	if (tx->status != 0) {
		dev_warn(&icp->dev, "tx status %d\n", tx->status);
		return tx->status;
	}

	rc = wait_for_completion_interruptible(&rx->completion);
	if (rc)
		return -EPROTO;

	return 0;
}

void obmf_icp_transport_tx_complete(struct obmf_icp *icp,
				    struct obmf_icp_tx *tx)
{
	complete(&tx->completion);
}

static int obmf_icp_ch0_init(struct obmf_icp *icp)
{
	struct obmf_icp_channel_device *ch0;
	int rc;

	ch0 = obmf_icp_channel_init(icp, 0);
	if (!ch0) {
		dev_err(obmf_icp_dev(icp), "ch0 init failed\n");
		return -ENOMEM;
	}

	ch0->type = OBMF_ICP_CHANNEL_TYPE_DISCOVERY;
	ch0->cfg_ptr = 0;
	ch0->cfg_size = 0;
	ch0->enabled = true;

	// TODO: part of MMIO channel init?
	rc = obmf_icp_channel_init_requester(ch0);
	if (rc) {
		dev_err(obmf_icp_dev(icp), "ch0 init req failed: %d\n", rc);
		return rc;
	}

	scoped_guard(spinlock_irq, &icp->lock) {
		icp->channels[0] = ch0;
		icp->ch0 = ch0;
	}

	rc = device_add(&ch0->dev);
	if (rc) {
		dev_err(obmf_icp_dev(icp), "ch0 device_add failed: %d\n", rc);
		goto err_put_device;
	}

	return 0;

err_put_device:
	put_device(&ch0->dev);
	return rc;
}

static DEFINE_IDA(obmf_icp_ida);

int obmf_icp_bus_register(struct obmf_icp *icp)
{
	struct device *dev = &icp->dev;
	int rc;

	device_initialize(dev);

	/* keep a reference now, so we can be consistent with ownership on
	 * exit */
	dev = get_device(dev);
	if (!dev)
		return -ENOMEM;

	rc = -EINVAL;
	if (!icp->transport) {
		dev_err(dev, "no transport?\n");
		goto err_put;
	}

	if (!icp->transport->ops->rx_submit) {
		dev_err(dev, "no transport->rx_submit?\n");
		goto err_put;
	}

	icp->transport->icp = icp;

	spin_lock_init(&icp->lock);

	rc = ida_alloc(&obmf_icp_ida, GFP_KERNEL);
	if (rc < 0)
		goto err_put;

	icp->idx = rc;


	dev_set_name(&icp->dev, "obmf-icp%u", icp->idx);

	/* initial channel configuration */
	icp->n_channels = 1;
	icp->channels = devm_kcalloc(dev, icp->n_channels,
				     sizeof(*icp->channels), GFP_KERNEL);
	if (!icp->channels) {
		dev_err(dev, "Failed to allocate initial channel set");
		rc = -ENOMEM;
		goto err_put;
	}

	rc = device_add(dev);
	if (rc)
		goto err_put;

	/* we can release our own reference now that we have added */
	put_device(dev);

	// TODO: we could defer this to a transaction time, or
	// the presence of a responder.
	rc = icp->transport->ops->rx_submit(icp);
	if (rc) {
		dev_err(dev, "can't prepare for receive: %d\n", rc);
		// todo: cancellations: or transport fini?
		goto err_del;
	}

	rc = obmf_icp_ch0_init(icp);
	if (rc) {
		dev_err(dev, "channel 0 init failed\n");
		goto err_cancel;
	}

	/* todo: discover channels */

	return 0;

err_cancel:
	if (icp->transport->ops->rx_cancel)
		icp->transport->ops->rx_cancel(icp->transport);
err_del:
	device_del(dev);
err_put:
	put_device(dev);
	return rc;
}

void obmf_icp_bus_unregister(struct obmf_icp *icp)
{
	unsigned int i;

	if (icp->transport->ops->rx_cancel)
		icp->transport->ops->rx_cancel(icp->transport);

	for (i = 0; i < icp->n_channels; i++) {
		struct obmf_icp_channel_device *chan = NULL;

		scoped_guard(spinlock_irq, &icp->lock) {
			swap(chan, icp->channels[i]);
		}

		if (chan)
			device_unregister(&chan->dev);
	}

	ida_free(&obmf_icp_ida, icp->idx);
	device_unregister(&icp->dev);
}

static int __init obmf_icp_init(void)
{
	int rc;

	rc = bus_register(&obmf_icp_bus_type);
	if (rc)
		pr_err("obmf-icp bus-type registration failure: %d\n", rc);

	return rc;
}

static void obmf_icp_exit(void)
{
	bus_unregister(&obmf_icp_bus_type);
}

module_init(obmf_icp_init);
module_exit(obmf_icp_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jeremy Kerr <jk@codeconstruct.com.au>");
MODULE_AUTHOR("Andrew Jeffery <andrew@codeconstruct.com.au>");
MODULE_DESCRIPTION("OBMF ICP");
