// SPDX-License-Identifier: GPL-2.0-only
/*
 * Open Boot Management Framework: Interface Consolidation Protocol (OBMF-ICP)
 * support.
 *
 * MMIO channel implementation. This only provides support for the
 * configuration channel at present.
 *
 * Copyright (c) 2026 Code Construct
 */

#include <asm/byteorder.h>
#include <linux/types.h>

#include "obmf-icp.h"

struct obmf_icp_mmio_hdr {
	u8 txn;
	u8 rsvd;
} __packed;

enum obmf_icp_mmio_txn {
	OBMF_ICP_MMIO_TXN_SHORT_READ = 0,
	OBMF_ICP_MMIO_TXN_SHORT_WRITE = 1,
};

struct obmf_icp_mmio_short_read_req {
	struct obmf_icp_mmio_hdr mmio_hdr;
	__le32 addr;
	__le16 len;
} __packed;

struct obmf_icp_mmio_short_read_resp {
	struct obmf_icp_mmio_hdr mmio_hdr;
	u8 data[];
} __packed;

struct obmf_icp_mmio_short_write_req {
	struct obmf_icp_mmio_hdr mmio_hdr;
	__le32 addr;
	u8 data[];
} __packed;

struct obmf_icp_mmio_short_write_resp {
	struct obmf_icp_mmio_hdr mmio_hdr;
} __packed;

/* todo: long read support */

#define obmf_icp_mmio_check_resp_typed(c, t, s, l, ty) \
	obmf_icp_mmio_check_resp((c), (t), (s), l, sizeof(ty));

#define obmf_icp_mmio_check_resp_flex(c, t, s, l, ty, n) \
	obmf_icp_mmio_check_resp((c), (t), (s), (l), \
				 struct_size_t(ty, data, n))

static int obmf_icp_mmio_check_resp(struct obmf_icp_channel_device *chan,
				    const char *type, u8 status,
				    size_t len, size_t exp_len)
{
	if (len != exp_len) {
		dev_warn(&chan->dev,
			 "invalid MMIO %s response length %zd (exp %zd)\n",
			 type, len, exp_len);
		return -EPROTO;
	}

	if (status != 0) {
		dev_dbg(&chan->dev, "MMIO %s request failed: 0x%x\n",
			type, status);
		return -EPROTO;
	}

	return 0;
}


int obmf_icp_mmio_short_read(struct obmf_icp_channel_device *chan,
			     u32 addr, u16 len, void *data)
{
	struct obmf_icp_mmio_short_read_resp *resp_msg;
	struct obmf_icp_mmio_short_read_req *req_msg;
	struct obmf_icp_requester *req;
	size_t resp_len;
	uint8_t status;
	int rc;

	req = obmf_icp_channel_get_requester(chan);
	if (!req)
		return -EINVAL;

	req_msg = obmf_icp_format_request_typed(chan, req,
			struct obmf_icp_mmio_short_read_req);
	if (IS_ERR(req_msg)) {
		rc = PTR_ERR(req_msg);
		goto out_release;
	}

	req_msg->mmio_hdr.txn = OBMF_ICP_MMIO_TXN_SHORT_READ;
	req_msg->addr = cpu_to_le32(addr);
	req_msg->len = cpu_to_le16(len);

	rc = obmf_icp_requester_txn(chan->icp, req);
	if (rc) {
		dev_warn(&chan->dev, "transaction failed: %d\n", rc);
		goto out_release;
	}

	resp_msg = obmf_icp_get_response(req, &status, &resp_len);
	if (IS_ERR(resp_msg)) {
		rc = PTR_ERR(resp_msg);
		goto out_release;
	}

	rc = obmf_icp_mmio_check_resp_flex(chan, "short read", status,
		resp_len, struct obmf_icp_mmio_short_read_resp, len);
	if (rc)
		goto out_release;

	memcpy(data, resp_msg->data, len);
	rc = 0;

out_release:
	obmf_icp_channel_put_requester(req);
	return rc;
}

int obmf_icp_mmio_short_write(struct obmf_icp_channel_device *chan,
			      u32 addr, u16 len, const void *data)
{
	struct obmf_icp_mmio_short_write_resp *resp_msg;
	struct obmf_icp_mmio_short_write_req *req_msg;
	struct obmf_icp_requester *req;
	size_t resp_len;
	uint8_t status;
	int rc;

	req = obmf_icp_channel_get_requester(chan);
	if (!req)
		return -EINVAL;

	req_msg = obmf_icp_format_request_flex(chan, req,
			struct obmf_icp_mmio_short_write_req, data, len);
	if (IS_ERR(req_msg)) {
		rc = PTR_ERR(req_msg);
		goto out_release;
	}

	req_msg->mmio_hdr.txn = OBMF_ICP_MMIO_TXN_SHORT_WRITE;
	req_msg->addr = cpu_to_le32(addr);
	memcpy(req_msg->data, data, len);

	rc = obmf_icp_requester_txn(chan->icp, req);
	if (rc)
		goto out_release;

	resp_msg = obmf_icp_get_response(req, &status, &resp_len);
	if (IS_ERR(resp_msg)) {
		rc = PTR_ERR(resp_msg);
		goto out_release;
	}

	rc = obmf_icp_mmio_check_resp_typed(chan, "short read", status,
					    resp_len, *resp_msg);
	if (rc)
		goto out_release;

	rc = 0;

out_release:
	obmf_icp_channel_put_requester(req);
	return rc;
}

/* We don't bind through the general device infrastructure at present,
 * as the configuration channel is handled explicitly through the obmf-icp
 * core.
 *
 * Eventually, actual-MMIO device implementations will need a type-specific
 * bind, then bridge to an appropriate channel implementation (say, through
 * a regmap).
 */
static int obmf_icp_mmio_channel_probe(struct obmf_icp_channel_device *dev)
{
	return -ENODEV;
}

struct obmf_icp_channel_device_id obmf_icp_mmio_ids[] = {
	OBMF_ICP_DEV_ID_TYPE(1),
	{ 0 },
};

struct obmf_icp_channel_driver obmf_icp_mmio_channel_driver = {
	.id_table = obmf_icp_mmio_ids,
	.drv = {
		.name = "obmf-icp-mmio",
	},
	.probe = obmf_icp_mmio_channel_probe,
};

module_obmf_icp_channel_driver(obmf_icp_mmio_channel_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jeremy Kerr <jk@codeconstruct.com.au>");
MODULE_DESCRIPTION("OBMF ICP MMIO channel driver");
