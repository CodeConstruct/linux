// SPDX-License-Identifier: GPL-2.0-only
/*
 * Open Boot Management Framework: Interface Consolidation Protocol (OBMF-ICP)
 * support.
 *
 * GPIO channel driver.
 *
 * Copyright (c) 2026 Code Construct
 */

#include <asm/byteorder.h>
#include <linux/gpio.h>
#include <linux/gpio/driver.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "obmf-icp.h"

enum obmf_icp_gpio_irq_config {
	OBMF_ICP_GPIO_IRQ_CFG_DISABLE = 0,
	OBMF_ICP_GPIO_IRQ_CFG_LEVEL_LOW = 1,
	OBMF_ICP_GPIO_IRQ_CFG_LEVEL_HIGH = 2,
	OBMF_ICP_GPIO_IRQ_CFG_EDGE_RISING = 3,
	OBMF_ICP_GPIO_IRQ_CFG_EDGE_FALLING = 4,
	OBMF_ICP_GPIO_IRQ_CFG_EDGE_BOTH = 5,
};

struct gpio_config {
	u8 direction;
	enum obmf_icp_gpio_irq_config irq;
	bool irq_enabled;
	bool irq_config_pending;
};

/* a single GPIO line */
struct obmf_icp_gpio {
	u16 idx;
	u8 name[32];
	struct gpio_config config;
};

struct obmf_icp_channel_gpio {
	struct obmf_icp_channel_device *chan;
	struct gpio_chip chip;

	struct mutex irq_lock;
	struct work_struct irq_work;

	size_t n_gpios;
	struct obmf_icp_gpio gpios[] __counted_by(n_gpios);
};

struct obmf_icp_gpio_config_entry {
	__le16 idx;
	u8 name[32];
	u8 direction;
	u8 default_output;
	u8 drive_config;
	u8 persist_state;
	u8 pull_config;
	__le16 pull_strength;
	__le16 drive_strength;
	__le16 slew_rate;
	__le16 debounce_us;
	u8 rsvd[3];
} __packed;

struct obmf_icp_gpio_config {
	__le16 gpio_count;
};

enum obmf_icp_gpio_msg_command {
	OBMF_ICP_GPIO_CMD_GET_VALUES = 0,
	OBMF_ICP_GPIO_CMD_SET_VALUES = 1,
	OBMF_ICP_GPIO_CMD_GET_IRQ_CONFIG = 2,
	OBMF_ICP_GPIO_CMD_SET_IRQ_CONFIG = 3,
	OBMF_ICP_GPIO_CMD_IRQ_NOTIFY = 4,
	OBMF_ICP_GPIO_CMD_IRQ_NOTIFY_DGRAM = 5,
};

struct obmf_icp_gpio_msg {
	u8 command;
	__le16 entries[];
} __packed;

static u16 obmf_icp_gpio_msg_get_idx(__le16 entry)
{
	return le16_to_cpu(entry) & GENMASK(11, 0);
}

static u8 obmf_icp_gpio_msg_get_data(__le16 entry)
{
	return (le16_to_cpu(entry) & GENMASK(15, 12)) >> 12;
}

static void obmf_icp_gpio_set_entry(__le16 *entry, u16 idx, u8 data)
{
	*entry = cpu_to_le16(idx | data << 12);
}

static void obmf_icp_gpio_config_parse(
	const struct obmf_icp_gpio_config_entry *entry,
	struct obmf_icp_gpio *gpio
)
{
	gpio->idx = le32_to_cpu(entry->idx);
	gpio->config.direction = entry->direction ? GPIO_LINE_DIRECTION_IN :
		GPIO_LINE_DIRECTION_OUT;
	strscpy(gpio->name, entry->name, sizeof(entry->name) - 1);
}

