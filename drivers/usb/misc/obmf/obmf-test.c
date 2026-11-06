// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: KUnit tests of the message layer and of channel
 * discovery, on a fake transport
 */

#include <kunit/device.h>
#include <kunit/fwnode.h>
#include <kunit/of.h>
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/auxiliary_bus.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "obmf-core.h"

#define FAKE_LOG_LEN 256
#define FAKE_KEEP 72
#define FAKE_STEPS 16
#define FAKE_DEFER 8
#define FAKE_MSG 528
#define FAKE_MEM SZ_8K
#define FAKE_MAX_MSG (OBMF_HDR_LEN + 512)
#define FAKE_WAIT_MS 5000

/* OBMF-ICP 1.0.0, 5.3, Table 5 */
#define FAKE_SHORT_READ 0
#define FAKE_SHORT_WRITE 1

/* OBMF-ICP 1.0.0, 5.15, Tables 64 and 65 */
#define FAKE_CH0_DEVICE_ID 0x04
#define FAKE_CH0_NUM 0x26
#define FAKE_CH0_OFFSETS 0x2c
#define FAKE_CH_CONTROL 0x14
#define FAKE_CH_HDR_LEN 0x20
#define FAKE_FIRST_CH 0x100

/* OBMF-ICP 1.0.0, 5.4.1, Table 18 */
#define FAKE_GPIO_INTERRUPT_NOTIFY 4
#define FAKE_GPIO_DATAGRAM 5

/* OBMF-ICP 1.0.0, 5.5.2, Table 23 */
#define FAKE_SERIAL_NACK 0x01

enum fake_ev {
	FAKE_START,
	FAKE_STOP,
	FAKE_QUIESCE,
	FAKE_RESUME,
	FAKE_SEND,
	FAKE_RX,
};

/* One logged event; for messages the first FAKE_KEEP bytes */
struct fake_rec {
	enum fake_ev ev;
	ktime_t t;
	size_t len;
	unsigned int timeout_ms;
	u8 msg[FAKE_KEEP];
};

enum fake_how {
	FAKE_INLINE,
	FAKE_TIMER,
	FAKE_WORK,
	FAKE_NEVER,
};

/**
 * struct fake_step - what the fake does with one request on channels 1-255
 * @send_ret: what send returns
 * @raced: reply inside send even though @send_ret is an error
 * @how: inside send, from an hrtimer, from a work item, or never
 * @delay_ms: delay of a reply from an hrtimer or a work item
 * @count: replies sent, 0 for one; the second differs in its last byte
 * @status: header status of the reply
 * @channel: channel of the reply, -1 for the request's
 * @echo: the reply payload starts with the request's command byte
 * @fill: payload byte k is k ^ 0x5a instead of @payload[k]
 * @plen: payload length after the echo byte
 * @payload: payload after the echo byte
 */
struct fake_step {
	int send_ret;
	bool raced;
	enum fake_how how;
	unsigned int delay_ms;
	unsigned int count;
	u8 status;
	int channel;
	bool echo;
	bool fill;
	size_t plen;
	u8 payload[16];
};

struct obmf_fake;

/* A reply sent later, from an hrtimer (atomic) or a work item (process) */
struct fake_defer {
	struct obmf_fake *f;
	struct hrtimer timer;
	struct delayed_work work;
	bool busy;
	size_t len;
	u8 msg[FAKE_MSG];
};

/* A function driver's device request operation, published by hand */
struct obmf_test_fn {
	struct obmf_fdev fdev;
	atomic_t calls;
	bool hold;
	bool keep;
	struct obmf_dreq *kept;
	struct completion entered;
	struct completion release;
	u8 cmd;
	int answer_ret;
};

struct obmf_fake {
	struct kunit *test;
	struct obmf_transport tp;
	struct obmf_core *core;
	bool attached;
	struct workqueue_struct *callwq;
	spinlock_t lock; /* log, counts, steps, deferred replies, channel 0 */
	wait_queue_head_t wq;
	struct fake_rec log[FAKE_LOG_LEN];
	unsigned int nlog;
	bool nolog;
	unsigned int sends;
	unsigned int quiesced_sends;
	bool stopped;
	bool quiesced;
	int resume_ret;
	unsigned int send_sleep_ms;
	struct fake_step steps[FAKE_STEPS];
	unsigned int nsteps;
	unsigned int step;
	struct fake_step dflt;
	struct fake_defer defer[FAKE_DEFER];

	/* channel 0: the configuration space and its faults */
	u8 *mem;
	size_t mem_len;
	bool head;
	unsigned int reads;
	unsigned int hdr_reads;
	int read_ret;
	unsigned int drop_read;
	bool drop_hdr;
	unsigned int bump_from;
	unsigned int bump_to;
	bool stale_first;
	bool stale_hdr;
	unsigned int ch0_delay_ms;
	unsigned int fatal_hdr;
	bool fatal_armed;

	/* a message the quiesce operation delivers before it returns */
	u8 q_msg[16];
	size_t q_len;

	/* checks run inside the stop operation */
	bool probe_stop;
	struct obmf_channel *probe_ch;
	int stop_state;
	int stop_state_after;
	long stop_fatals;
	int stop_req_ret;
	unsigned int stop_sends_before;
	unsigned int stop_sends_after;
	bool stop_respond;
	int stop_respond_ret;
	int stop_out;

	struct hrtimer fatal_timer;
	int fatal_err;

	struct obmf_test_fn fn;
};

static long ch_cnt(struct obmf_channel *ch, enum obmf_ch_counter c)
{
	return atomic_long_read(&ch->cnt[c]);
}

static long if_cnt(struct obmf_core *core, enum obmf_if_counter c)
{
	return atomic_long_read(&core->cnt[c]);
}

/* Records of @ch held by queued, delivered or kept device requests */
static int dreq_out(struct obmf_channel *ch)
{
	int i, n;

	guard(spinlock_irqsave)
		(&ch->dreq_lock);
	n = ch->queued;
	for (i = 0; i < OBMF_DREQ_RESERVE; i++)
		n += !ch->reserve[i];
	return n;
}

static void fake_log(struct obmf_fake *f, enum fake_ev ev, const u8 *msg,
		     size_t len, unsigned int timeout_ms)
{
	struct fake_rec *r;

	scoped_guard(spinlock_irqsave, &f->lock) {
		if (ev == FAKE_SEND) {
			f->sends++;
			if (f->quiesced)
				f->quiesced_sends++;
		}
		if (!f->nolog && f->nlog < FAKE_LOG_LEN) {
			r = &f->log[f->nlog++];
			r->ev = ev;
			r->t = ktime_get();
			r->len = len;
			r->timeout_ms = timeout_ms;
			if (msg)
				memcpy(r->msg, msg,
				       min_t(size_t, len, FAKE_KEEP));
		}
	}
	wake_up(&f->wq);
}

/* Index of the next record at or after @from, -1 if none */
static int fake_find(struct obmf_fake *f, int from, enum fake_ev ev, int ch,
		     int answer)
{
	struct fake_rec *r;
	int i;

	guard(spinlock_irqsave)
		(&f->lock);
	for (i = max(from, 0); i < f->nlog; i++) {
		r = &f->log[i];
		if (r->ev != ev)
			continue;
		if (ev == FAKE_SEND || ev == FAKE_RX) {
			if (ch >= 0 && r->msg[0] != ch)
				continue;
			if (ev == FAKE_SEND && answer >= 0 &&
			    !!(r->msg[1] & OBMF_HDR_RQRESP) != answer)
				continue;
		}
		return i;
	}
	return -1;
}

static int fake_count(struct obmf_fake *f, enum fake_ev ev, int ch, int answer)
{
	int i = -1, n = 0;

	while ((i = fake_find(f, i + 1, ev, ch, answer)) >= 0)
		n++;
	return n;
}

static unsigned int fake_nlog(struct obmf_fake *f)
{
	guard(spinlock_irqsave)
		(&f->lock);
	return f->nlog;
}

/* Waits until @n messages were sent */
static bool fake_wait_sends(struct obmf_fake *f, unsigned int n)
{
	return wait_event_timeout(f->wq, READ_ONCE(f->sends) >= n,
				  msecs_to_jiffies(FAKE_WAIT_MS));
}

/* Any context; never after the stop operation */
static void fake_rx(struct obmf_fake *f, const u8 *msg, size_t len)
{
	fake_log(f, FAKE_RX, msg, len, 0);
	obmf_core_rx(f->core, msg, len);
}

static enum hrtimer_restart fake_defer_timer(struct hrtimer *t)
{
	struct fake_defer *d = container_of(t, struct fake_defer, timer);

	fake_rx(d->f, d->msg, d->len);
	WRITE_ONCE(d->busy, false);
	wake_up(&d->f->wq);
	return HRTIMER_NORESTART;
}

static void fake_defer_work(struct work_struct *w)
{
	struct fake_defer *d =
		container_of(to_delayed_work(w), struct fake_defer, work);

	fake_rx(d->f, d->msg, d->len);
	WRITE_ONCE(d->busy, false);
	wake_up(&d->f->wq);
}

static bool fake_idle(struct obmf_fake *f)
{
	int i;

	for (i = 0; i < FAKE_DEFER; i++)
		if (READ_ONCE(f->defer[i].busy))
			return false;
	return true;
}

/* Waits until every deferred reply has been delivered */
static bool fake_wait_idle(struct obmf_fake *f)
{
	return wait_event_timeout(f->wq, fake_idle(f),
				  msecs_to_jiffies(FAKE_WAIT_MS));
}

static void fake_deliver(struct obmf_fake *f, enum fake_how how,
			 unsigned int delay_ms, const u8 *msg, size_t len)
{
	struct fake_defer *d = NULL;
	int i;

	if (how == FAKE_NEVER)
		return;
	if (how == FAKE_INLINE) {
		fake_rx(f, msg, len);
		return;
	}
	scoped_guard(spinlock_irqsave, &f->lock) {
		for (i = 0; i < FAKE_DEFER; i++) {
			if (!f->defer[i].busy) {
				d = &f->defer[i];
				d->busy = true;
				break;
			}
		}
	}
	if (!d) {
		KUNIT_FAIL(f->test, "no room for a deferred reply");
		return;
	}
	memcpy(d->msg, msg, len);
	d->len = len;
	if (how == FAKE_TIMER)
		hrtimer_start(&d->timer, ms_to_ktime(delay_ms),
			      HRTIMER_MODE_REL);
	else
		queue_delayed_work(system_dfl_wq, &d->work,
				   msecs_to_jiffies(delay_ms));
}

/* No upcall after this returns */
static void fake_cancel(struct obmf_fake *f)
{
	int i;

	for (i = 0; i < FAKE_DEFER; i++) {
		hrtimer_cancel(&f->defer[i].timer);
		cancel_delayed_work_sync(&f->defer[i].work);
		WRITE_ONCE(f->defer[i].busy, false);
	}
	hrtimer_cancel(&f->fatal_timer);
}

static size_t fake_msg(u8 *out, u8 ch, u8 status, const u8 *payload,
		       size_t plen)
{
	out[0] = ch;
	out[1] = status;
	put_unaligned_le16(plen, out + 2);
	if (plen)
		memcpy(out + OBMF_HDR_LEN, payload, plen);
	return OBMF_HDR_LEN + plen;
}

/* A message from the device, delivered now from the test's context */
static void fake_inject(struct obmf_fake *f, u8 ch, u8 status, const u8 *data,
			size_t len)
{
	u8 msg[FAKE_KEEP];

	fake_rx(f, msg, fake_msg(msg, ch, status, data, len));
}

static int fake_ch0(struct obmf_fake *f, const u8 *msg, size_t len)
{
	static const u8 stale[10] = { 0xee, 0xee, 0xee, 0xee, 0xee,
				      0xee, 0xee, 0xee, 0xee, 0xee };
	const u8 *p = msg + OBMF_HDR_LEN;
	size_t plen = len - OBMF_HDR_LEN, n, dlen;
	unsigned int idx, hidx = 0;
	u8 out[OBMF_HDR_LEN + 2 + 64];
	u8 *body = out + OBMF_HDR_LEN;
	u32 addr;
	bool hdr;

	/* the reserved bytes are 0 */
	KUNIT_EXPECT_GE(f->test, plen, 6);
	KUNIT_EXPECT_EQ(f->test, p[0] & ~7, 0);
	KUNIT_EXPECT_EQ(f->test, p[1], 0);
	addr = get_unaligned_le32(p + 2);

	if (p[0] == FAKE_SHORT_WRITE) {
		dlen = plen - 6;
		scoped_guard(spinlock_irqsave, &f->lock) {
			if (addr + dlen <= f->mem_len)
				memcpy(f->mem + addr, p + 6, dlen);
		}
		fake_deliver(f, f->ch0_delay_ms ? FAKE_WORK : FAKE_INLINE,
			     f->ch0_delay_ms, out,
			     fake_msg(out, 0, OBMF_HDR_RQRESP, NULL, 0));
		return 0;
	}

	KUNIT_EXPECT_EQ(f->test, plen, 8);
	n = get_unaligned_le16(p + 6);
	hdr = !addr;
	scoped_guard(spinlock_irqsave, &f->lock) {
		idx = ++f->reads;
		if (hdr)
			hidx = ++f->hdr_reads;
		if (hdr && f->bump_from && hidx >= f->bump_from &&
		    hidx <= f->bump_to)
			put_unaligned_le16(get_unaligned_le16(f->mem + 4) + 1,
					   f->mem + FAKE_CH0_DEVICE_ID);
	}
	if (idx == 1 && f->read_ret)
		return f->read_ret;
	if (idx == f->drop_read || (hdr && f->drop_hdr))
		return 0;
	if ((f->stale_first && idx == 1) || (f->stale_hdr && hdr))
		fake_rx(f, out,
			fake_msg(out, 0, OBMF_HDR_RQRESP, stale,
				 sizeof(stale)));
	if (n > 64 || addr + n > f->mem_len) {
		fake_rx(f, out,
			fake_msg(out, 0, OBMF_HDR_RQRESP | OBMF_STATUS_INVALID,
				 NULL, 0));
		return 0;
	}

	dlen = 0;
	if (f->head) {
		body[0] = FAKE_SHORT_READ;
		body[1] = 0;
		dlen = 2;
	}
	scoped_guard(spinlock_irqsave, &f->lock)
		memcpy(body + dlen, f->mem + addr, n);
	out[0] = 0;
	out[1] = OBMF_HDR_RQRESP;
	put_unaligned_le16(dlen + n, out + 2);
	if (hdr && hidx == f->fatal_hdr && f->fatal_armed)
		obmf_core_fatal(f->core, -EIO);
	fake_deliver(f, f->ch0_delay_ms ? FAKE_WORK : FAKE_INLINE,
		     f->ch0_delay_ms, out, OBMF_HDR_LEN + dlen + n);
	if (hdr && hidx == f->fatal_hdr && !f->fatal_armed)
		obmf_core_fatal(f->core, -EIO);
	return 0;
}

