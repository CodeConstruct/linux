// SPDX-License-Identifier: GPL-2.0-only
/*
 * Open Boot Management Framework: Interface Consolidation Protocol (OBMF-ICP)
 * USB transport.
 *
 * Copyright (c) 2026 Code Construct
 */
#include <linux/usb.h>

#include "obmf-icp.h"

struct obmf_icp_transport_usb {
	struct obmf_icp_transport ts;
	struct obmf_icp icp;

	struct usb_device *usbdev;
	struct usb_interface *intf;

	u8 ep_out;
	u8 ep_in;

	atomic_t cancelled;
	struct urb *in_urb;
	struct delayed_work rx_err_work;
	bool rx_clear_halt;
	unsigned int rx_err_count;
};

static const unsigned int rx_err_max = 10;
static const unsigned long rx_retry_delay = HZ / 10;

static void obmf_icp_usb_urb_in_complete(struct urb *urb)
{
	struct obmf_icp_transport_usb *ts = urb->context;
	struct device *dev = &ts->intf->dev;
	int status = urb->status;

	switch (status) {
	case 0:
		ts->rx_err_count = 0;
		break;

	case -EPIPE:
		ts->rx_clear_halt = true;
		fallthrough;
	default:
		dev_dbg(dev, "potentially transient IN urb status %d\n",
			status);
		if (++ts->rx_err_count < rx_err_max) {
			schedule_delayed_work(&ts->rx_err_work, rx_retry_delay);
			return;
		}

		break;

	case -ENOENT:
	case -ESHUTDOWN:
	case -ECONNRESET:
		dev_dbg(dev, "fatal IN urb status %d\n", status);
		break;
	}

	obmf_icp_transport_rx_complete(ts->ts.icp, status, urb->actual_length);
}

static int obmf_icp_transport_usb_rx_submit(struct obmf_icp *icp)
{
	struct obmf_icp_transport_usb *ts = container_of(icp->transport,
				     struct obmf_icp_transport_usb, ts);

	// TODO: ordering against async cancel, works for error cleanup path
	if (atomic_read_acquire(&ts->cancelled)) {
		return 0;
	}

	usb_fill_bulk_urb(ts->in_urb, ts->usbdev,
			  usb_rcvbulkpipe(ts->usbdev, ts->ep_in),
			  icp->in_buf, sizeof(icp->in_buf),
			  obmf_icp_usb_urb_in_complete, ts);

	return usb_submit_urb(ts->in_urb, GFP_ATOMIC);
}

static int obmf_icp_transport_usb_rx_cancel(struct obmf_icp_transport *ts)
{
	struct obmf_icp_transport_usb *usb_ts = container_of(ts,
				     struct obmf_icp_transport_usb, ts);

	atomic_set_release(&usb_ts->cancelled, 1);
	usb_kill_urb(usb_ts->in_urb);

	return 0;
}

static void obmf_icp_usb_rx_err_work(struct work_struct *work)
{
	struct obmf_icp_transport_usb *ts = container_of(work,
		 struct obmf_icp_transport_usb, rx_err_work.work);
	struct device *dev = &ts->intf->dev;
	int rc;

	if (atomic_read_acquire(&ts->cancelled))
		return;

	if (ts->rx_clear_halt) {
		int pipe = usb_rcvbulkpipe(ts->usbdev, ts->ep_in);

		rc = usb_clear_halt(ts->usbdev, pipe);
		if (rc) {
			dev_err(dev, "can't clear IN EP halt: %d\n", rc);

			if (++ts->rx_err_count >= rx_err_max)
				return;

			/* re-schedule */
			schedule_delayed_work(&ts->rx_err_work, rx_retry_delay);
			return;
		}
		ts->rx_clear_halt = false;
	}

	usb_fill_bulk_urb(ts->in_urb, ts->usbdev,
			  usb_rcvbulkpipe(ts->usbdev, ts->ep_in),
			  ts->icp.in_buf, sizeof(ts->icp.in_buf),
			  obmf_icp_usb_urb_in_complete, ts);

	rc = usb_submit_urb(ts->in_urb, GFP_KERNEL);
	if (rc) {
		if (++ts->rx_err_count >= rx_err_max)
			obmf_icp_transport_rx_complete(ts->ts.icp, rc, 0);
		else
			schedule_delayed_work(&ts->rx_err_work, rx_retry_delay);
	}
}

static void obmf_icp_usb_urb_out_complete(struct urb *urb)
{
	struct obmf_icp_tx *tx = urb->context;

	tx->status = urb->status;

	obmf_icp_transport_tx_complete(tx->icp, tx);
}

