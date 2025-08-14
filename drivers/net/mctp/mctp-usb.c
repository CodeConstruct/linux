// SPDX-License-Identifier: GPL-2.0
/*
 * mctp-usb.c - MCTP-over-USB (DMTF DSP0283) transport binding driver.
 *
 * DSP0283 is available at:
 * https://www.dmtf.org/sites/default/files/standards/documents/DSP0283_1.0.1.pdf
 *
 * Copyright (C) 2024-2025 Code Construct Pty Ltd
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/usb.h>
#include <linux/usb/mctp-usb.h>

#include <net/mctp.h>
#include <net/mctpdevice.h>
#include <net/pkt_sched.h>

#include <uapi/linux/if_arp.h>

/* number of IN/OUT urbs to queue */
static const unsigned int n_rx_queue = 8;
static const unsigned int n_tx_queue = 8;

/* Our max USB transfer is 512 bytes, into which we can pack 7 BTU-sized
 * packets (512 / (68 + 4)). Smaller packets are unlikely to need high
 * throughput; they won't be part of a fragmented message.
 */
#define N_TX_SKB	7
#define N_TX_SG		N_TX_SKB /* 1:1 with skbs, as they're linear */

struct mctp_usb_tx {
	struct mctp_usb *dev;
	struct urb *urb;
	struct sk_buff_head skbs;
	unsigned int len;
	enum mctp_usb_tx_buf_type {
		TX_SINGLE,
		TX_FLAT,
		TX_SG,
	} buf_type;
	uint8_t buf[];
};

struct mctp_usb {
	struct usb_device *usbdev;
	struct usb_interface *intf;
	bool stopped, can_sg;

	struct net_device *netdev;

	u8 ep_in;
	u8 ep_out;

	struct usb_anchor rx_anchor;
	struct usb_anchor tx_anchor;
	/* number of urbs currently queued */
	atomic_t rx_qlen, tx_qlen;

	/* pending tx state.
	 *
	 * In cases where we can pack multiple packets into a USB transfer,
	 * we will have an urb ready to send
	 */
	spinlock_t tx_lock;
	struct mctp_usb_tx *pending_tx;

	struct delayed_work rx_retry_work;
};

static void mctp_usb_tx_free(struct mctp_usb_tx *tx);

static void mctp_usb_dstats_dropped_multi(struct mctp_usb *mctp_usb, int n)
{
	struct pcpu_dstats *dstats = this_cpu_ptr(mctp_usb->netdev->dstats);

	u64_stats_update_begin(&dstats->syncp);
	u64_stats_add(&dstats->tx_drops, n);
	u64_stats_update_end(&dstats->syncp);
}

static void mctp_usb_out_complete(struct urb *urb)
{
	struct mctp_usb_tx *tx = urb->context;
	struct mctp_usb *mctp_usb = tx->dev;
	struct net_device *netdev = mctp_usb->netdev;
	struct pcpu_dstats *dstats = this_cpu_ptr(netdev->dstats);
	int status = urb->status;

	usb_unanchor_urb(urb);
	if (atomic_dec_return(&mctp_usb->tx_qlen) < n_tx_queue)
		netif_wake_queue(netdev);

	switch (status) {
	default:
		netdev_dbg(netdev, "unexpected tx urb status: %d\n", status);
		fallthrough;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -EPROTO:
		mctp_usb_dstats_dropped_multi(mctp_usb, tx->skbs.qlen);
		break;
	case 0:
		u64_stats_update_begin(&dstats->syncp);
		u64_stats_add(&dstats->tx_packets, tx->skbs.qlen);
		u64_stats_add(&dstats->tx_bytes, tx->len);
		u64_stats_update_end(&dstats->syncp);
		break;
	}
	mctp_usb_tx_free(tx);
}

static int mctp_usb_tx_create(struct mctp_usb *mctp_usb,
			      struct mctp_usb_tx **txp,
			      struct sk_buff *skb, bool single)
{
	enum mctp_usb_tx_buf_type type;
	struct mctp_usb_tx *tx;
	size_t sz = 0;

