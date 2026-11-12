// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: the core's side of a dialect
 *
 * A device that speaks another wire protocol is served by the same
 * core: its messages are translated at the edge by the ops of struct
 * obmf_dialect. This file holds what the core does only for such a
 * device: the decode buffers, the translated receive and send paths,
 * the wire transfer, the discovery pass and the channel header bytes.
 */

#include <linux/bug.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "obmf-core.h"
#include "obmf-dialect.h"

static bool obmf_dialect_sets_nothing(const struct obmf_dialect *dialect)
{
	return !dialect->encode && !dialect->decode && !dialect->discover &&
	       !dialect->status_to_errno && !dialect->ch_reg &&
	       !dialect->ch0_whole_msgs && !dialect->keep_ch_limits;
}

/*
 * The transport takes the protocol of its USB match entry. A table with
 * only one of encode and decode, with keep_ch_limits but no encode, or
 * with nothing but a name set, is a driver bug.
 */
int obmf_dialect_attach(struct obmf_transport *tp,
			const struct obmf_usb_proto *proto)
{
	const struct obmf_dialect *dialect = proto->dialect;

	if (dialect) {
		if (WARN_ON_ONCE(!dialect->encode != !dialect->decode))
			return -EINVAL;
		if (WARN_ON_ONCE(dialect->keep_ch_limits && !dialect->encode))
			return -EINVAL;
		if (WARN_ON_ONCE(obmf_dialect_sets_nothing(dialect)))
			return -EINVAL;
	}
	tp->dialect = dialect;
	return 0;
}

/* The decoded forms take a header more than the slot holds */
int obmf_dialect_ch_alloc(struct obmf_channel *ch, size_t rx_cap)
{
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);

	if (!dialect || !dialect->decode)
		return 0;
	ch->dec_cap = OBMF_HDR_LEN + rx_cap;
	ch->rx_dec = kmalloc(ch->dec_cap, GFP_KERNEL);
	ch->dreq_dec = kmalloc(ch->dec_cap, GFP_KERNEL);
	if (!ch->rx_dec || !ch->dreq_dec)
		return -ENOMEM;
	return 0;
}

void obmf_dialect_ch_free(struct obmf_channel *ch)
{
	kfree(ch->dialect_data);
	kfree(ch->dreq_dec);
	kfree(ch->rx_dec);
}

/*
 * The errno of a wire status with no OBMF-ICP code; 0 to map the
 * decoded status. The wire status is the one the upcall kept before
 * decode, or the status as received where nothing is decoded.
 */
int obmf_dialect_status_errno(struct obmf_channel *ch)
{
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	u8 raw;

	if (!dialect || !dialect->status_to_errno)
		return 0;
	raw = dialect->decode ? ch->slot.raw_status : ch->slot.status;
	if (!raw)
		return 0;
	return dialect->status_to_errno(ch, raw);
}

/* Rewrites the request in tx_buf into its wire form of *@len bytes */
int obmf_dialect_encode_request(struct obmf_channel *ch, size_t *len)
{
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	struct obmf_core *core = ch->core;
	ssize_t n;

	if (!dialect || !dialect->encode)
		return 0;
	n = dialect->encode(ch, ch->tx_buf, *len,
			    min(ch->tx_cap, core->tp->max_tx_msg));
	if (n < 0)
		return n;
	/* Only an answer may be dropped */
	if (WARN_ON_ONCE(!n))
		return -EINVAL;
	*len = n;
	return 0;
}

/*
 * Rewrites the answer in @msg into its wire form of *@len bytes; *@len
 * is 0 where the wire protocol has no answer for the request
 */
int obmf_dialect_encode_answer(struct obmf_channel *ch, u8 *msg, size_t *len)
{
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	struct obmf_core *core = ch->core;
	ssize_t n;

	if (!dialect || !dialect->encode)
		return 0;
	n = dialect->encode(ch, msg, *len, min(*len, core->tp->max_tx_msg));
	if (n < 0)
		return n;
	if (!n)
		obmf_ch_count(ch, OBMF_CH_RX_DROPPED);
	*len = n;
	return 0;
}

/*
 * The dialect's way to send a message in its own wire form: no encode,
 * no decode, no echo check. The reply comes back header included. The
 * slot's wire flag is set for this request only, under the slot lock;
 * only the translated receive path reads it, so the dialect must
 * decode.
 */