static int obmf_icp_transport_usb_tx_submit(struct obmf_icp *icp,
					    struct obmf_icp_tx *tx)
{
	struct obmf_icp_transport_usb *ts = container_of(icp->transport,
				     struct obmf_icp_transport_usb, ts);
	struct urb *urb = tx->transport_data;
	int rc;

	usb_fill_bulk_urb(urb, ts->usbdev,
			  usb_sndbulkpipe(ts->usbdev, ts->ep_out),
			  tx->buf, tx->len, obmf_icp_usb_urb_out_complete, tx);

	rc = usb_submit_urb(urb, GFP_KERNEL);
	if (rc)
		return rc;

	return 0;
}

static void obmf_icp_transport_usb_tx_cancel(struct obmf_icp *icp,
					     struct obmf_icp_tx *tx)
{
	struct urb *urb = tx->transport_data;

	usb_kill_urb(urb);
}

static int obmf_icp_transport_usb_tx_init(struct obmf_icp_transport *ts,
					  struct obmf_icp_tx *tx)
{
	struct urb *urb;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	tx->icp = ts->icp;
	tx->transport_data = urb;
	return 0;
}

static void obmf_icp_transport_usb_tx_fini(struct obmf_icp_transport *ts,
					  struct obmf_icp_tx *tx)
{
	struct urb *urb = tx->transport_data;

	usb_free_urb(urb);
}

static const struct obmf_icp_transport_ops obmf_icp_usb_transport_ops = {
	.tx_init = obmf_icp_transport_usb_tx_init,
	.tx_fini = obmf_icp_transport_usb_tx_fini,
	.tx_submit = obmf_icp_transport_usb_tx_submit,
	.tx_cancel = obmf_icp_transport_usb_tx_cancel,
	.rx_submit = obmf_icp_transport_usb_rx_submit,
	.rx_cancel = obmf_icp_transport_usb_rx_cancel,
};

static void obmf_icp_usb_release(struct device *dev)
{
	struct obmf_icp_transport_usb *ts = container_of(dev,
		 struct obmf_icp_transport_usb, icp.dev);

	usb_free_urb(ts->in_urb);
	kfree(ts);
}

static int obmf_icp_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *ep_in, *ep_out;
	struct usb_host_interface *iface_desc;
	struct obmf_icp_transport_usb *ts;
	struct device *dev = &intf->dev;
	struct obmf_icp *icp;
	int rc;

	iface_desc = intf->cur_altsetting;

	rc = usb_find_common_endpoints(iface_desc, &ep_in, &ep_out, NULL, NULL);
	if (rc) {
		dev_err(&intf->dev, "Need bulk-in and bulk-out endpoints\n");
		return -ENODEV;
	}

	ts = kzalloc_obj(*ts, GFP_KERNEL);
	if (!ts) {
		dev_err(dev, "Failed to allocate driver memory\n");
		return -ENOMEM;
	}

	icp = &ts->icp;

	ts->ts.ops = &obmf_icp_usb_transport_ops;
	ts->usbdev = interface_to_usbdev(intf);
	ts->intf = intf;
	usb_set_intfdata(intf, ts);
	INIT_DELAYED_WORK(&ts->rx_err_work, obmf_icp_usb_rx_err_work);

	ts->ep_out = ep_out->bEndpointAddress;
	ts->ep_in = ep_in->bEndpointAddress;
	ts->in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!ts->in_urb) {
		dev_err(dev, "Failed to allocate IN urb\n");
		goto err_free;
	}

	icp->transport = &ts->ts;
	icp->dev.parent = dev;
	icp->dev.release = obmf_icp_usb_release;

	rc = obmf_icp_bus_register(icp);
	if (rc)
		goto err_free;

	return 0;

err_free:
	usb_free_urb(ts->in_urb);
	kfree(ts);
	return rc;
}

static void obmf_icp_usb_disconnect(struct usb_interface *intf)
{
	struct obmf_icp_transport_usb *ts = usb_get_intfdata(intf);
	struct obmf_icp *icp = ts->ts.icp;

	usb_kill_urb(ts->in_urb);

	flush_delayed_work(&ts->rx_err_work);

	obmf_icp_bus_unregister(icp);
}

static const struct usb_device_id obmf_icp_usb_ids[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_MISC, 0x09, 0x01)},
	{},
};

static struct usb_driver obmf_icp_transport_usb = {
	.name = "obmf-icp",
	.probe = obmf_icp_usb_probe,
	.disconnect = obmf_icp_usb_disconnect,
	.id_table = obmf_icp_usb_ids,
};

module_usb_driver(obmf_icp_transport_usb);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jeremy Kerr <jk@codeconstruct.com.au>");
MODULE_DESCRIPTION("OBMF ICP USB transport");
MODULE_DEVICE_TABLE(usb, obmf_icp_usb_ids);