static int fake_reply_step(struct obmf_fake *f, const u8 *msg)
{
	struct fake_step s;
	u8 out[FAKE_MSG];
	size_t k, plen;
	unsigned int i;

	scoped_guard(spinlock_irqsave, &f->lock) {
		if (f->step < f->nsteps)
			s = f->steps[f->step++];
		else
			s = f->dflt;
	}
	if (s.send_ret && !s.raced)
		return s.send_ret;

	plen = s.plen + s.echo;
	if (WARN_ON(OBMF_HDR_LEN + plen > sizeof(out)))
		return -EINVAL;
	out[0] = s.channel >= 0 ? s.channel : msg[0];
	out[1] = OBMF_HDR_RQRESP | s.status;
	put_unaligned_le16(plen, out + 2);
	if (s.echo)
		out[OBMF_HDR_LEN] = msg[OBMF_HDR_LEN];
	for (k = 0; k < s.plen; k++)
		out[OBMF_HDR_LEN + s.echo + k] =
			s.fill ? (u8)(k ^ 0x5a) :
				 s.payload[min_t(size_t, k, 15)];
	for (i = 0; i < max(s.count, 1U); i++) {
		if (i && plen)
			out[OBMF_HDR_LEN + plen - 1] ^= 0xff;
		fake_deliver(f, s.how, s.delay_ms, out, OBMF_HDR_LEN + plen);
	}
	return s.send_ret;
}

static void fake_push(struct obmf_fake *f, const struct fake_step *s)
{
	guard(spinlock_irqsave)
		(&f->lock);
	if (f->nsteps < FAKE_STEPS)
		f->steps[f->nsteps++] = *s;
}

static struct obmf_fake *to_fake(struct obmf_transport *tp)
{
	return container_of(tp, struct obmf_fake, tp);
}

static int fake_start(struct obmf_transport *tp)
{
	struct obmf_fake *f = to_fake(tp);

	fake_log(f, FAKE_START, NULL, 0, 0);
	return 0;
}

static void fake_stop(struct obmf_transport *tp)
{
	struct obmf_fake *f = to_fake(tp);
	struct obmf_core *core = tp->core;
	size_t rlen;
	u8 resp[4];

	fake_log(f, FAKE_STOP, NULL, 0, 0);
	if (f->probe_stop) {
		f->stop_state = obmf_core_state(core);
		obmf_core_fatal(core, -EIO);
		f->stop_state_after = obmf_core_state(core);
		f->stop_fatals = if_cnt(core, OBMF_IF_FATAL_ERRORS);
		f->stop_sends_before = READ_ONCE(f->sends);
		f->stop_req_ret = obmf_ch_request(f->probe_ch, 0x01, NULL, 0,
						  resp, sizeof(resp), &rlen,
						  20);
		f->stop_sends_after = READ_ONCE(f->sends);
	}
	if (f->stop_respond) {
		f->stop_respond_ret = obmf_respond(&f->fn.fdev, f->fn.kept,
						   OBMF_STATUS_SUCCESS,
						   NULL, 0);
		f->stop_out = dreq_out(f->fn.fdev.ch);
	}
	fake_cancel(f);
	WRITE_ONCE(f->stopped, true);
}

static void fake_quiesce(struct obmf_transport *tp)
{
	struct obmf_fake *f = to_fake(tp);

	fake_log(f, FAKE_QUIESCE, NULL, 0, 0);
	if (f->q_len) {
		fake_rx(f, f->q_msg, f->q_len);
		f->q_len = 0;
	}
	fake_cancel(f);
	WRITE_ONCE(f->quiesced, true);
}

static int fake_resume(struct obmf_transport *tp)
{
	struct obmf_fake *f = to_fake(tp);

	fake_log(f, FAKE_RESUME, NULL, 0, 0);
	if (f->resume_ret)
		return f->resume_ret;
	WRITE_ONCE(f->quiesced, false);
	return 0;
}

static int fake_send(struct obmf_transport *tp, const void *buf, size_t len,
		     unsigned int timeout_ms)
{
	struct obmf_fake *f = to_fake(tp);
	const u8 *msg = buf;

	if (READ_ONCE(f->stopped)) {
		KUNIT_FAIL(f->test, "send after stop");
		return -ENODEV;
	}
	/* A request carries its command byte; an answer may be empty */
	KUNIT_EXPECT_GE(f->test, len,
			OBMF_HDR_LEN + !(msg[1] & OBMF_HDR_RQRESP));
	KUNIT_EXPECT_LE(f->test, len, tp->max_tx_msg);
	KUNIT_EXPECT_EQ(f->test, get_unaligned_le16(msg + 2),
			len - OBMF_HDR_LEN);
	fake_log(f, FAKE_SEND, msg, len, timeout_ms);
	if (f->send_sleep_ms)
		msleep(f->send_sleep_ms);
	if (READ_ONCE(f->quiesced))
		return -EAGAIN;
	if (msg[1] & OBMF_HDR_RQRESP)
		return 0;
	if (!msg[0])
		return fake_ch0(f, msg, len);
	return fake_reply_step(f, msg);
}

static const struct obmf_transport_ops fake_ops = {
	.start = fake_start,
	.stop = fake_stop,
	.quiesce = fake_quiesce,
	.resume = fake_resume,
	.send = fake_send,
};

static enum hrtimer_restart fake_fatal_timer(struct hrtimer *t)
{
	struct obmf_fake *f = container_of(t, struct obmf_fake, fatal_timer);

	obmf_core_fatal(f->core, f->fatal_err);
	return HRTIMER_NORESTART;
}

static void fake_fatal_in(struct obmf_fake *f, int err, unsigned int ms)
{
	f->fatal_err = err;
	hrtimer_start(&f->fatal_timer, ms_to_ktime(ms), HRTIMER_MODE_REL);
}

static void fake_cancel_action(void *data)
{
	fake_cancel(data);
}

KUNIT_DEFINE_ACTION_WRAPPER(obmf_test_destroy_wq, destroy_workqueue,
			    struct workqueue_struct *);

static atomic_t obmf_test_ids = ATOMIC_INIT(0);

static struct obmf_fake *fake_create(struct kunit *test, size_t max_msg,
				     unsigned int floor_ms)
{
	struct obmf_fake *f;
	char name[24];
	int i;

	f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f);
	f->test = test;
	f->mem = kunit_kzalloc(test, FAKE_MEM, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->mem);
	f->mem_len = FAKE_MEM;
	f->attached = true;
	spin_lock_init(&f->lock);
	init_waitqueue_head(&f->wq);
	for (i = 0; i < FAKE_DEFER; i++) {
		f->defer[i].f = f;
		hrtimer_setup(&f->defer[i].timer, fake_defer_timer,
			      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		INIT_DELAYED_WORK(&f->defer[i].work, fake_defer_work);
	}
	hrtimer_setup(&f->fatal_timer, fake_fatal_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);
	init_completion(&f->fn.entered);
	init_completion(&f->fn.release);
	f->dflt = (struct fake_step){
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 4,
		.payload = { 0xa1, 0xa2, 0xa3, 0xa4 },
	};

	snprintf(name, sizeof(name), "obmf-test.%d",
		 atomic_inc_return(&obmf_test_ids));
	f->tp.dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->tp.dev);
	f->tp.ops = &fake_ops;
	f->tp.max_tx_msg = max_msg;
	f->tp.max_rx_msg = max_msg;
	f->tp.resp_timeout_floor_ms = floor_ms;

	f->callwq = alloc_workqueue("obmf-test", WQ_UNBOUND, 0);
	KUNIT_ASSERT_NOT_NULL(test, f->callwq);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, obmf_test_destroy_wq,
						  f->callwq),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, fake_cancel_action, f),
			0);
	return f;
}

/* test_chan.off: the header bits the layout leaves clear */
#define FAKE_OFF_ENABLE BIT(0)
#define FAKE_OFF_VALID BIT(1)

struct test_chan {
	u8 type;
	u8 role;
	const char *name;
	u16 limit;
	u8 off;
	u32 config_len;
};

static u32 fake_ch_off(struct obmf_fake *f, int nr)
{
	return get_unaligned_le32(f->mem + FAKE_CH0_OFFSETS + 4 * (nr - 1));
}

/* Channel 0's configuration space for @n channels, numbered from 1 */
static void fake_layout(struct obmf_fake *f, const struct test_chan *c, int n)
{
	u32 off = FAKE_FIRST_CH, k;
	u8 *m = f->mem, *h;
	int i;

	memset(m, 0, f->mem_len);
	m[0] = 0x00;
	m[1] = 0x01;
	put_unaligned_le16(0x1234, m + 2);
	put_unaligned_le16(0x5678, m + FAKE_CH0_DEVICE_ID);
	memcpy(m + 6, "obmf-test", 9);
	m[FAKE_CH0_NUM] = n;
	for (i = 0; i < n; i++) {
		if (WARN_ON(off + FAKE_CH_HDR_LEN + c[i].config_len >
			    f->mem_len))
			return;
		put_unaligned_le32(off, m + FAKE_CH0_OFFSETS + 4 * i);
		h = m + off;
		h[0] = c[i].type;
		h[1] = i + 1;
		if (c[i].name)
			memcpy(h + 2, c[i].name, strlen(c[i].name));
		h[0x12] = c[i].role;
		/* HEALTH_STATUS 1 (OK) */
		h[OBMF_CH_REG_STATUS] = 0x01;
		if (!(c[i].off & FAKE_OFF_VALID))
			h[OBMF_CH_REG_STATUS] |= OBMF_CH_STATUS_CONFIG_VALID;
		if (!(c[i].off & FAKE_OFF_ENABLE))
			h[FAKE_CH_CONTROL] = OBMF_CH_CONTROL_ENABLE;
		put_unaligned_le16(c[i].limit, h + 0x17);
		put_unaligned_le16(c[i].limit, h + 0x19);
		put_unaligned_le32(c[i].config_len, h + 0x1c);
		for (k = 0; k < c[i].config_len; k++)
			h[FAKE_CH_HDR_LEN + k] = (u8)(i * 7 + k);
		off += ALIGN(FAKE_CH_HDR_LEN + c[i].config_len, 16);
	}
}

/* A configuration space of @len bytes instead of the default */
static void fake_mem(struct obmf_fake *f, size_t len)
{
	f->mem = kunit_kzalloc(f->test, len, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(f->test, f->mem);
	f->mem_len = len;
}

static void fake_unbind(struct obmf_fake *f)
{
	struct obmf_core *core = f->core;

	if (!core)
		return;
	obmf_core_unbind(core, f->attached);
	f->core = NULL;
}

static void fake_unbind_action(void *data)
{
	fake_unbind(data);
}

static int fake_bind(struct obmf_fake *f)
{
	struct kunit *test = f->test;

	f->core = obmf_core_create(&f->tp);
	KUNIT_ASSERT_NOT_NULL(test, f->core);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, fake_unbind_action, f),
			0);
	return obmf_core_bind(f->core);
}

/* The channels the message layer cases use */
static const struct test_chan core_chans[] = {
	{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c", 516 },
	{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio", 516 },
	{ OBMF_TYPE_IPMI, OBMF_ROLE_PRODUCER, "ipmi", 516 },
	{ OBMF_TYPE_SPI, OBMF_ROLE_PRODUCER, "spi", 516 },
	{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart", 516 },
	{ OBMF_TYPE_TPM, OBMF_ROLE_PRODUCER, "tpm", 516 },
	{ OBMF_TYPE_MMIO, OBMF_ROLE_PRODUCER, "mmio", 516 },
	{ OBMF_TYPE_I3C, OBMF_ROLE_PRODUCER, "i3c", 516 },
	{ OBMF_TYPE_IO, OBMF_ROLE_PRODUCER, "io", 516 },
	{ OBMF_TYPE_MEMDEV, OBMF_ROLE_PRODUCER, "memdev", 516 },
};

#define CH_I2C 1
#define CH_GPIO 2
#define CH_IPMI 3
#define CH_SPI 4
#define CH_SERIAL 5
#define CH_TPM 6
#define CH_MMIO 7
#define CH_I3C 8
#define CH_IO 9
#define CH_MEMDEV 10

static struct obmf_fake *core_setup(struct kunit *test, unsigned int floor_ms)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, floor_ms);

	fake_layout(f, core_chans, ARRAY_SIZE(core_chans));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_ASSERT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
	return f;
}

static struct obmf_channel *chan(struct obmf_fake *f, int nr)
{
	struct obmf_channel *ch = f->core->chans[nr];

	KUNIT_ASSERT_NOT_NULL(f->test, ch);
	return ch;
}

/* A request run from a work item, so the test can act while it waits */
struct test_call {
	struct work_struct work;
	struct obmf_fake *f;
	int (*fn)(struct test_call *c);
	struct obmf_channel *ch;
	u8 cmd;
	unsigned int timeout_ms;
	u8 resp[64];
	size_t resp_len;
	int ret;
	ktime_t start;
	ktime_t end;
	struct completion done;
};

static int call_request(struct test_call *c)
{
	return obmf_ch_request(c->ch, c->cmd, NULL, 0, c->resp, sizeof(c->resp),
			       &c->resp_len, c->timeout_ms);
}

static int call_quiesce(struct test_call *c)
{
	return obmf_core_quiesce(c->f->core);
}

static void call_work(struct work_struct *w)
{
	struct test_call *c = container_of(w, struct test_call, work);

	c->start = ktime_get();
	c->ret = c->fn(c);
	c->end = ktime_get();
	complete(&c->done);
}

static void call_flush(void *data)
{
	struct test_call *c = data;

	flush_work(&c->work);
}

static struct test_call *call_start(struct obmf_fake *f,
				    int (*fn)(struct test_call *c),
				    struct obmf_channel *ch, u8 cmd,
				    unsigned int timeout_ms)
{
	struct kunit *test = f->test;
	struct test_call *c;

	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c);
	c->f = f;
	c->fn = fn;
	c->ch = ch;
	c->cmd = cmd;
	c->timeout_ms = timeout_ms;
	init_completion(&c->done);
	INIT_WORK(&c->work, call_work);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, call_flush, c),
			0);
	queue_work(f->callwq, &c->work);
	return c;
}