int obmf_ch_xfer_wire(struct obmf_channel *ch, const void *msg, size_t len,
		      void *reply, size_t cap, size_t *reply_len,
		      unsigned int timeout_ms)
{
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	struct obmf_core *core = ch->core;
	struct obmf_slot *slot = &ch->slot;
	int ret;

	if (WARN_ON_ONCE(!dialect || !dialect->decode))
		return -EINVAL;
	if (len < OBMF_HDR_LEN || len > ch->tx_cap ||
	    len > core->tp->max_tx_msg)
		return -EMSGSIZE;

	CLASS(obmf_inflight, inflight)(core);
	if (!inflight)
		return -ENODEV;
	guard(mutex)(&slot->lock);

	memcpy(ch->tx_buf, msg, len);
	scoped_guard(spinlock_irqsave, &slot->rx_lock)
		slot->wire = true;
	ret = obmf_slot_xfer(ch, 0, len, timeout_ms);
	scoped_guard(spinlock_irqsave, &slot->rx_lock)
		slot->wire = false;
	if (ret)
		return ret;
	if (slot->len > cap)
		return -EMSGSIZE;
	memcpy(reply, slot->buf, slot->len);
	*reply_len = slot->len;
	return 0;
}

/*
 * A response under a dialect: the wire status stays in raw_status for
 * status_to_errno(), the decoded form goes to the slot. A wire request
 * takes the message as received.
 */
static void obmf_rx_wire_response(struct obmf_channel *ch, const u8 *msg,
				  size_t len)
{
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	struct obmf_slot *slot = &ch->slot;
	const u8 *body = msg;
	ssize_t n = len;

	guard(spinlock_irqsave)(&slot->rx_lock);
	if (!slot->armed) {
		obmf_ch_count(ch, OBMF_CH_RX_LATE);
		dev_dbg_ratelimited(ch->core->dev,
				    "channel %u: late response dropped\n",
				    ch->info.number);
		obmf_slot_late(slot);
		return;
	}
	slot->armed = false;
	slot->raw_status = msg[1] & OBMF_HDR_STATUS;
	slot->status = slot->raw_status;
	if (!slot->wire) {
		n = dialect->decode(ch, msg, len, ch->rx_dec, ch->dec_cap,
				    slot->cmd, NULL);
		if (n >= 0 && n < (ssize_t)OBMF_HDR_LEN)
			n = -EPROTO;
		if (n >= 0) {
			slot->status = ch->rx_dec[1] & OBMF_HDR_STATUS;
			body = ch->rx_dec + OBMF_HDR_LEN;
			n -= OBMF_HDR_LEN;
		}
	}
	/* The transport, not the channel, bounds a wire reply */
	if (n > (ssize_t)slot->cap)
		n = -EPROTO;
	if (n < 0) {
		slot->err = n;
		slot->len = len;
		obmf_ch_count(ch, OBMF_CH_PROTOCOL_ERRORS);
	} else {
		memcpy(slot->buf, body, n);
		slot->len = n;
	}
	obmf_ch_count(ch, OBMF_CH_RX_RESPONSES);
	complete(&slot->done);
}

/*
 * A device request under a dialect. One the dialect cannot decode may
 * still wait for an answer where the type answers every request: the
 * invalid answer is owed, and the dialect drops it where its wire
 * protocol has none.
 */
static void obmf_rx_wire_dreq(struct obmf_channel *ch, const u8 *msg,
			      size_t len)
{
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	struct obmf_core *core = ch->core;
	bool bad = false, queued = true, due = false;
	ssize_t n;

	obmf_ch_count(ch, OBMF_CH_RX_DEVICE_REQUESTS);
	scoped_guard(spinlock_irqsave, &ch->dreq_lock) {
		n = dialect->decode(ch, msg, len, ch->dreq_dec, ch->dec_cap, 0,
				    NULL);
		if (n < (ssize_t)OBMF_HDR_LEN ||
		    !obmf_dreq_len_ok(ch, ch->dreq_dec + OBMF_HDR_LEN,
				      n - OBMF_HDR_LEN)) {
			bad = true;
			obmf_dreq_owe_invalid(ch);
		} else {
			queued = obmf_dreq_add(ch, ch->dreq_dec + OBMF_HDR_LEN,
					       n - OBMF_HDR_LEN, &due);
		}
	}

	if (bad) {
		obmf_count(core, OBMF_IF_RX_MALFORMED);
		if (!obmf_core_discovering(core))
			obmf_report(core, core->dev, OBMF_EV_BAD_DREQ,
				    ch->info.number, len, 0);
	} else if (!queued) {
		obmf_dreq_full(ch, due);
	}
	queue_work(core->wq, &ch->work);
}