	if (single) {
		type = TX_SINGLE;
	} else if (mctp_usb->can_sg) {
		type = TX_SG;
		sz = sizeof(struct scatterlist) * N_TX_SG;
	} else {
		type = TX_FLAT;
		sz = MCTP_USB_XFER_SIZE;
	}

	tx = kzalloc(sizeof(*tx) + sz, GFP_ATOMIC);
	if (!tx)
		return -ENOMEM;

	tx->dev = mctp_usb;
	tx->buf_type = type;
	tx->urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!tx->urb) {
		kfree(tx);
		return -ENOMEM;
	}

	skb_queue_head_init(&tx->skbs);
	__skb_queue_tail(&tx->skbs, skb);
	tx->len += skb->len;
	*txp = tx;

	return 0;
}

static void mctp_usb_tx_free(struct mctp_usb_tx *tx)
{
	if (!tx)
		return;

	usb_free_urb(tx->urb);
	__skb_queue_purge(&tx->skbs);
	kfree(tx);
}

static int mctp_usb_tx_avail(struct mctp_usb_tx *tx)
{
	return tx->buf_type == TX_SINGLE ? 0 : MCTP_USB_XFER_SIZE - tx->len;
}

static bool mctp_usb_tx_should_send(struct mctp_usb_tx *tx)
{
	return mctp_usb_tx_avail(tx) < (68 + sizeof(struct mctp_usb_hdr)) ||
		(tx->buf_type == TX_SG && tx->skbs.qlen == N_TX_SG);
}

static int mctp_usb_tx_append(struct mctp_usb *mctp_usb, struct mctp_usb_tx *tx,
			      struct sk_buff *skb)
	__must_hold(&mctp_usb->tx_lock)
{
	if (tx->buf_type == TX_SINGLE)
		return -EINVAL;

	if (mctp_usb_tx_avail(tx) < skb->len)
		return -ENOBUFS;

	if (tx->buf_type == TX_SG && tx->skbs.qlen == N_TX_SG)
		return -ENOBUFS;

	__skb_queue_tail(&tx->skbs, skb);

	tx->len += skb->len;

	return 0;
}

static int mctp_usb_tx_send_pending(struct mctp_usb *mctp_usb)
	__must_hold(&mctp_usb->tx_lock)
{
	struct mctp_usb_tx *tx = mctp_usb->pending_tx;
	struct urb *urb = tx->urb;
	struct sk_buff *skb;
	int rc;

	mctp_usb->pending_tx = NULL;

	usb_fill_bulk_urb(urb, mctp_usb->usbdev,
			  usb_sndbulkpipe(mctp_usb->usbdev, mctp_usb->ep_out),
			  NULL, tx->len, mctp_usb_out_complete, tx);

	if (tx->buf_type == TX_SG) {
		struct scatterlist *sg = (struct scatterlist *)tx->buf;
		unsigned int i = 0;

		skb_queue_walk(&tx->skbs, skb) {
			sg_set_buf(&sg[i++], skb->data, skb->len);
		}

		urb->sg = sg;
		urb->num_sgs = i;

		sg_mark_end(&sg[i]);

	} else if (tx->buf_type == TX_FLAT) {
		size_t pos = 0;

		skb_queue_walk(&tx->skbs, skb) {
			skb_copy_bits(skb, 0, tx->buf + pos, skb->len);
			pos += skb->len;
		}

		urb->transfer_buffer = tx->buf;

	} else if (tx->buf_type == TX_SINGLE) {
		skb = tx->skbs.next;
		urb->transfer_buffer = skb->data;
	}

	usb_anchor_urb(urb, &mctp_usb->tx_anchor);
	rc = usb_submit_urb(urb, GFP_ATOMIC);
	if (rc) {
		netdev_dbg(mctp_usb->netdev, "TX urb submit failed, %d\n", rc);
		usb_unanchor_urb(urb);
		return rc;
	}

	if (atomic_inc_return(&mctp_usb->tx_qlen) >= n_tx_queue)
		netif_stop_queue(mctp_usb->netdev);

	return 0;
}