static int obmf_icp_gpio_check_resp(struct obmf_icp_channel_gpio *gpio,
				    const char *type, u8 status,
				    size_t len, size_t exp_len)
{
	if (len != exp_len) {
		dev_warn(&gpio->chan->dev,
			 "invalid GPIO %s response length %zd (exp %zd)\n",
			 type, len, exp_len);
		return -EPROTO;
	}

	if (status != 0) {
		dev_dbg(&gpio->chan->dev, "GPIO %s request failed: 0x%x\n",
			type, status);
		return -EPROTO;
	}

	return 0;
}

#define obmf_icp_gpio_check_resp_nent(g, t, s, l, n) \
	obmf_icp_gpio_check_resp((g), (t), (s), (l), \
		 struct_size_t(struct obmf_icp_gpio_msg, entries, n))

static int obmf_icp_gpio_set(struct gpio_chip *gc, unsigned int offset, int val)
{
	struct obmf_icp_channel_gpio *gpio = gpiochip_get_data(gc);
	struct obmf_icp_channel_device *chan = gpio->chan;
	struct obmf_icp_gpio_msg *resp_msg;
	struct obmf_icp_gpio_msg *req_msg;
	struct obmf_icp_requester *req;
	size_t resp_len;
	uint8_t status;
	int rc;

	req = obmf_icp_channel_get_requester(chan);
	if (!req)
		return -EINVAL;

	req_msg = obmf_icp_format_request_flex(chan, req,
					       struct obmf_icp_gpio_msg,
					       entries, 1);
	if (IS_ERR(req_msg)) {
		rc = PTR_ERR(req_msg);
		goto out_release;
	}
	req_msg->command = OBMF_ICP_GPIO_CMD_SET_VALUES;
	obmf_icp_gpio_set_entry(&req_msg->entries[0], offset, !!val);

	rc = obmf_icp_requester_txn(chan->icp, req);
	if (rc)
		goto out_release;

	resp_msg = obmf_icp_get_response(req, &status, &resp_len);
	if (IS_ERR(resp_msg)) {
		rc = PTR_ERR(resp_msg);
		goto out_release;
	}

	rc = obmf_icp_gpio_check_resp_nent(gpio, "set values", status,
					   resp_len, 0);
	if (rc)
		goto out_release;

	rc = 0;

out_release:
	obmf_icp_channel_put_requester(req);
	return rc;
}

static int obmf_icp_gpio_get_direction(struct gpio_chip *gc,
				       unsigned int offset)
{
	struct obmf_icp_channel_gpio *gpio = gpiochip_get_data(gc);

	guard(mutex)(&gpio->irq_lock);

	return gpio->gpios[offset].config.direction;
}

static int obmf_icp_gpio_set_direction(struct gpio_chip *gc,
				       unsigned int offset,
				       int direction, int out_val)
{
	struct obmf_icp_channel_gpio *gpio = gpiochip_get_data(gc);
	struct obmf_icp_gpio_config_entry entry;
	const u32 cfg_addr = OBMF_ICP_CH0_CONFIG_CHANNEL_SPECIFIC_OFFSET
		+ sizeof(struct obmf_icp_gpio_config)
		+ offset * sizeof(entry)
		+ offsetof(typeof(entry), direction);
	u16 cfg_size = sizeof(entry.direction);
	int rc;

	if (direction == GPIO_LINE_DIRECTION_OUT) {
		cfg_size += sizeof(entry.default_output);
		entry.direction = 0x0;
		entry.default_output = out_val;
	} else {
		entry.direction = 0x01;
	}

	guard(mutex)(&gpio->irq_lock);

	if (gpio->gpios[offset].config.direction == direction) {
		if (direction == GPIO_LINE_DIRECTION_OUT) {
			return obmf_icp_gpio_set(gc, offset, out_val);
		}
		return 0;
	}

	rc = obmf_icp_channel_config_write(gpio->chan, cfg_addr,
					   cfg_size, &entry.direction);

	if (rc) {
		dev_err(&gpio->chan->dev,
			"%s: channel config write (@%x, %d) failed: %d\n",
			__func__, cfg_addr, cfg_size, rc);
	} else {
		gpio->gpios[offset].config.direction = direction;
	}

	return rc;
}