/* Every message for a dialect that decodes, header included; any context */
void obmf_dialect_rx(struct obmf_channel *ch, const u8 *msg, size_t len)
{
	const struct obmf_hdr *hdr = (const struct obmf_hdr *)msg;

	if (hdr->status & OBMF_HDR_RQRESP)
		obmf_rx_wire_response(ch, msg, len);
	else
		obmf_rx_wire_dreq(ch, msg, len);
}

int obmf_dialect_discover(struct obmf_core *core, struct obmf_channel **found,
			  bool last)
{
	return obmf_dialect(core)->discover(core, found, last);
}

/* The channel header bytes, through the ch_reg op; under ch0_lock */
int obmf_dialect_control_update(struct obmf_channel *ch, u8 specific_mask,
				u8 specific_val)
{
	return obmf_dialect(ch->core)->ch_reg(ch, OBMF_CH_REG_SPECIFIC_CONTROL,
					      specific_mask, specific_val,
					      NULL);
}

/* Takes ch0_lock */
int obmf_dialect_status_read(struct obmf_channel *ch, u8 *status, u8 *specific)
{
	const struct obmf_dialect *dialect = obmf_dialect(ch->core);
	int ret;

	guard(mutex)(&ch->core->ch0_lock);
	ret = dialect->ch_reg(ch, OBMF_CH_REG_STATUS, 0, 0, status);
	if (!ret)
		ret = dialect->ch_reg(ch, OBMF_CH_REG_SPECIFIC_STATUS, 0, 0,
				      specific);
	return ret;
}

#if IS_ENABLED(CONFIG_OBMF_KUNIT_TEST)
#include <kunit/device.h>
#include <kunit/test.h>
#include <linux/unaligned.h>

/*
 * Each member of struct obmf_dialect alone, and the encode/decode pair
 * alone: one run of the same steps on an OBMF-ICP core gives the native
 * values, and on a core with the table only the member's site may
 * differ from them.
 */

#define DT_MSG 516
#define DT_MEM 0x200
/* channel 1: an I2C channel whose header is here, with these limits */
#define DT_CH 0x100
#define DT_LIMIT 1000
/* its configuration, OBMF-ICP 1.0.0, 5.15.2, Table 79: 100 kHz */
#define DT_CFG (DT_CH + 0x20)
#define DT_CFG_LEN 2
/* the wire form of the fake dialect: the payload XOR this byte */
#define DT_KEY 0x5a
/* a wire status, and the errno the dialect gives it */
#define DT_ERR 0x40
#define DT_ERRNO (-ENOTCONN)

/* OBMF-ICP 1.0.0, 5.15, Tables 64 and 65 */
#define DT_CH0_NUM 0x26
#define DT_CH0_OFFSETS 0x2c

enum {
	DT_ENCODE = BIT(0),
	DT_DECODE = BIT(1),
	DT_DISCOVER = BIT(2),
	DT_STATUS_TO_ERRNO = BIT(3),
	DT_CH_REG = BIT(4),
	DT_CH0_WHOLE_MSGS = BIT(5),
	DT_KEEP_CH_LIMITS = BIT(6),
};

/*
 * A device with channel 0 and one I2C channel. A request on the I2C
 * channel gets its command byte and two more back, with @status.
 */
struct dt_fake {
	struct kunit *test;
	struct obmf_transport tp;
	struct obmf_usb_proto proto;
	bool wire;
	u8 mem[DT_MEM];
	u8 regs[OBMF_CH_REG_SPECIFIC_CONTROL + 1];
	u8 status;
	int ch0_msgs;
	u8 req[16];
	size_t req_len;
	u8 ans[16];
	size_t ans_len;
	int encodes;
	int decodes;
	int discovers;
	int errnos;
	int ch_regs;
};

/* What one run saw; the messages as they went to the transport */
struct dt_obs {
	size_t ch0_resp, ch0_cap;
	size_t max_req, max_resp, cap;
	bool dec_bufs;
	int ch0_msgs;
	u8 req[16];
	size_t req_len;
	int req_ret;
	u8 resp[8];
	size_t resp_len;
	int err_ret;
	u8 ans[16];
	size_t ans_len;
	int st_ret, cu_ret;
	u8 cu_mem, st, spec, reg_spec_control;
	int encodes, decodes, discovers, errnos, ch_regs;
};