static netdev_tx_t mctp_usb_start_xmit(struct sk_buff *skb,
				       struct net_device *dev)
{
	struct mctp_usb *mctp_usb = netdev_priv(dev);
	bool more = netdev_xmit_more();
	struct mctp_usb_hdr *hdr;
	struct mctp_usb_tx *tx;
	unsigned int plen;
	int rc;

	plen = skb->len;
	if (plen + sizeof(*hdr) > MCTP_USB_XFER_SIZE)
		goto err_drop_single;

	rc = skb_cow_head(skb, sizeof(*hdr));
	if (rc)
		goto err_drop_single;

	hdr = skb_push(skb, sizeof(*hdr));
	if (!hdr)
		goto err_drop_single;

	hdr->id = cpu_to_be16(MCTP_USB_DMTF_ID);
	hdr->rsvd = 0;
	hdr->len = plen + sizeof(*hdr);

	spin_lock(&mctp_usb->tx_lock);

	tx = mctp_usb->pending_tx;
	if (tx) {
		rc = mctp_usb_tx_append(mctp_usb, tx, skb);
		if (rc) {
			/* can't append to the pending tx - send that
			 * now, we'll create a new tx below.
			 */
			rc = mctp_usb_tx_send_pending(mctp_usb);
			if (rc) {
				netdev_dbg(dev, "TX send-pending failed: %d\n",
					   rc);
				mctp_usb_tx_free(tx);
			}
			tx = NULL;
		}
	}

	if (!tx) {
		rc = mctp_usb_tx_create(mctp_usb, &tx, skb, !more);
		if (rc) {
			netdev_dbg(dev, "TX context create failed: %d\n", rc);
			goto err_unlock;
		}
		mctp_usb->pending_tx = tx;
	}

	/* skb is now owned by the tx context */
	skb = NULL;

	if (!more || mctp_usb_tx_should_send(tx)) {
		rc = mctp_usb_tx_send_pending(mctp_usb);
		if (rc) {
			netdev_dbg(dev, "TX send failed: %d", rc);
			goto err_drop_pending;
		}
	}
	tx = NULL;

	spin_unlock(&mctp_usb->tx_lock);
	return NETDEV_TX_OK;

err_drop_pending:
	mctp_usb->pending_tx = NULL;
	mctp_usb_dstats_dropped_multi(mctp_usb, tx->skbs.qlen);
	mctp_usb_tx_free(tx);

err_unlock:
	spin_unlock(&mctp_usb->tx_lock);

err_drop_single:
	if (skb)
		dev_dstats_tx_dropped(dev);
	kfree_skb(skb);
	return NETDEV_TX_OK;
}

static void mctp_usb_in_complete(struct urb *urb);

/* If we fail to queue an in urb atomically (either due to skb allocation or
 * urb submission), we will schedule a rx queue in nonatomic context
 * after a delay, specified in jiffies
 */
static const unsigned long RX_RETRY_DELAY = HZ / 4;

static int mctp_usb_rx_queue(struct mctp_usb *mctp_usb, struct urb *urb,
			     gfp_t gfp)
{
	struct sk_buff *skb;
	int rc;

	/* no point allocating if the queue is going to be rejected */
	if (READ_ONCE(mctp_usb->stopped))
		return 0;

	skb = __netdev_alloc_skb(mctp_usb->netdev, MCTP_USB_XFER_SIZE, gfp);
	if (!skb)
		return -ENOMEM;

	usb_fill_bulk_urb(urb, mctp_usb->usbdev,
			  usb_rcvbulkpipe(mctp_usb->usbdev, mctp_usb->ep_in),
			  skb->data, MCTP_USB_XFER_SIZE,
			  mctp_usb_in_complete, skb);

	rc = usb_submit_urb(urb, gfp);
	if (rc) {
		netdev_dbg(mctp_usb->netdev, "rx urb submit failure: %d\n", rc);
		kfree_skb(skb);
		return rc;
	}

	atomic_inc(&mctp_usb->rx_qlen);

	return 0;
}