static bool test_wait(struct completion *done)
{
	return wait_for_completion_timeout(done,
					   msecs_to_jiffies(FAKE_WAIT_MS));
}

static bool call_wait(struct test_call *c)
{
	return test_wait(&c->done);
}

static int test_fn_request(struct obmf_fdev *fdev, struct obmf_dreq *dreq)
{
	struct obmf_test_fn *fn = container_of(fdev, struct obmf_test_fn, fdev);

	atomic_inc(&fn->calls);
	fn->cmd = dreq->cmd;
	if (fn->hold) {
		complete(&fn->entered);
		wait_for_completion_timeout(&fn->release,
					    msecs_to_jiffies(FAKE_WAIT_MS));
	}
	if (fn->keep) {
		fn->kept = dreq;
		return -EINPROGRESS;
	}
	if (dreq->answer_due)
		fn->answer_ret =
			obmf_respond(fdev, dreq, OBMF_STATUS_SUCCESS, NULL, 0);
	return 0;
}

static const struct obmf_fdev_ops test_fn_ops = {
	.device_request = test_fn_request,
};

/* What devm_obmf_publish() sets, without an auxiliary driver */
static struct obmf_test_fn *test_fn_publish(struct obmf_fake *f,
					    struct obmf_channel *ch)
{
	struct obmf_test_fn *fn = &f->fn;

	fn->fdev.ch = ch;
	fn->fdev.info = &ch->info;
	guard(mutex)
		(&ch->dispatch_lock);
	ch->ops = &test_fn_ops;
	ch->publisher = &fn->fdev;
	return fn;
}

/* A GPIO Interrupt Notify from the device: due an answer */
static size_t gpio_notify(u8 *msg, u8 ch)
{
	static const u8 body[2] = { FAKE_GPIO_INTERRUPT_NOTIFY, 0x01 };

	return fake_msg(msg, ch, 0, body, sizeof(body));
}

static int send_index(struct obmf_fake *f, int ch)
{
	int i = -1, last = -1;

	while ((i = fake_find(f, i + 1, FAKE_SEND, ch, 0)) >= 0)
		last = i;
	return last;
}

/* a request's header and payload as sent, and the response taken */
static void obmf_test_request_encode(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	static const u8 req[3] = { 0x11, 0x22, 0x33 };
	struct fake_rec *r;
	size_t rlen, k;
	u8 *resp;
	int i;

	resp = kunit_kzalloc(test, 300, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, resp);
	f->dflt.fill = true;
	f->dflt.plen = 299;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x05, req, sizeof(req), resp, 300,
					&rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, rlen, 299);
	for (k = 0; k < rlen; k++)
		if (resp[k] != (u8)(k ^ 0x5a)) {
			KUNIT_FAIL(test, "response byte %zu is %#x", k,
				   resp[k]);
			break;
		}

	i = send_index(f, CH_GPIO);
	KUNIT_ASSERT_GE(test, i, 0);
	r = &f->log[i];
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 4);
	KUNIT_EXPECT_EQ(test, r->msg[0], CH_GPIO);
	KUNIT_EXPECT_EQ(test, r->msg[1], 0);
	KUNIT_EXPECT_EQ(test, r->msg[2], 4);
	KUNIT_EXPECT_EQ(test, r->msg[3], 0);
	KUNIT_EXPECT_EQ(test, r->msg[4], 0x05);
	KUNIT_EXPECT_MEMEQ(test, &r->msg[5], req, sizeof(req));

	i = fake_find(f, i, FAKE_RX, CH_GPIO, -1);
	KUNIT_ASSERT_GE(test, i, 0);
	r = &f->log[i];
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP);
	KUNIT_EXPECT_EQ(test, r->msg[2], 300 & 0xff);
	KUNIT_EXPECT_EQ(test, r->msg[3], 300 >> 8);

	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_REQUESTS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), 0);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_RX_MALFORMED), 0);
}

/* request and response lengths against the channel's and the caller's */
static void obmf_test_request_lengths(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	u8 *req, resp[8];
	unsigned int sends;
	size_t rlen;

	req = kunit_kzalloc(test, 512, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	KUNIT_EXPECT_EQ(test, ch->info.max_req, 512);

	sends = f->sends;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, req, 512, resp, 4, &rlen, 0),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, f->sends, sends);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_REQUESTS), 0);
	KUNIT_EXPECT_FALSE(test, ch->slot.armed);

	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, req, 511, resp, 4, &rlen, 0), 0);
	KUNIT_EXPECT_EQ(test, f->log[send_index(f, CH_GPIO)].len,
			OBMF_HDR_LEN + 512);

	/* longer than the caller's buffer */
	f->dflt.plen = 8;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, 4, &rlen, 0),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), 1);

	/* a fixed-length response, shorter */
	f->dflt.plen = 2;
	KUNIT_EXPECT_EQ(test, obmf_ch_request(ch, 1, NULL, 0, resp, 4, NULL, 0),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), 2);

	f->dflt.plen = 4;
	KUNIT_EXPECT_EQ(test, obmf_ch_request(ch, 1, NULL, 0, resp, 4, NULL, 0),
			0);

	/* longer than the channel accepts: dropped by the receive path */
	f->dflt.plen = 516;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, 4, &rlen, 0),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_REQUESTS), 5);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 5);
}

struct echo_vec {
	int ch;
	u8 flip;
	int ret;
};

/* echo masks, I2C [6:0], GPIO, IPMI, SPI [3:0], others none */
static void obmf_test_echo_masks(struct kunit *test)
{
	static const struct echo_vec vecs[] = {
		{ CH_I2C, 0, 0 },
		{ CH_I2C, BIT(0), -EPROTO },
		{ CH_I2C, BIT(6), -EPROTO },
		{ CH_I2C, BIT(7), 0 },
		{ CH_GPIO, BIT(3), -EPROTO },
		{ CH_GPIO, BIT(4), 0 },
		{ CH_IPMI, BIT(0), -EPROTO },
		{ CH_IPMI, BIT(7), 0 },
		{ CH_SPI, BIT(2), -EPROTO },
		{ CH_SPI, BIT(5), 0 },
		{ CH_SERIAL, BIT(0), 0 },
		{ CH_SERIAL, 0xff, 0 },
		{ CH_TPM, BIT(1), 0 },
	};
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch;
	const u8 cmd = 0x35;
	struct fake_step s;
	long perr;
	size_t rlen;
	u8 resp[8];
	bool echo;
	int i;

	for (i = 0; i < ARRAY_SIZE(vecs); i++) {
		ch = chan(f, vecs[i].ch);
		echo = vecs[i].ch != CH_SERIAL && vecs[i].ch != CH_TPM;
		s = (struct fake_step){
			.how = FAKE_INLINE,
			.channel = -1,
			.plen = 4,
			.payload = { cmd ^ vecs[i].flip, 0xb1, 0xb2, 0xb3 },
		};
		fake_push(f, &s);
		perr = ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS);
		memset(resp, 0, sizeof(resp));
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_ch_request(ch, cmd, NULL, 0, resp,
						    sizeof(resp), &rlen, 0),
				    vecs[i].ret, "vector %d", i);
		KUNIT_EXPECT_EQ_MSG(test,
				    ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS) - perr,
				    vecs[i].ret ? 1 : 0, "vector %d", i);
		if (vecs[i].ret)
			continue;
		if (echo) {
			KUNIT_EXPECT_EQ(test, rlen, 3);
			KUNIT_EXPECT_EQ(test, resp[0], 0xb1);
		} else {
			KUNIT_EXPECT_EQ(test, rlen, 4);
			KUNIT_EXPECT_EQ(test, resp[0], cmd ^ vecs[i].flip);
		}
	}

	/* an echoing type with an empty response */
	ch = chan(f, CH_GPIO);
	s = (struct fake_step){ .how = FAKE_INLINE, .channel = -1 };
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, cmd, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			-EPROTO);
}

struct mmio_vec {
	bool write;
	size_t plen;
	u8 b0;
	int ret;
};

/* MMIO responses with and without the head */
static void obmf_test_mmio_forms(struct kunit *test)
{
	static const struct mmio_vec vecs[] = {
		{ false, 6, FAKE_SHORT_READ, 0 },
		{ false, 6, FAKE_SHORT_READ | 0xf8, 0 },
		{ false, 4, 0xd1, 0 },
		{ false, 6, FAKE_SHORT_WRITE, -EPROTO },
		{ false, 5, FAKE_SHORT_READ, -EPROTO },
		{ false, 3, FAKE_SHORT_READ, -EPROTO },
		{ true, 0, 0, 0 },
		{ true, 2, FAKE_SHORT_WRITE, 0 },
		{ true, 2, FAKE_SHORT_READ, -EPROTO },
		{ true, 1, FAKE_SHORT_WRITE, -EPROTO },
	};
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_MMIO);
	static const u8 wreq[6] = { 0, 0x10, 0, 0, 0, 0x55 };
	static const u8 rreq[7] = { 0, 0x10, 0, 0, 0, 4, 0 };
	struct fake_step s;
	u8 resp[4];
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(vecs); i++) {
		s = (struct fake_step){
			.how = FAKE_INLINE,
			.channel = -1,
			.plen = vecs[i].plen,
			.payload = { vecs[i].b0, 0, 0xd1, 0xd2, 0xd3, 0xd4 },
		};
		if (vecs[i].plen == 4)
			memcpy(s.payload, "\xd1\xd2\xd3\xd4", 4);
		fake_push(f, &s);
		memset(resp, 0, sizeof(resp));
		if (vecs[i].write)
			ret = obmf_ch_request(ch, FAKE_SHORT_WRITE, wreq,
					      sizeof(wreq), NULL, 0, NULL, 0);
		else
			ret = obmf_ch_request(ch, FAKE_SHORT_READ, rreq,
					      sizeof(rreq), resp, sizeof(resp),
					      NULL, 0);
		KUNIT_EXPECT_EQ_MSG(test, ret, vecs[i].ret, "vector %d", i);
		if (!ret && !vecs[i].write)
			KUNIT_EXPECT_MEMEQ(test, resp, "\xd1\xd2\xd3\xd4", 4);
	}
}

/* ICP Table 4 and the per-type tables, each status with its errno */
static int test_status_errno(u8 type, u8 st)
{
	static const int generic[] = { 0,    -EIO,    -ETIMEDOUT, -EBUSY,
				       -EIO, -ECHRNG, -EMSGSIZE };

	if (st < ARRAY_SIZE(generic))
		return generic[st];
	if (st < 0x40)
		return -EIO;
	switch (type) {
	case OBMF_TYPE_I2C_CONTROLLER:
		switch (st) {
		case 0x40:
			return -ETIMEDOUT;
		case 0x41:
			return -EAGAIN;
		case 0x42:
			return -EBUSY;
		case 0x43:
			return -ENXIO;
		}
		break;
	case OBMF_TYPE_SERIAL:
		if (st == 0x40)
			return -ETIMEDOUT;
		break;
	case OBMF_TYPE_CONFIG:
	case OBMF_TYPE_MMIO:
		if (st == 0x40)
			return -EINVAL;
		if (st == 0x41)
			return -EACCES;
		break;
	case OBMF_TYPE_GPIO:
		if (st == 0x40)
			return -EINVAL;
		if (st == 0x41)
			return -EOPNOTSUPP;
		if (st == 0x42)
			return -EPERM;
		break;
	case OBMF_TYPE_I3C:
		switch (st) {
		case 0x40:
			return -ETIMEDOUT;
		case 0x41:
			return -EAGAIN;
		case 0x42:
			return -EBUSY;
		case 0x46:
			return -EPROTO;
		}
		break;
	case OBMF_TYPE_IO:
		if (st == 0x40)
			return -EINVAL;
		if (st == 0x41)
			return -EACCES;
		break;
	case OBMF_TYPE_MEMDEV:
		switch (st) {
		case 0x40:
			return -EACCES;
		case 0x41:
		case 0x42:
			return -EINVAL;
		case 0x43:
			return -EROFS;
		}
		break;
	}
	return -EIO;
}