static atomic_t dt_ids = ATOMIC_INIT(0);

static struct dt_fake *dt_of(struct obmf_core *core)
{
	return container_of(core->tp, struct dt_fake, tp);
}

static void dt_xor(u8 *msg, size_t len)
{
	size_t i;

	for (i = OBMF_HDR_LEN; i < len; i++)
		msg[i] ^= DT_KEY;
}

static size_t dt_hdr(u8 *msg, u8 ch, u8 status, size_t plen)
{
	msg[0] = ch;
	msg[1] = status;
	put_unaligned_le16(plen, msg + 2);
	return OBMF_HDR_LEN + plen;
}

static void dt_deliver(struct dt_fake *f, u8 *msg, size_t len)
{
	if (f->wire)
		dt_xor(msg, len);
	obmf_core_rx(f->tp.core, msg, len);
}

/* Channel 0 takes short reads and writes (OBMF-ICP 1.0.0, 5.3, Table 5) */
static void dt_ch0(struct dt_fake *f, const u8 *msg, size_t len)
{
	u32 addr = get_unaligned_le32(msg + 6);
	u8 out[OBMF_HDR_LEN + 64];
	size_t n = 0;

	f->ch0_msgs++;
	if (msg[4] == 1) {
		if (addr + len - 10 > DT_MEM)
			goto bad;
		memcpy(f->mem + addr, msg + 10, len - 10);
	} else {
		n = get_unaligned_le16(msg + 10);
		if (n > 64 || addr + n > DT_MEM)
			goto bad;
		memcpy(out + OBMF_HDR_LEN, f->mem + addr, n);
	}
	dt_deliver(f, out, dt_hdr(out, 0, OBMF_HDR_RQRESP, n));
	return;
bad:
	KUNIT_FAIL(f->test, "channel 0 access at %#x out of range", addr);
}

static int dt_send(struct obmf_transport *tp, const void *buf, size_t len,
		   unsigned int timeout_ms)
{
	struct dt_fake *f = container_of(tp, struct dt_fake, tp);
	u8 msg[DT_MSG], out[OBMF_HDR_LEN + 3];

	/* answers come from the core's work queue: no assertion */
	if (len > sizeof(msg)) {
		KUNIT_FAIL(f->test, "a %zu-byte message", len);
		return -EMSGSIZE;
	}
	memcpy(msg, buf, len);
	if (msg[1] & OBMF_HDR_RQRESP) {
		f->ans_len = min(len, sizeof(f->ans));
		memcpy(f->ans, msg, f->ans_len);
		return 0;
	}
	if (!msg[0]) {
		if (f->wire)
			dt_xor(msg, len);
		dt_ch0(f, msg, len);
		return 0;
	}
	f->req_len = min(len, sizeof(f->req));
	memcpy(f->req, msg, f->req_len);
	if (f->wire)
		dt_xor(msg, len);
	out[OBMF_HDR_LEN] = msg[OBMF_HDR_LEN];
	out[OBMF_HDR_LEN + 1] = 0xa1;
	out[OBMF_HDR_LEN + 2] = 0xa2;
	dt_deliver(f, out, dt_hdr(out, msg[0], OBMF_HDR_RQRESP | f->status, 3));
	return 0;
}

static int dt_start(struct obmf_transport *tp)
{
	return 0;
}

static void dt_stop(struct obmf_transport *tp)
{
}

static const struct obmf_transport_ops dt_ops = {
	.start = dt_start,
	.stop = dt_stop,
	.quiesce = dt_stop,
	.resume = dt_start,
	.send = dt_send,
};

static ssize_t dt_encode(struct obmf_channel *ch, u8 *buf, size_t len,
			 size_t cap)
{
	dt_of(ch->core)->encodes++;
	if (len > cap)
		return -EMSGSIZE;
	dt_xor(buf, len);
	return len;
}

static ssize_t dt_decode(struct obmf_channel *ch, const u8 *in, size_t len,
			 u8 *out, size_t cap, u8 pending_cmd,
			 unsigned long *levels)
{
	dt_of(ch->core)->decodes++;
	if (len > cap)
		return -EMSGSIZE;
	memcpy(out, in, len);
	dt_xor(out, len);
	return len;
}