static int obmf_icp_gpio_direction_input(struct gpio_chip *gc,
					 unsigned int offset)
{
	return obmf_icp_gpio_set_direction(gc, offset,
					   GPIO_LINE_DIRECTION_IN, 0);
}

static int obmf_icp_gpio_direction_output(struct gpio_chip *gc,
					  unsigned int offset,
					  int val)
{
	return obmf_icp_gpio_set_direction(gc, offset,
					   GPIO_LINE_DIRECTION_OUT, val);
}

static int obmf_icp_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct obmf_icp_channel_gpio *gpio = gpiochip_get_data(gc);
	struct obmf_icp_channel_device *chan = gpio->chan;
	struct obmf_icp_gpio_msg *resp_msg;
	struct obmf_icp_gpio_msg *req_msg;
	struct obmf_icp_requester *req;
	size_t resp_len;
	u16 resp_offset;
	uint8_t status;
	int rc;

	req = obmf_icp_channel_get_requester(chan);
	if (!req)
		return -EINVAL;

	req_msg = obmf_icp_format_request_flex(chan, req,
					       struct obmf_icp_gpio_msg,
					       entries, 1);
	if (IS_ERR(req_msg)) {
		rc = PTR_ERR(req_msg);
		goto out_release;
	}

	req_msg->command = OBMF_ICP_GPIO_CMD_GET_VALUES;
	obmf_icp_gpio_set_entry(&req_msg->entries[0], offset, 0);

	rc = obmf_icp_requester_txn(chan->icp, req);
	if (rc)
		goto out_release;

	resp_msg = obmf_icp_get_response(req, &status, &resp_len);
	if (IS_ERR(resp_msg)) {
		rc = PTR_ERR(resp_msg);
		goto out_release;
	}

	rc = obmf_icp_gpio_check_resp_nent(gpio, "get values", status,
					   resp_len, 1);
	if (rc)
		goto out_release;

	resp_offset = obmf_icp_gpio_msg_get_idx(resp_msg->entries[0]);
	if (resp_offset != offset) {
		dev_err(&chan->dev, "requested GPIO offset %d, got %d\n",
			offset, resp_offset);
		rc = -EPROTO;
		goto out_release;
	}

	rc = !!obmf_icp_gpio_msg_get_data(resp_msg->entries[0]);

out_release:
	obmf_icp_channel_put_requester(req);
	return rc;
}

/* irqchip handling */
static enum obmf_icp_gpio_irq_config
obmf_icp_gpio_irq_flow_type_to_config(unsigned int flow_type)
{
	switch (flow_type) {
	case IRQ_TYPE_NONE:
		return OBMF_ICP_GPIO_IRQ_CFG_DISABLE;
	case IRQ_TYPE_EDGE_RISING:
		return OBMF_ICP_GPIO_IRQ_CFG_EDGE_RISING;
	case IRQ_TYPE_EDGE_FALLING:
		return OBMF_ICP_GPIO_IRQ_CFG_EDGE_FALLING;
	case IRQ_TYPE_EDGE_BOTH:
		return OBMF_ICP_GPIO_IRQ_CFG_EDGE_BOTH;
	case IRQ_TYPE_LEVEL_HIGH:
		return OBMF_ICP_GPIO_IRQ_CFG_LEVEL_HIGH;
	case IRQ_TYPE_LEVEL_LOW:
		return OBMF_ICP_GPIO_IRQ_CFG_LEVEL_LOW;
	}

	return -1;
}

static struct obmf_icp_channel_gpio *
irq_data_get_gpio_chan(struct irq_data *data)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(data);

	return gpiochip_get_data(gc);
}