static void status_sweep(struct kunit *test, struct obmf_fake *f, int nr)
{
	struct obmf_channel *ch = chan(f, nr);
	bool mmio = ch->info.type == OBMF_TYPE_MMIO;
	long errs = ch_cnt(ch, OBMF_CH_STATUS_ERRORS);
	size_t rlen, cap = mmio ? 4 : 8;
	u8 resp[8];
	int st, ret;

	f->nolog = true;
	for (st = 1; st <= OBMF_HDR_STATUS; st++) {
		f->dflt.status = st;
		ret = obmf_ch_request(ch, 0x01, NULL, 0, resp, cap,
				      mmio ? NULL : &rlen, 0);
		KUNIT_EXPECT_LT_MSG(test, ret, 0, "type %#x status %#x",
				    ch->info.type, st);
		KUNIT_EXPECT_EQ_MSG(test, ret,
				    test_status_errno(ch->info.type, st),
				    "type %#x status %#x", ch->info.type, st);
	}
	f->dflt.status = 0;
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_STATUS_ERRORS) - errs,
			(long)OBMF_HDR_STATUS);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), 0);
	f->nolog = false;
}

/* every status is an errno, never 0 */
static void obmf_test_status_errno(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);

	status_sweep(test, f, CH_I2C);
	status_sweep(test, f, CH_IPMI);
	status_sweep(test, f, CH_SPI);
	status_sweep(test, f, CH_SERIAL);
	status_sweep(test, f, CH_TPM);
	status_sweep(test, f, CH_MMIO);
}

/*
 * The channel-specific codes of GPIO, I3C, I/O and memory device channels
 */
static void obmf_test_status_types(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);

	status_sweep(test, f, CH_GPIO);
	status_sweep(test, f, CH_I3C);
	status_sweep(test, f, CH_IO);
	status_sweep(test, f, CH_MEMDEV);
}

/* the wait the core passes to send */
static void obmf_test_wait_clamp(struct kunit *test)
{
	static const unsigned int vecs[][3] = {
		/* floor, timeout_ms, wait */
		{ 0, 0, OBMF_DEFAULT_TIMEOUT_MS },
		{ 0, 20, 20 },
		{ 0, OBMF_MAX_TIMEOUT_MS, OBMF_MAX_TIMEOUT_MS },
		{ 0, OBMF_MAX_TIMEOUT_MS + 1, OBMF_MAX_TIMEOUT_MS },
		{ 1000, 20, 1000 },
		{ 1000, 0, 1000 },
		{ 1000, 5000, 5000 },
		{ 1000, OBMF_MAX_TIMEOUT_MS + 1, OBMF_MAX_TIMEOUT_MS },
	};
	struct obmf_fake *f[2], *fk;
	struct obmf_channel *ch;
	struct fake_rec *r;
	size_t rlen;
	u8 resp[8];
	int i;

	f[0] = core_setup(test, 0);
	f[1] = core_setup(test, 1000);
	for (i = 0; i < ARRAY_SIZE(vecs); i++) {
		fk = f[vecs[i][0] ? 1 : 0];
		ch = chan(fk, CH_GPIO);
		KUNIT_EXPECT_EQ(test,
				obmf_ch_request(ch, 1, NULL, 0, resp,
						sizeof(resp), &rlen,
						vecs[i][1]),
				0);
		r = &fk->log[send_index(fk, CH_GPIO)];
		KUNIT_EXPECT_EQ_MSG(test, r->timeout_ms, vecs[i][2],
				    "vector %d", i);
	}
}

/* a host-side timeout, one send, and send time counts */
static void obmf_test_wait_timeout(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	size_t rlen;
	u8 resp[8];
	s64 ms;
	ktime_t t;

	fake_push(f, &s);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 50),
			-ETIMEDOUT);
	ms = ktime_ms_delta(ktime_get(), t);
	KUNIT_EXPECT_GE(test, ms, 50);
	KUNIT_EXPECT_LT(test, ms, 1000);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 0), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);

	/* one deadline from the send: 200 ms in send, 250 ms wait */
	fake_push(f, &s);
	f->send_sleep_ms = 200;
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 250),
			-ETIMEDOUT);
	ms = ktime_ms_delta(ktime_get(), t);
	f->send_sleep_ms = 0;
	KUNIT_EXPECT_GE(test, ms, 250);
	KUNIT_EXPECT_LT(test, ms, 440);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 0), 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 2);
}

/* a 1000 ms floor under a 20 ms request */
static void obmf_test_wait_floor(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 1000);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	size_t rlen;
	u8 resp[8];
	ktime_t t;

	fake_push(f, &s);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 20),
			-ETIMEDOUT);
	/* the deadline is in ticks, from within one */
	KUNIT_EXPECT_GE(test, ktime_ms_delta(ktime_get(), t),
			1000 - jiffies_to_msecs(1));
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 1);
}

/* replies from atomic and process context */
static void obmf_test_reply_contexts(struct kunit *test)
{
	static const enum fake_how hows[] = { FAKE_TIMER, FAKE_WORK };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_IPMI);
	struct fake_step s;
	size_t rlen;
	u8 resp[8];
	int i;

	for (i = 0; i < ARRAY_SIZE(hows); i++) {
		s = (struct fake_step){
			.how = hows[i],
			.delay_ms = 10,
			.channel = -1,
			.echo = true,
			.plen = 2,
			.payload = { 0xc1 + i, 0xc2 },
		};
		fake_push(f, &s);
		KUNIT_EXPECT_EQ(test,
				obmf_ch_request(ch, 0x21, NULL, 0, resp,
						sizeof(resp), &rlen, 500),
				0);
		KUNIT_EXPECT_EQ(test, rlen, 2);
		KUNIT_EXPECT_EQ(test, resp[0], 0xc1 + i);
	}
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 0);
}

/* the first reply wins, a duplicate is counted late */
static void obmf_test_first_reply_wins(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.count = 2,
		.channel = -1,
		.echo = true,
		.plen = 2,
		.payload = { 0x01, 0x02 },
	};
	size_t rlen;
	u8 resp[8];

	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, rlen, 2);
	KUNIT_EXPECT_EQ(test, resp[1], 0x02);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 1);

	/* twice from an hrtimer, at the same expiry */
	s.how = FAKE_TIMER;
	s.delay_ms = 10;
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 500),
			0);
	KUNIT_ASSERT_TRUE(test, fake_wait_idle(f));
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 2);
}

/* a reply after the timeout is late, not taken */
static void obmf_test_late_reply(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = {
		.how = FAKE_TIMER,
		.delay_ms = 150,
		.channel = -1,
		.echo = true,
		.plen = 1,
		.payload = { 0x77 },
	};
	size_t rlen;
	u8 resp[8];

	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 50),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 1);
	KUNIT_ASSERT_TRUE(test, fake_wait_idle(f));
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 0);

	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, rlen, 4);
	KUNIT_EXPECT_EQ(test, resp[0], 0xa1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 0), 2);
}

/* replies for another channel, and in either order */
static void obmf_test_reply_other_channel(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *gpio = chan(f, CH_GPIO);
	struct obmf_channel *spi = chan(f, CH_SPI);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = CH_SPI,
		.echo = true,
		.plen = 1,
	};
	struct test_call *a, *b;
	unsigned int sends;
	size_t rlen;
	u8 resp[8];

	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(gpio, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 30),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ch_cnt(gpio, OBMF_CH_TIMEOUTS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(spi, OBMF_CH_RX_LATE), 1);

	s.channel = 200;
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(gpio, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 30),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_RX_NO_CHANNEL), 1);

	/* two channels in flight, answered in reverse order */
	s = (struct fake_step){ .how = FAKE_NEVER, .channel = -1 };
	fake_push(f, &s);
	fake_push(f, &s);
	sends = f->sends;
	a = call_start(f, call_request, gpio, 0x02, 2000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	b = call_start(f, call_request, spi, 0x03, 2000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 2));
	fake_inject(f, CH_SPI, OBMF_HDR_RQRESP, (u8[]){ 0x03, 0x33 }, 2);
	fake_inject(f, CH_GPIO, OBMF_HDR_RQRESP, (u8[]){ 0x02, 0x22 }, 2);
	KUNIT_ASSERT_TRUE(test, call_wait(a));
	KUNIT_ASSERT_TRUE(test, call_wait(b));
	KUNIT_EXPECT_EQ(test, a->ret, 0);
	KUNIT_EXPECT_EQ(test, a->resp[0], 0x22);
	KUNIT_EXPECT_EQ(test, b->ret, 0);
	KUNIT_EXPECT_EQ(test, b->resp[0], 0x33);
}

/*
 * A reply at the moment of expiry is either the request's reply or late,
 * decided under the slot's rx_lock, and counted once.
 */
static void obmf_test_reply_at_expiry(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = {
		.how = FAKE_TIMER,
		.delay_ms = 20,
		.channel = -1,
		.echo = true,
		.plen = 1,
	};
	int i, ret, ok = 0, to = 0;
	size_t rlen;
	u8 resp[8];

	for (i = 0; i < FAKE_STEPS; i++)
		fake_push(f, &s);
	for (i = 0; i < FAKE_STEPS; i++) {
		ret = obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp), &rlen,
				      20);
		if (!ret)
			ok++;
		else if (ret == -ETIMEDOUT)
			to++;
		else
			KUNIT_FAIL(test, "request %d: %d", i, ret);
	}
	KUNIT_ASSERT_TRUE(test, fake_wait_idle(f));
	KUNIT_EXPECT_EQ(test, ok + to, FAKE_STEPS);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), ok);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), to);
	KUNIT_EXPECT_EQ(test,
			ch_cnt(ch, OBMF_CH_RX_RESPONSES) +
				ch_cnt(ch, OBMF_CH_RX_LATE),
			FAKE_STEPS);
}

/* a completion left by an earlier race cannot end a request */
static void obmf_test_stale_completion(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	size_t rlen;
	u8 resp[8];
	ktime_t t;

	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	complete(&ch->slot.done);
	complete(&ch->slot.done);
	fake_push(f, &s);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 30),
			-ETIMEDOUT);
	KUNIT_EXPECT_GE(test, ktime_ms_delta(ktime_get(), t), 30);
}

/*
 * A send error after arming returns the send's errno and disarms the slot; a
 * reply raced into it is taken and its data dropped, counted as the code
 * does.
 */
static void obmf_test_send_errors(struct kunit *test)
{
	static const int errs[] = { -EPIPE, -ENOMEM, -ENODEV, -EMSGSIZE,
				    -EAGAIN };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s;
	size_t rlen;
	u8 resp[8];
	int i;

	for (i = 0; i < ARRAY_SIZE(errs); i++) {
		s = (struct fake_step){ .send_ret = errs[i] };
		fake_push(f, &s);
		KUNIT_EXPECT_EQ(test,
				obmf_ch_request(ch, 1, NULL, 0, resp,
						sizeof(resp), &rlen, 0),
				errs[i]);
		KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_ERRORS), i + 1);
		KUNIT_EXPECT_FALSE(test, ch->slot.armed);
		KUNIT_EXPECT_EQ(test,
				obmf_ch_request(ch, 1, NULL, 0, resp,
						sizeof(resp), &rlen, 0),
				0);
	}
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);

	s = (struct fake_step){
		.send_ret = -EPIPE,
		.raced = true,
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 1,
		.payload = { 0x99 },
	};
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			-EPIPE);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES),
			ARRAY_SIZE(errs) + 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 0);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, resp[0], 0xa1);
}

/* what the receive path drops or keeps */
static void obmf_test_rx_malformed(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct obmf_core *core = f->core;
	struct test_call *c;
	unsigned int sends;
	u8 msg[16];

	fake_push(f, &s);
	sends = f->sends;
	c = call_start(f, call_request, ch, 0x06, 2000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));

	/* shorter than a header */
	fake_rx(f, (u8[]){ CH_GPIO, OBMF_HDR_RQRESP, 0 }, 3);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_MALFORMED), 1);

	/* a size beyond the message */
	fake_msg(msg, CH_GPIO, OBMF_HDR_RQRESP, (u8[]){ 0x06, 1 }, 2);
	put_unaligned_le16(10, msg + 2);
	fake_rx(f, msg, OBMF_HDR_LEN + 2);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_MALFORMED), 2);

	/* RqResp 0: a device request, not the reply */
	fake_rx(f, msg, fake_msg(msg, CH_GPIO, 0, (u8[]){ 0x07, 1 }, 2));
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DEVICE_REQUESTS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 0);
	KUNIT_EXPECT_FALSE(test, completion_done(&c->done));

	/* three bytes after the size: counted, taken with the size */
	fake_msg(msg, CH_GPIO, OBMF_HDR_RQRESP,
		 (u8[]){ 0x06, 0x61, 0x62, 0xe1, 0xe2, 0xe3 }, 6);
	put_unaligned_le16(3, msg + 2);
	fake_rx(f, msg, OBMF_HDR_LEN + 6);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, 0);
	KUNIT_EXPECT_EQ(test, c->resp_len, 2);
	KUNIT_EXPECT_EQ(test, c->resp[1], 0x62);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_MALFORMED), 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_RESPONSES), 1);
}

/* rx_no_channel before the channel table is published */
static void obmf_test_rx_before_publish(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct obmf_core *core;

	f->core = obmf_core_create(&f->tp);
	KUNIT_ASSERT_NOT_NULL(test, f->core);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, fake_unbind_action, f),
			0);
	core = f->core;
	fake_inject(f, 1, OBMF_HDR_RQRESP, (u8[]){ 1 }, 1);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_NO_CHANNEL), 1);
	fake_inject(f, 0, OBMF_HDR_RQRESP, NULL, 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(core->ch0, OBMF_CH_RX_LATE), 1);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_MESSAGES), 2);
}

/* a late reply passing echo and length is the next one's */
static void obmf_test_late_reply_limit(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	size_t rlen;
	u8 resp[8];

	fake_push(f, &s);
	s = (struct fake_step){
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 1,
		.payload = { 0x41 },
	};
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 30),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 30),
			0);
	KUNIT_EXPECT_EQ(test, resp[0], 0x41);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 0);
}