/* The channel the native pass finds, without reading channel 0 */
static int dt_discover(struct obmf_core *core, struct obmf_channel **found,
		       bool last)
{
	struct dt_fake *f = dt_of(core);
	const u8 *h = f->mem + DT_CH;
	struct obmf_channel *ch;

	f->discovers++;
	ch = obmf_ch_alloc(core, 1, DT_LIMIT - OBMF_HDR_LEN,
			   DT_LIMIT - OBMF_HDR_LEN, DT_LIMIT + OBMF_HDR_LEN);
	if (!ch)
		return -ENOMEM;
	ch->config = kmemdup(f->mem + DT_CFG, DT_CFG_LEN, GFP_KERNEL);
	if (!ch->config) {
		obmf_ch_free(ch);
		return -ENOMEM;
	}
	ch->info.config = ch->config;
	ch->info.config_len = DT_CFG_LEN;
	ch->info.type = h[0];
	ch->info.role = h[0x12];
	obmf_copy_name(ch->info.name, h + 2, 16);
	ch->hdr_off = DT_CH;
	ch->status = h[OBMF_CH_REG_STATUS];
	ch->control = h[OBMF_CH_REG_CONTROL];
	found[1] = ch;
	return 0;
}

static int dt_status_to_errno(const struct obmf_channel *ch, u8 raw)
{
	dt_of(ch->core)->errnos++;
	return raw == DT_ERR ? DT_ERRNO : 0;
}

/* The bytes in the fake's own registers, not in channel 0 */
static int dt_ch_reg(struct obmf_channel *ch, unsigned int reg, u8 mask, u8 val,
		     u8 *old)
{
	struct dt_fake *f = dt_of(ch->core);

	f->ch_regs++;
	if (reg >= ARRAY_SIZE(f->regs))
		return -EINVAL;
	if (old)
		*old = f->regs[reg];
	f->regs[reg] = (f->regs[reg] & ~mask) | (val & mask);
	return 0;
}

static const struct obmf_dialect dt_pair = {
	.encode = dt_encode,
	.decode = dt_decode,
};

static const struct obmf_dialect dt_discover_only = {
	.discover = dt_discover,
};

static const struct obmf_dialect dt_status_to_errno_only = {
	.status_to_errno = dt_status_to_errno,
};

static const struct obmf_dialect dt_ch_reg_only = {
	.ch_reg = dt_ch_reg,
};

static const struct obmf_dialect dt_ch0_whole_msgs_only = {
	.ch0_whole_msgs = true,
};

static const struct obmf_dialect dt_keep_ch_limits_pair = {
	.encode = dt_encode,
	.decode = dt_decode,
	.keep_ch_limits = true,
};

static void dt_unbind_action(void *data)
{
	struct obmf_core *core = data;

	obmf_core_unbind(core, true);
}

static struct dt_fake *dt_bind(struct kunit *test,
			       const struct obmf_dialect *dialect)
{
	struct dt_fake *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	struct obmf_core *core;
	char name[24];
	u8 *h;

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->test = test;
	f->mem[1] = 0x01;
	memcpy(f->mem + 6, "dt", 2);
	f->mem[DT_CH0_NUM] = 1;
	put_unaligned_le32(DT_CH, f->mem + DT_CH0_OFFSETS);
	h = f->mem + DT_CH;
	h[0] = OBMF_TYPE_I2C_CONTROLLER;
	h[1] = 1;
	memcpy(h + 2, "i2c", 3);
	h[0x12] = OBMF_ROLE_PRODUCER;
	h[OBMF_CH_REG_STATUS] = 0xa1;
	h[OBMF_CH_REG_CONTROL] = OBMF_CH_CONTROL_ENABLE;
	h[OBMF_CH_REG_SPECIFIC_STATUS] = 0x42;
	h[OBMF_CH_REG_SPECIFIC_CONTROL] = 0xa5;
	put_unaligned_le16(DT_LIMIT, h + 0x17);
	put_unaligned_le16(DT_LIMIT, h + 0x19);
	put_unaligned_le32(DT_CFG_LEN, h + 0x1c);
	f->regs[OBMF_CH_REG_STATUS] = 0x33;
	f->regs[OBMF_CH_REG_SPECIFIC_STATUS] = 0x44;
	f->regs[OBMF_CH_REG_SPECIFIC_CONTROL] = 0xa5;

	snprintf(name, sizeof(name), "obmf-dt.%d", atomic_inc_return(&dt_ids));
	f->tp.dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->tp.dev);
	f->tp.ops = &dt_ops;
	f->tp.max_tx_msg = DT_MSG;
	f->tp.max_rx_msg = DT_MSG;
	f->proto.dialect = dialect;
	f->wire = dialect && dialect->encode;
	KUNIT_ASSERT_EQ(test, obmf_dialect_attach(&f->tp, &f->proto), 0);

	core = obmf_core_create(&f->tp);
	KUNIT_ASSERT_NOT_NULL(test, core);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, dt_unbind_action, core),
			0);
	KUNIT_ASSERT_EQ(test, obmf_core_bind(core), 0);
	return f;
}