static void obmf_icp_gpio_irq_set_enabled(struct irq_data *data, bool enable)
{
	struct obmf_icp_channel_gpio *gpio = irq_data_get_gpio_chan(data);
	struct obmf_icp_gpio *line = &gpio->gpios[data->hwirq];
	irq_hw_number_t irq = data->hwirq;

	line->config.irq_enabled = enable;
	line->config.irq_config_pending = true;
	gpiochip_enable_irq(&gpio->chip, irq);
}

static void obmf_icp_gpio_irq_enable(struct irq_data *data)
{
	obmf_icp_gpio_irq_set_enabled(data, true);
}

static void obmf_icp_gpio_irq_disable(struct irq_data *data)
{
	obmf_icp_gpio_irq_set_enabled(data, false);
}

static int obmf_icp_gpio_irq_set_type(struct irq_data *data,
				      unsigned int flow_type)
{
	struct obmf_icp_channel_gpio *gpio = irq_data_get_gpio_chan(data);
	struct obmf_icp_gpio *line = &gpio->gpios[data->hwirq];
	int irq_config;

	if (line->config.irq_enabled) {
		dev_err(&gpio->chan->dev, "reconfigure (%ld) while active!\n",
			data->hwirq);
		return -EBUSY;
	}

	irq_config = obmf_icp_gpio_irq_flow_type_to_config(flow_type);
	if (irq_config < 0) {
		dev_err(&gpio->chan->dev,
			"invalid flow type 0x%x for gpio %ld\n",
			flow_type, data->hwirq);
		return -EINVAL;
	}

	line = &gpio->gpios[data->hwirq];
	line->config.irq = irq_config;
	line->config.irq_config_pending = true;

	return 0;
}

static void obmf_icp_gpio_irq_bus_lock(struct irq_data *data)
{
	struct obmf_icp_channel_gpio *gpio = irq_data_get_gpio_chan(data);

	mutex_lock(&gpio->irq_lock);
}

static int obmf_icp_gpio_sync_irq_config(struct obmf_icp_channel_gpio *gpio,
					 unsigned int offset)
{
	struct obmf_icp_channel_device *chan = gpio->chan;
	struct obmf_icp_gpio *line = &gpio->gpios[offset];
	struct obmf_icp_gpio_msg *resp_msg;
	struct obmf_icp_gpio_msg *req_msg;
	struct obmf_icp_requester *req;
	size_t resp_len;
	uint8_t status;
	u8 irq;
	int rc;


	if (line->config.irq_enabled)
		irq = line->config.irq;
	else
		irq = OBMF_ICP_GPIO_IRQ_CFG_DISABLE;

	req = obmf_icp_channel_get_requester(chan);
	if (!req)
		return -EINVAL;

	req_msg = obmf_icp_format_request_flex(chan, req,
					       struct obmf_icp_gpio_msg,
					       entries, 1);
	if (IS_ERR(req_msg)) {
		rc = PTR_ERR(req_msg);
		goto out_release;
	}
	req_msg->command = OBMF_ICP_GPIO_CMD_SET_IRQ_CONFIG;
	obmf_icp_gpio_set_entry(&req_msg->entries[0], offset, irq);

	rc = obmf_icp_requester_txn(chan->icp, req);
	if (rc)
		goto out_release;

	resp_msg = obmf_icp_get_response(req, &status, &resp_len);
	if (IS_ERR(resp_msg)) {
		rc = PTR_ERR(resp_msg);
		goto out_release;
	}

	rc = obmf_icp_gpio_check_resp_nent(gpio, "set irq config", status,
					   resp_len, 0);
	if (rc)
		goto out_release;

out_release:
	obmf_icp_channel_put_requester(req);
	return rc;
}

static void obmf_icp_gpio_irq_bus_sync_unlock(struct irq_data *data)
{
	struct obmf_icp_channel_gpio *gpio = irq_data_get_gpio_chan(data);
	struct obmf_icp_gpio *line = &gpio->gpios[data->hwirq];

	obmf_icp_gpio_sync_irq_config(gpio, data->hwirq);
	line->config.irq_config_pending = false;

	mutex_unlock(&gpio->irq_lock);
}

