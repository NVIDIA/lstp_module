// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: channel 0, discovery and channel control
 */

#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/nls.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/stddef.h>
#include <linux/string.h>

#include "obmf-core.h"
#include "obmf-dialect.h"

/* OBMF-ICP 1.0.0, 5.3, Table 5: payload byte 0, the transaction */
#define OBMF_MMIO_SHORT_READ 0
#define OBMF_MMIO_SHORT_WRITE 1

/* OBMF-ICP 1.0.0, 5.3, Tables 5 and 8: the bytes after the transaction */
struct obmf_mmio_short_read_req {
	u8 reserved;
	__le32 address;
	__le16 read_length;
} __packed;

/* OBMF-ICP 1.0.0, 5.3, Tables 5 and 10 */
struct obmf_mmio_short_write_req {
	u8 reserved;
	__le32 address;
	u8 data[];
} __packed;

/* OBMF-ICP 1.0.0, 5.15, Table 64 */
struct obmf_ch0_hdr {
	u8 version_minor_bcd;
	u8 version_major_bcd;
	__le16 vendor_id;
	__le16 device_id;
	u8 device_name[32];
	u8 num_of_channels;
	u8 config_status;
	__le32 vendor_configuration_offset;
} __packed;

/* OBMF-ICP 1.0.0, 5.15, Table 65 */
struct obmf_ch_hdr {
	u8 channel_type;
	u8 channel_number;
	u8 channel_name[16];
	u8 device_role;
	u8 channel_status;
	u8 channel_control;
	u8 channel_specific_status;
	u8 channel_specific_control;
	__le16 max_request_payload_size;
	__le16 max_response_payload_size;
	u8 reserved;
	__le32 config_size;
} __packed;

#define OBMF_CH0_OFFSETS 0x2c
#define OBMF_CH0_VERSION_MAJOR 0x01

#define OBMF_DISCOVERY_PASSES 3
#define OBMF_DISCOVERY_MS 30000
#define OBMF_CONFIG_MAX SZ_32K
#define OBMF_CONFIG_TOTAL SZ_256K

int obmf_ctrl_read(struct obmf_core *core, u32 addr, void *buf, size_t len)
{
	struct obmf_mmio_short_read_req req = {};
	size_t off, n;
	int ret;

	for (off = 0; off < len; off += n) {
		n = min(len - off, core->ch0_chunk);
		req.address = cpu_to_le32(addr + off);
		req.read_length = cpu_to_le16(n);
		ret = obmf_ch_request(core->ch0, OBMF_MMIO_SHORT_READ, &req,
				      sizeof(req), buf + off, n, NULL, 0);
		if (ret)
			return ret;
	}
	return 0;
}

int obmf_ctrl_write(struct obmf_core *core, u32 addr, const void *buf,
		    size_t len)
{
	struct obmf_mmio_short_write_req *req __free(kfree) =
		kmalloc_flex(*req, data, len);

	if (!req)
		return -ENOMEM;
	req->reserved = 0;
	req->address = cpu_to_le32(addr);
	memcpy(req->data, buf, len);
	return obmf_ch_request(core->ch0, OBMF_MMIO_SHORT_WRITE, req,
			       struct_size(req, data, len), NULL, 0, NULL, 0);
}

/*
 * A name is taken if it is valid UTF-8 without control characters, up to
 * its first NUL; anything else gives no name.
 */