static void dt_observe(struct kunit *test, const struct obmf_dialect *dialect,
		       struct dt_obs *o)
{
	static const u8 req[] = { 0x01, 0x11, 0x22 };
	static const u8 dreq[] = { 0x01, 0x00 };
	struct dt_fake *f = dt_bind(test, dialect);
	struct obmf_core *core = f->tp.core;
	struct obmf_channel *ch = core->chans[1];
	u8 msg[OBMF_HDR_LEN + sizeof(dreq)];
	size_t len;

	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	o->ch0_msgs = f->ch0_msgs;
	o->ch0_resp = core->ch0->info.max_resp;
	o->ch0_cap = core->ch0->slot.cap;
	o->max_req = ch->info.max_req;
	o->max_resp = ch->info.max_resp;
	o->cap = ch->slot.cap;
	o->dec_bufs = ch->rx_dec && ch->dreq_dec;

	o->req_ret = obmf_ch_request(ch, req[0], req + 1, sizeof(req) - 1,
				     o->resp, sizeof(o->resp), &o->resp_len,
				     OBMF_DEFAULT_TIMEOUT_MS);
	o->req_len = f->req_len;
	memcpy(o->req, f->req, f->req_len);
	f->status = DT_ERR;
	o->err_ret = obmf_ch_request(ch, req[0], req + 1, sizeof(req) - 1, msg,
				     sizeof(msg), &len,
				     OBMF_DEFAULT_TIMEOUT_MS);
	f->status = 0;

	/* no driver takes it: the core sends the default answer */
	memcpy(msg + OBMF_HDR_LEN, dreq, sizeof(dreq));
	dt_deliver(f, msg, dt_hdr(msg, 1, 0, sizeof(dreq)));
	flush_workqueue(core->wq);
	o->ans_len = f->ans_len;
	memcpy(o->ans, f->ans, f->ans_len);

	o->st_ret = obmf_ch_status_read(ch->fdev, &o->st, &o->spec);
	o->cu_ret = obmf_ch_control_update(ch->fdev, 0x0f, 0x03);
	o->cu_mem = f->mem[DT_CH + OBMF_CH_REG_SPECIFIC_CONTROL];
	o->reg_spec_control = f->regs[OBMF_CH_REG_SPECIFIC_CONTROL];

	o->encodes = f->encodes;
	o->decodes = f->decodes;
	o->discovers = f->discovers;
	o->errnos = f->errnos;
	o->ch_regs = f->ch_regs;
}