/* a fatal error from atomic context */
static void obmf_test_fatal(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct test_call *c;
	unsigned int sends;
	size_t rlen;
	u8 resp[8];

	fake_push(f, &s);
	sends = f->sends;
	c = call_start(f, call_request, ch, 1, 5000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	fake_fatal_in(f, -EPROTO, 10);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, -EIO);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(c->end, c->start), 1000);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_FATAL_ERRORS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 0);

	fake_fatal_in(f, -EPROTO, 1);
	msleep(20);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_FATAL_ERRORS), 1);

	sends = f->sends;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			-EIO);
	KUNIT_EXPECT_EQ(test, f->sends, sends);
}

/*
 * On unplug a waiting request ends -ENODEV; once stopped every request fails
 * -ENODEV with nothing sent, and a fatal error does not move a stopped core.
 */
static void obmf_test_stopped(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct test_call *c;
	unsigned int sends;

	fake_push(f, &s);
	sends = f->sends;
	c = call_start(f, call_request, ch, 1, 5000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	f->probe_stop = true;
	f->probe_ch = chan(f, CH_SPI);
	f->attached = false;
	fake_unbind(f);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, -ENODEV);
	KUNIT_EXPECT_EQ(test, f->stop_state, OBMF_STOPPED);
	KUNIT_EXPECT_EQ(test, f->stop_state_after, OBMF_STOPPED);
	KUNIT_EXPECT_EQ(test, f->stop_fatals, 0);
	KUNIT_EXPECT_EQ(test, f->stop_req_ret, -ENODEV);
	KUNIT_EXPECT_EQ(test, f->stop_sends_after, f->stop_sends_before);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_STOP, -1, -1), 1);
}

/*
 * A request armed at quiesce ends with its reply, which comes after quiesce
 * was called and before the transport quiesces.
 */
static void obmf_test_quiesce_armed_reply(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = {
		.how = FAKE_WORK,
		.delay_ms = 200,
		.channel = -1,
		.echo = true,
		.plen = 1,
		.payload = { 0x5e },
	};
	struct test_call *c;
	unsigned int sends;
	int q, rx, from;
	size_t rlen;
	u8 resp[8];

	fake_push(f, &s);
	sends = f->sends;
	c = call_start(f, call_request, ch, 1, 2000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	from = fake_nlog(f);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, 0);
	KUNIT_EXPECT_EQ(test, c->resp[0], 0x5e);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_QUIESCED);
	rx = fake_find(f, from, FAKE_RX, CH_GPIO, -1);
	q = fake_find(f, from, FAKE_QUIESCE, -1, -1);
	KUNIT_EXPECT_GE(test, rx, 0);
	KUNIT_EXPECT_GT(test, q, rx);

	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
}

/* a request armed at quiesce ends with its timeout */
static void obmf_test_quiesce_armed_timeout(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct test_call *c;
	unsigned int sends;
	int i, q;

	fake_push(f, &s);
	sends = f->sends;
	c = call_start(f, call_request, ch, 1, 200);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	i = send_index(f, CH_GPIO);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 1);
	q = fake_find(f, i, FAKE_QUIESCE, -1, -1);
	KUNIT_ASSERT_GT(test, q, i);
	KUNIT_EXPECT_GE(test, ktime_ms_delta(f->log[q].t, f->log[i].t), 200);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_QUIESCED);
}

/* -EAGAIN while quiesced, nothing armed or sent */
static void obmf_test_quiesce_new_request(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	unsigned int sends;
	size_t rlen;
	u8 resp[8];

	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	sends = f->sends;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			-EAGAIN);
	KUNIT_EXPECT_EQ(test, obmf_ctrl_read(f->core, 0, resp, 4), -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->sends, sends);
	KUNIT_EXPECT_FALSE(test, ch->slot.armed);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_REQUESTS), 0);
	KUNIT_EXPECT_EQ(test, f->quiesced_sends, 0);

	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), 0);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
}

/*
 * A device request that arrives as the transport quiesces stays queued, and
 * is delivered once after resume with its answer sent.
 */
static void obmf_test_quiesce_dreq_held(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	struct fake_rec *r;
	int res, a;

	f->q_len = gpio_notify(f->q_msg, CH_GPIO);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	msleep(50);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DEVICE_REQUESTS), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 0);

	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), 0);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), 1);
	KUNIT_EXPECT_EQ(test, fn->cmd, FAKE_GPIO_INTERRUPT_NOTIFY);
	KUNIT_EXPECT_EQ(test, fn->answer_ret, 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 1);
	res = fake_find(f, 0, FAKE_RESUME, -1, -1);
	a = fake_find(f, 0, FAKE_SEND, CH_GPIO, 1);
	KUNIT_ASSERT_GE(test, a, 0);
	KUNIT_EXPECT_GT(test, a, res);
	r = &f->log[a];
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, r->msg[4], FAKE_GPIO_INTERRUPT_NOTIFY);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 0);
	KUNIT_EXPECT_EQ(test, f->quiesced_sends, 0);
}

/*
 * A delivery in progress at quiesce finishes, and its answer goes out,
 * before the core quiesces.
 */
static void obmf_test_quiesce_delivery(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	struct test_call *c;
	int q, a;
	u8 msg[16];

	fn->hold = true;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	KUNIT_ASSERT_TRUE(test, test_wait(&fn->entered));
	c = call_start(f, call_quiesce, NULL, 0, 0);
	msleep(100);
	KUNIT_EXPECT_FALSE(test, completion_done(&c->done));
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_QUIESCE, -1, -1), 0);
	complete(&fn->release);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, 0);
	KUNIT_EXPECT_EQ(test, fn->answer_ret, 0);
	a = fake_find(f, 0, FAKE_SEND, CH_GPIO, 1);
	q = fake_find(f, 0, FAKE_QUIESCE, -1, -1);
	KUNIT_ASSERT_GE(test, a, 0);
	KUNIT_EXPECT_GT(test, q, a);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), 0);
}

/*
 * A failed resume leaves the core failed with its delivery works disabled:
 * nothing queued is delivered or answered. The records are counted
 * rx_dropped when the channel is freed, after its debugfs files are gone, so
 * that count is not read here.
 */
static void obmf_test_resume_fails(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	unsigned int sends;
	bool queued;
	size_t rlen;
	u8 resp[8];

	f->q_len = gpio_notify(f->q_msg, CH_GPIO);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	f->resume_ret = -EPROTO;
	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), -EPROTO);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_FATAL_ERRORS), 1);

	sends = f->sends;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			-EIO);
	msleep(50);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, f->sends, sends);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 0);
	scoped_guard(spinlock_irqsave, &ch->dreq_lock)
		queued = !list_empty(&ch->queue);
	KUNIT_EXPECT_TRUE(test, queued);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 0);
}

static int test_slow_probe(struct auxiliary_device *adev,
			   const struct auxiliary_device_id *id)
{
	return 0;
}

static void test_slow_remove(struct auxiliary_device *adev)
{
	msleep(1000);
}

static const struct auxiliary_device_id test_slow_ids[] = {
	{ .name = "obmf.i2c" },
	{}
};

static struct auxiliary_driver test_slow_driver = {
	.name = "test-slow",
	.probe = test_slow_probe,
	.remove = test_slow_remove,
	.id_table = test_slow_ids,
};

KUNIT_DEFINE_ACTION_WRAPPER(test_slow_unregister, auxiliary_driver_unregister,
			    struct auxiliary_driver *);

/*
 * A request timing out while a function driver's remove holds the core in
 * UNBINDING stops the core; a request still waiting ends -ENODEV, and the
 * state ends STOPPED
 */
static void obmf_test_unbind_timeout(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct test_call *quick, *slow;
	unsigned int sends;

	KUNIT_ASSERT_EQ(test, auxiliary_driver_register(&test_slow_driver), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, test_slow_unregister,
						  &test_slow_driver),
			0);
	KUNIT_ASSERT_NOT_NULL(test, chan(f, CH_I2C)->fdev->adev.dev.driver);

	fake_push(f, &s);
	fake_push(f, &s);
	sends = f->sends;
	quick = call_start(f, call_request, chan(f, CH_GPIO), 1, 100);
	slow = call_start(f, call_request, chan(f, CH_SPI), 1, 5000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 2));
	f->probe_stop = true;
	f->probe_ch = chan(f, CH_IPMI);
	fake_unbind(f);

	KUNIT_ASSERT_TRUE(test, call_wait(quick));
	KUNIT_ASSERT_TRUE(test, call_wait(slow));
	KUNIT_EXPECT_EQ(test, quick->ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, slow->ret, -ENODEV);
	/* Woken by the stop at the timeout, not at the end of remove */
	KUNIT_EXPECT_LT(test, ktime_ms_delta(slow->end, slow->start), 500);
	KUNIT_EXPECT_EQ(test, f->stop_state, OBMF_STOPPED);
	KUNIT_EXPECT_EQ(test, f->stop_state_after, OBMF_STOPPED);
	KUNIT_EXPECT_EQ(test, f->stop_fatals, 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_STOP, -1, -1), 1);
}

/* The last answer sent on channel @nr */
static struct fake_rec *last_answer(struct obmf_fake *f, int nr)
{
	int i = -1, last = -1;

	while ((i = fake_find(f, i + 1, FAKE_SEND, nr, 1)) >= 0)
		last = i;
	KUNIT_ASSERT_GE(f->test, last, 0);
	return &f->log[last];
}

/*
 * Device requests due an answer while the delivery is held take the reserve,
 * then the queue, then are owed; each is answered once, the owed one with
 * the default answer
 */
static void obmf_test_dreq_flood(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	int n = OBMF_DREQ_RESERVE + OBMF_DREQ_QUEUE_MAX + 1, i;
	unsigned int queued, owed;
	u8 msg[16];

	fn->hold = true;
	for (i = 0; i < n; i++)
		fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	KUNIT_ASSERT_TRUE(test, test_wait(&fn->entered));
	scoped_guard(spinlock_irqsave, &ch->dreq_lock) {
		queued = ch->queued;
		owed = ch->owed;
	}
	KUNIT_EXPECT_EQ(test, dreq_out(ch), n - 1);
	KUNIT_EXPECT_EQ(test, queued, OBMF_DREQ_QUEUE_MAX);
	KUNIT_EXPECT_EQ(test, owed, 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 0);

	fn->hold = false;
	complete(&fn->release);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), n - 1);
	KUNIT_EXPECT_EQ(test, fn->answer_ret, 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), n);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_ANSWERS), n);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
	KUNIT_EXPECT_EQ(test, ch->owed, 0);
}

/*
 * GPIO datagrams, due no answer, take the queue, then the reserve, then are
 * dropped; none is answered
 */
static void obmf_test_dreq_datagrams(struct kunit *test)
{
	static const u8 dgram[] = { FAKE_GPIO_DATAGRAM, 0x01 };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	int n = OBMF_DREQ_RESERVE + OBMF_DREQ_QUEUE_MAX + 1, i;
	unsigned int queued, owed;

	fn->hold = true;
	for (i = 0; i < n; i++) {
		/* The reserve is kept for requests due an answer */
		if (i == OBMF_DREQ_QUEUE_MAX) {
			scoped_guard(spinlock_irqsave, &ch->dreq_lock)
				queued = ch->queued;
			KUNIT_EXPECT_EQ(test, queued, OBMF_DREQ_QUEUE_MAX);
		}
		fake_inject(f, CH_GPIO, 0, dgram, sizeof(dgram));
	}
	KUNIT_ASSERT_TRUE(test, test_wait(&fn->entered));
	scoped_guard(spinlock_irqsave, &ch->dreq_lock) {
		queued = ch->queued;
		owed = ch->owed;
	}
	KUNIT_EXPECT_EQ(test, dreq_out(ch), n - 1);
	KUNIT_EXPECT_EQ(test, queued, OBMF_DREQ_QUEUE_MAX);
	KUNIT_EXPECT_EQ(test, owed, 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 1);

	fn->hold = false;
	complete(&fn->release);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), n - 1);
	KUNIT_EXPECT_EQ(test, fn->cmd, FAKE_GPIO_DATAGRAM);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 0);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
}

/*
 * The default answers with no driver bound: a serial write is refused with
 * nothing taken, a GPIO interrupt acknowledged, anything else answered busy
 */
static void obmf_test_default_answers(struct kunit *test)
{
	static const u8 write[] = { 0x00, 0x02, 0x00, 'o', 'k' };
	static const u8 notify[] = { FAKE_GPIO_INTERRUPT_NOTIFY, 0x01 };
	static const u8 other[] = { 0x01, 0x00 };
	struct obmf_fake *f = core_setup(test, 0);
	struct fake_rec *r;

	fake_inject(f, CH_SERIAL, 0, write, sizeof(write));
	fake_inject(f, CH_GPIO, 0, notify, sizeof(notify));
	fake_inject(f, CH_I2C, 0, other, sizeof(other));
	fake_inject(f, CH_TPM, 0, other, sizeof(other));
	flush_workqueue(f->core->wq);

	r = last_answer(f, CH_SERIAL);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 3);
	KUNIT_EXPECT_EQ(test, r->msg[4], FAKE_SERIAL_NACK);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(r->msg + 5), 0);
	r = last_answer(f, CH_GPIO);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 1);
	KUNIT_EXPECT_EQ(test, r->msg[4], FAKE_GPIO_INTERRUPT_NOTIFY);
	r = last_answer(f, CH_I2C);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_BUSY);
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 1);
	KUNIT_EXPECT_EQ(test, r->msg[4], 0x01);
	r = last_answer(f, CH_TPM);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_BUSY);
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN);

	fake_inject(f, CH_GPIO, 0, other, sizeof(other));
	flush_workqueue(f->core->wq);
	r = last_answer(f, CH_GPIO);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_BUSY);
	KUNIT_EXPECT_EQ(test, r->msg[4], 0x01);

	KUNIT_EXPECT_EQ(test,
			ch_cnt(chan(f, CH_SERIAL), OBMF_CH_DEFAULT_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, CH_GPIO), OBMF_CH_DEFAULT_ANSWERS),
			2);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, CH_I2C), OBMF_CH_DEFAULT_ANSWERS),
			1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, CH_TPM), OBMF_CH_DEFAULT_ANSWERS),
			1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_SERIAL, 1), 1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_TPM, 1), 1);
}