static void mctp_usb_in_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct net_device *netdev = skb->dev;
	struct mctp_usb *mctp_usb = netdev_priv(netdev);
	struct mctp_skb_cb *cb;
	unsigned int len;
	int status, rc;

	status = urb->status;
	atomic_dec(&mctp_usb->rx_qlen);

	switch (status) {
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -EPROTO:
		usb_unanchor_urb(urb);
		usb_free_urb(urb);
		kfree_skb(skb);
		return;
	case 0:
		break;
	default:
		netdev_dbg(netdev, "unexpected rx urb status: %d\n", status);
		usb_unanchor_urb(urb);
		usb_free_urb(urb);
		kfree_skb(skb);
		return;
	}

	len = urb->actual_length;
	__skb_put(skb, len);

	while (skb) {
		struct sk_buff *skb2 = NULL;
		struct mctp_usb_hdr *hdr;
		u8 pkt_len; /* length of MCTP packet, no USB header */

		skb_reset_mac_header(skb);
		hdr = skb_pull_data(skb, sizeof(*hdr));
		if (!hdr)
			break;

		if (be16_to_cpu(hdr->id) != MCTP_USB_DMTF_ID) {
			netdev_dbg(netdev, "rx: invalid id %04x\n",
				   be16_to_cpu(hdr->id));
			break;
		}

		if (hdr->len <
		    sizeof(struct mctp_hdr) + sizeof(struct mctp_usb_hdr)) {
			netdev_dbg(netdev, "rx: short packet (hdr) %d\n",
				   hdr->len);
			break;
		}

		/* we know we have at least sizeof(struct mctp_usb_hdr) here */
		pkt_len = hdr->len - sizeof(struct mctp_usb_hdr);
		if (pkt_len > skb->len) {
			netdev_dbg(netdev,
				   "rx: short packet (xfer) %d, actual %d\n",
				   hdr->len, skb->len);
			break;
		}

		if (pkt_len < skb->len) {
			/* more packets may follow - clone to a new
			 * skb to use on the next iteration
			 */
			skb2 = skb_clone(skb, GFP_ATOMIC);
			if (skb2) {
				if (!skb_pull(skb2, pkt_len)) {
					kfree_skb(skb2);
					skb2 = NULL;
				}
			}
			skb_trim(skb, pkt_len);
		}

		dev_dstats_rx_add(netdev, skb->len);

		skb->protocol = htons(ETH_P_MCTP);
		skb_reset_network_header(skb);
		cb = __mctp_cb(skb);
		cb->halen = 0;
		netif_rx(skb);

		skb = skb2;
	}

	if (skb)
		kfree_skb(skb);

	rc = mctp_usb_rx_queue(mctp_usb, urb, GFP_ATOMIC);
	if (rc) {
		usb_free_urb(urb);
		schedule_delayed_work(&mctp_usb->rx_retry_work, RX_RETRY_DELAY);
	}
}

static int mctp_usb_rx_queue_fill(struct mctp_usb *mctp_usb)
{
	int i, qlen, rc = 0;

	qlen = atomic_read(&mctp_usb->rx_qlen);
	if (qlen < 0 || qlen >= n_rx_queue)
		return 0;

	for (i = 0; i < n_rx_queue - qlen; i++) {
		struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);

		if (!urb) {
			rc = -ENOMEM;
			break;
		}

		usb_anchor_urb(urb, &mctp_usb->rx_anchor);

		rc = mctp_usb_rx_queue(mctp_usb, urb, GFP_KERNEL);
		if (rc) {
			usb_unanchor_urb(urb);
			usb_free_urb(urb);
			break;
		}
	}

	return rc;
}

static void mctp_usb_rx_retry_work(struct work_struct *work)
{
	struct mctp_usb *mctp_usb = container_of(work, struct mctp_usb,
						 rx_retry_work.work);
	int rc;

	if (READ_ONCE(mctp_usb->stopped))
		return;

	rc = mctp_usb_rx_queue_fill(mctp_usb);
	if (rc)
		schedule_delayed_work(&mctp_usb->rx_retry_work, RX_RETRY_DELAY);
}

static int mctp_usb_open(struct net_device *dev)
{
	struct mctp_usb *mctp_usb = netdev_priv(dev);

	WRITE_ONCE(mctp_usb->stopped, false);

	netif_start_queue(dev);

	return mctp_usb_rx_queue_fill(mctp_usb);
}