static void dt_check(struct kunit *test, const struct obmf_dialect *dialect,
		     unsigned int sites)
{
	struct dt_obs *n = kunit_kzalloc(test, sizeof(*n), GFP_KERNEL);
	struct dt_obs *o = kunit_kzalloc(test, sizeof(*o), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, n);
	KUNIT_ASSERT_NOT_NULL(test, o);
	dt_observe(test, NULL, n);
	KUNIT_ASSERT_EQ(test, n->req_ret, 0);
	KUNIT_ASSERT_NE(test, n->err_ret, 0);
	KUNIT_ASSERT_NE(test, n->err_ret, DT_ERRNO);
	KUNIT_ASSERT_GT(test, n->ans_len, OBMF_HDR_LEN);
	KUNIT_ASSERT_GT(test, n->ch0_msgs, 0);
	dt_observe(test, dialect, o);

	/* what a function driver sees is the same at every site */
	KUNIT_EXPECT_EQ(test, o->req_ret, 0);
	KUNIT_EXPECT_EQ(test, o->resp_len, n->resp_len);
	KUNIT_EXPECT_MEMEQ(test, o->resp, n->resp, n->resp_len);
	KUNIT_EXPECT_EQ(test, o->st_ret, 0);
	KUNIT_EXPECT_EQ(test, o->cu_ret, 0);

	KUNIT_ASSERT_EQ(test, o->req_len, n->req_len);
	KUNIT_ASSERT_EQ(test, o->ans_len, n->ans_len);
	if (sites & DT_ENCODE) {
		KUNIT_EXPECT_GT(test, o->encodes, 0);
		dt_xor(o->req, o->req_len);
		dt_xor(o->ans, o->ans_len);
	} else {
		KUNIT_EXPECT_EQ(test, o->encodes, 0);
	}
	KUNIT_EXPECT_MEMEQ(test, o->req, n->req, n->req_len);
	KUNIT_EXPECT_MEMEQ(test, o->ans, n->ans, n->ans_len);

	if (sites & DT_DECODE) {
		KUNIT_EXPECT_GT(test, o->decodes, 0);
		KUNIT_EXPECT_TRUE(test, o->dec_bufs);
	} else {
		KUNIT_EXPECT_EQ(test, o->decodes, 0);
		KUNIT_EXPECT_FALSE(test, o->dec_bufs);
	}

	if (sites & DT_DISCOVER) {
		KUNIT_EXPECT_EQ(test, o->discovers, 1);
		KUNIT_EXPECT_EQ(test, o->ch0_msgs, 0);
	} else {
		KUNIT_EXPECT_EQ(test, o->discovers, 0);
		KUNIT_EXPECT_EQ(test, o->ch0_msgs, n->ch0_msgs);
	}

	if (sites & DT_STATUS_TO_ERRNO) {
		KUNIT_EXPECT_EQ(test, o->err_ret, DT_ERRNO);
		KUNIT_EXPECT_EQ(test, o->errnos, 1);
	} else {
		KUNIT_EXPECT_EQ(test, o->err_ret, n->err_ret);
		KUNIT_EXPECT_EQ(test, o->errnos, 0);
	}

	if (sites & DT_CH_REG) {
		KUNIT_EXPECT_EQ(test, o->ch_regs, 3);
		KUNIT_EXPECT_EQ(test, o->st, 0x33);
		KUNIT_EXPECT_EQ(test, o->spec, 0x44);
		KUNIT_EXPECT_EQ(test, o->reg_spec_control, n->cu_mem);
		KUNIT_EXPECT_EQ(test, o->cu_mem, 0xa5);
	} else {
		KUNIT_EXPECT_EQ(test, o->ch_regs, 0);
		KUNIT_EXPECT_EQ(test, o->st, n->st);
		KUNIT_EXPECT_EQ(test, o->spec, n->spec);
		KUNIT_EXPECT_EQ(test, o->cu_mem, n->cu_mem);
	}

	/* a whole message, its payload cut to the transport's by a header */
	if (sites & DT_CH0_WHOLE_MSGS) {
		KUNIT_EXPECT_EQ(test, o->ch0_resp, DT_MSG - 4);
		KUNIT_EXPECT_EQ(test, o->ch0_cap, DT_MSG);
	} else {
		KUNIT_EXPECT_EQ(test, o->ch0_resp, n->ch0_resp);
		KUNIT_EXPECT_EQ(test, o->ch0_cap, n->ch0_cap);
	}

	/* the header's limits less a 4-byte header, and one more */
	if (sites & DT_KEEP_CH_LIMITS) {
		KUNIT_EXPECT_EQ(test, o->max_req, 996);
		KUNIT_EXPECT_EQ(test, o->max_resp, 996);
		KUNIT_EXPECT_EQ(test, o->cap, 1004);
	} else {
		KUNIT_EXPECT_EQ(test, o->max_req, n->max_req);
		KUNIT_EXPECT_EQ(test, o->max_resp, n->max_resp);
		KUNIT_EXPECT_EQ(test, o->cap, n->cap);
	}
}

static void dt_encode_decode(struct kunit *test)
{
	dt_check(test, &dt_pair, DT_ENCODE | DT_DECODE);
}

/*
 * Discovery reads no device name. Expected, from i2c-obmf: "no device
 * name, label 'i2c'".
 */
static void dt_discover_alone(struct kunit *test)
{
	dt_check(test, &dt_discover_only, DT_DISCOVER);
}

static void dt_status_to_errno_alone(struct kunit *test)
{
	dt_check(test, &dt_status_to_errno_only, DT_STATUS_TO_ERRNO);
}

static void dt_ch_reg_alone(struct kunit *test)
{
	dt_check(test, &dt_ch_reg_only, DT_CH_REG);
}

static void dt_ch0_whole_msgs_alone(struct kunit *test)
{
	dt_check(test, &dt_ch0_whole_msgs_only, DT_CH0_WHOLE_MSGS);
}

/* keep_ch_limits needs encode, so it comes with the pair */
static void dt_keep_ch_limits_with_pair(struct kunit *test)
{
	dt_check(test, &dt_keep_ch_limits_pair,
		 DT_ENCODE | DT_DECODE | DT_KEEP_CH_LIMITS);
}