/*
 * A kept channel is on from discovery, so the device may send before a
 * driver publishes its ops: the request gets the default answer whatever
 * the channel's role
 */
static void obmf_test_default_answer_roles(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio-0", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_CONSUMER, "gpio-1", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_SYMMETRIC, "gpio-2", 516 },
	};
	static const u8 notify[] = { FAKE_GPIO_INTERRUPT_NOTIFY, 0x01 };
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct fake_rec *r;
	int i;

	fake_layout(f, c, ARRAY_SIZE(c));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	for (i = 1; i <= ARRAY_SIZE(c); i++)
		fake_inject(f, i, 0, notify, sizeof(notify));
	flush_workqueue(f->core->wq);
	for (i = 1; i <= ARRAY_SIZE(c); i++) {
		KUNIT_EXPECT_NULL(test, chan(f, i)->ops);
		r = last_answer(f, i);
		KUNIT_EXPECT_EQ(test, r->msg[4], FAKE_GPIO_INTERRUPT_NOTIFY);
		KUNIT_EXPECT_EQ_MSG(test,
				    ch_cnt(chan(f, i), OBMF_CH_DEFAULT_ANSWERS),
				    1, "channel %d", i);
	}
}

/*
 * obmf_respond() on a request due no answer fails -EINVAL, and once the
 * transport has stopped -ENODEV; obmf_dreq_drop() counts the request
 * dropped. Each ends the request and sends nothing.
 */
static void obmf_test_respond_edges(struct kunit *test)
{
	static const u8 dgram[] = { FAKE_GPIO_DATAGRAM, 0x01 };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	u8 msg[16];

	fn->keep = true;
	fake_inject(f, CH_GPIO, 0, dgram, sizeof(dgram));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_NOT_NULL(test, fn->kept);
	KUNIT_EXPECT_FALSE(test, fn->kept->answer_due);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 1);
	KUNIT_EXPECT_EQ(test,
			obmf_respond(&fn->fdev, fn->kept, OBMF_STATUS_SUCCESS,
				     NULL, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);

	fn->kept = NULL;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_NOT_NULL(test, fn->kept);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 1);
	obmf_dreq_drop(&fn->fdev, fn->kept);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 1);

	fn->kept = NULL;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_NOT_NULL(test, fn->kept);
	f->stop_respond = true;
	fake_unbind(f);
	KUNIT_EXPECT_EQ(test, f->stop_respond_ret, -ENODEV);
	KUNIT_EXPECT_EQ(test, f->stop_out, 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 0);
}

static struct kunit_case obmf_core_cases[] = {
	KUNIT_CASE(obmf_test_request_encode),
	KUNIT_CASE(obmf_test_request_lengths),
	KUNIT_CASE(obmf_test_echo_masks),
	KUNIT_CASE(obmf_test_mmio_forms),
	KUNIT_CASE(obmf_test_status_errno),
	KUNIT_CASE(obmf_test_status_types),
	KUNIT_CASE(obmf_test_wait_clamp),
	KUNIT_CASE(obmf_test_wait_timeout),
	KUNIT_CASE_SLOW(obmf_test_wait_floor),
	KUNIT_CASE(obmf_test_reply_contexts),
	KUNIT_CASE(obmf_test_first_reply_wins),
	KUNIT_CASE(obmf_test_late_reply),
	KUNIT_CASE(obmf_test_reply_other_channel),
	KUNIT_CASE(obmf_test_reply_at_expiry),
	KUNIT_CASE(obmf_test_stale_completion),
	KUNIT_CASE(obmf_test_send_errors),
	KUNIT_CASE(obmf_test_rx_malformed),
	KUNIT_CASE(obmf_test_rx_before_publish),
	KUNIT_CASE(obmf_test_late_reply_limit),
	KUNIT_CASE(obmf_test_fatal),
	KUNIT_CASE(obmf_test_stopped),
	KUNIT_CASE(obmf_test_quiesce_armed_reply),
	KUNIT_CASE(obmf_test_quiesce_armed_timeout),
	KUNIT_CASE(obmf_test_quiesce_new_request),
	KUNIT_CASE(obmf_test_quiesce_dreq_held),
	KUNIT_CASE(obmf_test_quiesce_delivery),
	KUNIT_CASE(obmf_test_resume_fails),
	KUNIT_CASE(obmf_test_unbind_timeout),
	KUNIT_CASE(obmf_test_dreq_flood),
	KUNIT_CASE(obmf_test_dreq_datagrams),
	KUNIT_CASE(obmf_test_default_answers),
	KUNIT_CASE(obmf_test_default_answer_roles),
	KUNIT_CASE(obmf_test_respond_edges),
	{}
};

static struct kunit_suite obmf_core_suite = {
	.name = "obmf-core",
	.test_cases = obmf_core_cases,
};

/* Discovery */

static const struct test_chan disc_chans[] = {
	{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-0", 72 },
	{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart\xc3\xa9", 516, 0, 100 },
	{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, NULL, 516 },
};

static int disc_hdr_send(struct obmf_fake *f, int from)
{
	struct fake_rec *r;
	int i = from - 1;

	while ((i = fake_find(f, i + 1, FAKE_SEND, 0, 0)) >= 0) {
		r = &f->log[i];
		if (r->msg[4] == FAKE_SHORT_READ &&
		    !get_unaligned_le32(r->msg + 6))
			return i;
	}
	return -1;
}

/* The channel 0 reads that start at @addr */
static int disc_reads_at(struct obmf_fake *f, u32 addr)
{
	struct fake_rec *r;
	int i = -1, n = 0;

	while ((i = fake_find(f, i + 1, FAKE_SEND, 0, 0)) >= 0) {
		r = &f->log[i];
		n += r->msg[4] == FAKE_SHORT_READ &&
		     get_unaligned_le32(r->msg + 6) == addr;
	}
	return n;
}

/* Milliseconds between the header reads that start passes @a and @b */
static s64 disc_gap(struct obmf_fake *f, int a, int b)
{
	int i = -1, n = 0, ia = -1, ib = -1;

	while ((i = disc_hdr_send(f, i + 1)) >= 0) {
		if (n == a)
			ia = i;
		if (n == b)
			ib = i;
		n++;
	}
	if (ia < 0 || ib < 0)
		return -1;
	return ktime_ms_delta(f->log[ib].t, f->log[ia].t);
}

static void disc_check(struct kunit *test, struct obmf_fake *f)
{
	struct obmf_core *core = f->core;
	struct obmf_channel *ch;
	struct fake_rec *r;
	u32 cfg;
	int i;

	KUNIT_EXPECT_EQ(test, core->version_major, 1);
	KUNIT_EXPECT_EQ(test, core->version_minor, 0);
	KUNIT_EXPECT_EQ(test, core->vendor_id, 0x1234);
	KUNIT_EXPECT_EQ(test, core->device_id, 0x5678);
	KUNIT_EXPECT_STREQ(test, core->device_name, "obmf-test");

	ch = chan(f, 1);
	KUNIT_EXPECT_EQ(test, ch->info.type, OBMF_TYPE_I2C_CONTROLLER);
	KUNIT_EXPECT_STREQ(test, ch->info.name, "i2c-0");
	KUNIT_EXPECT_EQ(test, ch->info.max_req,
			min_t(size_t, 72, f->tp.max_tx_msg) - OBMF_HDR_LEN);
	ch = chan(f, 2);
	KUNIT_EXPECT_EQ(test, ch->info.type, OBMF_TYPE_SERIAL);
	KUNIT_EXPECT_STREQ(test, ch->info.name, "uart\xc3\xa9");
	KUNIT_EXPECT_EQ(test, ch->info.config_len, 100);
	KUNIT_EXPECT_MEMEQ(test, ch->info.config,
			   f->mem + fake_ch_off(f, 2) + FAKE_CH_HDR_LEN, 100);
	ch = chan(f, 3);
	KUNIT_EXPECT_STREQ(test, ch->info.name, "");
	KUNIT_EXPECT_NULL(test, core->chans[4]);

	/* reads cut to channel 0's chunk, at increasing addresses */
	cfg = fake_ch_off(f, 2) + FAKE_CH_HDR_LEN;
	i = -1;
	while ((i = fake_find(f, i + 1, FAKE_SEND, 0, 0)) >= 0) {
		r = &f->log[i];
		KUNIT_EXPECT_LE(test, get_unaligned_le16(r->msg + 10),
				core->ch0_chunk);
		if (get_unaligned_le32(r->msg + 6) == cfg)
			KUNIT_EXPECT_EQ(test, get_unaligned_le16(r->msg + 10),
					64);
		if (get_unaligned_le32(r->msg + 6) == cfg + 64)
			KUNIT_EXPECT_EQ(test, get_unaligned_le16(r->msg + 10),
					36);
	}
}

/*
 * Discovery, in both forms of the channel 0 short read response and on the
 * smallest link (4 + 64)
 */
static void obmf_test_discover(struct kunit *test)
{
	struct obmf_fake *f;

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	disc_check(test, f);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_PROTOCOL_ERRORS), 0);

	f = fake_create(test, OBMF_HDR_LEN + 64, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->head = true;
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	disc_check(test, f);
	KUNIT_EXPECT_EQ(test, f->core->ch0_chunk, 64);
	KUNIT_EXPECT_EQ(test, f->core->mmio_form, OBMF_MMIO_FORM_HEAD);
}

/*
 * the status bytes come from channel 0; an update keeps the other bits, in
 * one short write of the byte
 */
static void obmf_test_ch_regs(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct obmf_fdev *fdev;
	u8 status, specific;
	struct fake_rec *r;
	u8 *h;
	int i;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	fdev = chan(f, 1)->fdev;
	KUNIT_ASSERT_NOT_NULL(test, fdev);
	h = f->mem + fake_ch_off(f, 1);

	h[OBMF_CH_REG_STATUS] = 0x81;
	h[OBMF_CH_REG_SPECIFIC_STATUS] = 0x42;
	KUNIT_EXPECT_EQ(test, obmf_ch_status_read(fdev, &status, &specific), 0);
	KUNIT_EXPECT_EQ(test, status, 0x81);
	KUNIT_EXPECT_EQ(test, specific, 0x42);

	h[OBMF_CH_REG_SPECIFIC_CONTROL] = 0xa5;
	KUNIT_EXPECT_EQ(test, obmf_ch_control_update(fdev, 0x0f, 0x03), 0);
	KUNIT_EXPECT_EQ(test, h[OBMF_CH_REG_SPECIFIC_CONTROL], 0xa3);
	i = send_index(f, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	r = &f->log[i];
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 7);
	KUNIT_EXPECT_EQ(test, r->msg[4], FAKE_SHORT_WRITE);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(r->msg + 6),
			fake_ch_off(f, 1) + OBMF_CH_REG_SPECIFIC_CONTROL);
	KUNIT_EXPECT_EQ(test, r->msg[10], 0xa3);
}

/*
 * An unsupported major version fails at once; a newer minor version binds
 */
static void obmf_test_discover_version(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->mem[1] = 0x02;
	KUNIT_EXPECT_EQ(test, fake_bind(f), -ENODEV);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 1);
	KUNIT_EXPECT_EQ(test, f->reads, 1);

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->mem[0] = 0x07;
	KUNIT_EXPECT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->core->version_minor, 7);
}

static const struct {
	const char *name;
	const char *want;
} disc_names[] = {
	{ "uart\xc3\xa9", "uart\xc3\xa9" },
	{ "\xe2\x82\xac\xf0\x9f\x98\x80", "\xe2\x82\xac\xf0\x9f\x98\x80" },
	{ "abcdefghijklmnop", "abcdefghijklmnop" },
	{ "a\001b", "" },
	{ "a\x7f", "" },
	{ "ab\xc3", "" },
	{ "\xe2\x28\xa1", "" },
	{ "\xc0\xaf", "" },
	{ "\xe0\x80\xaf", "" },
	{ "\xf0\x80\x80\xaf", "" },
	{ "\xed\xa0\x80", "" },
	{ "\xf4\x90\x80\x80", "" },
	{ "\xf8\x88\x80\x80\x80", "" },
	{ "ab", "ab" },
};

/*
 * A name is valid UTF-8 without control characters up to its first NUL, or
 * no name; overlong forms, surrogates and code points past U+10FFFF are not
 * valid UTF-8
 */
static void obmf_test_discover_names(struct kunit *test)
{
	struct test_chan c[ARRAY_SIZE(disc_names)];
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	int i, last = ARRAY_SIZE(disc_names);

	for (i = 0; i < last; i++)
		c[i] = (struct test_chan){ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER,
					   disc_names[i].name, 516 };
	fake_layout(f, c, last);
	/* Bytes after the first NUL do not count */
	f->mem[fake_ch_off(f, last) + 2 + 3] = 0xff;
	memcpy(f->mem + 6, "0123456789abcdef0123456789abcdef", 32);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	for (i = 0; i < last; i++)
		KUNIT_EXPECT_STREQ_MSG(test, chan(f, i + 1)->info.name,
				       disc_names[i].want, "channel %d", i + 1);
	KUNIT_EXPECT_STREQ(test, f->core->device_name,
			   "0123456789abcdef0123456789abcdef");

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, c, 1);
	memcpy(f->mem + 6, "obmf\xe0\x80\xaf", 7);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_STREQ(test, f->core->device_name, "");
}

/*
 * A channel whose header gives another number, lies over channel 0's header,
 * overlaps a channel found before it, or has limits below a message header
 * and its type's head is absent; a zero offset is skipped; the other
 * channels bind
 */