static const struct irq_chip obmf_icp_gpio_irq_chip = {
	.name = "obmf-icp-gpio-irq",
	.irq_enable = obmf_icp_gpio_irq_enable,
	.irq_disable = obmf_icp_gpio_irq_disable,
	.irq_set_type = obmf_icp_gpio_irq_set_type,
	.irq_bus_lock = obmf_icp_gpio_irq_bus_lock,
	.irq_bus_sync_unlock = obmf_icp_gpio_irq_bus_sync_unlock,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void obmf_icp_gpio_channel_action(void *data)
{
	struct obmf_icp_channel_gpio *gpio = data;

	kfree(gpio);
}

static u8 obmf_icp_gpio_responder_msg(struct obmf_icp_channel_device *chan,
				       struct obmf_icp_responder *rsp)
{
	struct obmf_icp_channel_gpio *gpio = chan->chan_priv;
	struct obmf_icp_gpio_msg *resp_msg;
	struct obmf_icp_gpio_msg *req_msg;
	unsigned int i, n_ent;
	size_t req_len;
	u8 cmd, status;

	req_msg = obmf_icp_get_request(rsp, &status, &req_len);
	if (IS_ERR(req_msg)) {
		dev_warn(&chan->dev, "unable to retrieve request: %ld\n",
			 PTR_ERR(req_msg));
		return OBMF_ICP_MSG_STS_CHAN_PERM_ERR;
	}

	if (req_len < sizeof(*req_msg)) {
		dev_warn(&chan->dev,
			 "invalid (min) request length: %zd\n", req_len);
		return OBMF_ICP_MSG_STS_REQ_SIZE_INVALID;
	}

	req_len -= sizeof(*req_msg);
	if (req_len % sizeof(req_msg->entries[0])) {
		dev_warn(&chan->dev,
			 "invalid (mod) request length: %zd, entry size %zd\n",
			 req_len, sizeof(req_msg->entries[0]));
		return OBMF_ICP_MSG_STS_REQ_SIZE_INVALID;
	}

	n_ent = req_len / sizeof(req_msg->entries[0]);

	cmd = req_msg->command;
	if (cmd != OBMF_ICP_GPIO_CMD_IRQ_NOTIFY) {
		dev_warn(&chan->dev, "unsupported command 0x%02x\n", cmd);
		return OBMF_ICP_MSG_STS_CMD_PARAM_INVALID;
	}

	scoped_guard(mutex, &gpio->irq_lock) {
		for (i = 0; i < n_ent; i++) {
			struct obmf_icp_gpio *line;
			u16 idx;
			int irq;

			idx = obmf_icp_gpio_msg_get_idx(req_msg->entries[i]);
			line = &gpio->gpios[idx];

			if (!line->config.irq_enabled)
				continue;

			irq = irq_find_mapping(gpio->chip.irq.domain, idx);
			if (!irq)
				continue;

			generic_handle_irq_safe(irq);
		}
	}

	resp_msg = obmf_icp_format_response_flex(chan, rsp, 0,
						 typeof(*resp_msg),
						 entries, 0);
	if (IS_ERR(resp_msg)) {
		dev_warn(&chan->dev, "can't format response %ld\n",
			 PTR_ERR(resp_msg));
		return OBMF_ICP_MSG_STS_CHAN_PERM_ERR;
	}

	resp_msg->command = cmd;
	return 0;
}

static int obmf_icp_gpio_channel_probe(struct obmf_icp_channel_device *chan)
{
	const u32 offset = OBMF_ICP_CH0_CONFIG_CHANNEL_SPECIFIC_OFFSET;
	struct obmf_icp_channel_gpio *gpio;
	struct obmf_icp_gpio_config config;
	struct device *dev = &chan->dev;
	const char **names = NULL;
	unsigned int i, n_gpio;
	struct gpio_chip *chip;
	int rc;

	rc = obmf_icp_channel_config_read(chan, offset, sizeof(config),
					  &config);
	if (rc) {
		dev_err(dev, "%s: channel config read (@%x, %zd) failed: %d\n",
			__func__, offset, sizeof(config), rc);
		return rc;
	}

	n_gpio = le16_to_cpu(config.gpio_count);

	gpio = kzalloc_flex(*gpio, gpios, n_gpio);
	if (!gpio)
		return -ENOMEM;

	rc = devm_add_action_or_reset(dev, obmf_icp_gpio_channel_action, gpio);
	if (rc)
		return rc;

	gpio->n_gpios = n_gpio;
	mutex_init(&gpio->irq_lock);

	names = devm_kcalloc(dev, n_gpio, sizeof(*names), GFP_KERNEL);
	if (!names)
		return -ENOMEM;

	gpio->chan = chan;
	chan->chan_priv = gpio;

	for (i = 0; i < n_gpio; i++) {
		struct obmf_icp_gpio_config_entry entry;
		const u32 offset = OBMF_ICP_CH0_CONFIG_CHANNEL_SPECIFIC_OFFSET
			+ sizeof(config) + i * sizeof(entry);

		rc = obmf_icp_channel_config_read(chan, offset, sizeof(entry),
						  &entry);
		if (rc) {
			dev_err(dev,
				"%s: channel config (idx %d) read (@%x, %zd) failed: %d\n",
				__func__, i, offset, sizeof(config), rc);
			return rc;
		}

		obmf_icp_gpio_config_parse(&entry, &gpio->gpios[i]);
		// TODO: sanitize name
		names[i] = (char *)&gpio->gpios[i].name;
	}

	chip = &gpio->chip;
	chip->label = devm_kasprintf(&chan->dev, GFP_KERNEL, "obmf-icp%s-gpio",
	                             dev_name(&chan->dev));
	chip->ngpio = n_gpio;
	chip->get_direction = obmf_icp_gpio_get_direction;
	chip->direction_input = obmf_icp_gpio_direction_input;
	chip->direction_output = obmf_icp_gpio_direction_output;
	// TODO: set/get multiple
	chip->set = obmf_icp_gpio_set;
	chip->get = obmf_icp_gpio_get;
	chip->parent = dev;
	chip->base = -1;
	chip->names = names;
	chip->can_sleep = true;

	rc = obmf_icp_channel_init_requester(chan);
	if (rc)
		return rc;

	rc = obmf_icp_channel_init_responder(chan,
					     obmf_icp_gpio_responder_msg);
	if (rc)
		return rc;

	gpio_irq_chip_set_chip(&chip->irq, &obmf_icp_gpio_irq_chip);

	chip->irq.parent_handler = NULL;
	chip->irq.num_parents = 0;
	chip->irq.parents = NULL;
	chip->irq.default_type = IRQ_TYPE_NONE;
	chip->irq.handler = handle_simple_irq;

	rc = devm_gpiochip_add_data(dev, &gpio->chip, gpio);
	if (rc) {
		dev_err(dev, "gpiochip init failed, %d\n", rc);
		return rc;
	}

	rc = obmf_icp_channel_enable(gpio->chan);
	if (rc) {
		dev_err(dev, "channel enable failed, %d\n", rc);
		return rc;
	}

	return 0;
}

struct obmf_icp_channel_device_id obmf_icp_gpio_ids[] = {
	OBMF_ICP_DEV_ID_TYPE(2),
	{ 0 },
};

struct obmf_icp_channel_driver obmf_icp_gpio_channel_driver = {
	.id_table = obmf_icp_gpio_ids,
	.drv = {
		.name = "obmf-icp-gpio",
	},
	.probe = obmf_icp_gpio_channel_probe,
};

module_obmf_icp_channel_driver(obmf_icp_gpio_channel_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Jeremy Kerr <jk@codeconstruct.com.au>");
MODULE_DESCRIPTION("OBMF ICP GPIO channel driver");