/*
 * A table with one of encode and decode, with keep_ch_limits but no
 * encode, or with nothing but a name set, is refused and leaves the
 * transport without a dialect; every table of this suite and no table
 * at all are taken. Expected: one WARN for each of the three checks,
 * unless an earlier test has already hit it.
 */
static void dt_attach(struct kunit *test)
{
	static const struct obmf_dialect encode_only = { .encode = dt_encode };
	static const struct obmf_dialect decode_only = { .decode = dt_decode };
	static const struct obmf_dialect keep_only = { .keep_ch_limits = true };
	static const struct obmf_dialect name_only = { .name = "DT" };
	static const struct obmf_dialect empty = {};
	static const struct obmf_dialect *const good[] = {
		&dt_pair,
		&dt_discover_only,
		&dt_status_to_errno_only,
		&dt_ch_reg_only,
		&dt_ch0_whole_msgs_only,
		&dt_keep_ch_limits_pair,
	};
	static const struct obmf_dialect *const bad[] = {
		&encode_only,
		&decode_only,
		&keep_only,
		&name_only,
		&empty,
	};
	struct obmf_usb_proto proto = {};
	struct obmf_transport tp = {};
	int i;

	for (i = 0; i < ARRAY_SIZE(good); i++) {
		proto.dialect = good[i];
		tp.dialect = NULL;
		KUNIT_EXPECT_EQ_MSG(test, obmf_dialect_attach(&tp, &proto), 0,
				    "table %d", i);
		KUNIT_EXPECT_PTR_EQ(test, tp.dialect, good[i]);
	}
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		proto.dialect = bad[i];
		tp.dialect = NULL;
		KUNIT_EXPECT_EQ_MSG(test, obmf_dialect_attach(&tp, &proto),
				    -EINVAL, "table %d", i);
		KUNIT_EXPECT_NULL(test, tp.dialect);
	}
	proto.dialect = NULL;
	tp.dialect = &dt_pair;
	KUNIT_EXPECT_EQ(test, obmf_dialect_attach(&tp, &proto), 0);
	KUNIT_EXPECT_NULL(test, tp.dialect);
}

/* A wire transfer needs the translated receive path; one WARN expected */
static void dt_wire_needs_decode(struct kunit *test)
{
	struct dt_fake *f = dt_bind(test, &dt_status_to_errno_only);
	u8 msg[OBMF_HDR_LEN + 1] = {}, reply[16];
	int sent = f->ch0_msgs;
	size_t len = 0;

	dt_hdr(msg, 0, 0, 1);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_xfer_wire(f->tp.core->ch0, msg, sizeof(msg),
					  reply, sizeof(reply), &len,
					  OBMF_DEFAULT_TIMEOUT_MS),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, f->ch0_msgs, sent);
}

/* The line of a bound device names the dialect's protocol, or OBMF-ICP */
static void dt_info_line(struct kunit *test)
{
	static const struct obmf_dialect named = {
		.name = "DT",
		.encode = dt_encode,
		.decode = dt_decode,
	};
	char buf[OBMF_INFO_LEN];
	struct dt_fake *f;

	f = dt_bind(test, &named);
	obmf_core_info(f->tp.core, buf);
	KUNIT_EXPECT_STREQ(test, buf, "DT 1 device 'dt': I2C ch1 'i2c'");

	f = dt_bind(test, &dt_pair);
	obmf_core_info(f->tp.core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.0 device 'dt': I2C ch1 'i2c'");
}

static struct kunit_case obmf_dialect_cases[] = {
	KUNIT_CASE(dt_encode_decode),
	KUNIT_CASE(dt_discover_alone),
	KUNIT_CASE(dt_status_to_errno_alone),
	KUNIT_CASE(dt_ch_reg_alone),
	KUNIT_CASE(dt_ch0_whole_msgs_alone),
	KUNIT_CASE(dt_keep_ch_limits_with_pair),
	KUNIT_CASE(dt_attach),
	KUNIT_CASE(dt_wire_needs_decode),
	KUNIT_CASE(dt_info_line),
	{}
};

static struct kunit_suite obmf_dialect_suite = {
	.name = "obmf-dialect",
	.test_cases = obmf_dialect_cases,
};

kunit_test_suite(obmf_dialect_suite);

#endif /* CONFIG_OBMF_KUNIT_TEST */