static void obmf_test_discover_checks(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "number", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "in-hdr", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "host", 516, 0, 64 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "overlap", 516 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart-6",
		  OBMF_HDR_LEN + 2 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart-7",
		  OBMF_HDR_LEN + 3 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-7",
		  OBMF_HDR_LEN + 3 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-8",
		  OBMF_HDR_LEN + 4 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio-4", OBMF_HDR_LEN },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio-5",
		  OBMF_HDR_LEN + 1 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "zero", 516 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "resp-6", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "last", 516 },
	};
	static const bool bound[] = { false, false, true, false, false,
				      true,  false, true, false, true,
				      false, false, true };
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	u8 *m = f->mem;
	u32 off;
	int i;

	fake_layout(f, c, ARRAY_SIZE(c));
	m[fake_ch_off(f, 1) + 1] = 9;
	/* Over the reserved bytes and the offsets table, numbered 2 */
	m[0x2b] = 2;
	put_unaligned_le32(0x2a, m + FAKE_CH0_OFFSETS + 4);
	/* Channel 4's header inside channel 3's configuration */
	off = fake_ch_off(f, 3) + FAKE_CH_HDR_LEN + 16;
	memcpy(m + off, m + fake_ch_off(f, 4), FAKE_CH_HDR_LEN);
	put_unaligned_le32(off, m + FAKE_CH0_OFFSETS + 4 * 3);
	put_unaligned_le32(0, m + FAKE_CH0_OFFSETS + 4 * 10);
	put_unaligned_le16(OBMF_HDR_LEN + 2, m + fake_ch_off(f, 12) + 0x19);

	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
	for (i = 0; i < ARRAY_SIZE(c); i++) {
		if (!bound[i]) {
			KUNIT_EXPECT_NULL_MSG(test, f->core->chans[i + 1],
					      "channel %d", i + 1);
			continue;
		}
		KUNIT_EXPECT_STREQ(test, chan(f, i + 1)->info.name, c[i].name);
		KUNIT_EXPECT_EQ(test, chan(f, i + 1)->info.max_req,
				c[i].limit - OBMF_HDR_LEN);
	}
}

/*
 * A channel with CHANNEL_ENABLE or CONFIGURATION_VALID clear, or both, is
 * left out after its header is read, before its configuration; the other
 * channels bind. Expected: the warnings "channel 2: not enabled, ignored",
 * "channel 3: configuration not valid, ignored" and "channel 4: not
 * enabled, configuration not valid, ignored".
 */
static void obmf_test_discover_unusable(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c", 516 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "off", 516,
		  FAKE_OFF_ENABLE, 64 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "invalid", 516,
		  FAKE_OFF_VALID, 64 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "both", 516,
		  FAKE_OFF_ENABLE | FAKE_OFF_VALID, 64 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart", 516, 0, 64 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	u32 off;
	int i;

	fake_layout(f, c, ARRAY_SIZE(c));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	for (i = 2; i <= 4; i++) {
		off = fake_ch_off(f, i);
		KUNIT_EXPECT_NULL_MSG(test, f->core->chans[i], "channel %d", i);
		KUNIT_EXPECT_EQ_MSG(test, disc_reads_at(f, off), 1,
				    "channel %d", i);
		KUNIT_EXPECT_EQ_MSG(test,
				    disc_reads_at(f, off + FAKE_CH_HDR_LEN), 0,
				    "channel %d", i);
	}
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 1)->fdev);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 5)->fdev);
	KUNIT_EXPECT_EQ(test, chan(f, 5)->info.config_len, 64);
}

/*
 * A fatal error after discovery's last read fails the bind at the INIT to
 * LIVE edge, with no function device; one while that read is armed fails the
 * read
 */
static void obmf_test_discover_fatal(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	int i;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 1)->fdev);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 2)->fdev);

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->fatal_hdr = 2;
	KUNIT_EXPECT_EQ(test, fake_bind(f), -EIO);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_FATAL_ERRORS), 1);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 2);
	for (i = 1; i <= ARRAY_SIZE(disc_chans); i++)
		KUNIT_EXPECT_NULL(test, chan(f, i)->fdev);

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->fatal_hdr = 2;
	f->fatal_armed = true;
	KUNIT_EXPECT_EQ(test, fake_bind(f), -EIO);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_FATAL_ERRORS), 1);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 2);
	KUNIT_EXPECT_NULL(test, f->core->chans[1]);
}

/*
 * A channel's configuration is at most 32 KiB, all of them together at most
 * 256 KiB; a channel past either is absent
 */
static void obmf_test_discover_sizes(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct test_chan c[11];
	int i;

	for (i = 0; i < ARRAY_SIZE(c); i++)
		c[i] = (struct test_chan){
			OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio", 516, 0,
			SZ_32K
		};
	c[1].config_len = SZ_32K + 1;
	c[9].config_len = 1;
	c[10].config_len = 0;
	fake_mem(f, SZ_512K);
	fake_layout(f, c, ARRAY_SIZE(c));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	for (i = 0; i < ARRAY_SIZE(c); i++) {
		if (i == 1 || i == 9) {
			KUNIT_EXPECT_NULL_MSG(test, f->core->chans[i + 1],
					      "channel %d", i + 1);
			continue;
		}
		KUNIT_EXPECT_EQ(test, chan(f, i + 1)->info.config_len,
				c[i].config_len);
	}
	KUNIT_EXPECT_MEMEQ(test, chan(f, 9)->info.config,
			   f->mem + fake_ch_off(f, 9) + FAKE_CH_HDR_LEN,
			   SZ_32K);
}

/* a read failing -ENODEV is not retried */
static void obmf_test_discover_enodev(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->read_ret = -ENODEV;
	KUNIT_EXPECT_EQ(test, fake_bind(f), -ENODEV);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, 0, 0), 1);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_INIT);
}

/* a read timing out in the first pass only */
static void obmf_test_discover_timeout_once(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_read = 2;
	KUNIT_EXPECT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_TIMEOUTS), 1);
	KUNIT_EXPECT_GE(test, disc_gap(f, 0, 1),
			2 * OBMF_DEFAULT_TIMEOUT_MS - 50);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
}

/* timing out in every pass: three passes, then -ETIMEDOUT */
static void obmf_test_discover_timeout_always(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_hdr = true;
	KUNIT_EXPECT_EQ(test, fake_bind(f), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_TIMEOUTS), 3);
}

/* a device header that differs on every pass, and on one */
static void obmf_test_discover_header_changes(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->bump_from = 1;
	f->bump_to = UINT_MAX;
	KUNIT_EXPECT_EQ(test, fake_bind(f), -EIO);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 6);
	KUNIT_EXPECT_GE(test, disc_gap(f, 1, 2), OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_EXPECT_GE(test, disc_gap(f, 3, 4), OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_EXPECT_NULL(test, f->core->chans[1]);

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->bump_from = 2;
	f->bump_to = 2;
	KUNIT_EXPECT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 4);
	KUNIT_EXPECT_EQ(test, f->core->device_id, 0x5679);
}

/* a stale reply of another length, once and always */
static void obmf_test_discover_stale_reply(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct obmf_channel *ch0;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->stale_first = true;
	KUNIT_EXPECT_EQ(test, fake_bind(f), 0);
	ch0 = f->core->ch0;
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch0, OBMF_CH_PROTOCOL_ERRORS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch0, OBMF_CH_RX_LATE), 1);
	KUNIT_EXPECT_GE(test, disc_gap(f, 0, 1), OBMF_DEFAULT_TIMEOUT_MS);

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->stale_hdr = true;
	KUNIT_EXPECT_EQ(test, fake_bind(f), -EPROTO);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_PROTOCOL_ERRORS), 3);
}

/* replies slowed so the first pass outlasts the 30 s budget */
static void obmf_test_discover_budget(struct kunit *test)
{
	static const struct test_chan slow[] = {
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio", 516, 0, 3072 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	ktime_t t;

	fake_layout(f, slow, ARRAY_SIZE(slow));
	f->ch0_delay_ms = 700;
	f->bump_from = 1;
	f->bump_to = UINT_MAX;
	t = ktime_get();
	KUNIT_EXPECT_EQ(test, fake_bind(f), -ETIMEDOUT);
	KUNIT_EXPECT_GE(test, ktime_ms_delta(ktime_get(), t), 30000);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_TIMEOUTS), 0);
}

/*
 * The line of a bound device: the protocol, its version in BCD, the device
 * name and each channel in order, with its kind, number and name; a channel
 * with no function device is marked, one of a type with no kind named gives
 * the type, one left out is not listed. Expected: one "channel 2: not
 * enabled, ignored" warning.
 */
static void obmf_test_info_line(struct kunit *test)
{
	static const struct test_chan kit[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "I2C", 516 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "UART", 516 },
	};
	static const struct test_chan gap[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "I2C", 516 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "off", 516,
		  FAKE_OFF_ENABLE },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "UART", 516 },
	};
	static const struct test_chan bare[] = {
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio", 516 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_SYMMETRIC, NULL, 516 },
		{ 0xf8, OBMF_ROLE_PRODUCER, "oem", 516 },
	};
	char buf[OBMF_INFO_LEN];
	struct obmf_fake *f;

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.0 device 'obmf-test': I2C ch1 'i2c-0', serial ch2 'uart\xc3\xa9', GPIO ch3 '' (no driver)");

	/* I2C on channel 1 and a UART on channel 2 */
	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, kit, ARRAY_SIZE(kit));
	memcpy(f->mem + 6, "OpenSMA\0\0", 9);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.0 device 'OpenSMA': I2C ch1 'I2C', serial ch2 'UART'");

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, gap, ARRAY_SIZE(gap));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.0 device 'obmf-test': I2C ch1 'I2C', serial ch3 'UART'");

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, bare, 1);
	f->mem[0] = 0x12;
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.12 device 'obmf-test': GPIO ch1 'gpio' (no driver)");

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, bare, ARRAY_SIZE(bare));
	memset(f->mem + 6, 0, 32);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.0 device '': GPIO ch1 'gpio' (no driver), serial ch2 '', type 0xf8 ch3 'oem' (no driver)");
}

#define INFO_DEV(name) "OBMF-ICP 1.0 device '" name "'"
#define INFO_CH(sep, n) sep "serial ch" #n " 'abcdefghijklmnop'"

/*
 * Twenty channels: the line lists those that fit and counts the others. A
 * device name of 2 bytes leaves room for the seventh channel to the last
 * byte; one of 3 bytes does not
 */
static void obmf_test_info_many(struct kunit *test)
{
	static const char *const want[] = {
		INFO_DEV("ab") INFO_CH(": ", 1) INFO_CH(", ", 2)
			INFO_CH(", ", 3) INFO_CH(", ", 4) INFO_CH(", ", 5)
			INFO_CH(", ", 6) INFO_CH(", ", 7) " and 13 more",
		INFO_DEV("abc") INFO_CH(": ", 1) INFO_CH(", ", 2)
			INFO_CH(", ", 3) INFO_CH(", ", 4) INFO_CH(", ", 5)
			INFO_CH(", ", 6) " and 14 more",
	};
	struct test_chan c[20];
	char buf[OBMF_INFO_LEN];
	struct obmf_fake *f;
	int i, k;

	for (i = 0; i < ARRAY_SIZE(c); i++)
		c[i] = (struct test_chan){ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER,
					   "abcdefghijklmnop", 516 };
	for (k = 0; k < ARRAY_SIZE(want); k++) {
		f = fake_create(test, FAKE_MAX_MSG, 0);
		fake_layout(f, c, ARRAY_SIZE(c));
		memset(f->mem + 6, 0, 32);
		memcpy(f->mem + 6, "abc", 2 + k);
		KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
		obmf_core_info(f->core, buf);
		KUNIT_EXPECT_STREQ(test, buf, want[k]);
	}
}

static struct kunit_case obmf_ctrl_cases[] = {
	KUNIT_CASE(obmf_test_discover),
	KUNIT_CASE(obmf_test_ch_regs),
	KUNIT_CASE(obmf_test_discover_version),
	KUNIT_CASE(obmf_test_discover_names),
	KUNIT_CASE(obmf_test_discover_checks),
	KUNIT_CASE(obmf_test_discover_unusable),
	KUNIT_CASE(obmf_test_discover_fatal),
	KUNIT_CASE(obmf_test_discover_sizes),
	KUNIT_CASE(obmf_test_discover_enodev),
	KUNIT_CASE(obmf_test_info_line),
	KUNIT_CASE(obmf_test_info_many),
	KUNIT_CASE_SLOW(obmf_test_discover_timeout_once),
	KUNIT_CASE_SLOW(obmf_test_discover_timeout_always),
	KUNIT_CASE_SLOW(obmf_test_discover_header_changes),
	KUNIT_CASE_SLOW(obmf_test_discover_stale_reply),
	KUNIT_CASE_SLOW(obmf_test_discover_budget),
	{}
};

static struct kunit_suite obmf_ctrl_suite = {
	.name = "obmf-ctrl",
	.test_cases = obmf_ctrl_cases,
};

/* Function devices */

static bool dbg_has(struct dentry *dir, const char *name)
{
	struct dentry *d = debugfs_lookup(name, dir);

	dput(d);
	return !IS_ERR_OR_NULL(d);
}

/*
 * The interface's counters, channel 0's, and a directory for each channel
 * with a function device only; all gone after unbind
 */