void obmf_copy_name(char *dst, const u8 *src, size_t n)
{
	size_t i, len;
	unicode_t c;
	int k;

	len = strnlen((const char *)src, n);
	for (i = 0; i < len; i += k) {
		k = utf8_to_utf32(src + i, len - i, &c);
		if (k < 0 || c < 0x20 || c == 0x7f)
			goto bad;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
	return;
bad:
	dst[0] = '\0';
}

/* Shortest request and response payload a channel type needs */
static size_t obmf_type_head(u8 type)
{
	switch (type) {
	case OBMF_TYPE_SERIAL:
		/* OBMF-ICP 1.0.0, 5.5, Tables 22 and 23 */
		return 3;
	case OBMF_TYPE_I2C_CONTROLLER:
		/* OBMF-ICP 1.0.0, 5.6, Tables 26 and 28 */
		return 4;
	}
	return 1;
}

/*
 * OBMF-ICP 1.0.0, 5.4, Tables 18 and 19: a Get of N lines takes 1 + 2N
 * payload bytes each way, a Set 1 + 2N out. Outputs are not read: Table
 * 17 names "Read on OUT" an invalid operation.
 */
static void obmf_gpio_quirks(struct obmf_channel_info *info)
{
	info->quirks.gpio_per_get =
		(min(info->max_req, info->max_resp) - 1) / 2;
	info->quirks.gpio_per_set = (info->max_req - 1) / 2;
}

static bool obmf_overlaps(struct obmf_channel **found, int nr, u32 off,
			  size_t len)
{
	struct obmf_channel *ch;
	int i;

	for (i = 1; i < nr; i++) {
		ch = found[i];
		if (ch &&
		    off < ch->hdr_off + sizeof(struct obmf_ch_hdr) +
				    ch->info.config_len &&
		    ch->hdr_off < off + len)
			return true;
	}
	return false;
}

/*
 * Why the channel cannot be used: CHANNEL_ENABLE or CONFIGURATION_VALID
 * clear (OBMF-ICP 1.0.0, 5.15, Table 65); NULL if both are set. The host
 * never switches a channel on, so the device must have done both.
 */
static const char *obmf_ch_unusable(const struct obmf_ch_hdr *h)
{
	bool on = h->channel_control & OBMF_CH_CONTROL_ENABLE;
	bool valid = h->channel_status & OBMF_CH_STATUS_CONFIG_VALID;

	if (!on && !valid)
		return "not enabled, configuration not valid";
	if (!on)
		return "not enabled";
	if (!valid)
		return "configuration not valid";
	return NULL;
}

static int obmf_discover_channel(struct obmf_core *core,
				 struct obmf_channel **found, int nr, u32 off,
				 u32 hdr_end, size_t *total)
{
	struct device *dev = core->dev;
	size_t cfg, head, max_req, max_resp;
	struct obmf_ch_hdr h;
	const char *why;
	u32 end;
	int ret;

	ret = obmf_ctrl_read(core, off, &h, sizeof(h));
	if (ret)
		return ret;

	cfg = le32_to_cpu(h.config_size);
	max_req = le16_to_cpu(h.max_request_payload_size);
	max_resp = le16_to_cpu(h.max_response_payload_size);
	head = obmf_type_head(h.channel_type);

	if (h.channel_number != nr) {
		obmf_bind_warn(core,
			       "channel %d: header gives number %u, ignored\n",
			       nr, h.channel_number);
		return 0;
	}
	if (off < hdr_end || cfg > OBMF_CONFIG_MAX ||
	    check_add_overflow(off, sizeof(h) + cfg, &end) ||
	    *total + cfg > OBMF_CONFIG_TOTAL ||
	    obmf_overlaps(found, nr, off, sizeof(h) + cfg)) {
		obmf_bind_warn(core, "channel %d: bad layout at %#x, ignored\n",
			       nr, off);
		return 0;
	}
	/*
	 * Whether the payload limits count the message header is open: send
	 * at most the limit less a header, accept up to the limit plus one.
	 */
	if (max_req < OBMF_HDR_LEN + head || max_resp < OBMF_HDR_LEN + head) {
		obmf_bind_warn(core,
			       "channel %d: limits %zu/%zu too small, ignored\n",
			       nr, max_req, max_resp);
		return 0;
	}
	why = obmf_ch_unusable(&h);
	if (why) {
		obmf_bind_warn(core, "channel %d: %s, ignored\n", nr, why);
		return 0;
	}

	struct obmf_channel *ch __free(obmf_ch) =
		obmf_ch_alloc(core, nr, max_req - OBMF_HDR_LEN,
			      max_resp - OBMF_HDR_LEN, max_resp + OBMF_HDR_LEN);
	if (!ch)
		return -ENOMEM;
	ch->info.type = h.channel_type;
	ch->info.role = h.device_role;
	obmf_copy_name(ch->info.name, h.channel_name, sizeof(h.channel_name));
	ch->hdr_off = off;
	ch->status = h.channel_status;
	ch->control = h.channel_control;

	if (cfg) {
		ch->config = kmalloc(cfg, GFP_KERNEL);
		if (!ch->config)
			return -ENOMEM;
		ret = obmf_ctrl_read(core, off + sizeof(h), ch->config, cfg);
		if (ret)
			return ret;
	}
	ch->info.config = ch->config;
	ch->info.config_len = cfg;
	*total += cfg;
	if (ch->info.type == OBMF_TYPE_GPIO)
		obmf_gpio_quirks(&ch->info);

	if (ch->info.max_req < max_req - OBMF_HDR_LEN ||
	    ch->info.max_resp < max_resp - OBMF_HDR_LEN)
		dev_dbg(dev,
			"channel %d: limits cut to %zu/%zu by the transport\n",
			nr, ch->info.max_req, ch->info.max_resp);
	dev_dbg(dev,
		"channel %d: type %#x role %u name '%s' status %#x control %#x limits %zu/%zu config %zu\n",
		nr, ch->info.type, ch->info.role, ch->info.name, ch->status,
		ch->control, ch->info.max_req, ch->info.max_resp, cfg);
	found[nr] = no_free_ptr(ch);
	return 0;
}

static int obmf_discover_pass(struct obmf_core *core,
			      struct obmf_channel **found)
{
	struct obmf_ch0_hdr dh, again;
	size_t total = 0;
	u32 hdr_end;
	int n, i, ret;

	ret = obmf_ctrl_read(core, 0, &dh, sizeof(dh));
	if (ret)
		return ret;
	if (dh.version_major_bcd != OBMF_CH0_VERSION_MAJOR) {
		dev_err(core->dev, "unsupported OBMF-ICP version %x.%x\n",
			dh.version_major_bcd, dh.version_minor_bcd);
		return -ENODEV;
	}

	n = dh.num_of_channels;
	__le32 *offs __free(kfree) = kzalloc_objs(*offs, n, GFP_KERNEL);
	if (!offs)
		return -ENOMEM;
	hdr_end = OBMF_CH0_OFFSETS + n * sizeof(*offs);
	ret = obmf_ctrl_read(core, OBMF_CH0_OFFSETS, offs, n * sizeof(*offs));
	if (ret)
		return ret;
	for (i = 1; i <= n; i++) {
		if (!offs[i - 1])
			continue;
		ret = obmf_discover_channel(core, found, i,
					    le32_to_cpu(offs[i - 1]), hdr_end,
					    &total);
		if (ret)
			return ret;
	}

	ret = obmf_ctrl_read(core, 0, &again, sizeof(again));
	if (ret)
		return ret;
	if (memcmp(&dh, &again, sizeof(dh)))
		return -EAGAIN;

	core->version_major = dh.version_major_bcd;
	core->version_minor = dh.version_minor_bcd;
	core->vendor_id = le16_to_cpu(dh.vendor_id);
	core->device_id = le16_to_cpu(dh.device_id);
	obmf_copy_name(core->device_name, dh.device_name,
		       sizeof(dh.device_name));
	dev_dbg(core->dev,
		"OBMF-ICP %x.%x device %04x:%04x '%s', %d channels\n",
		core->version_major, core->version_minor, core->vendor_id,
		core->device_id, core->device_name, n);
	return 0;
}

/*
 * Before another pass, wait until a response timeout passes without a
 * response arriving late or for an unknown channel. An unbind or a
 * suspend ends the wait at once.
 */
static void obmf_discover_settle(struct obmf_core *core, unsigned long deadline)
{
	atomic_long_t *late = &core->ch0->cnt[OBMF_CH_RX_LATE];
	atomic_long_t *stray = &core->cnt[OBMF_IF_RX_NO_CHANNEL];
	long seen;

	do {
		seen = atomic_long_read(late) + atomic_long_read(stray);
		wait_event_timeout(core->state_wq, !obmf_core_discovering(core),
				   msecs_to_jiffies(OBMF_DEFAULT_TIMEOUT_MS));
	} while (seen != atomic_long_read(late) + atomic_long_read(stray) &&
		 obmf_core_discovering(core) && time_before(jiffies, deadline));
}

/*
 * OBMF-ICP 1.0.0, 5.15, Table 65: a channel whose HEALTH_STATUS is 3
 * (not ready) or 15 (channel error), or that sets CHANNEL_ERROR_INDICATOR,
 * still binds; once per bind, after the pass that is kept.
 */
#define OBMF_CH_HEALTH_NOT_READY 3
#define OBMF_CH_HEALTH_ERROR 15

static void obmf_discover_health(struct obmf_core *core)
{
	struct obmf_channel *ch;
	u8 health;
	bool ind;
	int i;

	for (i = 1; i < OBMF_NR_CHANNELS; i++) {
		ch = core->chans[i];
		if (!ch)
			continue;
		health = FIELD_GET(OBMF_CH_STATUS_HEALTH, ch->status);
		ind = ch->status & OBMF_CH_STATUS_ERROR;
		if (health == OBMF_CH_HEALTH_NOT_READY ||
		    health == OBMF_CH_HEALTH_ERROR || ind)
			obmf_report_bounded(core, core->dev, OBMF_EV_CH_HEALTH,
					    i, health, ind);
	}
}

/*
 * A reply left from an earlier binding fails a read's checks or makes
 * the next read time out; a device header that changed, or a timeout
 * the device reports, means the device was still settling. Such a pass
 * is discarded and run again.
 */
static bool obmf_discover_again(int ret)
{
	return ret == -ETIMEDOUT || ret == -ETIME || ret == -EPROTO ||
	       ret == -EAGAIN;
}

int obmf_ctrl_discover(struct obmf_core *core)
{
	const struct obmf_dialect *dialect = obmf_dialect(core);
	unsigned long deadline = jiffies + msecs_to_jiffies(OBMF_DISCOVERY_MS);
	struct obmf_channel **found __free(kfree) =
		kzalloc_objs(*found, OBMF_NR_CHANNELS, GFP_KERNEL);
	int pass, i, ret = 0;

	BUILD_BUG_ON(sizeof(struct obmf_ch0_hdr) != OBMF_CH0_OFFSETS);
	BUILD_BUG_ON(sizeof(struct obmf_ch_hdr) != 0x20);
	BUILD_BUG_ON(offsetof(struct obmf_ch_hdr, max_request_payload_size) !=
		     0x17);
	BUILD_BUG_ON(sizeof(struct obmf_mmio_short_read_req) != 7);
	BUILD_BUG_ON(offsetof(struct obmf_ch_hdr, channel_status) !=
		     OBMF_CH_REG_STATUS);
	BUILD_BUG_ON(offsetof(struct obmf_ch_hdr, channel_control) !=
		     OBMF_CH_REG_CONTROL);
	BUILD_BUG_ON(offsetof(struct obmf_ch_hdr, channel_specific_status) !=
		     OBMF_CH_REG_SPECIFIC_STATUS);
	BUILD_BUG_ON(offsetof(struct obmf_ch_hdr, channel_specific_control) !=
		     OBMF_CH_REG_SPECIFIC_CONTROL);

	if (!found)
		return -ENOMEM;

	for (pass = 0; pass < OBMF_DISCOVERY_PASSES; pass++) {
		bool last = pass == OBMF_DISCOVERY_PASSES - 1;

		if (pass)
			obmf_discover_settle(core, deadline);
		if (dialect && dialect->discover)
			ret = obmf_dialect_discover(core, found, last);
		else
			ret = obmf_discover_pass(core, found);
		if (!ret) {
			memcpy(core->chans, found,
			       OBMF_NR_CHANNELS * sizeof(*found));
			break;
		}
		for (i = 0; i < OBMF_NR_CHANNELS; i++) {
			obmf_ch_free(found[i]);
			found[i] = NULL;
		}
		if (!obmf_discover_again(ret) || !obmf_core_discovering(core))
			break;
		dev_dbg(core->dev, "discovery pass %d failed: %pe, again\n",
			pass + 1, ERR_PTR(ret));
		if (time_after(jiffies, deadline)) {
			ret = -ETIMEDOUT;
			break;
		}
	}

	if (ret && !obmf_core_discovering(core)) {
		dev_dbg(core->dev, "discovery stopped: %pe\n", ERR_PTR(ret));
	} else if (ret == -EAGAIN) {
		dev_err(core->dev, "channel 0 changed during discovery\n");
		ret = -EIO;
	} else if (ret) {
		dev_err(core->dev, "discovery failed: %pe\n", ERR_PTR(ret));
	} else {
		obmf_discover_health(core);
	}
	return ret;
}

static u32 obmf_ch_reg(struct obmf_channel *ch, size_t field)
{
	return ch->hdr_off + field;
}

/**
 * obmf_ch_status_read - read the channel's status registers
 * @fdev: the function device
 * @status: CHANNEL_STATUS
 * @specific: CHANNEL_SPECIFIC_STATUS
 */
int obmf_ch_status_read(struct obmf_fdev *fdev, u8 *status, u8 *specific)
{
	struct obmf_channel *ch = fdev->ch;
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	struct obmf_core *core = ch->core;
	u32 addr =
		obmf_ch_reg(ch, offsetof(struct obmf_ch_hdr, channel_status));
	u8 regs[3];
	int ret;

	if (dialect && dialect->ch_reg)
		return obmf_dialect_status_read(ch, status, specific);
	ret = obmf_ctrl_read(core, addr, regs, sizeof(regs));
	if (ret)
		return ret;
	*status = regs[0];
	*specific = regs[2];
	return 0;
}

/**
 * obmf_ch_control_update - read-modify-write CHANNEL_SPECIFIC_CONTROL
 * @fdev: the function device
 * @specific_mask: the bits to change; include every write-1 action bit of
 *	the register, 0 in @specific_val for one not meant, as a device may
 *	read it back as written
 * @specific_val: their new values
 *
 * A @specific_mask of 0 changes nothing: nothing is read or written.
 */
int obmf_ch_control_update(struct obmf_fdev *fdev, u8 specific_mask,
			   u8 specific_val)
{
	struct obmf_channel *ch = fdev->ch;
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	struct obmf_core *core = ch->core;
	u32 addr = obmf_ch_reg(ch, offsetof(struct obmf_ch_hdr,
					    channel_specific_control));
	u8 val;
	int ret;

	if (!specific_mask)
		return 0;
	guard(mutex)(&core->ch0_lock);
	if (dialect && dialect->ch_reg)
		return obmf_dialect_control_update(ch, specific_mask,
						   specific_val);
	ret = obmf_ctrl_read(core, addr, &val, 1);
	if (ret)
		return ret;
	val = (val & ~specific_mask) | (specific_val & specific_mask);
	return obmf_ctrl_write(core, addr, &val, 1);
}