static int mctp_usb_stop(struct net_device *dev)
{
	struct mctp_usb *mctp_usb = netdev_priv(dev);

	netif_stop_queue(dev);

	/* prevent RX submission retry */
	WRITE_ONCE(mctp_usb->stopped, true);

	usb_kill_anchored_urbs(&mctp_usb->rx_anchor);
	usb_kill_anchored_urbs(&mctp_usb->tx_anchor);

	cancel_delayed_work_sync(&mctp_usb->rx_retry_work);

	return 0;
}

static const struct net_device_ops mctp_usb_netdev_ops = {
	.ndo_start_xmit = mctp_usb_start_xmit,
	.ndo_open = mctp_usb_open,
	.ndo_stop = mctp_usb_stop,
};

static void mctp_usb_netdev_setup(struct net_device *dev)
{
	dev->type = ARPHRD_MCTP;

	dev->mtu = MCTP_USB_MTU_MIN;
	dev->min_mtu = MCTP_USB_MTU_MIN;
	dev->max_mtu = MCTP_USB_MTU_MAX;

	dev->hard_header_len = sizeof(struct mctp_usb_hdr);
	dev->tx_queue_len = DEFAULT_TX_QUEUE_LEN;
	dev->flags = IFF_NOARP;
	dev->netdev_ops = &mctp_usb_netdev_ops;
	dev->pcpu_stat_type = NETDEV_PCPU_STAT_DSTATS;
}

static int mctp_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *ep_in, *ep_out;
	struct usb_host_interface *iface_desc;
	struct net_device *netdev;
	struct mctp_usb *dev;
	int rc;

	/* only one alternate */
	iface_desc = intf->cur_altsetting;

	rc = usb_find_common_endpoints(iface_desc, &ep_in, &ep_out, NULL, NULL);
	if (rc) {
		dev_err(&intf->dev, "invalid endpoints on device?\n");
		return rc;
	}

	netdev = alloc_netdev(sizeof(*dev), "mctpusb%d", NET_NAME_ENUM,
			      mctp_usb_netdev_setup);
	if (!netdev)
		return -ENOMEM;

	SET_NETDEV_DEV(netdev, &intf->dev);
	dev = netdev_priv(netdev);
	dev->netdev = netdev;
	dev->usbdev = usb_get_dev(interface_to_usbdev(intf));
	dev->intf = intf;
	dev->can_sg = usb_device_no_sg_constraint(dev->usbdev);
	usb_set_intfdata(intf, dev);
	spin_lock_init(&dev->tx_lock);

	dev->ep_in = ep_in->bEndpointAddress;
	dev->ep_out = ep_out->bEndpointAddress;

	init_usb_anchor(&dev->rx_anchor);
	init_usb_anchor(&dev->tx_anchor);

	INIT_DELAYED_WORK(&dev->rx_retry_work, mctp_usb_rx_retry_work);

	rc = mctp_register_netdev(netdev, NULL, MCTP_PHYS_BINDING_USB);
	if (rc)
		goto err_free_tx_urb;

	return 0;

err_free_tx_urb:
	free_netdev(netdev);
	return rc;
}

static void mctp_usb_disconnect(struct usb_interface *intf)
{
	struct mctp_usb *dev = usb_get_intfdata(intf);

	mctp_unregister_netdev(dev->netdev);
	usb_put_dev(dev->usbdev);
	free_netdev(dev->netdev);
}

static const struct usb_device_id mctp_usb_devices[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_MCTP, 0x0, 0x1) },
	{ 0 },
};

MODULE_DEVICE_TABLE(usb, mctp_usb_devices);

static struct usb_driver mctp_usb_driver = {
	.name		= "mctp-usb",
	.id_table	= mctp_usb_devices,
	.probe		= mctp_usb_probe,
	.disconnect	= mctp_usb_disconnect,
};

module_usb_driver(mctp_usb_driver)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jeremy Kerr <jk@codeconstruct.com.au>");
MODULE_DESCRIPTION("MCTP USB transport");