static void obmf_test_debugfs(struct kunit *test)
{
	static const char *const if_files[] = {
		"rx_messages",	 "tx_messages",	  "rx_malformed",
		"rx_no_channel", "rx_urb_errors", "tx_urb_errors",
		"stalls",	 "fatal_errors",
	};
	static const char *const ch_files[] = {
		"tx_requests",	   "rx_responses",	 "rx_late",
		"timeouts",	   "status_errors",	 "protocol_errors",
		"tx_errors",	   "rx_device_requests", "tx_answers",
		"default_answers", "rx_dropped",	 "rx_replaced",
	};
	static const char *const ch_dirs[] = { "ch0", "ch1", "ch2" };
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct dentry *dir, *root;
	int i, k;

	if (!IS_ENABLED(CONFIG_DEBUG_FS))
		kunit_skip(test, "needs CONFIG_DEBUG_FS");
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	dir = f->core->dbg;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dir);
	for (i = 0; i < ARRAY_SIZE(if_files); i++)
		KUNIT_EXPECT_TRUE_MSG(test, dbg_has(dir, if_files[i]), "%s",
				      if_files[i]);
	for (k = 0; k < ARRAY_SIZE(ch_dirs); k++) {
		struct dentry *ch = debugfs_lookup(ch_dirs[k], dir);

		KUNIT_ASSERT_NOT_ERR_OR_NULL_MSG(test, ch, "%s", ch_dirs[k]);
		for (i = 0; i < ARRAY_SIZE(ch_files); i++)
			KUNIT_EXPECT_TRUE_MSG(test, dbg_has(ch, ch_files[i]),
					      "%s/%s", ch_dirs[k], ch_files[i]);
		dput(ch);
	}
	KUNIT_EXPECT_FALSE(test, dbg_has(dir, "ch3"));

	root = dget(dir->d_parent);
	KUNIT_EXPECT_TRUE(test, dbg_has(root, dev_name(f->tp.dev)));
	fake_unbind(f);
	KUNIT_EXPECT_FALSE(test, dbg_has(root, dev_name(f->tp.dev)));
	dput(root);
}

/*
 * A label is "<device name>_<channel name>", the device name cut on a UTF-8
 * boundary to fit; with no device name the channel name, with no channel
 * name the function device's name
 */
static void obmf_test_labels(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "x", 516 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, NULL, 516 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct obmf_fdev *fdev;
	char buf[48];

	fake_layout(f, c, ARRAY_SIZE(c));
	memset(f->mem + 6, 0, 32);
	memcpy(f->mem + 6, "abc\xc3\xa9", 5);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	fdev = chan(f, 1)->fdev;
	KUNIT_ASSERT_NOT_NULL(test, fdev);
	obmf_fdev_label(fdev, buf, sizeof(buf));
	KUNIT_EXPECT_STREQ(test, buf, "abc\xc3\xa9_x");
	obmf_fdev_label(fdev, buf, 8);
	KUNIT_EXPECT_STREQ(test, buf, "abc\xc3\xa9_x");
	obmf_fdev_label(fdev, buf, 7);
	KUNIT_EXPECT_STREQ(test, buf, "abc_x");
	obmf_fdev_label(fdev, buf, 5);
	KUNIT_EXPECT_STREQ(test, buf, "ab_x");
	fdev = chan(f, 2)->fdev;
	KUNIT_ASSERT_NOT_NULL(test, fdev);
	obmf_fdev_label(fdev, buf, sizeof(buf));
	KUNIT_EXPECT_STREQ(test, buf, dev_name(&fdev->adev.dev));

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, c, ARRAY_SIZE(c));
	memset(f->mem + 6, 0, 32);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_fdev_label(chan(f, 1)->fdev, buf, sizeof(buf));
	KUNIT_EXPECT_STREQ(test, buf, "x");
}

/* What the devres case's driver saw */
struct test_devres {
	int probe_ret;
	int req_ret;
	int req_state;
	bool req_unpublished;
};

static struct test_devres *test_devres_seen;

static const struct obmf_fdev_ops test_devres_ops = {};

static void test_devres_request(void *data)
{
	struct obmf_fdev *fdev = data;
	struct test_devres *t = test_devres_seen;
	size_t rlen;
	u8 resp[8];

	t->req_state = obmf_core_state(fdev->ch->core);
	t->req_unpublished = !fdev->ch->ops;
	t->req_ret =
		obmf_request(fdev, 0x01, NULL, 0, resp, sizeof(resp), &rlen, 0);
}

static int test_devres_probe(struct auxiliary_device *adev,
			     const struct auxiliary_device_id *id)
{
	struct obmf_fdev *fdev = container_of(adev, struct obmf_fdev, adev);
	int ret;

	ret = devm_obmf_enable(fdev);
	if (!ret)
		ret = devm_add_action_or_reset(&adev->dev, test_devres_request,
					       fdev);
	if (!ret)
		ret = devm_obmf_publish(fdev, &test_devres_ops);
	test_devres_seen->probe_ret = ret;
	return ret;
}

static struct auxiliary_driver test_devres_driver = {
	.name = "test-devres",
	.probe = test_devres_probe,
	.id_table = test_slow_ids,
};

/* The channel 0 writes sent */
static int ch0_writes(struct obmf_fake *f)
{
	int i = -1, n = 0;

	while ((i = fake_find(f, i + 1, FAKE_SEND, 0, 0)) >= 0)
		n += f->log[i].msg[4] == FAKE_SHORT_WRITE;
	return n;
}

/*
 * C-15, C-14: a function driver's devres runs in reverse at unbind:
 * unpublish, then a request the driver booked, answered while the core
 * is UNBINDING, then the transport stops. Neither bind nor unbind writes
 * through channel 0, so no channel is switched on or off. The helpers
 * called with no driver bound fail -EINVAL and book nothing, so a driver
 * binds afterwards.
 */
static void obmf_test_devres_order(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct obmf_fdev *fdev;
	struct test_devres *t;
	int req, stop;

	t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	fdev = chan(f, 1)->fdev;
	KUNIT_ASSERT_NOT_NULL(test, fdev);

	KUNIT_EXPECT_EQ(test, devm_obmf_enable(fdev), -EINVAL);
	KUNIT_EXPECT_EQ(test, devm_obmf_publish(fdev, &test_devres_ops),
			-EINVAL);
	KUNIT_EXPECT_FALSE(test, chan(f, 1)->bound);

	test_devres_seen = t;
	KUNIT_ASSERT_EQ(test, auxiliary_driver_register(&test_devres_driver),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, test_slow_unregister,
						  &test_devres_driver),
			0);
	KUNIT_EXPECT_EQ(test, t->probe_ret, 0);
	KUNIT_ASSERT_NOT_NULL(test, fdev->adev.dev.driver);
	KUNIT_EXPECT_TRUE(test, chan(f, 1)->bound);
	KUNIT_EXPECT_PTR_EQ(test, chan(f, 1)->ops, &test_devres_ops);

	fake_unbind(f);
	KUNIT_EXPECT_EQ(test, t->req_ret, 0);
	KUNIT_EXPECT_EQ(test, t->req_state, OBMF_UNBINDING);
	KUNIT_EXPECT_TRUE(test, t->req_unpublished);
	req = fake_find(f, 0, FAKE_SEND, 1, 0);
	stop = fake_find(f, 0, FAKE_STOP, -1, -1);
	KUNIT_EXPECT_GE(test, req, 0);
	KUNIT_EXPECT_GT(test, stop, req);
	KUNIT_EXPECT_EQ(test, ch0_writes(f), 0);
}

/* disc_chans bound on an interface whose firmware node is set */
static struct obmf_fake *node_bind(struct obmf_fake *f)
{
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	KUNIT_ASSERT_EQ(f->test, fake_bind(f), 0);
	return f;
}

/* The I2C channel of disc_chans has a device, with @node as its own */
static void node_check(struct obmf_fake *f, struct fwnode_handle *node)
{
	struct kunit *test = f->test;
	struct obmf_channel *ch = chan(f, 1);

	KUNIT_EXPECT_FALSE(test, ch->no_device);
	KUNIT_EXPECT_PTR_EQ(test, ch->fwnode, node);
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	KUNIT_EXPECT_PTR_EQ(test, dev_fwnode(&ch->fdev->adev.dev), node);
}

/* A node of obmf-test-nodes.dtso, applied before the fake is created */
static struct device_node *of_test_node(struct kunit *test, const char *path)
{
	struct device_node *np = of_find_node_by_path(path);

	KUNIT_ASSERT_NOT_NULL(test, np);
	of_node_put_kunit(test, np);
	return np;
}

/* The node's references, or 0 where OF nodes are not counted */
static unsigned int of_test_refs(struct device_node *np)
{
#ifdef CONFIG_OF_KOBJ
	return kref_read(&np->kobj.kref);
#else
	return 0;
#endif
}

static struct obmf_fake *of_test_bind(struct kunit *test,
				      struct device_node *np)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	device_set_node(f->tp.dev, of_fwnode_handle(np));
	return node_bind(f);
}

/*
 * A channel takes the child of the interface's node whose reg is its
 * number and whose name is its class, and gives it to its function
 * device; both references go at unbind. A child with another reg, or
 * no child, leaves the channel without a node.
 */
static void obmf_test_of_nodes(struct kunit *test)
{
	struct device_node *np, *i2c;
	struct obmf_fake *f;
	unsigned int refs;

	KUNIT_ASSERT_EQ(test, of_overlay_apply_kunit(test, obmf_test_nodes), 0);
	np = of_test_node(test, "/obmf-test-match");
	i2c = of_test_node(test, "/obmf-test-match/i2c@1");
	refs = of_test_refs(i2c);
	f = of_test_bind(test, np);
	node_check(f, of_fwnode_handle(i2c));
	fake_unbind(f);
	KUNIT_EXPECT_EQ(test, of_test_refs(i2c), refs);

	node_check(of_test_bind(test, of_test_node(test, "/obmf-test-reg")),
		   NULL);
	node_check(of_test_bind(test, of_test_node(test, "/obmf-test-empty")),
		   NULL);
}

/*
 * A child with the channel's reg and another class's name is not
 * attached. Expected: one "channel 1: node ... ignored" warning.
 */
static void obmf_test_of_misnamed(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, of_overlay_apply_kunit(test, obmf_test_nodes), 0);
	node_check(of_test_bind(test, of_test_node(test, "/obmf-test-name")),
		   NULL);
}

/*
 * A disabled node gives its channel no function device, and its
 * reference goes at once; the other channels still get theirs
 */
static void obmf_test_of_disabled(struct kunit *test)
{
	struct device_node *i2c;
	struct obmf_fake *f;
	unsigned int refs;

	KUNIT_ASSERT_EQ(test, of_overlay_apply_kunit(test, obmf_test_nodes), 0);
	i2c = of_test_node(test, "/obmf-test-disabled/i2c@1");
	refs = of_test_refs(i2c);
	f = of_test_bind(test, of_test_node(test, "/obmf-test-disabled"));
	KUNIT_EXPECT_TRUE(test, chan(f, 1)->no_device);
	KUNIT_EXPECT_NULL(test, chan(f, 1)->fwnode);
	KUNIT_EXPECT_NULL(test, chan(f, 1)->fdev);
	KUNIT_EXPECT_EQ(test, of_test_refs(i2c), refs);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 2)->fdev);
}

static const struct property_entry sw_reg1[] = {
	PROPERTY_ENTRY_U32("reg", 1),
	{},
};

static const struct property_entry sw_reg2[] = {
	PROPERTY_ENTRY_U32("reg", 2),
	{},
};

static const struct property_entry sw_reg3[] = {
	PROPERTY_ENTRY_U32("reg", 3),
	{},
};

/* The interfaces of obmf-test-nodes.dtso but the disabled one */
static const struct software_node sw_match = { .name = "obmf-test-match" };

static const struct software_node sw_match_serial = {
	.name = "serial@2",
	.parent = &sw_match,
	.properties = sw_reg2,
};

static const struct software_node sw_match_i2c = {
	.name = "i2c@1",
	.parent = &sw_match,
	.properties = sw_reg1,
};

static const struct software_node sw_reg = { .name = "obmf-test-reg" };

static const struct software_node sw_reg_i2c = {
	.name = "i2c@3",
	.parent = &sw_reg,
	.properties = sw_reg3,
};

static const struct software_node sw_name = { .name = "obmf-test-name" };

static const struct software_node sw_name_spi = {
	.name = "spi@1",
	.parent = &sw_name,
	.properties = sw_reg1,
};

static const struct software_node sw_empty = { .name = "obmf-test-empty" };

static const struct software_node *const sw_nodes[] = {
	&sw_match, &sw_match_serial, &sw_match_i2c, &sw_reg, &sw_reg_i2c,
	&sw_name,  &sw_name_spi,     &sw_empty,	    NULL,
};

/* The nodes are registered before the fake is created */
static struct obmf_fake *sw_bind(struct kunit *test,
				 const struct software_node *node)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	int ret;

	ret = kunit_device_add_software_node(test, f->tp.dev, node);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return node_bind(f);
}

/* obmf_test_of_nodes() with software nodes */
static void obmf_test_swnodes(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test,
			kunit_software_node_register_node_group(test, sw_nodes),
			0);
	node_check(sw_bind(test, &sw_match),
		   software_node_fwnode(&sw_match_i2c));
	node_check(sw_bind(test, &sw_reg), NULL);
	node_check(sw_bind(test, &sw_empty), NULL);
}

/*
 * obmf_test_of_misnamed() with software nodes. Expected: one "channel 1:
 * node ... ignored" warning.
 */
static void obmf_test_swnode_misnamed(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test,
			kunit_software_node_register_node_group(test, sw_nodes),
			0);
	node_check(sw_bind(test, &sw_name), NULL);
}

static struct kunit_case obmf_bus_cases[] = {
	KUNIT_CASE(obmf_test_debugfs),
	KUNIT_CASE(obmf_test_labels),
	KUNIT_CASE(obmf_test_devres_order),
	KUNIT_CASE(obmf_test_of_nodes),
	KUNIT_CASE(obmf_test_of_misnamed),
	KUNIT_CASE(obmf_test_of_disabled),
	KUNIT_CASE(obmf_test_swnodes),
	KUNIT_CASE(obmf_test_swnode_misnamed),
	{}
};

static struct kunit_suite obmf_bus_suite = {
	.name = "obmf-bus",
	.test_cases = obmf_bus_cases,
};

kunit_test_suites(&obmf_core_suite, &obmf_ctrl_suite, &obmf_bus_suite);
