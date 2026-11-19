// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: KUnit tests of the message layer, of channel
 * discovery and of the I2C controller, serial and GPIO drivers, on a fake
 * transport
 */

#include <kunit/device.h>
#include <kunit/fwnode.h>
#include <kunit/of.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/auxiliary_bus.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/serial.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/tracepoint.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/unaligned.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <trace/events/printk.h>

#include "obmf-core.h"
#include "obmf-kunit.h"

#define FAKE_LOG_LEN 256
#define FAKE_KEEP 72
#define FAKE_STEPS 24
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
 * @spi_read: an SPI answer: the request's read size, then byte k of the
 *	data is k ^ 0x5a; @plen and @fill are set from it
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
	bool spi_read;
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
	/* answers, then returns -EINPROGRESS as if it had kept the request */
	bool answer_keep;
	bool ignore;
	struct obmf_dreq *kept;
	struct completion entered;
	struct completion release;
	u8 cmd;
	int answer_ret;
	atomic_t failed;
	bool failed_in_work;
	int failed_errno;
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
	/* sends per channel number, the message's first byte */
	unsigned int ch_sends[U8_MAX + 1];
	unsigned int quiesced_sends;
	bool stopped;
	bool quiesced;
	int resume_ret;
	/*
	 * A send sleeps send_sleep_ms, at most its own timeout, and then
	 * fails with send_sleep_ret if set: with -ETIMEDOUT, a device that
	 * takes no data. Only on the channels in send_sleep_chans, a bit
	 * each, if any are set.
	 */
	unsigned int send_sleep_ms;
	int send_sleep_ret;
	unsigned long send_sleep_chans;
	bool send_until_unbind;
	int answer_ret;
	/* answer_ret fails one answer only */
	bool answer_once;
	/* a channel whose requests get no reply, after their step's send */
	int silent_ch;
	struct fake_step steps[FAKE_STEPS];
	unsigned int nsteps;
	unsigned int step;
	struct fake_step dflt;
	struct fake_defer defer[FAKE_DEFER];

	/*
	 * channel 0: the configuration space and its faults; addresses from
	 * hi_base up (if set) are the last bytes of mem
	 */
	u8 *mem;
	size_t mem_len;
	u32 hi_base;
	bool head;
	unsigned int reads;
	unsigned int hdr_reads;
	int read_ret;
	unsigned int drop_read;
	/* the read answered with the timeout status */
	unsigned int status_read;
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
	/* a device request whose work runs after the stop */
	struct obmf_channel *stop_dreq_ch;
	long stop_dreq_answers;
	long stop_dreq_errors;
	/*
	 * sends refused after the stop, from any context; fake_unbind()
	 * expects none unless the case sets late_send_ok and says why
	 */
	atomic_t sends_after_stop;
	bool late_send_ok;
	long stop_events[OBMF_EV_NR];

	/*
	 * the release operation's calls; with release_unbinds it unbinds
	 * from a work under dev_lock, as the USB transport does
	 */
	atomic_t releases;
	bool release_unbinds;
	struct work_struct release_work;
	struct completion release_done;
	unsigned int released;
	/* the USB device's lock, which every unbind holds */
	struct mutex dev_lock;
	/* the release operation waits for release_go */
	bool release_hold;
	struct completion release_entered;
	struct completion release_go;

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

static long ev_cnt(struct obmf_core *core, enum obmf_event ev)
{
	return atomic_long_read(&core->events[ev]);
}

#define TEST_LOG_LINES 32
#define TEST_LOG_LINE 160

/*
 * The kernel log lines of one device in the running case, as the console
 * tracepoint passes them: works and timers log too, and a static stub
 * sees only the case's own thread. Each case has its own probe and keeps
 * only the lines of its device, as an earlier device of the case can
 * still log.
 */
struct test_log {
	raw_spinlock_t lock;
	int reg;
	char dev[48];
	char dev2[48];
	unsigned int n;
	unsigned int lost;
	char line[TEST_LOG_LINES][TEST_LOG_LINE];
};

/* Called with interrupts off, from every printk */
static void test_log_probe(void *data, const char *buf, size_t len)
{
	struct test_log *log = data;
	unsigned long flags;

	/* No line of ours comes from NMI, which could find the lock held */
	if (in_nmi())
		return;
	raw_spin_lock_irqsave(&log->lock, flags);
	if ((log->dev[0] && strnstr(buf, log->dev, len)) ||
	    (log->dev2[0] && strnstr(buf, log->dev2, len))) {
		/* @buf is not NUL-terminated */
		if (log->n < TEST_LOG_LINES)
			strscpy(log->line[log->n++], buf,
				min(len + 1, sizeof(log->line[0])));
		else
			log->lost++;
	}
	raw_spin_unlock_irqrestore(&log->lock, flags);
}

/* Sets up @log, the start of what test->priv points to */
static void test_log_init(struct kunit *test, struct test_log *log)
{
	raw_spin_lock_init(&log->lock);
	log->reg = register_trace_console(test_log_probe, log);
	test->priv = log;
}

static void test_log_exit(struct kunit *test)
{
	struct test_log *log = test->priv;

	if (log && !log->reg) {
		unregister_trace_console(test_log_probe, log);
		tracepoint_synchronize_unregister();
	}
}

/*
 * From now on, keep the lines of @dev. Without printk or tracepoints
 * there are none, and a case that counts lines is skipped here, before
 * it acts: a skip later would hide its failures.
 */
static struct test_log *test_log_start(struct kunit *test, struct device *dev)
{
	struct test_log *log = test->priv;

	if (!IS_ENABLED(CONFIG_PRINTK) || log->reg)
		kunit_skip(test, "needs CONFIG_PRINTK and CONFIG_TRACEPOINTS");
	scoped_guard(raw_spinlock_irqsave, &log->lock) {
		snprintf(log->dev, sizeof(log->dev), " %s: ", dev_name(dev));
		log->dev2[0] = '\0';
		log->n = 0;
		log->lost = 0;
	}
	return log;
}

/* Keeps the lines of @dev too, as test_log_start() keeps its device's */
static void __maybe_unused test_log_also(struct test_log *log,
					 struct device *dev)
{
	guard(raw_spinlock_irqsave)
		(&log->lock);
	snprintf(log->dev2, sizeof(log->dev2), " %s: ", dev_name(dev));
}

static void test_log_exit_action(void *data)
{
	test_log_exit(data);
}

/* A log for a case of a suite without one, from now on */
static struct test_log *test_log_case(struct kunit *test, struct device *dev)
{
	struct test_log *log = kunit_kzalloc(test, sizeof(*log), GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, log);
	KUNIT_ASSERT_NULL(test, test->priv);
	test_log_init(test, log);
	ret = kunit_add_action_or_reset(test, test_log_exit_action, test);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return test_log_start(test, dev);
}

/* Lines in @log, since test_log_start(), that contain @text */
static int test_log_lines(struct kunit *test, struct test_log *log,
			  const char *text)
{
	unsigned int i, lost;
	int n = 0;

	scoped_guard(raw_spinlock_irqsave, &log->lock) {
		for (i = 0; i < log->n; i++)
			if (strstr(log->line[i], text))
				n++;
		lost = log->lost;
	}
	KUNIT_ASSERT_EQ_MSG(test, lost, 0, "log lines not kept");
	return n;
}

/* Waits up to @ms for @n lines that test_log_lines() counts */
static int test_log_wait(struct kunit *test, struct test_log *log,
			 const char *text, int n, unsigned int ms)
{
	unsigned int t;

	for (t = 0; t < ms / 100; t++) {
		if (test_log_lines(test, log, text) >= n)
			break;
		msleep(100);
	}
	return test_log_lines(test, log, text);
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
			f->ch_sends[*msg]++;
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

	/*
	 * Called from the core's thread, which a failed assertion must not
	 * end: a short message fails the case and is not read
	 */
	if (plen < 6) {
		KUNIT_FAIL(f->test, "channel 0 message of %zu bytes", len);
		return -EINVAL;
	}
	/* the reserved bytes are 0 */
	KUNIT_EXPECT_EQ(f->test, p[0] & ~7, 0);
	KUNIT_EXPECT_EQ(f->test, p[1], 0);
	addr = get_unaligned_le32(p + 2);
	if (f->hi_base && addr >= f->hi_base)
		addr = f->mem_len - (u32)-addr;

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

	if (plen != 8) {
		KUNIT_FAIL(f->test, "channel 0 read of %zu bytes", len);
		return -EINVAL;
	}
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
	if (idx == f->status_read) {
		fake_deliver(f, FAKE_INLINE, 0, out,
			     fake_msg(out, 0,
				      OBMF_HDR_RQRESP | OBMF_STATUS_TIMEOUT,
				      NULL, 0));
		return 0;
	}
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
	if (msg[0] == READ_ONCE(f->silent_ch))
		return s.send_ret;

	/* Table 51: the read size of the request (Table 50), then the data */
	if (s.spi_read) {
		k = get_unaligned_le16(msg + OBMF_HDR_LEN + 3);
		s.plen = 2 + k;
		s.fill = false;
		s.payload[0] = k;
		s.payload[1] = k >> 8;
	}
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
			s.fill		     ? (u8)(k ^ 0x5a) :
			s.spi_read && k >= 2 ? (u8)((k - 2) ^ 0x5a) :
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
	int i;

	fake_log(f, FAKE_STOP, NULL, 0, 0);
	for (i = 0; i < OBMF_EV_NR; i++)
		f->stop_events[i] = ev_cnt(core, i);
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
	if (f->stop_dreq_ch) {
		struct obmf_channel *ch = f->stop_dreq_ch;

		fake_inject(f, ch->info.number, 0, (u8[]){ 0x01 }, 1);
		flush_work(&ch->work);
		f->stop_dreq_answers = ch_cnt(ch, OBMF_CH_TX_ANSWERS);
		f->stop_dreq_errors = ch_cnt(ch, OBMF_CH_TX_ERRORS);
	}
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

static bool fake_send_sleeps(struct obmf_fake *f, u8 ch)
{
	unsigned long chans = READ_ONCE(f->send_sleep_chans);

	if (!READ_ONCE(f->send_sleep_ms))
		return false;
	return !chans || (ch < BITS_PER_LONG && (chans & BIT(ch)));
}

static int fake_send(struct obmf_transport *tp, const void *buf, size_t len,
		     unsigned int timeout_ms)
{
	struct obmf_fake *f = to_fake(tp);
	/* Read before fake_log() wakes the test, which may then set it */
	bool until = READ_ONCE(f->send_until_unbind);
	const u8 *msg = buf;
	int state, err;

	/* As the USB transport: refused, nothing sent */
	if (READ_ONCE(f->stopped)) {
		atomic_inc(&f->sends_after_stop);
		return -ENODEV;
	}
	/*
	 * A request carries its command byte; an answer may be empty. A
	 * message shorter than its header fails the case and is not read.
	 */
	if (len < OBMF_HDR_LEN) {
		KUNIT_FAIL(f->test, "a message of %zu bytes", len);
		return -EINVAL;
	}
	KUNIT_EXPECT_GE(f->test, len,
			OBMF_HDR_LEN + !(msg[1] & OBMF_HDR_RQRESP));
	KUNIT_EXPECT_LE(f->test, len, tp->max_tx_msg);
	KUNIT_EXPECT_EQ(f->test, get_unaligned_le16(msg + 2),
			len - OBMF_HDR_LEN);
	fake_log(f, FAKE_SEND, msg, len, timeout_ms);
	if (fake_send_sleeps(f, msg[0])) {
		msleep(min(READ_ONCE(f->send_sleep_ms), timeout_ms));
		err = READ_ONCE(f->send_sleep_ret);
		if (err)
			return err;
	}
	if (until &&
	    read_poll_timeout(obmf_core_state, state, state != OBMF_LIVE, 1000,
			      FAKE_WAIT_MS * 1000, false, tp->core))
		KUNIT_FAIL(f->test, "send: still live");
	if (READ_ONCE(f->quiesced))
		return -EAGAIN;
	if (msg[1] & OBMF_HDR_RQRESP) {
		int ret = READ_ONCE(f->answer_ret);

		if (ret && READ_ONCE(f->answer_once))
			WRITE_ONCE(f->answer_ret, 0);
		return ret;
	}
	if (!msg[0])
		return fake_ch0(f, msg, len);
	return fake_reply_step(f, msg);
}

static void fake_unbind(struct obmf_fake *f);

/* As the USB transport: only the binding that asked, under the lock */
static void fake_release_work(struct work_struct *work)
{
	struct obmf_fake *f =
		container_of(work, struct obmf_fake, release_work);

	scoped_guard(mutex, &f->dev_lock) {
		if (f->core && obmf_core_state(f->core) == OBMF_FAILED) {
			f->released++;
			fake_unbind(f);
		}
	}
	complete(&f->release_done);
}

static void fake_release(struct obmf_transport *tp)
{
	struct obmf_fake *f = to_fake(tp);

	atomic_inc(&f->releases);
	if (f->release_unbinds)
		queue_work(f->callwq, &f->release_work);
	if (f->release_hold) {
		complete(&f->release_entered);
		wait_for_completion(&f->release_go);
	}
}

static const struct obmf_transport_ops fake_ops = {
	.start = fake_start,
	.stop = fake_stop,
	.quiesce = fake_quiesce,
	.resume = fake_resume,
	.send = fake_send,
	.release = fake_release,
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

static void obmf_test_mutex_destroy(void *lock)
{
	mutex_destroy(lock);
}

static atomic_t obmf_test_ids = ATOMIC_INIT(0);

/*
 * A fake transport's function devices are named "test-<name>", so no
 * function driver binds them and the cases' own drivers bind nothing
 * else. The function driver suites name them as a device does: their
 * driver then binds the fake's devices, which reach no hardware.
 */
static bool obmf_test_fn_names;

static int __maybe_unused obmf_test_fn_suite_init(struct kunit_suite *suite)
{
	obmf_test_fn_names = true;
	return 0;
}

static void __maybe_unused obmf_test_fn_suite_exit(struct kunit_suite *suite)
{
	obmf_test_fn_names = false;
}

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
	f->tp.test_names = !obmf_test_fn_names;
	f->tp.max_tx_msg = max_msg;
	f->tp.max_rx_msg = max_msg;
	f->tp.resp_timeout_floor_ms = floor_ms;

	INIT_WORK(&f->release_work, fake_release_work);
	init_completion(&f->release_done);
	init_completion(&f->release_entered);
	init_completion(&f->release_go);
	mutex_init(&f->dev_lock);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, obmf_test_mutex_destroy,
						  &f->dev_lock),
			0);
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

/* An SPI channel's configuration (OBMF-ICP 1.0.0, 5.15.2, Table 84) */
static void fake_spi_cfg(u8 *p, u32 flags, u8 devices, u32 hz)
{
	struct obmf_spi_cfg *c = (struct obmf_spi_cfg *)p;

	memset(c, 0, sizeof(*c));
	c->flags = cpu_to_le32(flags);
	c->num_devices = devices;
	c->rd_max_speed_hz = cpu_to_le32(hz);
	c->wr_max_speed_hz = cpu_to_le32(hz);
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
		/* OBMF-ICP 1.0.0, 5.15.2, Table 79: 100 kHz, bus recovery */
		if (c[i].type == OBMF_TYPE_I2C_CONTROLLER &&
		    c[i].config_len >= 2) {
			h[FAKE_CH_HDR_LEN] = 0x00;
			h[FAKE_CH_HDR_LEN + 1] = 0x01;
		}
		/* Table 84: mode 0, 8 bits, one device, 1 MHz both ways */
		if (c[i].type == OBMF_TYPE_SPI &&
		    c[i].config_len >= sizeof(struct obmf_spi_cfg))
			fake_spi_cfg(h + FAKE_CH_HDR_LEN, 0, 1, 1000000);
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
	/*
	 * The core's works are disabled by now, so no send of the core can
	 * still be counted; only one of the case's own threads could
	 */
	if (!f->late_send_ok)
		KUNIT_EXPECT_EQ(f->test, atomic_read(&f->sends_after_stop), 0);
}

static void fake_unbind_action(void *data)
{
	fake_unbind(data);
}

/* Probe: returns once the transport is up; the bind work is not waited for */
static int fake_probe(struct obmf_fake *f)
{
	struct kunit *test = f->test;

	/* a bind after an unbind is a new probe, on a transport not stopped */
	WRITE_ONCE(f->stopped, false);
	f->core = obmf_core_create(&f->tp);
	KUNIT_ASSERT_NOT_NULL(test, f->core);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, fake_unbind_action, f),
			0);
	return obmf_core_bind(f->core);
}

/* Probe, then the bind work's result: discovery's errno or 0 */
static int fake_bind(struct obmf_fake *f)
{
	int ret = fake_probe(f);

	if (ret)
		return ret;
	flush_work(&f->core->bind_work);
	return f->core->bind_err;
}

/* The channels the message layer cases use */
static const struct test_chan core_chans[] = {
	{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c", 516, 0, 2 },
	{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio", 516 },
	{ OBMF_TYPE_IPMI, OBMF_ROLE_PRODUCER, "ipmi", 516 },
	{ OBMF_TYPE_SPI, OBMF_ROLE_PRODUCER, "spi", 516, 0, 20 },
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

/* Requests in a loop on one channel, from a thread, until it is stopped */
struct test_busy {
	struct obmf_channel *ch;
	int nr;
	/* the channel's sends before the thread started */
	unsigned int sends;
	unsigned int timeout_ms;
	struct task_struct *task;
	unsigned int n;
	int ret;
};

static int test_busy_fn(void *data)
{
	struct test_busy *b = data;
	size_t rlen;
	u8 resp[8];
	int ret;

	while (!kthread_should_stop()) {
		ret = obmf_ch_request(b->ch, 0x01, NULL, 0, resp, sizeof(resp),
				      &rlen, b->timeout_ms);
		WRITE_ONCE(b->ret, ret);
		WRITE_ONCE(b->n, b->n + 1);
		/* A refused request returns at once */
		if (ret != -ETIMEDOUT)
			usleep_range(1000, 2000);
	}
	return 0;
}

static void test_busy_stop(void *data)
{
	struct test_busy *b = data;

	kthread_stop(b->task);
}

/* Whether each of the @n threads @b has sent on its channel */
static bool test_busy_sent(struct obmf_fake *f, struct test_busy *b, int n)
{
	int i;

	guard(spinlock_irqsave)
		(&f->lock);
	for (i = 0; i < n; i++)
		if (f->ch_sends[b[i].nr] == b[i].sends)
			return false;
	return true;
}

/*
 * A thread on each of the @n channels @chs sending requests that wait
 * @timeout_ms, until test_busy_end(); returns once each has sent one
 */
static struct test_busy *test_busy_start(struct obmf_fake *f, const int *chs,
					 int n, unsigned int timeout_ms)
{
	struct kunit *test = f->test;
	struct test_busy *b;
	bool sent;
	int i;

	b = kunit_kcalloc(test, n, sizeof(*b), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, b);
	for (i = 0; i < n; i++) {
		b[i].ch = chan(f, chs[i]);
		b[i].nr = chs[i];
		scoped_guard(spinlock_irqsave, &f->lock)
			b[i].sends = f->ch_sends[b[i].nr];
		b[i].timeout_ms = timeout_ms;
		b[i].task = kthread_run(test_busy_fn, &b[i], "obmf-busy-%d",
					chs[i]);
		KUNIT_ASSERT_FALSE(test, IS_ERR(b[i].task));
		KUNIT_ASSERT_EQ(test,
				kunit_add_action_or_reset(test, test_busy_stop,
							  &b[i]),
				0);
	}
	KUNIT_ASSERT_EQ(test,
			read_poll_timeout(test_busy_sent, sent, sent, 1000,
					  1000000, false, f, b, n),
			0);
	return b;
}

/* Stops the threads; each has sent a request and the last timed out */
static void test_busy_end(struct obmf_fake *f, struct test_busy *b, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		kunit_release_action(f->test, test_busy_stop, &b[i]);
		KUNIT_EXPECT_GE(f->test, b[i].n, 1);
		KUNIT_EXPECT_EQ(f->test, b[i].ret, -ETIMEDOUT);
	}
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
	if (fn->answer_keep) {
		fn->answer_ret =
			obmf_respond(fdev, dreq, OBMF_STATUS_SUCCESS, NULL, 0);
		return -EINPROGRESS;
	}
	if (fn->ignore)
		return 0;
	if (dreq->answer_due)
		fn->answer_ret =
			obmf_respond(fdev, dreq, OBMF_STATUS_SUCCESS, NULL, 0);
	return 0;
}

static void test_fn_failed(struct obmf_fdev *fdev)
{
	struct obmf_test_fn *fn = container_of(fdev, struct obmf_test_fn, fdev);

	fn->failed_in_work = current_work();
	fn->failed_errno = obmf_fdev_errno(fdev);
	atomic_inc(&fn->failed);
}

static const struct obmf_fdev_ops test_fn_ops = {
	.device_request = test_fn_request,
	.failed = test_fn_failed,
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

/* The last request sent on @ch; the case stops if there is none */
static int send_index(struct obmf_fake *f, int ch)
{
	int i = -1, last = -1;

	while ((i = fake_find(f, i + 1, FAKE_SEND, ch, 0)) >= 0)
		last = i;
	KUNIT_ASSERT_GE_MSG(f->test, last, 0, "no request on channel %d", ch);
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
	static const int generic[] = { 0,    -EIO,    -ETIME,	-EBUSY,
				       -EIO, -ECHRNG, -EMSGSIZE };

	if (st < ARRAY_SIZE(generic))
		return generic[st];
	if (st < 0x40)
		return -EIO;
	switch (type) {
	case OBMF_TYPE_I2C_CONTROLLER:
		switch (st) {
		case 0x40:
			return -ETIME;
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
			return -ETIME;
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
			return -ETIME;
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

/*
 * A GPIO Get and Set reach the device as the function driver wrote them,
 * the Get's entries come back with their values, and the Set's response
 * is the echo alone (OBMF-ICP 1.0.0, 5.4, Tables 18 and 19)
 */
static void obmf_test_gpio_values(struct kunit *test)
{
	static const u8 get[] = { 0x01, 0x00, 0x03, 0x00 };
	static const u8 set[] = { 0x01, 0x10, 0x03, 0x00 };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 4,
		.payload = { 0x01, 0x10, 0x03, 0x00 },
	};
	struct fake_rec *r;
	size_t rlen;
	u8 resp[8];

	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_request(ch->fdev, 0, get, sizeof(get), resp,
				     sizeof(resp), &rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, rlen, 4);
	KUNIT_EXPECT_MEMEQ(test, resp, s.payload, 4);
	r = &f->log[send_index(f, CH_GPIO)];
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 1 + sizeof(get));
	KUNIT_EXPECT_EQ(test, r->msg[OBMF_HDR_LEN], 0);
	KUNIT_EXPECT_MEMEQ(test, &r->msg[OBMF_HDR_LEN + 1], get, sizeof(get));

	s.plen = 0;
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			obmf_request(ch->fdev, 1, set, sizeof(set), NULL, 0,
				     &rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, rlen, 0);
	r = &f->log[send_index(f, CH_GPIO)];
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 1 + sizeof(set));
	KUNIT_EXPECT_EQ(test, r->msg[OBMF_HDR_LEN], 1);
	KUNIT_EXPECT_MEMEQ(test, &r->msg[OBMF_HDR_LEN + 1], set, sizeof(set));
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), 0);
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
	fake_inject(f, CH_GPIO, OBMF_HDR_RQRESP, (u8[]){ 1 }, 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 1);
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

/*
 * A send that takes its whole wait and succeeds: the reply wait is what
 * is left of the deadline, at least a jiffy, so a reply already in is
 * taken and a missing one times out at once
 */
static void obmf_test_wait_send_late(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	size_t rlen;
	u8 resp[8];
	s64 ms;
	ktime_t t;

	WRITE_ONCE(f->send_sleep_ms, 250);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 250),
			0);
	ms = ktime_ms_delta(ktime_get(), t);
	KUNIT_EXPECT_GE(test, ms, 250);
	KUNIT_EXPECT_LT(test, ms, 250 + 300);

	fake_push(f, &s);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 250),
			-ETIMEDOUT);
	ms = ktime_ms_delta(ktime_get(), t);
	WRITE_ONCE(f->send_sleep_ms, 0);
	KUNIT_EXPECT_GE(test, ms, 250);
	KUNIT_EXPECT_LT(test, ms, 250 + 300);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 1);
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

/*
 * After a reply timeout the next request on the channel is sent once the
 * late reply came, which it does not take, or one default timeout later,
 * whatever the timed-out request waited; a suspend or a fatal stop ends
 * the wait.
 */
static void obmf_test_late_hold(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = {
		.how = FAKE_TIMER,
		.delay_ms = 300,
		.channel = -1,
		.echo = true,
		.plen = 1,
		.payload = { 0x77 },
	};
	struct fake_step never = { .how = FAKE_NEVER, .channel = -1 };
	struct test_call *c;
	unsigned int sends, from;
	int first, late, next;
	size_t rlen;
	u8 resp[8];
	ktime_t t;

	fake_push(f, &s);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 50),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	/* the late reply at 300 ms ends the wait */
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t), 800);
	KUNIT_EXPECT_EQ(test, resp[0], 0xa1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 1);
	first = fake_find(f, 0, FAKE_SEND, CH_GPIO, 0);
	KUNIT_ASSERT_GE(test, first, 0);
	late = fake_find(f, first + 1, FAKE_RX, CH_GPIO, -1);
	next = fake_find(f, first + 1, FAKE_SEND, CH_GPIO, 0);
	KUNIT_ASSERT_GE(test, late, 0);
	KUNIT_EXPECT_GT(test, next, late);

	/*
	 * a 2000 ms request answered at 3500 ms: the next, held one default
	 * timeout, not 2000 ms, takes that answer as its own
	 */
	s.delay_ms = 3500;
	fake_push(f, &s);
	fake_push(f, &never);
	from = fake_nlog(f);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 2000),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, resp[0], 0x77);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 1);
	first = fake_find(f, from, FAKE_SEND, CH_GPIO, 0);
	KUNIT_ASSERT_GE(test, first, 0);
	late = fake_find(f, first + 1, FAKE_RX, CH_GPIO, -1);
	next = fake_find(f, first + 1, FAKE_SEND, CH_GPIO, 0);
	KUNIT_ASSERT_GE(test, late, 0);
	KUNIT_ASSERT_GE(test, next, 0);
	KUNIT_EXPECT_LT(test, next, late);
	KUNIT_EXPECT_GE(test, ktime_ms_delta(f->log[next].t, f->log[first].t),
			2000 + OBMF_DEFAULT_TIMEOUT_MS - jiffies_to_msecs(2));

	fake_push(f, &never);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 50),
			-ETIMEDOUT);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	KUNIT_EXPECT_GE(test, ktime_ms_delta(ktime_get(), t),
			OBMF_DEFAULT_TIMEOUT_MS - jiffies_to_msecs(2));

	/* a suspend during the hold ends it at once */
	fake_push(f, &never);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 50),
			-ETIMEDOUT);
	sends = f->sends;
	c = call_start(f, call_request, ch, 0x02, 0);
	msleep(100);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t), 500);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, -EAGAIN);
	KUNIT_EXPECT_EQ(test, f->sends, sends);
	KUNIT_ASSERT_EQ(test, obmf_core_resume(f->core), 0);

	/*
	 * a timeout while suspending leaves a hold: a request fails at once
	 * and drops it
	 */
	fake_push(f, &never);
	c = call_start(f, call_request, ch, 0x02, 200);
	msleep(50);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, -ETIMEDOUT);
	KUNIT_EXPECT_TRUE(test, ch->slot.held);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			-EAGAIN);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t), 500);
	KUNIT_EXPECT_FALSE(test, ch->slot.held);
	KUNIT_ASSERT_EQ(test, obmf_core_resume(f->core), 0);

	fake_push(f, &never);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 50),
			-ETIMEDOUT);
	sends = f->sends;
	c = call_start(f, call_request, ch, 0x02, 0);
	msleep(100);
	KUNIT_EXPECT_EQ(test, f->sends, sends);
	t = ktime_get();
	obmf_core_fatal(f->core, -EPROTO);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, -EIO);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t), 500);
	KUNIT_EXPECT_EQ(test, f->sends, sends);
}

/* replies for another channel, and in either order */
static void obmf_test_reply_other_channel(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *gpio = chan(f, CH_GPIO);
	struct obmf_channel *ipmi = chan(f, CH_IPMI);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = CH_IPMI,
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
	KUNIT_EXPECT_EQ(test, ch_cnt(ipmi, OBMF_CH_RX_LATE), 1);

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
	b = call_start(f, call_request, ipmi, 0x03, 2000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 2));
	fake_inject(f, CH_IPMI, OBMF_HDR_RQRESP, (u8[]){ 0x03, 0x33 }, 2);
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

	/* Counted, not reported, before discovery ends */
	fake_rx(f, (u8[]){ 1, OBMF_HDR_RQRESP, 0 }, 3);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_MALFORMED), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_RX_SHORT), 0);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_RX_NO_CHANNEL), 0);
	/* channel 0 takes device requests before publish: an empty one */
	fake_inject(f, 0, 0, NULL, 0);
	flush_workqueue(core->wq);
	KUNIT_EXPECT_EQ(test, ch_cnt(core->ch0, OBMF_CH_RX_DEVICE_REQUESTS), 1);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_MALFORMED), 2);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_BAD_DREQ), 0);
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

/*
 * A suspend ends the hold after a reply timeout, so the next request after
 * the resume is sent at once and takes a late reply passing echo and length.
 */
static void obmf_test_late_reply_resume(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	size_t rlen;
	u8 resp[8];
	ktime_t t;

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
	KUNIT_EXPECT_TRUE(test, ch->slot.held);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_EXPECT_FALSE(test, ch->slot.held);
	KUNIT_ASSERT_EQ(test, obmf_core_resume(f->core), 0);
	t = ktime_get();
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 30),
			0);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t), 500);
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
	f->probe_ch = chan(f, CH_IPMI);
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
 * On unplug every waiting request, one per channel up to the last
 * channel, ends -ENODEV at once, not at its timeout
 */
static void obmf_test_unplug_waiters(struct kunit *test)
{
	static const int nr[] = { CH_GPIO, CH_IPMI, CH_IO, CH_MEMDEV };
	struct obmf_fake *f = core_setup(test, 0);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct test_call *c[ARRAY_SIZE(nr)];
	unsigned int sends;
	int i;

	for (i = 0; i < ARRAY_SIZE(nr); i++) {
		fake_push(f, &s);
		sends = f->sends;
		c[i] = call_start(f, call_request, chan(f, nr[i]), 1, 5000);
		KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	}
	f->attached = false;
	fake_unbind(f);
	for (i = 0; i < ARRAY_SIZE(nr); i++) {
		KUNIT_ASSERT_TRUE(test, call_wait(c[i]));
		KUNIT_EXPECT_EQ_MSG(test, c[i]->ret, -ENODEV, "channel %d",
				    nr[i]);
		KUNIT_EXPECT_LT_MSG(test,
				    ktime_ms_delta(c[i]->end, c[i]->start),
				    1000, "channel %d", nr[i]);
	}
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
	KUNIT_EXPECT_GE(test, ktime_ms_delta(f->log[q].t, f->log[i].t),
			200 - 2 * jiffies_to_msecs(1));
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
	/* Its function devices stay: only a failed discovery gives it up */
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 0);

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

/* The cases' drivers bind only a fake transport's I2C device */
static const struct auxiliary_device_id test_slow_ids[] = {
	{ .name = "obmf.test-i2c" },
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

	KUNIT_ASSERT_NULL(test, chan(f, CH_I2C)->fdev->adev.dev.driver);
	KUNIT_ASSERT_EQ(test, auxiliary_driver_register(&test_slow_driver), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, test_slow_unregister,
						  &test_slow_driver),
			0);
	KUNIT_ASSERT_PTR_EQ(test, chan(f, CH_I2C)->fdev->adev.dev.driver,
			    &test_slow_driver.driver);

	fake_push(f, &s);
	fake_push(f, &s);
	sends = f->sends;
	quick = call_start(f, call_request, chan(f, CH_GPIO), 1, 100);
	slow = call_start(f, call_request, chan(f, CH_IO), 1, 5000);
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
	/* The timeout while unbinding is no event */
	KUNIT_EXPECT_EQ(test, f->stop_events[OBMF_EV_NO_RESPONSE], 0);
}

/*
 * A request whose send times out while a function driver's remove holds
 * the core in UNBINDING stops the core as a reply timeout does: a request
 * still waiting ends -ENODEV then, not at the end of remove
 */
static void obmf_test_unbind_send_timeout(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct fake_step never = { .how = FAKE_NEVER, .channel = -1 };
	struct fake_step out = { .send_ret = -ETIMEDOUT };
	struct test_call *quick, *slow;
	unsigned int sends;

	KUNIT_ASSERT_NULL(test, chan(f, CH_I2C)->fdev->adev.dev.driver);
	KUNIT_ASSERT_EQ(test, auxiliary_driver_register(&test_slow_driver), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, test_slow_unregister,
						  &test_slow_driver),
			0);
	KUNIT_ASSERT_PTR_EQ(test, chan(f, CH_I2C)->fdev->adev.dev.driver,
			    &test_slow_driver.driver);

	fake_push(f, &never);
	fake_push(f, &out);
	sends = f->sends;
	slow = call_start(f, call_request, chan(f, CH_IO), 1, 5000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	/* the second send returns its -ETIMEDOUT once the unbind runs */
	WRITE_ONCE(f->send_until_unbind, true);
	quick = call_start(f, call_request, chan(f, CH_GPIO), 1, 5000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 2));
	fake_unbind(f);

	KUNIT_ASSERT_TRUE(test, call_wait(quick));
	KUNIT_ASSERT_TRUE(test, call_wait(slow));
	KUNIT_EXPECT_EQ(test, quick->ret, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, slow->ret, -ENODEV);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(slow->end, slow->start), 500);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_STOP, -1, -1), 1);
}

/*
 * A live core that fails, then is quiesced and resumed, stays failed and
 * keeps its function devices: the transport is quiesced and resumed, and
 * nothing gives the device up. The same for one that fails while
 * quiesced and whose transport resumes.
 */
static void obmf_test_pm_failed(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);

	obmf_core_fatal(f->core, -EIO);
	KUNIT_ASSERT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_QUIESCE, -1, -1), 1);
	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_RESUME, -1, -1), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 0);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, CH_I2C)->fdev);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_FATAL_ERRORS), 1);

	f = core_setup(test, 0);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	obmf_core_fatal(f->core, -EIO);
	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 0);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, CH_I2C)->fdev);
}

/* The hold driver's remove waits until the case lets it go */
static DECLARE_COMPLETION(test_hold_entered);
static DECLARE_COMPLETION(test_hold_go);

static int test_hold_probe(struct auxiliary_device *adev,
			   const struct auxiliary_device_id *id)
{
	return 0;
}

static void test_hold_remove(struct auxiliary_device *adev)
{
	complete(&test_hold_entered);
	wait_for_completion_timeout(&test_hold_go,
				    msecs_to_jiffies(FAKE_WAIT_MS));
}

static struct auxiliary_driver test_hold_driver = {
	.name = "test-hold",
	.probe = test_hold_probe,
	.remove = test_hold_remove,
	.id_table = test_slow_ids,
};

struct test_unbind_call {
	struct work_struct work;
	struct obmf_fake *f;
};

static void test_unbind_work(struct work_struct *work)
{
	struct test_unbind_call *u =
		container_of(work, struct test_unbind_call, work);

	fake_unbind(u->f);
}

/*
 * A quiesce and a resume while an unbind waits for a function driver's
 * remove leave the core UNBINDING: the unbind goes on to stop the
 * transport once, and nothing gives the device up
 */
static void obmf_test_pm_unbinding(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_core *core = f->core;
	struct test_unbind_call u = { .f = f };
	int state, q, r;

	KUNIT_ASSERT_NULL(test, chan(f, CH_I2C)->fdev->adev.dev.driver);
	reinit_completion(&test_hold_entered);
	reinit_completion(&test_hold_go);
	KUNIT_ASSERT_EQ(test, auxiliary_driver_register(&test_hold_driver), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, test_slow_unregister,
						  &test_hold_driver),
			0);
	KUNIT_ASSERT_PTR_EQ(test, chan(f, CH_I2C)->fdev->adev.dev.driver,
			    &test_hold_driver.driver);

	INIT_WORK_ONSTACK(&u.work, test_unbind_work);
	queue_work(f->callwq, &u.work);
	if (!wait_for_completion_timeout(&test_hold_entered,
					 msecs_to_jiffies(FAKE_WAIT_MS))) {
		complete(&test_hold_go);
		flush_work(&u.work);
		destroy_work_on_stack(&u.work);
		KUNIT_FAIL_AND_ABORT(test, "the unbind never reached remove");
	}
	q = obmf_core_quiesce(core);
	r = obmf_core_resume(core);
	state = obmf_core_state(core);
	complete(&test_hold_go);
	flush_work(&u.work);
	destroy_work_on_stack(&u.work);

	KUNIT_EXPECT_EQ(test, q, 0);
	KUNIT_EXPECT_EQ(test, r, 0);
	KUNIT_EXPECT_EQ(test, state, OBMF_UNBINDING);
	KUNIT_EXPECT_NULL(test, f->core);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_STOP, -1, -1), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 0);
}

#define TEST_FLOOD 8

/* The fake whose channel the flood driver's unbind floods */
static struct obmf_fake *test_flood_fake;

static void test_flood(void *data)
{
	static const u8 other[] = { 0x01, 0x00 };
	struct obmf_fdev *fdev = data;
	struct obmf_fake *f = test_flood_fake;
	int i;

	WRITE_ONCE(f->answer_ret, -ETIMEDOUT);
	for (i = 0; i < TEST_FLOOD; i++)
		fake_inject(f, fdev->info->number, 0, other, sizeof(other));
}

static int test_flood_probe(struct auxiliary_device *adev,
			    const struct auxiliary_device_id *id)
{
	struct obmf_fdev *fdev = to_obmf_fdev(adev);
	int ret;

	ret = devm_obmf_enable(fdev);
	if (ret)
		return ret;
	return devm_add_action_or_reset(&adev->dev, test_flood, fdev);
}

static struct auxiliary_driver test_flood_driver = {
	.name = "test-flood",
	.probe = test_flood_probe,
	.id_table = test_slow_ids,
};

/*
 * Device requests that keep coming in an attached unbind, their answers
 * timing out: the first timeout stops the core, and no answer is sent
 * after it, so the unbind does not wait a timeout per request
 */
static void obmf_test_unbind_answers(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_fdev *fdev = chan(f, CH_I2C)->fdev;
	int ret, rx, answers;

	KUNIT_ASSERT_NULL(test, fdev->adev.dev.driver);
	test_flood_fake = f;
	ret = auxiliary_driver_register(&test_flood_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, test_slow_unregister,
					&test_flood_driver);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_PTR_EQ(test, fdev->adev.dev.driver,
			    &test_flood_driver.driver);

	rx = fake_count(f, FAKE_RX, CH_I2C, -1);
	answers = fake_count(f, FAKE_SEND, CH_I2C, 1);
	fake_unbind(f);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_RX, CH_I2C, -1),
			rx + TEST_FLOOD);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 1), answers + 1);
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

/* The answers sent on channel @nr with header status @status */
static int answers_with(struct obmf_fake *f, int nr, u8 status)
{
	int i = -1, n = 0;

	while ((i = fake_find(f, i + 1, FAKE_SEND, nr, 1)) >= 0)
		n += (f->log[i].msg[1] & OBMF_HDR_STATUS) == status;
	return n;
}

/* The request the keep driver kept last */
static struct obmf_dreq *test_keep_dreq;

static int test_keep_request(struct obmf_fdev *fdev, struct obmf_dreq *dreq)
{
	WRITE_ONCE(test_keep_dreq, dreq);
	return -EINPROGRESS;
}

static const struct obmf_fdev_ops test_keep_ops = {
	.device_request = test_keep_request,
};

static int test_keep_probe(struct auxiliary_device *adev,
			   const struct auxiliary_device_id *id)
{
	struct obmf_fdev *fdev = to_obmf_fdev(adev);
	int ret;

	ret = devm_obmf_enable(fdev);
	if (ret)
		return ret;
	return devm_obmf_publish(fdev, &test_keep_ops);
}

/* Binds only a fake transport's GPIO device */
static const struct auxiliary_device_id test_keep_ids[] = {
	{ .name = "obmf.test-gpio" },
	{}
};

static struct auxiliary_driver test_keep_driver = {
	.name = "test-keep",
	.probe = test_keep_probe,
	.id_table = test_keep_ids,
};

/*
 * A driver unbound while it keeps a device request due an answer: the
 * unbind answers it, once, with the default answer
 */
static void obmf_test_unbind_kept(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_fdev *fdev = ch->fdev;
	struct obmf_dreq *dreq;
	struct fake_rec *r;
	u8 msg[16];

	KUNIT_ASSERT_NOT_NULL(test, fdev);
	KUNIT_ASSERT_NULL(test, fdev->adev.dev.driver);
	WRITE_ONCE(test_keep_dreq, NULL);
	KUNIT_ASSERT_EQ(test, auxiliary_driver_register(&test_keep_driver), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, test_slow_unregister,
						  &test_keep_driver),
			0);
	KUNIT_ASSERT_PTR_EQ(test, fdev->adev.dev.driver,
			    &test_keep_driver.driver);

	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	dreq = READ_ONCE(test_keep_dreq);
	KUNIT_ASSERT_NOT_NULL(test, dreq);
	KUNIT_EXPECT_TRUE(test, dreq->answer_due);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 0);

	device_release_driver(&fdev->adev.dev);
	WRITE_ONCE(test_keep_dreq, NULL);
	KUNIT_EXPECT_NULL(test, fdev->adev.dev.driver);
	KUNIT_EXPECT_FALSE(test, ch->bound);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 1);
	r = last_answer(f, CH_GPIO);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, r->msg[4], FAKE_GPIO_INTERRUPT_NOTIFY);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
}

/* What the channel unbind cases' driver does in its teardown, and saw */
struct test_ub {
	struct obmf_fake *f;
	bool keep;
	unsigned int nreq;
	bool io;
	int ret[3];
	s64 ms[3];
	int errno_after;
	int io_ret;
};

static struct test_ub *test_ub_seen;

/*
 * Between unpublish and disable: @nreq requests on its channel, then
 * with @io one on another channel; once
 */
static void test_ub_teardown(void *data)
{
	struct obmf_fdev *fdev = data;
	struct test_ub *u = test_ub_seen;
	unsigned int i, n = u->nreq;
	ktime_t start;
	size_t rlen;
	u8 resp[8];

	u->nreq = 0;
	for (i = 0; i < n && i < ARRAY_SIZE(u->ret); i++) {
		start = ktime_get();
		u->ret[i] = obmf_request(fdev, 0x01, NULL, 0, resp,
					 sizeof(resp), &rlen, 0);
		u->ms[i] = ktime_ms_delta(ktime_get(), start);
		if (!i)
			u->errno_after = obmf_fdev_errno(fdev);
	}
	if (u->io)
		u->io_ret = obmf_ch_request(u->f->core->chans[CH_IO], 0x01,
					    NULL, 0, resp, sizeof(resp), &rlen,
					    0);
	u->io = false;
}

static int test_ub_probe(struct auxiliary_device *adev,
			 const struct auxiliary_device_id *id)
{
	struct obmf_fdev *fdev = to_obmf_fdev(adev);
	const struct obmf_fdev_ops *ops = test_ub_seen->keep ? &test_keep_ops :
							       NULL;
	int ret;

	ret = devm_obmf_enable(fdev);
	if (!ret)
		ret = devm_add_action_or_reset(&adev->dev, test_ub_teardown,
					       fdev);
	if (!ret)
		ret = devm_obmf_publish(fdev, ops);
	return ret;
}

static struct auxiliary_driver test_ub_driver = {
	.name = "test-unbind",
	.probe = test_ub_probe,
	.id_table = test_slow_ids,
};

/* The core cases' fake with test_ub_driver bound to the I2C channel */
static struct obmf_fake *ub_setup(struct kunit *test, bool keep,
				  struct test_ub **up)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_fdev *fdev = chan(f, CH_I2C)->fdev;
	struct test_ub *u;

	u = kunit_kzalloc(test, sizeof(*u), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, u);
	u->f = f;
	u->keep = keep;
	test_ub_seen = u;
	KUNIT_ASSERT_NOT_NULL(test, fdev);
	KUNIT_ASSERT_NULL(test, fdev->adev.dev.driver);
	KUNIT_ASSERT_EQ(test, auxiliary_driver_register(&test_ub_driver), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, test_slow_unregister,
						  &test_ub_driver),
			0);
	KUNIT_ASSERT_PTR_EQ(test, fdev->adev.dev.driver,
			    &test_ub_driver.driver);
	*up = u;
	return f;
}

/* Unbinds the I2C channel's driver only; its time in ms */
static s64 ub_unbind(struct obmf_fake *f)
{
	struct device *dev = &chan(f, CH_I2C)->fdev->adev.dev;
	ktime_t start = ktime_get();

	device_release_driver(dev);
	KUNIT_EXPECT_NULL(f->test, dev->driver);
	return ktime_ms_delta(ktime_get(), start);
}

/* The bound on one request: a hold, its own wait, and some slack */
#define UB_REQ_MS (2 * OBMF_DEFAULT_TIMEOUT_MS + 300)

/*
 * A function driver unbound while the device has stopped answering, its
 * teardown sending three requests, the first held by an earlier timeout:
 * each is sent and times out on its own, within the hold of the timeout
 * before it and its own wait, and each is reported as any timeout. The
 * core stays live, another channel is answered during and after the
 * unbind, and the next binding's requests are answered.
 */
static void obmf_test_ch_unbind_silent(struct kunit *test)
{
	struct test_ub *u;
	struct obmf_fake *f = ub_setup(test, false, &u);
	struct obmf_channel *ch = chan(f, CH_I2C);
	struct obmf_fdev *fdev = ch->fdev;
	struct test_log *log = test_log_case(test, f->tp.dev);
	long ev = ev_cnt(f->core, OBMF_EV_NO_RESPONSE);
	int sends, i;
	size_t rlen;
	u8 resp[8];

	WRITE_ONCE(f->silent_ch, CH_I2C);
	/* Reported, and the next request waits the hold */
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, NULL, 0, resp, sizeof(resp),
					&rlen, 100),
			-ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_NO_RESPONSE), ev + 1);
	sends = fake_count(f, FAKE_SEND, CH_I2C, 0);
	u->nreq = 3;
	u->io = true;
	KUNIT_EXPECT_LT(test, ub_unbind(f), 3 * UB_REQ_MS);
	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_EQ_MSG(test, u->ret[i], -ETIMEDOUT, "request %d",
				    i);
		KUNIT_EXPECT_LT_MSG(test, u->ms[i], UB_REQ_MS, "request %d", i);
	}
	KUNIT_EXPECT_EQ(test, u->errno_after, 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), sends + 3);
	KUNIT_EXPECT_EQ(test, u->io_ret, 0);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_NO_RESPONSE), ev + 4);
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, "during unbind", 1, 500),
			0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
	KUNIT_EXPECT_EQ(test, obmf_fdev_errno(fdev), 0);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(chan(f, CH_IO), 0x01, NULL, 0, resp,
					sizeof(resp), &rlen, 0),
			0);

	/* The next binding: its requests go out and are answered */
	WRITE_ONCE(f->silent_ch, 0);
	KUNIT_ASSERT_EQ(test, device_attach(&fdev->adev.dev), 1);
	KUNIT_EXPECT_EQ(test, obmf_fdev_errno(fdev), 0);
	KUNIT_EXPECT_EQ(test,
			obmf_request(fdev, 0x01, NULL, 0, resp, sizeof(resp),
				     &rlen, 0),
			0);
}

/*
 * The same with every send timing out: each request fails on its own
 * send's timeout, with no hold, and a device request on the channel still
 * gets its default answer
 */
static void obmf_test_ch_unbind_send_timeout(struct kunit *test)
{
	static const u8 other[] = { 0x01, 0x00 };
	struct test_ub *u;
	struct obmf_fake *f = ub_setup(test, false, &u);
	struct obmf_channel *ch = chan(f, CH_I2C);
	struct test_log *log = test_log_case(test, f->tp.dev);
	long tx_errors = ch_cnt(ch, OBMF_CH_TX_ERRORS);
	int sends = fake_count(f, FAKE_SEND, CH_I2C, 0);
	long answers, dflt;

	fake_push(f, &(struct fake_step){ .send_ret = -ETIMEDOUT });
	fake_push(f, &(struct fake_step){ .send_ret = -ETIMEDOUT });
	u->nreq = 2;
	KUNIT_EXPECT_LT(test, ub_unbind(f), 500);
	KUNIT_EXPECT_EQ(test, u->ret[0], -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, u->errno_after, 0);
	KUNIT_EXPECT_EQ(test, u->ret[1], -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), sends + 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_ERRORS), tx_errors + 2);

	answers = ch_cnt(ch, OBMF_CH_TX_ANSWERS);
	dflt = ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS);
	fake_inject(f, CH_I2C, 0, other, sizeof(other));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), dflt + 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_ANSWERS), answers + 1);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "during unbind"), 0);
}

/*
 * A device that drops one reply during a function driver's unbind and
 * then answers again: the teardown request after the timeout waits the
 * hold, goes out and is answered, as at any time
 */
static void obmf_test_ch_unbind_transient(struct kunit *test)
{
	struct test_ub *u;
	struct obmf_fake *f = ub_setup(test, false, &u);
	struct obmf_channel *ch = chan(f, CH_I2C);
	long ev = ev_cnt(f->core, OBMF_EV_NO_RESPONSE);
	long timeouts = ch_cnt(ch, OBMF_CH_TIMEOUTS);
	int sends = fake_count(f, FAKE_SEND, CH_I2C, 0);

	fake_push(f, &(struct fake_step){ .how = FAKE_NEVER, .channel = -1 });
	u->nreq = 2;
	KUNIT_EXPECT_LT(test, ub_unbind(f), 2 * UB_REQ_MS);
	KUNIT_EXPECT_EQ(test, u->ret[0], -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, u->errno_after, 0);
	KUNIT_EXPECT_EQ(test, u->ret[1], 0);
	KUNIT_EXPECT_LT(test, u->ms[1], UB_REQ_MS);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), sends + 2);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_NO_RESPONSE), ev + 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), timeouts + 1);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
}

/*
 * Two device requests the driver kept when it is unbound: the first
 * default answer's send times out, the second answer is still sent, and
 * a request after the unbind goes out and is answered
 */
static void obmf_test_ch_unbind_answer_timeout(struct kunit *test)
{
	static const u8 other[] = { 0x01, 0x00 };
	struct test_ub *u;
	struct obmf_fake *f = ub_setup(test, true, &u);
	struct obmf_channel *ch = chan(f, CH_I2C);
	struct test_log *log = test_log_case(test, f->tp.dev);
	int answers = fake_count(f, FAKE_SEND, CH_I2C, 1);
	long tx = ch_cnt(ch, OBMF_CH_TX_ANSWERS);
	int sends, i;
	size_t rlen;
	u8 resp[8];

	for (i = 0; i < 2; i++)
		fake_inject(f, CH_I2C, 0, other, sizeof(other));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_EQ(test, dreq_out(ch), 2);
	WRITE_ONCE(f->answer_once, true);
	WRITE_ONCE(f->answer_ret, -ETIMEDOUT);
	ub_unbind(f);
	WRITE_ONCE(test_keep_dreq, NULL);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 1), answers + 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_ANSWERS), tx + 1);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
	sends = fake_count(f, FAKE_SEND, CH_I2C, 0);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), sends + 1);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "during unbind"), 0);
}

/*
 * A function driver unbound while the device answers: every request is
 * answered and nothing is logged. Once it is unbound, an answer whose
 * send times out changes nothing for the next request.
 */
static void obmf_test_ch_unbind_answered(struct kunit *test)
{
	static const u8 other[] = { 0x01, 0x00 };
	struct test_ub *u;
	struct obmf_fake *f = ub_setup(test, false, &u);
	struct obmf_channel *ch = chan(f, CH_I2C);
	struct test_log *log = test_log_case(test, f->tp.dev);
	size_t rlen;
	u8 resp[8];
	int i;

	u->nreq = 3;
	ub_unbind(f);
	for (i = 0; i < 3; i++)
		KUNIT_EXPECT_EQ(test, u->ret[i], 0);
	KUNIT_EXPECT_EQ(test, u->errno_after, 0);

	WRITE_ONCE(f->answer_once, true);
	WRITE_ONCE(f->answer_ret, -ETIMEDOUT);
	fake_inject(f, CH_I2C, 0, other, sizeof(other));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->answer_ret), 0);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			0);
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, "during unbind", 1, 500),
			0);
}

/*
 * The interface unbound with the driver bound and the device silent: the
 * core stops at the first timeout, with one line, and the next request
 * fails -ENODEV at once
 */
static void obmf_test_ch_unbind_interface(struct kunit *test)
{
	struct test_ub *u;
	struct obmf_fake *f = ub_setup(test, false, &u);
	struct test_log *log = test_log_case(test, f->tp.dev);

	WRITE_ONCE(f->silent_ch, CH_I2C);
	u->nreq = 2;
	fake_unbind(f);
	KUNIT_EXPECT_EQ(test, u->ret[0], -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, u->ret[1], -ENODEV);
	KUNIT_EXPECT_LT(test, u->ms[1], 300);
	KUNIT_EXPECT_EQ(test,
			test_log_wait(test, log, "device stopped", 1, 1000), 1);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "during unbind"), 1);
}

/* The other channels that send while one channel's driver is unbound */
static const int ub_busy_chs[] = { CH_GPIO, CH_IPMI, CH_SPI, CH_SERIAL, CH_IO };

/*
 * The device takes no data, and five other channels send in a loop, each
 * send taking its whole timeout: each request of a channel's unbind
 * still ends within the hold of an earlier timeout plus its own wait, as
 * each send ends within its own timeout, and the other channels go on
 */
static void obmf_test_ch_unbind_busy(struct kunit *test)
{
	struct test_ub *u;
	struct obmf_fake *f = ub_setup(test, false, &u);
	struct obmf_channel *ch = chan(f, CH_I2C);
	int n = ARRAY_SIZE(ub_busy_chs);
	struct test_busy *b;
	size_t rlen;
	u8 resp[8];
	s64 ms;
	int i;

	/* A reply timeout first: the unbind's first request waits the hold */
	WRITE_ONCE(f->silent_ch, CH_I2C);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, NULL, 0, resp, sizeof(resp),
					&rlen, 100),
			-ETIMEDOUT);
	WRITE_ONCE(f->send_sleep_ret, -ETIMEDOUT);
	WRITE_ONCE(f->send_sleep_ms, OBMF_MAX_TIMEOUT_MS);
	b = test_busy_start(f, ub_busy_chs, n, 0);
	u->nreq = 3;
	ms = ub_unbind(f);
	KUNIT_EXPECT_LT(test, ms, 3 * UB_REQ_MS);
	for (i = 0; i < 3; i++) {
		KUNIT_EXPECT_EQ_MSG(test, u->ret[i], -ETIMEDOUT, "request %d",
				    i);
		KUNIT_EXPECT_LT_MSG(test, u->ms[i], UB_REQ_MS, "request %d", i);
	}
	test_busy_end(f, b, n);
	WRITE_ONCE(f->send_sleep_ms, 0);
	WRITE_ONCE(f->send_sleep_ret, 0);
	for (i = 0; i < n; i++) {
		KUNIT_EXPECT_EQ(test,
				obmf_ch_request(b[i].ch, 0x01, NULL, 0, resp,
						sizeof(resp), &rlen, 0),
				0);
	}
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
}

/*
 * A driver that answers a device request and then returns -EINPROGRESS:
 * the answer is the only one sent, and the request is not kept
 */
static void obmf_test_answer_then_keep(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	bool kept;
	u8 msg[16];

	fn->answer_keep = true;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), 1);
	KUNIT_EXPECT_EQ(test, fn->answer_ret, 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 1);
	KUNIT_EXPECT_EQ(test, answers_with(f, CH_GPIO, OBMF_STATUS_SUCCESS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 0);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
	scoped_guard(spinlock_irqsave, &ch->dreq_lock)
		kept = !list_empty(&ch->kept);
	KUNIT_EXPECT_FALSE(test, kept);
}

/*
 * Table 22: a serial device request is a command byte and a length, then
 * at least that many bytes. A shorter one is malformed and answered
 * invalid; one with its bytes, or more, is delivered (here: no driver, so
 * the default answer). An answer of the channel's request limit, echo
 * byte included, is sent; one byte more is refused -EMSGSIZE.
 */
static void obmf_test_dreq_len_bounds(struct kunit *test)
{
	static const u8 none[] = { 0x00, 0x02 };
	static const u8 short_by_one[] = { 0x00, 0x02, 0x00, 'o' };
	static const u8 exact[] = { 0x00, 0x01, 0x00, 'o' };
	static const u8 empty[] = { 0x00, 0x00, 0x00 };
	static const u8 more[] = { 0x00, 0x01, 0x00, 'o', 'k' };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_SERIAL);
	struct obmf_channel *gpio = chan(f, CH_GPIO);
	struct obmf_test_fn *fn;
	long malformed, dreqs;
	size_t lim = gpio->info.max_req;
	u8 msg[16], *body;

	KUNIT_ASSERT_NULL(test, ch->fdev->adev.dev.driver);
	malformed = if_cnt(f->core, OBMF_IF_RX_MALFORMED);
	dreqs = ch_cnt(ch, OBMF_CH_RX_DEVICE_REQUESTS);
	fake_inject(f, CH_SERIAL, 0, none, sizeof(none));
	fake_inject(f, CH_SERIAL, 0, short_by_one, sizeof(short_by_one));
	fake_inject(f, CH_SERIAL, 0, exact, sizeof(exact));
	fake_inject(f, CH_SERIAL, 0, empty, sizeof(empty));
	fake_inject(f, CH_SERIAL, 0, more, sizeof(more));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DEVICE_REQUESTS),
			dreqs + 5);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_RX_MALFORMED),
			malformed + 2);
	KUNIT_EXPECT_EQ(test, answers_with(f, CH_SERIAL, OBMF_STATUS_INVALID),
			2);
	KUNIT_EXPECT_EQ(test, answers_with(f, CH_SERIAL, OBMF_STATUS_SUCCESS),
			3);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 5);

	/* the GPIO type echoes the command byte */
	body = kunit_kzalloc(test, lim, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, body);
	fn = test_fn_publish(f, gpio);
	fn->keep = true;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_NOT_NULL(test, fn->kept);
	KUNIT_EXPECT_EQ(test,
			obmf_respond(&fn->fdev, fn->kept, OBMF_STATUS_SUCCESS,
				     body, lim),
			-EMSGSIZE);
	fn->kept = NULL;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_NOT_NULL(test, fn->kept);
	KUNIT_EXPECT_EQ(test,
			obmf_respond(&fn->fdev, fn->kept, OBMF_STATUS_SUCCESS,
				     body, lim - 1),
			0);
	KUNIT_EXPECT_EQ(test, last_answer(f, CH_GPIO)->len, OBMF_HDR_LEN + lim);
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
 * dreq_max_depth is the most device request records in use at
 * once: one for requests answered in turn, all but the one dropped in a
 * flood, and no fewer once they are answered
 */
static void obmf_test_dreq_max_depth(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	int n = OBMF_DREQ_RESERVE + OBMF_DREQ_QUEUE_MAX + 1, i;
	u8 msg[16];

	for (i = 0; i < 3; i++) {
		fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
		flush_workqueue(f->core->wq);
	}
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DREQ_MAX_DEPTH), 1);

	fn->hold = true;
	for (i = 0; i < n; i++)
		fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	KUNIT_ASSERT_TRUE(test, test_wait(&fn->entered));
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DREQ_MAX_DEPTH), n - 1);

	fn->hold = false;
	complete(&fn->release);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DREQ_MAX_DEPTH), n - 1);
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
 * The default answers with no driver bound, or one that published no
 * operations (I2C, when i2c-obmf is built in): a serial write is refused
 * with nothing taken, a GPIO interrupt acknowledged, a Set Values from a
 * GPIO Producer answered invalid, anything else answered busy
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
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_INVALID);
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
 * Every GPIO command with no driver to take it, on each role. Table 16:
 * a Producer requests interrupts only, so its commands 0 to 3 are
 * invalid; from a Consumer or a symmetric device they are busy. Table 18:
 * 6 to 0xff are reserved, invalid on every role. An interrupt is
 * acknowledged; a datagram gets no answer.
 */
static void obmf_test_default_answer_gpio(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio-0", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_CONSUMER, "gpio-1", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_SYMMETRIC, "gpio-2", 516 },
	};
	static const struct {
		u8 cmd;
		u8 status[3];
	} want[] = {
		{ 0x00,
		  { OBMF_STATUS_INVALID, OBMF_STATUS_BUSY, OBMF_STATUS_BUSY } },
		{ 0x01,
		  { OBMF_STATUS_INVALID, OBMF_STATUS_BUSY, OBMF_STATUS_BUSY } },
		{ 0x02,
		  { OBMF_STATUS_INVALID, OBMF_STATUS_BUSY, OBMF_STATUS_BUSY } },
		{ 0x03,
		  { OBMF_STATUS_INVALID, OBMF_STATUS_BUSY, OBMF_STATUS_BUSY } },
		{ FAKE_GPIO_INTERRUPT_NOTIFY,
		  { OBMF_STATUS_SUCCESS, OBMF_STATUS_SUCCESS,
		    OBMF_STATUS_SUCCESS } },
		{ 0x06,
		  { OBMF_STATUS_INVALID, OBMF_STATUS_INVALID,
		    OBMF_STATUS_INVALID } },
		{ 0xff,
		  { OBMF_STATUS_INVALID, OBMF_STATUS_INVALID,
		    OBMF_STATUS_INVALID } },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	u8 req[2] = { 0, 0x01 };
	struct fake_rec *r;
	int i, k;

	fake_layout(f, c, ARRAY_SIZE(c));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	for (i = 0; i < ARRAY_SIZE(want); i++) {
		req[0] = want[i].cmd;
		for (k = 0; k < ARRAY_SIZE(c); k++) {
			fake_inject(f, k + 1, 0, req, sizeof(req));
			flush_workqueue(f->core->wq);
			r = last_answer(f, k + 1);
			KUNIT_EXPECT_EQ_MSG(test, r->msg[1],
					    OBMF_HDR_RQRESP | want[i].status[k],
					    "command %#x channel %d",
					    want[i].cmd, k + 1);
			KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 1);
			KUNIT_EXPECT_EQ(test, r->msg[4], want[i].cmd);
			KUNIT_EXPECT_EQ(test,
					ch_cnt(chan(f, k + 1),
					       OBMF_CH_DEFAULT_ANSWERS),
					i + 1);
		}
	}
	fake_inject(f, 1, 0, (const u8[]){ FAKE_GPIO_DATAGRAM, 0x01 }, 2);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_DEFAULT_ANSWERS),
			ARRAY_SIZE(want));
}

/*
 * A serial write whose count runs past its data, or an empty IPMI request,
 * is malformed and answered invalid with no body but IPMI's echo, so the
 * device does not send it again; an empty GPIO request gets no answer,
 * and a well-formed write after them still gets the NACK
 */
static void obmf_test_dreq_malformed(struct kunit *test)
{
	static const u8 write[] = { 0x00, 0x03, 0x00, 'o', 'k' };
	static const u8 good[] = { 0x00, 0x02, 0x00, 'o', 'k' };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ser = chan(f, CH_SERIAL);
	struct fake_rec *r;

	fake_inject(f, CH_SERIAL, 0, write, sizeof(write));
	fake_inject(f, CH_IPMI, 0, NULL, 0);
	fake_inject(f, CH_GPIO, 0, NULL, 0);
	flush_workqueue(f->core->wq);

	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_RX_MALFORMED), 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(ser, OBMF_CH_PROTOCOL_ERRORS), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(ser, OBMF_CH_DEFAULT_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_SERIAL, 1), 1);
	r = last_answer(f, CH_SERIAL);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_INVALID);
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN);
	r = last_answer(f, CH_IPMI);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_INVALID);
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, CH_GPIO), OBMF_CH_DEFAULT_ANSWERS),
			0);

	fake_inject(f, CH_SERIAL, 0, good, sizeof(good));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_SERIAL, 1), 2);
	r = last_answer(f, CH_SERIAL);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, r->len, OBMF_HDR_LEN + 3);
	KUNIT_EXPECT_EQ(test, r->msg[4], FAKE_SERIAL_NACK);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_RX_MALFORMED), 3);
}

/*
 * A driver that returns 0 without answering: a request due an answer gets
 * the default answer, a datagram none
 */
static void obmf_test_dreq_unanswered(struct kunit *test)
{
	static const u8 dgram[] = { FAKE_GPIO_DATAGRAM, 0x01 };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	struct fake_rec *r;
	u8 msg[16];

	fn->ignore = true;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), 1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 1);
	r = last_answer(f, CH_GPIO);
	KUNIT_EXPECT_EQ(test, r->msg[1], OBMF_HDR_RQRESP | OBMF_STATUS_SUCCESS);
	KUNIT_EXPECT_EQ(test, r->msg[4], FAKE_GPIO_INTERRUPT_NOTIFY);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);

	fake_inject(f, CH_GPIO, 0, dgram, sizeof(dgram));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->calls), 2);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
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

/*
 * A device request whose work runs once the transport has stopped gets
 * no answer: the core starts none, and nothing is counted as an error
 */
static void obmf_test_stop_default_answer(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);

	f->stop_dreq_ch = chan(f, CH_TPM);
	fake_unbind(f);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->sends_after_stop), 0);
	KUNIT_EXPECT_EQ(test, f->stop_dreq_answers, 0);
	KUNIT_EXPECT_EQ(test, f->stop_dreq_errors, 0);
}

static struct kunit_case obmf_core_cases[] = {
	KUNIT_CASE(obmf_test_request_encode),
	KUNIT_CASE(obmf_test_request_lengths),
	KUNIT_CASE(obmf_test_echo_masks),
	KUNIT_CASE(obmf_test_mmio_forms),
	KUNIT_CASE(obmf_test_status_errno),
	KUNIT_CASE(obmf_test_status_types),
	KUNIT_CASE(obmf_test_gpio_values),
	KUNIT_CASE(obmf_test_wait_clamp),
	KUNIT_CASE(obmf_test_wait_timeout),
	KUNIT_CASE_SLOW(obmf_test_wait_floor),
	KUNIT_CASE(obmf_test_wait_send_late),
	KUNIT_CASE(obmf_test_reply_contexts),
	KUNIT_CASE(obmf_test_first_reply_wins),
	KUNIT_CASE(obmf_test_late_reply),
	KUNIT_CASE_SLOW(obmf_test_late_hold),
	KUNIT_CASE(obmf_test_reply_other_channel),
	KUNIT_CASE(obmf_test_reply_at_expiry),
	KUNIT_CASE(obmf_test_stale_completion),
	KUNIT_CASE(obmf_test_send_errors),
	KUNIT_CASE(obmf_test_rx_malformed),
	KUNIT_CASE(obmf_test_rx_before_publish),
	KUNIT_CASE(obmf_test_late_reply_limit),
	KUNIT_CASE(obmf_test_late_reply_resume),
	KUNIT_CASE(obmf_test_fatal),
	KUNIT_CASE(obmf_test_stopped),
	KUNIT_CASE(obmf_test_unplug_waiters),
	KUNIT_CASE(obmf_test_quiesce_armed_reply),
	KUNIT_CASE(obmf_test_quiesce_armed_timeout),
	KUNIT_CASE(obmf_test_quiesce_new_request),
	KUNIT_CASE(obmf_test_quiesce_dreq_held),
	KUNIT_CASE(obmf_test_quiesce_delivery),
	KUNIT_CASE(obmf_test_resume_fails),
	KUNIT_CASE(obmf_test_unbind_timeout),
	KUNIT_CASE(obmf_test_unbind_send_timeout),
	KUNIT_CASE(obmf_test_unbind_answers),
	KUNIT_CASE(obmf_test_dreq_flood),
	KUNIT_CASE(obmf_test_dreq_datagrams),
	KUNIT_CASE(obmf_test_dreq_max_depth),
	KUNIT_CASE(obmf_test_default_answers),
	KUNIT_CASE(obmf_test_default_answer_roles),
	KUNIT_CASE(obmf_test_default_answer_gpio),
	KUNIT_CASE(obmf_test_dreq_malformed),
	KUNIT_CASE(obmf_test_dreq_unanswered),
	KUNIT_CASE(obmf_test_respond_edges),
	KUNIT_CASE(obmf_test_stop_default_answer),
	KUNIT_CASE(obmf_test_unbind_kept),
	KUNIT_CASE_SLOW(obmf_test_ch_unbind_silent),
	KUNIT_CASE(obmf_test_ch_unbind_send_timeout),
	KUNIT_CASE_SLOW(obmf_test_ch_unbind_transient),
	KUNIT_CASE(obmf_test_ch_unbind_answer_timeout),
	KUNIT_CASE(obmf_test_ch_unbind_answered),
	KUNIT_CASE(obmf_test_ch_unbind_interface),
	KUNIT_CASE_SLOW(obmf_test_ch_unbind_busy),
	KUNIT_CASE(obmf_test_answer_then_keep),
	KUNIT_CASE(obmf_test_dreq_len_bounds),
	KUNIT_CASE(obmf_test_pm_failed),
	KUNIT_CASE(obmf_test_pm_unbinding),
	{}
};

static struct kunit_suite obmf_core_suite = {
	.name = "obmf-core",
	.test_cases = obmf_core_cases,
};

/* Discovery */

static const struct test_chan disc_chans[] = {
	{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-0", 72, 0, 2 },
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

/*
 * Milliseconds between the header reads that start passes @a and @b. The
 * wait between passes is counted in jiffies, so on a busy machine it can
 * read up to one jiffy short of the time asked.
 */
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

	/* A mask of 0 changes nothing and sends nothing */
	i = READ_ONCE(f->sends);
	KUNIT_EXPECT_EQ(test, obmf_ch_control_update(fdev, 0, 0xff), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), i);
	KUNIT_EXPECT_EQ(test, h[OBMF_CH_REG_SPECIFIC_CONTROL], 0xa3);
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
 * channels bind. Expected, from i2c-obmf: "channel limits 4/4 too small"
 * for i2c-8, which is kept and refused by its driver.
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
 * A channel whose header is at 0xffffffe0, so its 64 bytes of
 * configuration would run past the 32-bit space; the other channels bind.
 * Expected: "channel 2: bad layout at 0xffffffe0, ignored".
 */
static void obmf_test_discover_wrap(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "low", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "wrap", 516, 0, 64 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "high", 516 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	const u32 off = 0xffffffe0;
	u8 *m = f->mem;

	fake_layout(f, c, ARRAY_SIZE(c));
	memcpy(m + f->mem_len - FAKE_CH_HDR_LEN, m + fake_ch_off(f, 2),
	       FAKE_CH_HDR_LEN);
	put_unaligned_le32(off, m + FAKE_CH0_OFFSETS + 4);
	f->hi_base = off;

	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, disc_reads_at(f, off), 1);
	KUNIT_EXPECT_NULL(test, f->core->chans[2]);
	KUNIT_EXPECT_STREQ(test, chan(f, 1)->info.name, "low");
	KUNIT_EXPECT_STREQ(test, chan(f, 3)->info.name, "high");
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
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c", 516, 0,
		  2 },
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
 * A device that lists 30 unusable channels gets OBMF_BIND_WARN_MAX of
 * their warnings and one line for the rest, at every bind; a second
 * device bound at once gets its own
 */
static void obmf_test_discover_unusable_many(struct kunit *test)
{
	static const char more[] = " 20 more configuration warnings not logged";
	const int n = 30;
	struct obmf_fake *f;
	struct test_log *log;
	struct test_chan *c;
	int i, dev;

	c = kunit_kcalloc(test, n + 1, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c);
	for (i = 0; i <= n; i++)
		c[i] = (struct test_chan){ OBMF_TYPE_SERIAL,
					   OBMF_ROLE_PRODUCER,
					   "off",
					   516,
					   FAKE_OFF_ENABLE,
					   0 };
	c[n].name = "uart";
	c[n].off = 0;
	c[n].config_len = 64;
	for (dev = 0; dev < 2; dev++) {
		f = fake_create(test, FAKE_MAX_MSG, 0);
		log = test_log_start(test, f->tp.dev);
		fake_layout(f, c, n + 1);
		KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
		for (i = 1; i <= n; i++)
			KUNIT_EXPECT_NULL_MSG(test, f->core->chans[i],
					      "channel %d", i);
		KUNIT_EXPECT_NOT_NULL(test, chan(f, n + 1)->fdev);
		KUNIT_EXPECT_EQ(test,
				test_log_lines(test, log, ": not enabled"),
				OBMF_BIND_WARN_MAX);
		KUNIT_EXPECT_EQ(test, test_log_lines(test, log, more), 1);
		KUNIT_EXPECT_EQ(test, f->core->bind_warns, n);
	}
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
	/* Probe has returned: a failed discovery leaves the core failed */
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
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

/*
 * a read the device answers with its timeout status in the first pass
 * only: that pass is run again, and no timeout is counted
 */
static void obmf_test_discover_status_once(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->status_read = 2;
	KUNIT_EXPECT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_STATUS_ERRORS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_TIMEOUTS), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
}

/*
 * A device that never answers: probe returns before the first wait could
 * end, then three passes time out and leave the core failed
 */
static void obmf_test_discover_timeout_always(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	ktime_t t;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_hdr = true;
	t = ktime_get();
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t),
			OBMF_DEFAULT_TIMEOUT_MS / 2);
	flush_work(&f->core->bind_work);
	KUNIT_EXPECT_EQ(test, f->core->bind_err, -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_TIMEOUTS), 3);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
}

/*
 * An unbind during discovery ends it at its pending read: no pass
 * again, no wait for the reply timeout, nothing sent after it
 */
static void obmf_test_discover_unbind(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	ktime_t t;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_hdr = true;
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, 1));
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_INIT);
	t = ktime_get();
	fake_unbind(f);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t),
			OBMF_DEFAULT_TIMEOUT_MS / 2);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 1);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), 1);
}

/*
 * A suspend during discovery ends it at its pending read; resume runs it
 * again, and the function devices come then
 */
static void obmf_test_discover_suspend(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	ktime_t t;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_hdr = true;
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, 1));
	t = ktime_get();
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t),
			OBMF_DEFAULT_TIMEOUT_MS / 2);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_QUIESCED);
	KUNIT_EXPECT_EQ(test, f->core->bind_err, -EAGAIN);
	KUNIT_EXPECT_FALSE(test, f->core->table);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 0);

	WRITE_ONCE(f->drop_hdr, false);
	KUNIT_ASSERT_EQ(test, obmf_core_resume(f->core), 0);
	flush_work(&f->core->bind_work);
	KUNIT_EXPECT_EQ(test, f->core->bind_err, 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
	KUNIT_EXPECT_EQ(test, f->hdr_reads, 3);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 1)->fdev);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 2)->fdev);
}

/*
 * An unbind or a suspend while discovery waits between two passes ends
 * the wait at once: no second pass, no wait for the rest of it
 */
static void obmf_test_discover_settle_ends(struct kunit *test)
{
	struct obmf_fake *f;
	ktime_t t;
	int how;

	for (how = 0; how < 2; how++) {
		f = fake_create(test, FAKE_MAX_MSG, 0);
		fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
		/* the first read gets the timeout status: the pass is retried */
		f->status_read = 1;
		KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
		KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, 1));
		msleep(OBMF_DEFAULT_TIMEOUT_MS / 10);
		t = ktime_get();
		if (how) {
			KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
			KUNIT_EXPECT_EQ(test, f->core->bind_err, -EAGAIN);
		} else {
			fake_unbind(f);
		}
		KUNIT_EXPECT_LT_MSG(test, ktime_ms_delta(ktime_get(), t),
				    OBMF_DEFAULT_TIMEOUT_MS / 2, "how %d", how);
		KUNIT_EXPECT_EQ_MSG(test, READ_ONCE(f->sends), 1, "how %d",
				    how);
	}
}

static int fake_count_child(struct device *dev, void *data)
{
	(*(int *)data)++;
	return 0;
}

/* The devices below the transport's: the function devices */
static int fake_children(struct obmf_fake *f)
{
	int n = 0;

	device_for_each_child(f->tp.dev, &n, fake_count_child);
	return n;
}

static bool fake_wait_done(struct completion *done)
{
	return wait_for_completion_timeout(done,
					   msecs_to_jiffies(FAKE_WAIT_MS));
}

/*
 * A failed discovery gives the device up: the release is asked once, with
 * the core failed and no function device, and the unbind it starts runs
 * once the device lock is free
 */
static void obmf_test_discover_fail_release(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	bool entered;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->read_ret = -ENODEV;
	f->release_unbinds = true;
	f->release_hold = true;
	/* No assertion under the lock: the cleanup would wait for it */
	mutex_lock(&f->dev_lock);
	KUNIT_EXPECT_EQ(test, fake_probe(f), 0);
	entered = fake_wait_done(&f->release_entered);
	KUNIT_EXPECT_TRUE(test, entered);
	if (entered) {
		KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
		KUNIT_EXPECT_EQ(test, f->core->bind_err, -ENODEV);
		KUNIT_EXPECT_EQ(test, fake_children(f), 0);
	}
	complete(&f->release_go);
	mutex_unlock(&f->dev_lock);

	KUNIT_ASSERT_TRUE(test, fake_wait_done(&f->release_done));
	KUNIT_EXPECT_EQ(test, f->released, 1);
	KUNIT_EXPECT_NULL(test, f->core);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
	KUNIT_EXPECT_EQ(test, fake_children(f), 0);
}

/*
 * An unbind racing a failed discovery. Before the failure, it ends the
 * discovery and no release is asked; after it, the release finds the
 * interface unbound. The unbind holds the device lock while it waits for
 * the bind work, so the bind work never takes that lock.
 */
static void obmf_test_discover_fail_unbind(struct kunit *test)
{
	struct obmf_fake *f;
	bool entered;

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_hdr = true;
	f->release_unbinds = true;
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, 1));
	scoped_guard(mutex, &f->dev_lock)
		fake_unbind(f);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 0);

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->read_ret = -ENODEV;
	f->release_unbinds = true;
	f->release_hold = true;
	mutex_lock(&f->dev_lock);
	KUNIT_EXPECT_EQ(test, fake_probe(f), 0);
	entered = fake_wait_done(&f->release_entered);
	KUNIT_EXPECT_TRUE(test, entered);
	/* the release work is queued and waits for the lock */
	complete(&f->release_go);
	fake_unbind(f);
	mutex_unlock(&f->dev_lock);
	KUNIT_ASSERT_TRUE(test, entered);
	KUNIT_ASSERT_TRUE(test, fake_wait_done(&f->release_done));
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
	KUNIT_EXPECT_EQ(test, f->released, 0);
}

/*
 * A discovery that fails gives the device up; a new bind runs discovery
 * again (the first read only fails), it succeeds, and no release is asked
 */
static void obmf_test_discover_fail_rebind(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	unsigned int reads;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->read_ret = -ENODEV;
	f->release_unbinds = true;
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	KUNIT_ASSERT_TRUE(test, fake_wait_done(&f->release_done));
	KUNIT_ASSERT_NULL(test, f->core);
	KUNIT_EXPECT_EQ(test, f->released, 1);
	reads = f->hdr_reads;

	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
	KUNIT_EXPECT_GT(test, f->hdr_reads, reads);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 1)->fdev);
	KUNIT_EXPECT_GT(test, fake_children(f), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
}

/*
 * A suspend that ends discovery, then a resume that fails: the core is
 * failed with no function device, and the resume gives the device up while
 * it holds the device lock, as the PM core does; the unbind runs once the
 * lock is free. A core that failed while quiesced is given up by a resume
 * that succeeds; one whose discovery failed before the suspend was given up
 * then, and a failed resume asks no second release.
 */
static void obmf_test_discover_suspend_fail(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	bool done;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_hdr = true;
	f->release_unbinds = true;
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, 1));
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 0);
	f->resume_ret = -EPROTO;
	/* No assertion under the lock: the cleanup would wait for it */
	mutex_lock(&f->dev_lock);
	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), -EPROTO);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, fake_children(f), 0);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
	done = completion_done(&f->release_done);
	mutex_unlock(&f->dev_lock);
	KUNIT_EXPECT_FALSE(test, done);
	KUNIT_ASSERT_TRUE(test, fake_wait_done(&f->release_done));
	KUNIT_EXPECT_EQ(test, f->released, 1);
	KUNIT_EXPECT_NULL(test, f->core);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
	KUNIT_EXPECT_EQ(test, fake_children(f), 0);

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_hdr = true;
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, 1));
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	obmf_core_fatal(f->core, -EIO);
	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_state(f->core), OBMF_FAILED);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->read_ret = -ENODEV;
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	flush_work(&f->core->bind_work);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	f->resume_ret = -EPROTO;
	KUNIT_EXPECT_EQ(test, obmf_core_resume(f->core), -EPROTO);
	KUNIT_EXPECT_EQ(test, atomic_read(&f->releases), 1);
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
	KUNIT_EXPECT_GE(test, disc_gap(f, 1, 2),
			OBMF_DEFAULT_TIMEOUT_MS - jiffies_to_msecs(1));
	KUNIT_EXPECT_GE(test, disc_gap(f, 3, 4),
			OBMF_DEFAULT_TIMEOUT_MS - jiffies_to_msecs(1));
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
	KUNIT_EXPECT_GE(test, disc_gap(f, 0, 1),
			OBMF_DEFAULT_TIMEOUT_MS - jiffies_to_msecs(1));

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
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "I2C", 516, 0,
		  2 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "UART", 516 },
	};
	static const struct test_chan gap[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "I2C", 516, 0,
		  2 },
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
			   "OBMF-ICP 1.0 device 'obmf-test': I2C ch1 'i2c-0', serial ch2 'uart\xc3\xa9', GPIO ch3 ''");

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
			   "OBMF-ICP 1.12 device 'obmf-test': GPIO ch1 'gpio'");

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, bare, ARRAY_SIZE(bare));
	memset(f->mem + 6, 0, 32);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.0 device '': GPIO ch1 'gpio', serial ch2 '', type 0xf8 ch3 'oem' (no driver)");
}

/*
 * The lines one GPIO Get and one Set carry, from the channel's limits: a
 * Get takes 1 + 2N payload bytes each way, a Set 1 + 2N out (OBMF-ICP
 * 1.0.0, 5.4, Tables 18 and 19). A limit with no room for a line gives 0;
 * outputs are not read; other types keep 0.
 */
static void obmf_test_gpio_quirks(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "big", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "small",
		  OBMF_HDR_LEN + 4 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "none",
		  OBMF_HDR_LEN + 2 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "resp", 516 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "req", 516 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart", 516 },
	};
	static const u16 want[][2] = {
		{ 255, 255 }, { 1, 1 }, { 0, 0 },
		{ 3, 255 },   { 2, 2 }, { 0, 0 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	const struct obmf_channel_quirks *q;
	int i;

	fake_layout(f, c, ARRAY_SIZE(c));
	put_unaligned_le16(OBMF_HDR_LEN + 7, f->mem + fake_ch_off(f, 4) + 0x19);
	put_unaligned_le16(OBMF_HDR_LEN + 5, f->mem + fake_ch_off(f, 5) + 0x17);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	for (i = 0; i < ARRAY_SIZE(c); i++) {
		q = &chan(f, i + 1)->info.quirks;
		KUNIT_EXPECT_EQ_MSG(test, q->gpio_per_get, want[i][0],
				    "channel %d", i + 1);
		KUNIT_EXPECT_EQ_MSG(test, q->gpio_per_set, want[i][1],
				    "channel %d", i + 1);
	}
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
	int i, k, n = 0;

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
	for (i = 1; i < OBMF_NR_CHANNELS; i++)
		n += !!f->core->chans[i];
	KUNIT_EXPECT_EQ(test, n, ARRAY_SIZE(c));
}

/*
 * Each channel type has its kind in the line; type 0 and the first type
 * past the last kind give the type
 */
static void obmf_test_info_kinds(struct kunit *test)
{
	static const struct test_chan one[] = {
		{ OBMF_TYPE_CONFIG, OBMF_ROLE_PRODUCER, NULL, 516 },
		{ OBMF_TYPE_MMIO, OBMF_ROLE_PRODUCER, NULL, 516 },
		{ OBMF_TYPE_I2C_TARGET, OBMF_ROLE_PRODUCER, NULL, 516 },
		{ OBMF_TYPE_I3C, OBMF_ROLE_PRODUCER, NULL, 516 },
		{ OBMF_TYPE_IPMI, OBMF_ROLE_PRODUCER, NULL, 516 },
		{ OBMF_TYPE_SPI, OBMF_ROLE_PRODUCER, NULL, 516, 0, 20 },
		{ OBMF_TYPE_IO, OBMF_ROLE_PRODUCER, NULL, 516 },
	};
	static const struct test_chan two[] = {
		{ OBMF_TYPE_MEMDEV, OBMF_ROLE_PRODUCER, NULL, 516 },
		{ OBMF_TYPE_TPM, OBMF_ROLE_PRODUCER, NULL, 516 },
		{ OBMF_TYPE_RTC, OBMF_ROLE_PRODUCER, NULL, 516 },
		{ OBMF_TYPE_RTC + 1, OBMF_ROLE_PRODUCER, NULL, 516 },
	};
	char buf[OBMF_INFO_LEN];
	struct obmf_fake *f;

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, one, ARRAY_SIZE(one));
	memset(f->mem + 6, 0, 32);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.0 device '': type 0x00 ch1 '' (no driver), MMIO ch2 '' (no driver), I2C-target ch3 '' (no driver), I3C ch4 '' (no driver), IPMI ch5 '' (no driver), SPI ch6 '', IO ch7 '' (no driver)");

	f = fake_create(test, FAKE_MAX_MSG, 0);
	fake_layout(f, two, ARRAY_SIZE(two));
	memset(f->mem + 6, 0, 32);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "OBMF-ICP 1.0 device '': memory ch1 '' (no driver), TPM ch2 '' (no driver), RTC ch3 '' (no driver), type 0x0d ch4 '' (no driver)");
}

static struct kunit_case obmf_ctrl_cases[] = {
	KUNIT_CASE(obmf_test_discover),
	KUNIT_CASE(obmf_test_ch_regs),
	KUNIT_CASE(obmf_test_discover_version),
	KUNIT_CASE(obmf_test_discover_names),
	KUNIT_CASE(obmf_test_discover_checks),
	KUNIT_CASE(obmf_test_discover_wrap),
	KUNIT_CASE(obmf_test_discover_unusable),
	KUNIT_CASE(obmf_test_discover_fatal),
	KUNIT_CASE(obmf_test_discover_sizes),
	KUNIT_CASE(obmf_test_gpio_quirks),
	KUNIT_CASE(obmf_test_discover_enodev),
	KUNIT_CASE(obmf_test_info_line),
	KUNIT_CASE(obmf_test_info_many),
	KUNIT_CASE(obmf_test_info_kinds),
	KUNIT_CASE_SLOW(obmf_test_discover_timeout_once),
	KUNIT_CASE(obmf_test_discover_status_once),
	KUNIT_CASE_SLOW(obmf_test_discover_timeout_always),
	KUNIT_CASE_SLOW(obmf_test_discover_header_changes),
	KUNIT_CASE_SLOW(obmf_test_discover_stale_reply),
	KUNIT_CASE_SLOW(obmf_test_discover_budget),
	KUNIT_CASE(obmf_test_discover_unbind),
	KUNIT_CASE(obmf_test_discover_suspend),
	KUNIT_CASE(obmf_test_discover_settle_ends),
	KUNIT_CASE(obmf_test_discover_fail_release),
	KUNIT_CASE(obmf_test_discover_fail_unbind),
	KUNIT_CASE(obmf_test_discover_fail_rebind),
	KUNIT_CASE(obmf_test_discover_suspend_fail),
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
		"dreq_max_depth",
	};
	static const char *const ev_files[] = {
		"transport_failed",
		"in_stalled",
		"out_stalled",
		"in_clear_halt_failed",
		"out_clear_halt_failed",
		"no_response",
		"device_error",
		"bad_response",
		"rx_short",
		"rx_size_over_transfer",
		"rx_trailing_bytes",
		"rx_unknown_channel",
		"bad_device_request",
		"answer_not_sent",
		"channel_health",
		"device_request_queue_full",
		"tx_dropped",
		"gpio_set_failed",
		"gpio_irq_config_failed",
		"gpio_irq_unused",
	};
	static const char *const ch_dirs[] = { "ch0", "ch1", "ch2", "ch3" };
	struct test_chan c[ARRAY_SIZE(disc_chans) + 1];
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct dentry *dir, *root, *ev;
	int i, k;

	if (!IS_ENABLED(CONFIG_DEBUG_FS))
		kunit_skip(test, "needs CONFIG_DEBUG_FS");
	memcpy(c, disc_chans, sizeof(disc_chans));
	c[ARRAY_SIZE(disc_chans)] =
		(struct test_chan){ 0xf8, OBMF_ROLE_PRODUCER, "oem", 516 };
	fake_layout(f, c, ARRAY_SIZE(c));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	dir = f->core->dbg;
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dir);
	for (i = 0; i < ARRAY_SIZE(if_files); i++)
		KUNIT_EXPECT_TRUE_MSG(test, dbg_has(dir, if_files[i]), "%s",
				      if_files[i]);
	ev = debugfs_lookup("events", dir);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, ev);
	KUNIT_EXPECT_EQ(test, (int)ARRAY_SIZE(ev_files), OBMF_EV_NR);
	for (i = 0; i < ARRAY_SIZE(ev_files); i++)
		KUNIT_EXPECT_TRUE_MSG(test, dbg_has(ev, ev_files[i]),
				      "events/%s", ev_files[i]);
	dput(ev);
	for (k = 0; k < ARRAY_SIZE(ch_dirs); k++) {
		struct dentry *ch = debugfs_lookup(ch_dirs[k], dir);

		KUNIT_ASSERT_NOT_ERR_OR_NULL_MSG(test, ch, "%s", ch_dirs[k]);
		for (i = 0; i < ARRAY_SIZE(ch_files); i++)
			KUNIT_EXPECT_TRUE_MSG(test, dbg_has(ch, ch_files[i]),
					      "%s/%s", ch_dirs[k], ch_files[i]);
		dput(ch);
	}
	KUNIT_EXPECT_FALSE(test, dbg_has(dir, "ch4"));

	root = dget(dir->d_parent);
	KUNIT_EXPECT_TRUE(test, dbg_has(root, dev_name(f->tp.dev)));
	fake_unbind(f);
	KUNIT_EXPECT_FALSE(test, dbg_has(root, dev_name(f->tp.dev)));
	dput(root);
}

/*
 * A label is "<device name>_<channel name>", the device name cut on a UTF-8
 * boundary to fit; with no device name the channel name, with no channel
 * name the function device's name. Expected, from i2c-obmf: its "no device
 * name" and "no channel name" warnings for the same labels.
 */
static void obmf_test_labels(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "x", 516, 0,
		  2 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, NULL, 516, 0,
		  2 },
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
	KUNIT_ASSERT_NULL(test, fdev->adev.dev.driver);

	obmf_test_warns(test)
	{
		KUNIT_EXPECT_EQ(test, devm_obmf_enable(fdev), -EINVAL);
		KUNIT_EXPECT_EQ(test, devm_obmf_publish(fdev, &test_devres_ops),
				-EINVAL);
	}
	KUNIT_EXPECT_FALSE(test, chan(f, 1)->bound);

	test_devres_seen = t;
	KUNIT_ASSERT_EQ(test, auxiliary_driver_register(&test_devres_driver),
			0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, test_slow_unregister,
						  &test_devres_driver),
			0);
	KUNIT_EXPECT_EQ(test, t->probe_ret, 0);
	KUNIT_ASSERT_PTR_EQ(test, fdev->adev.dev.driver,
			    &test_devres_driver.driver);
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

/*
 * A fake transport's function devices have the test names, which neither
 * a function driver nor a case's driver matches unless it registers
 */
static void obmf_test_fake_names(struct kunit *test)
{
	static const char *const names[] = {
		[CH_I2C] = "obmf.test-i2c.",
		[CH_GPIO] = "obmf.test-gpio.",
		[CH_SERIAL] = "obmf.test-serial.",
		[CH_SPI] = "obmf.test-spi.",
	};
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch;
	const char *name;
	int nr, n = 0;

	for (nr = 1; nr < OBMF_NR_CHANNELS; nr++) {
		ch = f->core->chans[nr];
		if (!ch || !ch->fdev)
			continue;
		n++;
		KUNIT_ASSERT_LT_MSG(test, nr, ARRAY_SIZE(names), "channel %d",
				    nr);
		KUNIT_ASSERT_NOT_NULL_MSG(test, names[nr], "channel %d", nr);
		name = dev_name(&ch->fdev->adev.dev);
		KUNIT_EXPECT_TRUE_MSG(test, str_has_prefix(name, names[nr]),
				      "%s", name);
		KUNIT_EXPECT_NULL_MSG(test, ch->fdev->adev.dev.driver, "%s",
				      name);
	}
	KUNIT_EXPECT_EQ(test, n, 4);
}

/* disc_chans bound on an interface whose firmware node is set */
static struct obmf_fake *node_bind(struct obmf_fake *f)
{
	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	KUNIT_ASSERT_EQ(f->test, fake_bind(f), 0);
	return f;
}

/* Channel @nr of disc_chans has a device, with @node as its own */
static void ch_node_check(struct obmf_fake *f, int nr,
			  struct fwnode_handle *node)
{
	struct kunit *test = f->test;
	struct obmf_channel *ch = chan(f, nr);

	KUNIT_EXPECT_FALSE_MSG(test, ch->no_device, "channel %d", nr);
	KUNIT_EXPECT_PTR_EQ_MSG(test, ch->fwnode, node, "channel %d", nr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, ch->fdev, "channel %d", nr);
	KUNIT_EXPECT_PTR_EQ_MSG(test, dev_fwnode(&ch->fdev->adev.dev), node,
				"channel %d", nr);
}

/* The I2C and serial channels of disc_chans have @i2c and @serial */
static void node_check(struct obmf_fake *f, struct fwnode_handle *i2c,
		       struct fwnode_handle *serial)
{
	ch_node_check(f, 1, i2c);
	ch_node_check(f, 2, serial);
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
 * device; all references go at unbind. This holds for the I2C, the
 * serial and the GPIO channel alike. A child with another reg, or no
 * child, leaves the channel without a node.
 */
static void obmf_test_of_nodes(struct kunit *test)
{
	struct device_node *np, *i2c, *serial, *gpio;
	unsigned int refs, serial_refs, gpio_refs;
	struct obmf_fake *f;

	KUNIT_ASSERT_EQ(test, of_overlay_apply_kunit(test, obmf_test_nodes), 0);
	np = of_test_node(test, "/obmf-test-match");
	i2c = of_test_node(test, "/obmf-test-match/i2c@1");
	serial = of_test_node(test, "/obmf-test-match/serial@2");
	gpio = of_test_node(test, "/obmf-test-match/gpio@3");
	refs = of_test_refs(i2c);
	serial_refs = of_test_refs(serial);
	gpio_refs = of_test_refs(gpio);
	f = of_test_bind(test, np);
	node_check(f, of_fwnode_handle(i2c), of_fwnode_handle(serial));
	ch_node_check(f, 3, of_fwnode_handle(gpio));
	fake_unbind(f);
	KUNIT_EXPECT_EQ(test, of_test_refs(i2c), refs);
	KUNIT_EXPECT_EQ(test, of_test_refs(serial), serial_refs);
	KUNIT_EXPECT_EQ(test, of_test_refs(gpio), gpio_refs);

	node_check(of_test_bind(test, of_test_node(test, "/obmf-test-reg")),
		   NULL, NULL);
	node_check(of_test_bind(test, of_test_node(test, "/obmf-test-empty")),
		   NULL, NULL);
}

/*
 * A child with the channel's reg and another class's name is not
 * attached: spi@1 for the I2C channel, i2c@2 for the serial one.
 * Expected: a "channel 1: node spi@1 ignored" and a "channel 2: node
 * i2c@2 ignored" warning.
 */
static void obmf_test_of_misnamed(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, of_overlay_apply_kunit(test, obmf_test_nodes), 0);
	node_check(of_test_bind(test, of_test_node(test, "/obmf-test-name")),
		   NULL, NULL);
}

/*
 * Bound on @parent, channel @nr's node @off is disabled: the channel has
 * no function device and the node's reference is back at once. Channel
 * @other still gets its device, with @on as its node.
 */
static void of_disabled_check(struct kunit *test, const char *parent, int nr,
			      const char *off, int other, const char *on)
{
	struct device_node *np = of_test_node(test, off);
	unsigned int refs = of_test_refs(np);
	struct obmf_fake *f;

	f = of_test_bind(test, of_test_node(test, parent));
	KUNIT_EXPECT_TRUE(test, chan(f, nr)->no_device);
	KUNIT_EXPECT_NULL(test, chan(f, nr)->fwnode);
	KUNIT_EXPECT_NULL(test, chan(f, nr)->fdev);
	KUNIT_EXPECT_EQ(test, of_test_refs(np), refs);
	ch_node_check(f, other, of_fwnode_handle(of_test_node(test, on)));
}

/*
 * A disabled node gives its channel no function device, and its
 * reference goes at once; the other channel still gets its device and
 * node. Once with the I2C node disabled, once with the serial node.
 */
static void obmf_test_of_disabled(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, of_overlay_apply_kunit(test, obmf_test_nodes), 0);
	of_disabled_check(test, "/obmf-test-disabled", 1,
			  "/obmf-test-disabled/i2c@1", 2,
			  "/obmf-test-disabled/serial@2");
	of_disabled_check(test, "/obmf-test-disabled-serial", 2,
			  "/obmf-test-disabled-serial/serial@2", 1,
			  "/obmf-test-disabled-serial/i2c@1");
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

static const struct property_entry sw_reg4[] = {
	PROPERTY_ENTRY_U32("reg", 4),
	{},
};

static const struct property_entry sw_reg5[] = {
	PROPERTY_ENTRY_U32("reg", 5),
	{},
};

/* The interfaces of obmf-test-nodes.dtso but the disabled ones */
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

static const struct software_node sw_match_gpio = {
	.name = "gpio@3",
	.parent = &sw_match,
	.properties = sw_reg3,
};

static const struct software_node sw_reg = { .name = "obmf-test-reg" };

static const struct software_node sw_reg_i2c = {
	.name = "i2c@5",
	.parent = &sw_reg,
	.properties = sw_reg5,
};

static const struct software_node sw_reg_serial = {
	.name = "serial@4",
	.parent = &sw_reg,
	.properties = sw_reg4,
};

static const struct software_node sw_name = { .name = "obmf-test-name" };

static const struct software_node sw_name_spi = {
	.name = "spi@1",
	.parent = &sw_name,
	.properties = sw_reg1,
};

static const struct software_node sw_name_i2c = {
	.name = "i2c@2",
	.parent = &sw_name,
	.properties = sw_reg2,
};

static const struct software_node sw_empty = { .name = "obmf-test-empty" };

static const struct software_node *const sw_nodes[] = {
	&sw_match,    &sw_match_serial, &sw_match_i2c,	&sw_match_gpio,
	&sw_reg,      &sw_reg_i2c,	&sw_reg_serial, &sw_name,
	&sw_name_spi, &sw_name_i2c,	&sw_empty,	NULL,
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
	struct obmf_fake *f;

	KUNIT_ASSERT_EQ(test,
			kunit_software_node_register_node_group(test, sw_nodes),
			0);
	f = sw_bind(test, &sw_match);
	node_check(f, software_node_fwnode(&sw_match_i2c),
		   software_node_fwnode(&sw_match_serial));
	ch_node_check(f, 3, software_node_fwnode(&sw_match_gpio));
	node_check(sw_bind(test, &sw_reg), NULL, NULL);
	node_check(sw_bind(test, &sw_empty), NULL, NULL);
}

/*
 * obmf_test_of_misnamed() with software nodes. Expected: a "channel 1:
 * node spi@1 ignored" and a "channel 2: node i2c@2 ignored" warning.
 */
static void obmf_test_swnode_misnamed(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test,
			kunit_software_node_register_node_group(test, sw_nodes),
			0);
	node_check(sw_bind(test, &sw_name), NULL, NULL);
}

/* What sysfs reads from attribute @name of @groups on @dev */
static void sysfs_check(struct kunit *test, struct device *dev,
			const struct attribute_group **groups, const char *name,
			const char *want)
{
	struct device_attribute *da = NULL;
	struct attribute **a;
	char *buf = kunit_kzalloc(test, PAGE_SIZE, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_ASSERT_NOT_NULL(test, groups);
	for (a = groups[0]->attrs; *a; a++)
		if (!strcmp((*a)->name, name))
			da = container_of(*a, struct device_attribute, attr);
	KUNIT_ASSERT_NOT_NULL_MSG(test, da, "%s", name);
	KUNIT_EXPECT_EQ_MSG(test, da->show(dev, da, buf), strlen(want), "%s",
			    name);
	KUNIT_EXPECT_STREQ_MSG(test, buf, want, "%s", name);
}

/*
 * A function device's name and channel, and the interface's obmf/name,
 * read back the channel name, the channel number and the device name
 */
static void obmf_test_sysfs_names(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct obmf_fdev *fdev;
	struct device *dev;

	fake_layout(f, core_chans, ARRAY_SIZE(core_chans));
	memset(f->mem + 6, 0, 32);
	memcpy(f->mem + 6, "bmc-card", 8);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	fdev = chan(f, CH_SERIAL)->fdev;
	KUNIT_ASSERT_NOT_NULL(test, fdev);
	sysfs_check(test, &fdev->adev.dev, fdev->adev.dev.groups, "name",
		    "uart\n");
	sysfs_check(test, &fdev->adev.dev, fdev->adev.dev.groups, "channel",
		    "5\n");

	dev = kunit_device_register(test, "obmf-intf");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	dev_set_drvdata(dev, &f->tp);
	KUNIT_EXPECT_STREQ(test, obmf_intf_groups[0]->name, "obmf");
	sysfs_check(test, dev, obmf_intf_groups, "name", "bmc-card\n");
}

/*
 * The interface's obmf/name reads empty while discovery runs, whatever
 * the name field holds, and the device name once the table is set
 */
static void obmf_test_sysfs_name_early(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct device *dev;

	fake_layout(f, disc_chans, ARRAY_SIZE(disc_chans));
	f->drop_hdr = true;
	dev = kunit_device_register(test, "obmf-intf");
	KUNIT_ASSERT_FALSE(test, IS_ERR(dev));
	dev_set_drvdata(dev, &f->tp);
	KUNIT_ASSERT_EQ(test, fake_probe(f), 0);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, 1));
	/* as a pass that has copied the name and not set the table yet */
	strscpy(f->core->device_name, "half");
	sysfs_check(test, dev, obmf_intf_groups, "name", "\n");

	WRITE_ONCE(f->drop_hdr, false);
	flush_work(&f->core->bind_work);
	KUNIT_ASSERT_EQ(test, f->core->bind_err, 0);
	sysfs_check(test, dev, obmf_intf_groups, "name", "obmf-test\n");
}

/*
 * Only a GPIO, an I2C controller, a serial or an SPI controller channel
 * the device serves gets a function device; every other type, and those
 * with another role, is discovered and left without one
 */
static void obmf_test_no_fdev(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c", 516, 0,
		  2 },
		{ OBMF_TYPE_MMIO, OBMF_ROLE_PRODUCER, "mmio", 516 },
		{ OBMF_TYPE_I2C_TARGET, OBMF_ROLE_PRODUCER, "i2ct", 516 },
		{ OBMF_TYPE_I3C, OBMF_ROLE_PRODUCER, "i3c", 516 },
		{ OBMF_TYPE_IPMI, OBMF_ROLE_PRODUCER, "ipmi", 516 },
		{ OBMF_TYPE_SPI, OBMF_ROLE_CONSUMER, "spic", 516 },
		{ OBMF_TYPE_IO, OBMF_ROLE_PRODUCER, "io", 516 },
		{ OBMF_TYPE_MEMDEV, OBMF_ROLE_PRODUCER, "memdev", 516 },
		{ OBMF_TYPE_TPM, OBMF_ROLE_PRODUCER, "tpm", 516 },
		{ OBMF_TYPE_RTC, OBMF_ROLE_PRODUCER, "rtc", 516 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_CONSUMER, "i2cc", 516, 0,
		  2 },
		{ OBMF_TYPE_GPIO, OBMF_ROLE_CONSUMER, "gpioc", 516 },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_CONSUMER, "uartc", 516 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	int i;

	fake_layout(f, c, ARRAY_SIZE(c));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_NOT_NULL(test, chan(f, 1)->fdev);
	for (i = 1; i < ARRAY_SIZE(c); i++)
		KUNIT_EXPECT_NULL_MSG(test, chan(f, i + 1)->fdev, "%s",
				      c[i].name);
}

/* Each SPI controller channel gets its own obmf.spi device */
static void obmf_test_spi_fdev(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_SPI, OBMF_ROLE_PRODUCER, "spi0", 516, 0, 20 },
		{ OBMF_TYPE_SPI, OBMF_ROLE_PRODUCER, "spi1", 516, 0, 20 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct obmf_fdev *a, *b;

	fake_layout(f, c, ARRAY_SIZE(c));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	a = chan(f, 1)->fdev;
	b = chan(f, 2)->fdev;
	KUNIT_ASSERT_NOT_NULL(test, a);
	KUNIT_ASSERT_NOT_NULL(test, b);
	KUNIT_EXPECT_STREQ(test, a->adev.name, "test-spi");
	KUNIT_EXPECT_STREQ(test, b->adev.name, "test-spi");
	KUNIT_EXPECT_NE(test, a->adev.id, b->adev.id);
	KUNIT_EXPECT_PTR_EQ(test, a->info, &chan(f, 1)->info);
	KUNIT_EXPECT_PTR_EQ(test, b->info, &chan(f, 2)->info);
}

static struct kunit_case obmf_bus_cases[] = {
	KUNIT_CASE(obmf_test_debugfs),
	KUNIT_CASE(obmf_test_labels),
	KUNIT_CASE(obmf_test_sysfs_names),
	KUNIT_CASE(obmf_test_sysfs_name_early),
	KUNIT_CASE(obmf_test_no_fdev),
	KUNIT_CASE(obmf_test_spi_fdev),
	KUNIT_CASE(obmf_test_devres_order),
	KUNIT_CASE(obmf_test_of_nodes),
	KUNIT_CASE(obmf_test_of_misnamed),
	KUNIT_CASE(obmf_test_of_disabled),
	KUNIT_CASE(obmf_test_swnodes),
	KUNIT_CASE(obmf_test_swnode_misnamed),
	KUNIT_CASE(obmf_test_fake_names),
	{}
};

static struct kunit_suite obmf_bus_suite = {
	.name = "obmf-bus",
	.test_cases = obmf_bus_cases,
};

#define ER_RECS 256

struct er_rec {
	enum obmf_event ev;
	int ch;
	long a;
	long b;
	struct ratelimit_state *rs;
};

/* test->priv of the obmf-report suite */
struct er_ctx {
	struct test_log log; /* first: test_log_start() takes test->priv */
	unsigned int n;
	struct er_rec rec[ER_RECS];
};

/* Records the events raised on the case's own thread, in its interrupts too */
static void er_stub(struct device *dev, enum obmf_event ev, int ch, long a,
		    long b, struct ratelimit_state *rs, const char *func)
{
	struct er_ctx *x = kunit_get_current_test()->priv;

	if (x->n < ER_RECS)
		x->rec[x->n] = (struct er_rec){
			.ev = ev, .ch = ch, .a = a, .b = b, .rs = rs
		};
	x->n++;
}

/* From now on, record the events of the case's thread instead of logging */
static struct er_ctx *er_start(struct kunit *test)
{
	struct er_ctx *x = test->priv;

	x->n = 0;
	kunit_activate_static_stub(test, obmf_report_log, er_stub);
	return x;
}

/* Recorded events @ev */
static int er_seen(struct er_ctx *x, enum obmf_event ev)
{
	unsigned int i;
	int n = 0;

	for (i = 0; i < min(x->n, ER_RECS); i++)
		if (x->rec[i].ev == ev)
			n++;
	return n;
}

static int obmf_report_test_init(struct kunit *test)
{
	struct er_ctx *x;

	x = kunit_kzalloc(test, sizeof(*x), GFP_KERNEL);
	if (!x)
		return -ENOMEM;
	test_log_init(test, &x->log);
	return 0;
}

/* Lines of @log, within 1 s, that contain the text @fmt prints */
static __printf(3, 4) int er_lines(struct kunit *test, struct test_log *log,
				   const char *fmt, ...)
{
	char text[TEST_LOG_LINE];
	va_list args;

	va_start(args, fmt);
	vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);
	return test_log_wait(test, log, text, 1, 1000);
}

/*
 * A fatal stop is one event, with no rate limit as the state
 * machine bounds it; a second fatal error is none, and later requests
 * fail -EIO.
 */
static void obmf_test_report_fatal(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct er_ctx *x = er_start(test);
	size_t rlen;
	u8 resp[8];

	obmf_core_fatal(f->core, -EPROTO);
	obmf_core_fatal(f->core, -EIO);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_FATAL), 1);
	KUNIT_ASSERT_EQ(test, x->n, 1);
	KUNIT_EXPECT_EQ(test, x->rec[0].ev, OBMF_EV_FATAL);
	KUNIT_EXPECT_EQ(test, x->rec[0].a, -EPROTO);
	KUNIT_EXPECT_NULL(test, x->rec[0].rs);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp),
					&rlen, 0),
			-EIO);
	KUNIT_EXPECT_EQ(test, er_seen(x, OBMF_EV_FATAL), 1);
}

/*
 * After a fatal stop a published driver's failed operation
 * runs once, from a work, and the state errno it reads is -EIO; one
 * published later is refused; none runs after unpublishing.
 */
static void obmf_test_report_failed_op(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);

	KUNIT_EXPECT_EQ(test, obmf_fdev_errno(&fn->fdev), 0);
	KUNIT_EXPECT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_EXPECT_EQ(test, obmf_fdev_errno(&fn->fdev), -EAGAIN);
	KUNIT_ASSERT_EQ(test, obmf_core_resume(f->core), 0);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->failed), 0);

	obmf_core_fatal(f->core, -EPROTO);
	obmf_core_fatal(f->core, -EIO);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->failed), 1);
	KUNIT_EXPECT_TRUE(test, fn->failed_in_work);
	KUNIT_EXPECT_EQ(test, fn->failed_errno, -EIO);
	KUNIT_EXPECT_EQ(test, obmf_fdev_errno(&fn->fdev), -EIO);
}

/* No failed operation for a driver no longer published */
static void obmf_test_report_failed_unpublished(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);

	scoped_guard(mutex, &ch->dispatch_lock) {
		ch->ops = NULL;
		ch->publisher = NULL;
	}
	obmf_core_fatal(f->core, -EPROTO);
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(&fn->failed), 0);
}

/*
 * The text each event logs, and its count. The transport's
 * events have no fake here: raised directly, they check the text.
 */
static void obmf_test_report_text(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_core *core = f->core;
	struct test_log *log = test_log_start(test, core->dev);
	int n;

	obmf_report(core, core->dev, OBMF_EV_IN_STALL, -1, 0, 0);
	KUNIT_EXPECT_EQ(test, er_lines(test, log, "bulk in stalled"), 1);
	obmf_report(core, core->dev, OBMF_EV_OUT_STALL, -1, 0, 0);
	KUNIT_EXPECT_EQ(test, er_lines(test, log, "bulk out stalled"), 1);
	obmf_report(core, core->dev, OBMF_EV_IN_CLEAR_HALT, -1, -EPROTO, 0);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log, "bulk in clear halt failed: %pe",
				 ERR_PTR(-EPROTO)),
			1);
	obmf_report(core, core->dev, OBMF_EV_OUT_CLEAR_HALT, -1, -EPIPE, 0);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log, "bulk out clear halt failed: %pe",
				 ERR_PTR(-EPIPE)),
			1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_IN_STALL), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_OUT_CLEAR_HALT), 1);

	obmf_report(core, core->dev, OBMF_EV_NO_RESPONSE, 2, 1000, 0);
	n = er_lines(test, log, "channel 2: no response in 1000 ms");
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_DEVICE_ERROR, 2, 0x04, 0);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log, "channel 2: device error 0x4"), 1);
	obmf_report(core, core->dev, OBMF_EV_BAD_RESPONSE, 2, 5, 0);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log, "channel 2: bad response, 5 bytes"),
			1);
	obmf_report(core, core->dev, OBMF_EV_RX_SHORT, -1, 3, 0);
	n = er_lines(test, log, "malformed message: short, 3 bytes");
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_RX_OVERSIZE, -1, 9, 1);
	n = er_lines(test, log, "malformed message: %s, 9 of 1 bytes",
		     "size over the transfer");
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_RX_TRAILING, -1, 2, 0);
	n = er_lines(test, log, "malformed message: trailing bytes, 2");
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_RX_NO_CHANNEL, 200, 0, 0);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log, "message for unknown channel 200"),
			1);
	obmf_report(core, core->dev, OBMF_EV_BAD_DREQ, 5, 2, 0);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log,
				 "channel 5: bad device request, 2 bytes"),
			1);
	obmf_report(core, core->dev, OBMF_EV_ANSWER_NOT_SENT, 2, -EPROTO, 0);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log, "channel 2: answer not sent: %pe",
				 ERR_PTR(-EPROTO)),
			1);
	obmf_report_bounded(core, core->dev, OBMF_EV_CH_HEALTH, 3, 15, 1);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log,
				 "channel 3: health 15, error indicator 1"),
			1);
	obmf_report(core, core->dev, OBMF_EV_QUEUE_FULL, 4, 1, 0);
	n = er_lines(test, log,
		     "channel 4: device request queue full, request refused");
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_QUEUE_FULL, 2, 0, 0);
	n = er_lines(test, log,
		     "channel 2: device request queue full, request dropped");
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_TX_DROPPED, 5, 4, -EIO);
	n = er_lines(test, log, "dropped 4 bytes: %pe", ERR_PTR(-EIO));
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_GPIO_SET, 3, 7, -EBUSY);
	n = er_lines(test, log, "cannot set line 7: %pe", ERR_PTR(-EBUSY));
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_GPIO_SET, 3, -1, -EIO);
	n = er_lines(test, log, "cannot set lines: %pe", ERR_PTR(-EIO));
	KUNIT_EXPECT_EQ(test, n, 1);
	obmf_report(core, core->dev, OBMF_EV_GPIO_IRQ, 3, 9, -EIO);
	n = er_lines(test, log, "cannot configure the interrupt of line 9: %pe",
		     ERR_PTR(-EIO));
	KUNIT_EXPECT_EQ(test, n, 1);

	obmf_core_fatal(f->core, -EPROTO);
	KUNIT_EXPECT_EQ(test,
			er_lines(test, log,
				 "transport failed: %pe, device stopped",
				 ERR_PTR(-EPROTO)),
			1);
}

/* Events the core counted, of every kind but @but */
static long er_total(struct obmf_core *core, enum obmf_event but)
{
	long n = 0;
	int i;

	for (i = 0; i < OBMF_EV_NR; i++)
		if (i != but)
			n += ev_cnt(core, i);
	return n;
}

static int er_req(struct obmf_channel *ch, unsigned int timeout_ms)
{
	size_t rlen;
	u8 resp[8];

	return obmf_ch_request(ch, 1, NULL, 0, resp, sizeof(resp), &rlen,
			       timeout_ms);
}

/*
 * A request with no response is one event, on its channel,
 * rate limited; obmf_test_unbind_timeout() checks none while unbinding.
 */
static void obmf_test_report_no_response(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct er_ctx *x = er_start(test);

	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, er_req(ch, 50), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_NO_RESPONSE), 1);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_NO_RESPONSE), 0);
	KUNIT_ASSERT_EQ(test, x->n, 1);
	KUNIT_EXPECT_EQ(test, x->rec[0].ev, OBMF_EV_NO_RESPONSE);
	KUNIT_EXPECT_EQ(test, x->rec[0].ch, CH_GPIO);
	KUNIT_EXPECT_EQ(test, x->rec[0].a, 50);
	KUNIT_EXPECT_NOT_NULL(test, x->rec[0].rs);
}

/*
 * Of the Table 4 statuses, the device's own failures are an
 * event (0x01, 0x04, 0x05, a reserved one, a type's unknown one); a
 * timeout, busy, an unsupported size and a type's own code are answers.
 */
static void obmf_test_report_status(struct kunit *test)
{
	static const struct {
		int ch;
		u8 status;
		int err;
		bool ev;
	} vec[] = {
		{ CH_GPIO, OBMF_STATUS_INVALID, -EIO, true },
		{ CH_GPIO, OBMF_STATUS_PERMANENT_ERROR, -EIO, true },
		{ CH_GPIO, OBMF_STATUS_UNKNOWN_CHANNEL, -ECHRNG, true },
		{ CH_GPIO, 0x3f, -EIO, true },
		{ CH_SPI, 0x40, -EIO, true },
		{ CH_GPIO, OBMF_STATUS_TIMEOUT, -ETIME, false },
		{ CH_GPIO, OBMF_STATUS_BUSY, -EBUSY, false },
		{ CH_GPIO, OBMF_STATUS_SIZE_UNSUPPORTED, -EMSGSIZE, false },
		{ CH_I2C, OBMF_STATUS_I2C_NACK, -ENXIO, false },
	};
	struct obmf_fake *f = core_setup(test, 0);
	struct er_ctx *x = er_start(test);
	struct fake_step s;
	long n = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(vec); i++) {
		s = (struct fake_step){ .channel = -1,
					.echo = true,
					.status = vec[i].status };
		fake_push(f, &s);
		KUNIT_EXPECT_EQ_MSG(test, er_req(chan(f, vec[i].ch), 0),
				    vec[i].err, "status %#x", vec[i].status);
		n += vec[i].ev;
		KUNIT_EXPECT_EQ_MSG(test, ev_cnt(f->core, OBMF_EV_DEVICE_ERROR),
				    n, "status %#x", vec[i].status);
		if (vec[i].ev && x->n == n) {
			KUNIT_EXPECT_EQ(test, x->rec[n - 1].ch, vec[i].ch);
			KUNIT_EXPECT_EQ(test, x->rec[n - 1].a, vec[i].status);
		}
	}
	KUNIT_EXPECT_EQ(test, x->n, n);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_DEVICE_ERROR), 0);
}

/* A response for @ch with @size bytes of payload, all zero */
static void er_rx_big(struct obmf_fake *f, u8 ch, size_t size)
{
	u8 *msg = kunit_kzalloc(f->test, OBMF_HDR_LEN + size, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(f->test, msg);
	msg[0] = ch;
	msg[1] = OBMF_HDR_RQRESP;
	put_unaligned_le16(size, msg + 2);
	fake_rx(f, msg, OBMF_HDR_LEN + size);
}

/*
 * Each malformed message shape is one event and its counter;
 * a response over the slot is a bad response where the request returns,
 * and so is one that fails the echo check.
 */
static void obmf_test_report_malformed(struct kunit *test)
{
	static const u8 write[] = { 0x00, 0x03, 0x00, 'o', 'k' };
	static const u8 trailing[] = { 0x02, 0x80, 0x01, 0x00, 0x01, 0x00 };
	static const u8 oversize[] = { 0x02, 0x80, 0x09, 0x00, 0x01 };
	static const u8 unknown[] = { 0xc8, 0x80, 0x00, 0x00 };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct er_ctx *x = er_start(test);
	struct obmf_core *core = f->core;
	struct test_call *c;
	unsigned int sends;

	fake_rx(f, write, 3);
	fake_rx(f, oversize, sizeof(oversize));
	fake_rx(f, trailing, sizeof(trailing));
	fake_rx(f, unknown, sizeof(unknown));
	fake_inject(f, CH_SERIAL, 0, write, sizeof(write));
	flush_workqueue(core->wq);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_MALFORMED), 4);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_NO_CHANNEL), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_RX_SHORT), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_RX_OVERSIZE), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_RX_TRAILING), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_RX_NO_CHANNEL), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_BAD_DREQ), 1);
	KUNIT_EXPECT_EQ(test, er_seen(x, OBMF_EV_RX_SHORT), 1);
	KUNIT_EXPECT_EQ(test, er_seen(x, OBMF_EV_RX_OVERSIZE), 1);
	KUNIT_EXPECT_EQ(test, er_seen(x, OBMF_EV_RX_TRAILING), 1);
	KUNIT_EXPECT_EQ(test, er_seen(x, OBMF_EV_RX_NO_CHANNEL), 1);
	KUNIT_EXPECT_EQ(test, er_seen(x, OBMF_EV_BAD_DREQ), 1);
	KUNIT_EXPECT_EQ(test, x->n, 5);

	fake_push(f, &s);
	sends = f->sends;
	c = call_start(f, call_request, ch, 1, 5000);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	er_rx_big(f, CH_GPIO, ch->slot.cap + 1);
	KUNIT_ASSERT_TRUE(test, call_wait(c));
	KUNIT_EXPECT_EQ(test, c->ret, -EPROTO);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_BAD_RESPONSE), 1);

	s = (struct fake_step){ .channel = -1, .echo = false, .plen = 2 };
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, er_req(chan(f, CH_IPMI), 0), -EPROTO);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_BAD_RESPONSE), 2);
	KUNIT_EXPECT_EQ(test, er_seen(x, OBMF_EV_BAD_RESPONSE), 1);

	/* One a function driver finds, on its own device */
	obmf_fdev_protocol_error(chan(f, CH_I2C)->fdev);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_BAD_RESPONSE), 3);
	KUNIT_ASSERT_EQ(test, er_seen(x, OBMF_EV_BAD_RESPONSE), 2);
	KUNIT_EXPECT_EQ(test, x->rec[x->n - 1].ch, CH_I2C);
	KUNIT_EXPECT_EQ(test, x->rec[x->n - 1].a, -1);

	/* A device request a function driver finds malformed */
	obmf_fdev_bad_request(chan(f, CH_I2C)->fdev, 3);
	KUNIT_EXPECT_EQ(test, if_cnt(core, OBMF_IF_RX_MALFORMED), 5);
	KUNIT_EXPECT_EQ(test, ev_cnt(core, OBMF_EV_BAD_DREQ), 2);
	KUNIT_ASSERT_EQ(test, er_seen(x, OBMF_EV_BAD_DREQ), 2);
	KUNIT_EXPECT_EQ(test, x->rec[x->n - 1].ch, CH_I2C);
	KUNIT_EXPECT_EQ(test, x->rec[x->n - 1].a, 3);
}

/*
 * A function driver reports only its own events; one of the
 * core's, or one past the list, warns and is neither counted nor logged.
 * An event it only counts is counted and not logged.
 */
static void obmf_test_report_fdev_core(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_fdev *fdev = chan(f, CH_I2C)->fdev;
	struct er_ctx *x = er_start(test);
	long ev = er_total(f->core, OBMF_EV_NR);

	kunit_warning_suppress(test) {
		obmf_fdev_report(fdev, OBMF_EV_FDEV_FIRST - 1, 1, 0);
		obmf_fdev_report(fdev, OBMF_EV_NR, 1, 0);
		obmf_fdev_count(fdev, OBMF_EV_FDEV_FIRST - 1);
		obmf_fdev_count(fdev, OBMF_EV_NR);
		/* one WARN_ON_ONCE each: twice only on some arches */
		KUNIT_EXPECT_GE(test, KUNIT_SUPPRESSED_WARNING_COUNT(), 2);
	}
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_NR), ev);
	KUNIT_EXPECT_EQ(test, x->n, 0);

	obmf_fdev_count(fdev, OBMF_EV_GPIO_IRQ_UNUSED);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ_UNUSED), 1);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_NR), ev + 1);
	KUNIT_EXPECT_EQ(test, x->n, 0);
}

/* A response after the timeout is counted late, no event */
static void obmf_test_report_late(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct fake_step s = {
		.how = FAKE_TIMER,
		.delay_ms = 150,
		.channel = -1,
		.echo = true,
		.plen = 1,
	};

	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, er_req(ch, 50), -ETIMEDOUT);
	KUNIT_ASSERT_TRUE(test, fake_wait_idle(f));
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_LATE), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_NO_RESPONSE), 1);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_NO_RESPONSE), 0);
}

/*
 * An answer the transport fails, and one above the channel's
 * request limit, count as transmit errors and are one event each; one to
 * a device that is gone is neither.
 */
static void obmf_test_report_answer(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);
	struct obmf_test_fn *fn = test_fn_publish(f, ch);
	struct er_ctx *x = er_start(test);
	size_t big = ch->info.max_req + 1;
	u8 msg[16], *body;

	body = kunit_kzalloc(test, big, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, body);
	fn->keep = true;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_NOT_NULL(test, fn->kept);
	WRITE_ONCE(f->answer_ret, -EPROTO);
	KUNIT_EXPECT_EQ(test,
			obmf_respond(&fn->fdev, fn->kept, OBMF_STATUS_SUCCESS,
				     NULL, 0),
			-EPROTO);

	fn->kept = NULL;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_NOT_NULL(test, fn->kept);
	KUNIT_EXPECT_EQ(test,
			obmf_respond(&fn->fdev, fn->kept, OBMF_STATUS_SUCCESS,
				     body, big),
			-EMSGSIZE);

	fn->kept = NULL;
	fake_rx(f, msg, gpio_notify(msg, CH_GPIO));
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_NOT_NULL(test, fn->kept);
	WRITE_ONCE(f->answer_ret, -ENODEV);
	KUNIT_EXPECT_EQ(test,
			obmf_respond(&fn->fdev, fn->kept, OBMF_STATUS_SUCCESS,
				     NULL, 0),
			-ENODEV);

	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TX_ERRORS), 2);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_ANSWER_NOT_SENT), 2);
	KUNIT_ASSERT_EQ(test, x->n, 2);
	KUNIT_EXPECT_EQ(test, x->rec[0].a, -EPROTO);
	KUNIT_EXPECT_EQ(test, x->rec[1].a, -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, x->rec[1].ch, CH_GPIO);
}

/*
 * The rate limit is per call site, not per event: 200 short
 * messages are counted and pass one state, the timeout after them
 * another.
 */
static void obmf_test_report_sites(struct kunit *test)
{
	struct obmf_fake *f = core_setup(test, 0);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct er_ctx *x = er_start(test);
	u8 msg[2] = {};
	int i, same = 0;

	for (i = 0; i < 200; i++)
		fake_rx(f, msg, sizeof(msg));
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, er_req(chan(f, CH_GPIO), 20), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_RX_SHORT), 200);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_NO_RESPONSE), 1);
	KUNIT_ASSERT_EQ(test, x->n, 201);
	for (i = 0; i < 200; i++)
		same += x->rec[i].rs == x->rec[0].rs;
	KUNIT_EXPECT_EQ(test, same, 200);
	KUNIT_EXPECT_NOT_NULL(test, x->rec[0].rs);
	KUNIT_EXPECT_EQ(test, x->rec[200].ev, OBMF_EV_NO_RESPONSE);
	KUNIT_EXPECT_NOT_NULL(test, x->rec[200].rs);
	KUNIT_EXPECT_PTR_NE(test, x->rec[200].rs, x->rec[0].rs);
}

/*
 * A bind whose first discovery pass meets a stale reply
 * binds and reports nothing: discovery takes it as a leftover.
 */
static void obmf_test_report_rebind(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct test_log *log = test_log_start(test, f->tp.dev);

	fake_layout(f, core_chans, ARRAY_SIZE(core_chans));
	f->stale_first = true;
	KUNIT_EXPECT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_PROTOCOL_ERRORS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_RX_LATE), 1);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_NR), 0);
	/* Discovery runs in the bind work: only the bound line is logged */
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, ": "), 1);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "device 'obmf-test'"),
			1);
}

/*
 * A Notify Datagram on a channel no driver serves is counted
 * dropped; it is owed no answer.
 */
static void obmf_test_report_datagram(struct kunit *test)
{
	static const u8 dgram[] = { FAKE_GPIO_DATAGRAM, 0x01 };
	struct obmf_fake *f = core_setup(test, 0);
	struct obmf_channel *ch = chan(f, CH_GPIO);

	fake_inject(f, CH_GPIO, 0, dgram, sizeof(dgram));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 0);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_GPIO, 1), 0);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_NR), 0);
}

static void er_set_health(struct obmf_fake *f, int nr, u8 health)
{
	u8 *h = f->mem + fake_ch_off(f, nr);

	h[OBMF_CH_REG_STATUS] =
		(h[OBMF_CH_REG_STATUS] & ~OBMF_CH_STATUS_HEALTH) | health;
}

/*
 * A channel whose HEALTH_STATUS is 15 (channel error) or 3
 * (not ready), and one with CHANNEL_ERROR_INDICATOR set, still bind;
 * each is one event, from the pass that is kept, with no rate limit.
 * The values next to them (0, 2, 4, 14) stay quiet.
 */
static void obmf_test_report_health(struct kunit *test)
{
	static const char *const lines[] = {
		"channel 2: health 15, error indicator 0",
		"channel 3: health 1, error indicator 1",
		"channel 4: health 3, error indicator 0",
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct test_log *log = test_log_start(test, f->tp.dev);
	unsigned int i;
	u8 *h;

	fake_layout(f, core_chans, ARRAY_SIZE(core_chans));
	er_set_health(f, CH_GPIO, 15);
	h = f->mem + fake_ch_off(f, CH_IPMI);
	h[OBMF_CH_REG_STATUS] |= OBMF_CH_STATUS_ERROR;
	er_set_health(f, CH_SPI, 3);
	er_set_health(f, CH_SERIAL, 2);
	er_set_health(f, CH_TPM, 4);
	er_set_health(f, CH_MMIO, 14);
	er_set_health(f, CH_I3C, 0);
	f->drop_read = 2;
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(f->core->ch0, OBMF_CH_TIMEOUTS), 1);
	KUNIT_EXPECT_NOT_NULL(test, f->core->chans[CH_GPIO]);
	KUNIT_EXPECT_NOT_NULL(test, f->core->chans[CH_IPMI]);
	KUNIT_EXPECT_NOT_NULL(test, f->core->chans[CH_SPI]);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_CH_HEALTH), 3);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_CH_HEALTH), 0);
	/* Discovery runs in the bind work: its lines, not the case's stub */
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, ": health "), 3);
	for (i = 0; i < ARRAY_SIZE(lines); i++)
		KUNIT_EXPECT_EQ_MSG(test, test_log_lines(test, log, lines[i]),
				    1, "%s", lines[i]);
}

static struct kunit_case obmf_report_cases[] = {
	KUNIT_CASE(obmf_test_report_no_response),
	KUNIT_CASE(obmf_test_report_status),
	KUNIT_CASE(obmf_test_report_malformed),
	KUNIT_CASE(obmf_test_report_fdev_core),
	KUNIT_CASE(obmf_test_report_late),
	KUNIT_CASE(obmf_test_report_answer),
	KUNIT_CASE(obmf_test_report_sites),
	KUNIT_CASE(obmf_test_report_rebind),
	KUNIT_CASE(obmf_test_report_datagram),
	KUNIT_CASE(obmf_test_report_health),
	KUNIT_CASE(obmf_test_discover_unusable_many),
	KUNIT_CASE(obmf_test_report_failed_op),
	KUNIT_CASE(obmf_test_report_failed_unpublished),
	KUNIT_CASE(obmf_test_report_fatal),
	KUNIT_CASE(obmf_test_report_text),
	{}
};

static struct kunit_suite obmf_report_suite = {
	.name = "obmf-report",
	.init = obmf_report_test_init,
	.exit = test_log_exit,
	.test_cases = obmf_report_cases,
};

kunit_test_suites(&obmf_core_suite, &obmf_ctrl_suite, &obmf_bus_suite,
		  &obmf_report_suite);

/* The I2C controller driver on the fake transport; only when built in */
#if IS_BUILTIN(CONFIG_I2C_OBMF)

#define I2C_ADDR 0x50
#define I2C_CMD 0x10

/* Channel 1: I2C at 100 kHz, bus recovery offered (Table 79: 0x00, 0x01) */
static const struct test_chan i2c_chans[] = {
	{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c", 516, 0, 2 },
};

static struct obmf_fake *i2c_setup(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, i2c_chans, ARRAY_SIZE(i2c_chans));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	return f;
}

KUNIT_DEFINE_ACTION_WRAPPER(test_put_device, put_device, struct device *);

/* The adapter the I2C driver registered on channel 1 */
static struct i2c_adapter *i2c_adapter(struct obmf_fake *f)
{
	struct kunit *test = f->test;
	struct obmf_channel *ch = chan(f, CH_I2C);
	struct i2c_adapter *adap;
	struct device *dev;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	dev = device_find_any_child(&ch->fdev->adev.dev);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	ret = kunit_add_action_or_reset(test, test_put_device, dev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	adap = i2c_verify_adapter(dev);
	KUNIT_ASSERT_NOT_NULL(test, adap);
	return adap;
}

/* The PEC of a byte read of I2C_CMD at I2C_ADDR returning @val */
static u8 i2c_read_pec(u8 val)
{
	u8 b[] = { I2C_ADDR << 1, I2C_CMD, I2C_ADDR << 1 | 1, val };

	return i2c_smbus_pec(0, b, sizeof(b));
}

/* The last request on the I2C channel, after the header */
static const u8 *i2c_sent(struct obmf_fake *f, size_t *len)
{
	struct fake_rec *r;
	int i = send_index(f, CH_I2C);

	KUNIT_ASSERT_GE(f->test, i, 0);
	r = &f->log[i];
	*len = r->len - OBMF_HDR_LEN;
	return r->msg + OBMF_HDR_LEN;
}

/*
 * An SMBus byte read and write of a client with PEC: one Write Read with
 * the PEC byte read last, one Write with it written last; a wrong PEC
 * from the device is -EBADMSG from the I2C core
 */
static void obmf_test_i2c_pec(struct kunit *test)
{
	struct obmf_fake *f = i2c_setup(test);
	struct i2c_adapter *adap = i2c_adapter(f);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 5,
		.payload = { 2, 0, 0, 0x5a, i2c_read_pec(0x5a) },
	};
	const u8 rd[] = { 0x03, I2C_ADDR, 2, 0, I2C_CMD };
	u8 wr[] = { 0x01, I2C_ADDR, 0, 0, I2C_CMD, 0x77, 0 };
	u8 pw[] = { I2C_ADDR << 1, I2C_CMD, 0x77 };
	union i2c_smbus_data data;
	const u8 *p;
	size_t len;

	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			i2c_smbus_xfer(adap, I2C_ADDR, I2C_CLIENT_PEC,
				       I2C_SMBUS_READ, I2C_CMD,
				       I2C_SMBUS_BYTE_DATA, &data),
			0);
	KUNIT_EXPECT_EQ(test, data.byte, 0x5a);
	p = i2c_sent(f, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(rd));
	KUNIT_EXPECT_MEMEQ(test, p, rd, sizeof(rd));

	s.payload[4] ^= 0x01;
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test,
			i2c_smbus_xfer(adap, I2C_ADDR, I2C_CLIENT_PEC,
				       I2C_SMBUS_READ, I2C_CMD,
				       I2C_SMBUS_BYTE_DATA, &data),
			-EBADMSG);

	/* the device answers the write with the echo alone */
	s.plen = 0;
	fake_push(f, &s);
	data.byte = 0x77;
	KUNIT_EXPECT_EQ(test,
			i2c_smbus_xfer(adap, I2C_ADDR, I2C_CLIENT_PEC,
				       I2C_SMBUS_WRITE, I2C_CMD,
				       I2C_SMBUS_BYTE_DATA, &data),
			0);
	wr[6] = i2c_smbus_pec(0, pw, sizeof(pw));
	p = i2c_sent(f, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(wr));
	KUNIT_EXPECT_MEMEQ(test, p, wr, sizeof(wr));
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), 3);
}

/*
 * Message flags the controller does not implement are refused unsent,
 * alone or with the PEC flag, and so is an SCCB client's transfer
 */
static void obmf_test_i2c_flags(struct kunit *test)
{
	static const u16 flags[] = {
		I2C_M_NOSTART,
		I2C_M_REV_DIR_ADDR,
		I2C_M_IGNORE_NAK,
		I2C_M_NO_RD_ACK | I2C_M_RD,
		I2C_M_RECV_LEN | I2C_M_RD,
		I2C_M_STOP,
		I2C_M_STOP | I2C_CLIENT_PEC,
	};
	struct obmf_fake *f = i2c_setup(test);
	struct i2c_adapter *adap = i2c_adapter(f);
	u8 buf[1] = { I2C_CMD };
	struct i2c_msg msg = { .addr = I2C_ADDR, .len = 1, .buf = buf };
	union i2c_smbus_data data;
	int i;

	for (i = 0; i < ARRAY_SIZE(flags); i++) {
		msg.flags = flags[i];
		KUNIT_EXPECT_EQ_MSG(test, i2c_transfer(adap, &msg, 1),
				    -EOPNOTSUPP, "flags %#x", flags[i]);
	}
	KUNIT_EXPECT_EQ(test,
			i2c_smbus_xfer(adap, I2C_ADDR, I2C_CLIENT_SCCB,
				       I2C_SMBUS_READ, I2C_CMD,
				       I2C_SMBUS_BYTE_DATA, &data),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), 0);
}

/* An SMBus byte read of I2C_CMD at I2C_ADDR, without PEC */
static int i2c_read_byte(struct i2c_adapter *adap, union i2c_smbus_data *data)
{
	return i2c_smbus_xfer(adap, I2C_ADDR, 0, I2C_SMBUS_READ, I2C_CMD,
			      I2C_SMBUS_BYTE_DATA, data);
}

/* A recovery under the bus lock, which the transfer path holds */
static int i2c_recover(struct i2c_adapter *adap)
{
	int ret;

	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	ret = i2c_recover_bus(adap);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return ret;
}

/*
 * The channel offers bus recovery, but the adapter has none, and
 * i2c_recover_bus() gives -EBUSY without a request. A busy or timeout
 * status, generic or of the I2C table, or no response fails the
 * transfer, and nothing else is sent.
 */
static void obmf_test_i2c_no_recovery(struct kunit *test)
{
	static const struct {
		u8 status;
		int err;
	} st[] = {
		{ OBMF_STATUS_BUSY, -EBUSY },
		{ OBMF_STATUS_TIMEOUT, -ETIMEDOUT },
		{ OBMF_STATUS_I2C_BUS_BUSY, -EBUSY },
		{ OBMF_STATUS_I2C_TIMEOUT, -ETIMEDOUT },
	};
	struct obmf_fake *f = i2c_setup(test);
	struct i2c_adapter *adap = i2c_adapter(f);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
	};
	u32 opts = fake_ch_off(f, CH_I2C) + FAKE_CH_HDR_LEN + 1;
	union i2c_smbus_data data;
	unsigned int sends = READ_ONCE(f->sends);
	int i;

	/* Table 79 options: BUS_RECOVERY_SUPPORTED */
	KUNIT_ASSERT_NE(test, f->mem[opts] & BIT(0), 0);
	KUNIT_EXPECT_NULL(test, adap->bus_recovery_info);
	KUNIT_EXPECT_EQ(test, i2c_recover(adap), -EBUSY);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends);
	for (i = 0; i < ARRAY_SIZE(st); i++) {
		s.status = st[i].status;
		fake_push(f, &s);
		KUNIT_EXPECT_EQ_MSG(test, i2c_read_byte(adap, &data), st[i].err,
				    "status %#x", st[i].status);
		KUNIT_EXPECT_EQ_MSG(test, READ_ONCE(f->sends), sends + i + 1,
				    "status %#x", st[i].status);
	}

	s.how = FAKE_NEVER;
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + ARRAY_SIZE(st) + 1);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0),
			ARRAY_SIZE(st) + 1);
}

/*
 * No response, and the reply comes late, while the next transfer waits:
 * the next transfer is sent after the late reply, which is dropped, and
 * gets its own reply
 */
static void obmf_test_i2c_late_reply(struct kunit *test)
{
	struct obmf_fake *f = i2c_setup(test);
	struct i2c_adapter *adap = i2c_adapter(f);
	struct fake_step s = {
		.how = FAKE_WORK,
		.delay_ms = OBMF_DEFAULT_TIMEOUT_MS + 500,
		.channel = -1,
		.echo = true,
		.plen = 4,
		.payload = { 1, 0, 0, 0x5a },
	};
	union i2c_smbus_data data = {};
	int a, b, late;

	fake_push(f, &s);
	s.how = FAKE_INLINE;
	s.payload[3] = 0xa5;
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), 0);
	KUNIT_EXPECT_EQ(test, data.byte, 0xa5);

	a = fake_find(f, 0, FAKE_SEND, CH_I2C, 0);
	KUNIT_ASSERT_GE(test, a, 0);
	late = fake_find(f, a + 1, FAKE_RX, CH_I2C, -1);
	b = fake_find(f, a + 1, FAKE_SEND, -1, -1);
	KUNIT_ASSERT_GE(test, late, 0);
	KUNIT_EXPECT_GT(test, b, late);
	KUNIT_EXPECT_EQ(test, b, fake_find(f, a + 1, FAKE_SEND, CH_I2C, 0));
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, CH_I2C), OBMF_CH_TIMEOUTS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, CH_I2C), OBMF_CH_RX_LATE), 1);
}

/*
 * Table 79: with BUS_RECOVERY_SUPPORTED 0 the adapter has no recovery, and
 * i2c_recover_bus() gives -EBUSY without a request
 */
static void obmf_test_i2c_recover_none(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct i2c_adapter *adap;
	unsigned int n;

	fake_layout(f, i2c_chans, ARRAY_SIZE(i2c_chans));
	f->mem[fake_ch_off(f, CH_I2C) + FAKE_CH_HDR_LEN + 1] = 0;
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	adap = i2c_adapter(f);
	KUNIT_EXPECT_NULL(test, adap->bus_recovery_info);
	n = fake_nlog(f);
	KUNIT_EXPECT_EQ(test, i2c_recover(adap), -EBUSY);
	KUNIT_EXPECT_EQ(test, fake_nlog(f), n);
}

/* Whether a driver is bound to channel @nr's function device */
static bool i2c_bound(struct obmf_fake *f, int nr)
{
	struct obmf_fdev *fdev = chan(f, nr)->fdev;

	KUNIT_ASSERT_NOT_NULL(f->test, fdev);
	return fdev->adev.dev.driver;
}

/*
 * A configuration shorter than Table 79's 2 bytes gets no adapter; one of
 * 2 bytes does. Expected: "configuration data too short" and a failed
 * probe, for channels 1 and 2.
 */
static void obmf_test_i2c_short_config(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-0", 516 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-1", 516, 0,
		  1 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-2", 516, 0,
		  2 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, c, ARRAY_SIZE(c));
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_FALSE(test, i2c_bound(f, 1));
	KUNIT_EXPECT_FALSE(test, i2c_bound(f, 2));
	KUNIT_EXPECT_TRUE(test, i2c_bound(f, 3));
}

static const struct property_entry sw_speed_props1[] = {
	PROPERTY_ENTRY_U32("reg", 1),
	PROPERTY_ENTRY_U32("clock-frequency", 400000),
	{},
};

static const struct property_entry sw_speed_props2[] = {
	PROPERTY_ENTRY_U32("reg", 2),
	PROPERTY_ENTRY_U32("clock-frequency", 400000),
	{},
};

static const struct software_node sw_speed = { .name = "obmf-test-speed" };

static const struct software_node sw_speed_i2c1 = {
	.name = "i2c@1",
	.parent = &sw_speed,
	.properties = sw_speed_props1,
};

static const struct software_node sw_speed_i2c2 = {
	.name = "i2c@2",
	.parent = &sw_speed,
	.properties = sw_speed_props2,
};

static const struct software_node *const sw_speed_nodes[] = {
	&sw_speed,
	&sw_speed_i2c1,
	&sw_speed_i2c2,
	NULL,
};

/* Channel @nr's Table 79 I2C_CONTROLLER_SPEED */
static void i2c_speed_set(struct obmf_fake *f, int nr, u8 speed)
{
	f->mem[fake_ch_off(f, nr) + FAKE_CH_HDR_LEN] = speed;
}

/*
 * Speed codes 0 to 3 get an adapter, the reserved 4 and 0x7f
 * none. A clock-frequency in the channel's node other than the channel's
 * speed gets none, an equal one an adapter. Expected: "unknown bus speed
 * 4" and "unknown bus speed 127", then "clock-frequency 400000, channel
 * runs at 100000".
 */
static void obmf_test_i2c_speed(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-1", 516, 0,
		  2 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-2", 516, 0,
		  2 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-3", 516, 0,
		  2 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-4", 516, 0,
		  2 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-5", 516, 0,
		  2 },
		{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c-6", 516, 0,
		  2 },
	};
	static const u8 speed[] = { 0, 1, 2, 3, 4, 0x7f };
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	int i, ret;

	fake_layout(f, c, ARRAY_SIZE(c));
	for (i = 0; i < ARRAY_SIZE(speed); i++)
		i2c_speed_set(f, i + 1, speed[i]);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	for (i = 0; i < ARRAY_SIZE(speed); i++)
		KUNIT_EXPECT_EQ_MSG(test, i2c_bound(f, i + 1), speed[i] < 4,
				    "speed %u", speed[i]);

	ret = kunit_software_node_register_node_group(test, sw_speed_nodes);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f = fake_create(test, FAKE_MAX_MSG, 0);
	ret = kunit_device_add_software_node(test, f->tp.dev, &sw_speed);
	KUNIT_ASSERT_EQ(test, ret, 0);
	fake_layout(f, c, 2);
	i2c_speed_set(f, 1, 0);
	i2c_speed_set(f, 2, 1);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_FALSE(test, i2c_bound(f, 1));
	KUNIT_EXPECT_TRUE(test, i2c_bound(f, 2));
}

/* The next request on the I2C channel after log entry @i is @want */
static int i2c_next(struct obmf_fake *f, int i, const u8 *want, size_t len)
{
	struct kunit *test = f->test;
	struct fake_rec *r;

	i = fake_find(f, i + 1, FAKE_SEND, CH_I2C, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	r = &f->log[i];
	KUNIT_ASSERT_EQ(test, r->len, OBMF_HDR_LEN + len);
	KUNIT_EXPECT_MEMEQ(test, r->msg + OBMF_HDR_LEN, want, len);
	return i;
}

#define I2C_ADDR2 0x51
#define I2C_ADDR3 0x52

/*
 * A write then a read of one address is
 * one Write Read; any other message is one Read or Write; each carries
 * the address and the Read Length, and all but the last of a transfer
 * "do not send STOP" (bit 7); no other command goes out. A 10-bit or an
 * above 7-bit address is refused before anything is sent, also as the
 * second message, and the adapter offers no SMBus Quick.
 */
static void obmf_test_i2c_xfer(struct kunit *test)
{
	static const u8 wr_rd[] = { 0x03, I2C_ADDR, 2, 0, I2C_CMD };
	static const u8 chain[][5] = {
		{ 0x81, I2C_ADDR, 0, 0, 1 },
		{ 0x81, I2C_ADDR2, 0, 0, 3 },
		{ 0x00, I2C_ADDR3, 1, 0 },
	};
	static const u8 rd_wr[][5] = {
		{ 0x80, I2C_ADDR, 1, 0 },
		{ 0x01, I2C_ADDR, 0, 0, 5 },
	};
	struct obmf_fake *f = i2c_setup(test);
	struct i2c_adapter *adap = i2c_adapter(f);
	struct fake_step rd = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 5,
		.payload = { 2, 0, 0, 0xaa, 0xbb },
	};
	struct fake_step wr = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
	};
	u8 cmd[1] = { I2C_CMD }, w1[1] = { 1 }, w3[1] = { 3 }, w5[1] = { 5 };
	u8 r2[2] = {}, r1[1] = {};
	struct i2c_msg m[3] = {
		{ .addr = I2C_ADDR, .len = 1, .buf = cmd },
		{ .addr = I2C_ADDR, .flags = I2C_M_RD, .len = 2, .buf = r2 },
	};
	int i = fake_nlog(f) - 1;

	fake_push(f, &rd);
	KUNIT_EXPECT_EQ(test, i2c_transfer(adap, m, 2), 2);
	KUNIT_EXPECT_EQ(test, r2[0], 0xaa);
	KUNIT_EXPECT_EQ(test, r2[1], 0xbb);
	i = i2c_next(f, i, wr_rd, sizeof(wr_rd));

	m[0] = (struct i2c_msg){ .addr = I2C_ADDR, .len = 1, .buf = w1 };
	m[1] = (struct i2c_msg){ .addr = I2C_ADDR2, .len = 1, .buf = w3 };
	m[2] = (struct i2c_msg){
		.addr = I2C_ADDR3, .flags = I2C_M_RD, .len = 1, .buf = r1
	};
	fake_push(f, &wr);
	fake_push(f, &wr);
	rd.plen = 4;
	rd.payload[0] = 1;
	fake_push(f, &rd);
	KUNIT_EXPECT_EQ(test, i2c_transfer(adap, m, 3), 3);
	KUNIT_EXPECT_EQ(test, r1[0], 0xaa);
	i = i2c_next(f, i, chain[0], 5);
	i = i2c_next(f, i, chain[1], 5);
	i = i2c_next(f, i, chain[2], 4);

	m[0] = (struct i2c_msg){
		.addr = I2C_ADDR, .flags = I2C_M_RD, .len = 1, .buf = r1
	};
	m[1] = (struct i2c_msg){ .addr = I2C_ADDR, .len = 1, .buf = w5 };
	fake_push(f, &rd);
	fake_push(f, &wr);
	KUNIT_EXPECT_EQ(test, i2c_transfer(adap, m, 2), 2);
	i = i2c_next(f, i, rd_wr[0], 4);
	i = i2c_next(f, i, rd_wr[1], 5);
	KUNIT_EXPECT_LT(test, fake_find(f, i + 1, FAKE_SEND, CH_I2C, 0), 0);

	m[1].flags = I2C_M_TEN;
	KUNIT_EXPECT_EQ(test, i2c_transfer(adap, m, 2), -EAFNOSUPPORT);
	m[1].flags = 0;
	m[1].addr = 0x80;
	KUNIT_EXPECT_EQ(test, i2c_transfer(adap, m, 2), -EINVAL);
	KUNIT_EXPECT_LT(test, fake_find(f, i + 1, FAKE_SEND, CH_I2C, 0), 0);

	KUNIT_EXPECT_TRUE(test, i2c_check_functionality(adap, I2C_FUNC_I2C));
	KUNIT_EXPECT_FALSE(test,
			   i2c_check_functionality(adap, I2C_FUNC_SMBUS_QUICK));
}

/*
 * A response whose Read Length is not the one asked, or that
 * is shorter than it, is -EPROTO and a protocol error; the Reserved byte
 * is not checked, and a write answered with Read Length 0 succeeds. The
 * statuses that have no I2C fault code, Unknown Channel and Size Not
 * Supported, and a stalled send are -EIO, -EOPNOTSUPP and -EIO.
 */
static void obmf_test_i2c_response(struct kunit *test)
{
	static const struct {
		u8 status;
		int send_ret;
		int err;
	} st[] = {
		{ OBMF_STATUS_UNKNOWN_CHANNEL, 0, -EIO },
		{ OBMF_STATUS_SIZE_UNSUPPORTED, 0, -EOPNOTSUPP },
		{ 0, -EPIPE, -EIO },
	};
	struct obmf_fake *f = i2c_setup(test);
	struct i2c_adapter *adap = i2c_adapter(f);
	struct obmf_channel *ch = chan(f, CH_I2C);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 4,
		.payload = { 2, 0, 0, 0x5a },
	};
	union i2c_smbus_data data = {};
	long errs = ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS);
	int i;

	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), -EPROTO);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), errs + 1);
	s.plen = 3;
	s.payload[0] = 1;
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), -EPROTO);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), errs + 2);

	s.plen = 4;
	s.payload[2] = 0xff;
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), 0);
	KUNIT_EXPECT_EQ(test, data.byte, 0x5a);
	s.plen = 3;
	s.payload[0] = 0;
	s.payload[2] = 0;
	fake_push(f, &s);
	data.byte = 0x77;
	KUNIT_EXPECT_EQ(test,
			i2c_smbus_xfer(adap, I2C_ADDR, 0, I2C_SMBUS_WRITE,
				       I2C_CMD, I2C_SMBUS_BYTE_DATA, &data),
			0);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), errs + 2);

	s.plen = 0;
	for (i = 0; i < ARRAY_SIZE(st); i++) {
		s.status = st[i].status;
		s.send_ret = st[i].send_ret;
		fake_push(f, &s);
		KUNIT_EXPECT_EQ_MSG(test, i2c_read_byte(adap, &data), st[i].err,
				    "status %#x send %d", st[i].status,
				    st[i].send_ret);
	}
}

/* The adapter name keeps a 32-byte device name and the channel name whole */
static void obmf_test_i2c_name(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, i2c_chans, ARRAY_SIZE(i2c_chans));
	memcpy(f->mem + 6, "0123456789abcdef0123456789abcdef", 32);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_STREQ(test, i2c_adapter(f)->name,
			   "0123456789abcdef0123456789abcdef_i2c");
}

/*
 * The adapter's retries and timeout; a lost arbitration (status 0x41,
 * -EAGAIN) is sent once and then once per retry
 */
static void i2c_tries_check(struct obmf_fake *f, int retries)
{
	struct kunit *test = f->test;
	struct i2c_adapter *adap = i2c_adapter(f);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.status = OBMF_STATUS_I2C_ARB_LOST,
	};
	union i2c_smbus_data data;
	int i, sends = fake_count(f, FAKE_SEND, CH_I2C, 0);

	KUNIT_EXPECT_EQ(test, adap->retries, retries);
	KUNIT_EXPECT_EQ(test, adap->timeout, (retries + 1) * HZ);
	for (i = 0; i <= retries; i++)
		fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), -EAGAIN);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0),
			sends + retries + 1);
	/* i2c_del_adapter() waits for this reference */
	kunit_release_action(test, test_put_device, &adap->dev);
}

/*
 * The I2C core tries a transfer three more times after a lost
 * arbitration, unless the device retries it itself: then once
 */
static void obmf_test_i2c_retries(struct kunit *test)
{
	struct obmf_fake *f = i2c_setup(test);
	struct obmf_channel *ch = chan(f, CH_I2C);
	struct device *dev = &ch->fdev->adev.dev;

	i2c_tries_check(f, 3);
	device_release_driver(dev);
	KUNIT_ASSERT_NULL(test, dev->driver);
	ch->info.quirks.i2c_dev_retries = true;
	KUNIT_ASSERT_EQ(test, device_attach(dev), 1);
	i2c_tries_check(f, 0);
}

/* A client's transfers from another thread while the adapter goes */
struct i2c_ub {
	struct work_struct work;
	struct kunit *test;
	struct i2c_adapter *adap;
	int ret[2];
	s64 ms[2];
	struct completion done;
};

static void i2c_ub_work(struct work_struct *w)
{
	struct i2c_ub *x = container_of(w, struct i2c_ub, work);
	union i2c_smbus_data data;
	ktime_t start;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(x->ret); i++) {
		start = ktime_get();
		x->ret[i] = i2c_read_byte(x->adap, &data);
		x->ms[i] = ktime_ms_delta(ktime_get(), start);
	}
	/* i2c_del_adapter() waits for this reference */
	kunit_release_action(x->test, test_put_device, &x->adap->dev);
	complete(&x->done);
}

/*
 * The driver unbound while the device has stopped answering, a client's
 * transfer waiting: it times out, and the client's next transfer is sent
 * after the hold for the late reply and times out on its own wait, so the
 * adapter goes after two bounded waits. The next binding's transfers are
 * answered.
 */
static void obmf_test_i2c_unbind_silent(struct kunit *test)
{
	struct obmf_fake *f = i2c_setup(test);
	struct obmf_channel *ch = chan(f, CH_I2C);
	struct device *dev = &ch->fdev->adev.dev;
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 4,
		.payload = { 1, 0, 0, 0xa5 },
	};
	union i2c_smbus_data data = {};
	struct i2c_ub *x;
	unsigned int sends;

	x = kunit_kzalloc(test, sizeof(*x), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, x);
	x->test = test;
	x->adap = i2c_adapter(f);
	init_completion(&x->done);
	INIT_WORK(&x->work, i2c_ub_work);
	WRITE_ONCE(f->silent_ch, CH_I2C);
	sends = f->sends;
	queue_work(f->callwq, &x->work);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	device_release_driver(dev);
	KUNIT_ASSERT_TRUE(test, test_wait(&x->done));
	KUNIT_EXPECT_NULL(test, dev->driver);
	KUNIT_EXPECT_EQ(test, x->ret[0], -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, x->ret[1], -ETIMEDOUT);
	KUNIT_EXPECT_GT(test, x->ms[1], OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_EXPECT_LT(test, x->ms[1], 2 * OBMF_DEFAULT_TIMEOUT_MS + 300);
	KUNIT_EXPECT_EQ(test, f->sends, sends + 2);

	WRITE_ONCE(f->silent_ch, 0);
	KUNIT_ASSERT_EQ(test, device_attach(dev), 1);
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(i2c_adapter(f), &data), 0);
	KUNIT_EXPECT_EQ(test, data.byte, 0xa5);
}

/*
 * A transfer while the adapter is suspended is -ESHUTDOWN from the I2C
 * core with nothing sent; after resume it goes out
 */
static void obmf_test_i2c_suspended(struct kunit *test)
{
	struct obmf_fake *f = i2c_setup(test);
	struct i2c_adapter *adap = i2c_adapter(f);
	struct device *dev = &chan(f, CH_I2C)->fdev->adev.dev;
	const struct dev_pm_ops *pm = dev->driver->pm;
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.plen = 4,
		.payload = { 1, 0, 0, 0x5a },
	};
	union i2c_smbus_data data;

	if (!pm || !pm->suspend)
		kunit_skip(test, "no system sleep");
	KUNIT_ASSERT_EQ(test, pm->suspend(dev), 0);
	kunit_warning_suppress(test) {
		KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), -ESHUTDOWN);
		KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), -ESHUTDOWN);
	}
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), 0);
	KUNIT_ASSERT_EQ(test, pm->resume(dev), 0);
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, i2c_read_byte(adap, &data), 0);
	KUNIT_EXPECT_EQ(test, data.byte, 0x5a);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), 1);
}

/*
 * A zero-length message, a write alone, a write before a read or a read,
 * is refused by the I2C core (I2C_AQ_NO_ZERO_LEN) with nothing sent
 */
static void obmf_test_i2c_zero_len(struct kunit *test)
{
	struct obmf_fake *f = i2c_setup(test);
	struct i2c_adapter *adap = i2c_adapter(f);
	u8 r1[1] = {};
	struct i2c_msg m[2] = {
		{ .addr = I2C_ADDR, .len = 0, .buf = r1 },
		{ .addr = I2C_ADDR, .flags = I2C_M_RD, .len = 1, .buf = r1 },
	};

	KUNIT_EXPECT_EQ(test, i2c_transfer(adap, m, 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, i2c_transfer(adap, m, 2), -EOPNOTSUPP);
	m[1].len = 0;
	KUNIT_EXPECT_EQ(test, i2c_transfer(adap, &m[1], 1), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, CH_I2C, 0), 0);
}

static struct kunit_case obmf_i2c_cases[] = {
	KUNIT_CASE(obmf_test_i2c_pec),
	KUNIT_CASE(obmf_test_i2c_flags),
	KUNIT_CASE_SLOW(obmf_test_i2c_no_recovery),
	KUNIT_CASE_SLOW(obmf_test_i2c_late_reply),
	KUNIT_CASE(obmf_test_i2c_recover_none),
	KUNIT_CASE(obmf_test_i2c_short_config),
	KUNIT_CASE(obmf_test_i2c_speed),
	KUNIT_CASE(obmf_test_i2c_xfer),
	KUNIT_CASE(obmf_test_i2c_response),
	KUNIT_CASE(obmf_test_i2c_name),
	KUNIT_CASE(obmf_test_i2c_retries),
	KUNIT_CASE_SLOW(obmf_test_i2c_unbind_silent),
	KUNIT_CASE(obmf_test_i2c_suspended),
	KUNIT_CASE(obmf_test_i2c_zero_len),
	{}
};

static struct kunit_suite obmf_i2c_suite = {
	.name = "obmf-i2c",
	.suite_init = obmf_test_fn_suite_init,
	.suite_exit = obmf_test_fn_suite_exit,
	.test_cases = obmf_i2c_cases,
};

kunit_test_suite(obmf_i2c_suite);

#endif /* IS_BUILTIN(CONFIG_I2C_OBMF) */

/* The SPI controller driver; only when built in */
#if IS_BUILTIN(CONFIG_SPI_OBMF)

#define SPI_CH 1

/* Table 84 flags: read and write words of 16 bits */
#define SPI_CFG_BPW16 (16 << 18 | 16 << 24)

/* The channels at most after the SPI one, of a type no driver takes */
#define SPI_EXTRA 6

/*
 * Channel 1: an SPI controller channel with request limit @limit and
 * response limit @rlimit, @devices chip selects and @flags, at @rd and
 * @wr Hz, then @extra IPMI channels. Bound; the driver's probe result is
 * the case's to check.
 */
static struct obmf_fake *spi_fake_n(struct kunit *test,
				    const struct software_node *node, u16 limit,
				    u16 rlimit, u32 flags, u8 devices, u32 rd,
				    u32 wr, int extra)
{
	struct test_chan c[1 + SPI_EXTRA] = {
		{ OBMF_TYPE_SPI, OBMF_ROLE_PRODUCER, "spi", limit, 0, 20 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.spi_read = true,
	};
	u8 *h;
	int i;

	KUNIT_ASSERT_LE(test, extra, SPI_EXTRA);
	for (i = 1; i <= extra; i++)
		c[i] = (struct test_chan){ OBMF_TYPE_IPMI, OBMF_ROLE_PRODUCER,
					   "ipmi", 516 };
	if (node)
		KUNIT_ASSERT_EQ(test,
				kunit_device_add_software_node(test, f->tp.dev,
							       node),
				0);
	fake_layout(f, c, 1 + extra);
	h = f->mem + fake_ch_off(f, SPI_CH);
	fake_spi_cfg(h + FAKE_CH_HDR_LEN, flags, devices, rd);
	put_unaligned_le32(wr, h + FAKE_CH_HDR_LEN + 16);
	put_unaligned_le16(rlimit, h + 0x19);
	f->dflt = s;
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	return f;
}

static struct obmf_fake *spi_fake_lim(struct kunit *test,
				      const struct software_node *node,
				      u16 limit, u16 rlimit, u32 flags,
				      u8 devices, u32 rd, u32 wr)
{
	return spi_fake_n(test, node, limit, rlimit, flags, devices, rd, wr, 0);
}

static struct obmf_fake *spi_fake(struct kunit *test, u16 limit, u32 flags,
				  u8 devices, u32 rd, u32 wr)
{
	return spi_fake_lim(test, NULL, limit, limit, flags, devices, rd, wr);
}

/* The controller spi-obmf registered on channel 1, or NULL */
static struct spi_controller *spi_ctlr(struct obmf_fake *f)
{
	struct kunit *test = f->test;
	struct obmf_channel *ch = chan(f, SPI_CH);
	struct device *dev;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	dev = device_find_any_child(&ch->fdev->adev.dev);
	if (!dev)
		return NULL;
	ret = kunit_add_action_or_reset(test, test_put_device, dev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return container_of(dev, struct spi_controller, dev);
}

static void spi_dev_remove(void *data)
{
	struct spi_device *spi = data;

	if (device_is_registered(&spi->dev))
		spi_unregister_device(spi);
	spi_dev_put(spi);
}

/*
 * A client on @cs, added as a board's description adds it; NULL if
 * refused. spi_new_device() marks the other chip select slots unused,
 * which some kernels need to add a client at all.
 */
static struct spi_device *spi_client(struct obmf_fake *f, u8 cs, u32 mode,
				     u32 hz)
{
	struct spi_board_info info = {
		.modalias = "obmf-kunit-spi",
		.max_speed_hz = hz,
		.chip_select = cs,
		.mode = mode,
	};
	struct kunit *test = f->test;
	struct spi_controller *ctlr = spi_ctlr(f);
	struct spi_device *spi;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ctlr);
	spi = spi_new_device(ctlr, &info);
	if (!spi)
		return NULL;
	spi_dev_get(spi);
	ret = kunit_add_action_or_reset(test, spi_dev_remove, spi);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return spi;
}

/* Request @n on channel 1 since @from: command byte, sizes, data */
static const u8 *spi_sent(struct obmf_fake *f, unsigned int from, int n,
			  size_t *len)
{
	int i = from - 1;

	while (n-- >= 0) {
		i = fake_find(f, i + 1, FAKE_SEND, SPI_CH, 0);
		KUNIT_ASSERT_GE(f->test, i, 0);
	}
	*len = f->log[i].len - OBMF_HDR_LEN;
	return f->log[i].msg + OBMF_HDR_LEN;
}

/*
 * Configurations the driver cannot serve leave the channel without a
 * controller, and nothing registered; 4 devices at 20 Hz (one byte a
 * transfer) and limits one byte above the heads are served
 */
static void obmf_test_spi_probe(struct kunit *test)
{
	static const struct {
		const char *what;
		u16 limit;
		u32 flags;
		u8 devices;
		u32 rd;
		u32 wr;
		bool ok;
	} c[] = {
		{ "0 devices", 516, 0, 0, 1000000, 1000000 },
		{ "5 devices", 516, 0, 5, 1000000, 1000000 },
		{ "no read clock", 516, 0, 1, 0, 1000000 },
		{ "no write clock", 516, 0, 1, 1000000, 0 },
		{ "19 Hz", 516, 0, 1, 19, 1000000 },
		{ "NO_CS", 516, SPI_NO_CS, 1, 1000000, 1000000 },
		{ "READY", 516, SPI_READY, 1, 1000000, 1000000 },
		{ "CS_WORD", 516, SPI_CS_WORD, 1, 1000000, 1000000 },
		{ "TX_QUAD", 516, SPI_TX_QUAD, 1, 1000000, 1000000 },
		{ "RX_OCTAL", 516, SPI_RX_OCTAL, 1, 1000000, 1000000 },
		{ "16-bit words", 516, SPI_CFG_BPW16, 1, 1000000, 1000000 },
		{ "limits 5", 9, 0, 1, 1000000, 1000000 },
		{ "4 devices, 20 Hz", 516, 0, 4, 20, 20, true },
		{ "limits 6, mode 3", 10, SPI_MODE_3, 1, 1000000, 1000000,
		  true },
		{ "8-bit words", 516, 8 << 18 | 8 << 24, 1, 1000000, 1000000,
		  true },
	};
	struct spi_controller *ctlr;
	struct device_driver *drv;
	struct obmf_fake *f;
	int i;

	for (i = 0; i < ARRAY_SIZE(c); i++) {
		f = spi_fake(test, c[i].limit, c[i].flags, c[i].devices,
			     c[i].rd, c[i].wr);
		ctlr = spi_ctlr(f);
		KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, SPI_CH, -1), 0);
		if (!c[i].ok) {
			KUNIT_EXPECT_NULL_MSG(test, ctlr, "%s", c[i].what);
			drv = chan(f, SPI_CH)->fdev->adev.dev.driver;
			KUNIT_EXPECT_NULL_MSG(test, drv, "%s", c[i].what);
			continue;
		}
		KUNIT_ASSERT_NOT_NULL_MSG(test, ctlr, "%s", c[i].what);
		KUNIT_EXPECT_EQ(test, ctlr->num_chipselect, c[i].devices);
		KUNIT_EXPECT_EQ(test, ctlr->max_speed_hz, c[i].rd);
		KUNIT_EXPECT_EQ(test, ctlr->min_speed_hz, c[i].rd);
		KUNIT_EXPECT_EQ(test, ctlr->mode_bits, c[i].flags & SPI_MODE_3);
		KUNIT_EXPECT_EQ(test, ctlr->bits_per_word_mask,
				SPI_BPW_MASK(8));
	}
}

/*
 * A client must use the channel's mode on the wire and its clock; the
 * SPI core passes CS_WORD, NO_TX and NO_RX and drops a quad mode. The
 * same refusals reach a bound client that calls spi_setup() again,
 * which returns their errno, and so do chip select setup and hold times.
 * User space can ask for another mode or clock at will, so the driver
 * logs those two at debug level only.
 */
static void obmf_test_spi_setup(struct kunit *test)
{
	struct obmf_fake *f =
		spi_fake(test, 516, SPI_MODE_3, 2, 2000000, 1000000);
	struct test_log *log;
	struct spi_device *spi;
	u32 mode;

	/* skips without printk before the case acts; the client's comes later */
	log = test_log_start(test, &chan(f, SPI_CH)->fdev->adev.dev);
	KUNIT_EXPECT_NULL(test, spi_client(f, 0, SPI_MODE_0, 0));
	KUNIT_EXPECT_NULL(test, spi_client(f, 0, SPI_MODE_3 | SPI_CS_HIGH, 0));
	KUNIT_EXPECT_NULL(test, spi_client(f, 0, SPI_MODE_3 | SPI_NO_CS, 0));
	/* rated below the higher of the two clocks */
	KUNIT_EXPECT_NULL(test, spi_client(f, 0, SPI_MODE_3, 1999999));

	spi = spi_client(f, 0, SPI_MODE_3 | SPI_CS_WORD | SPI_NO_TX, 0);
	KUNIT_EXPECT_NOT_NULL(test, spi);
	if (spi)
		KUNIT_EXPECT_EQ(test, spi->max_speed_hz, 2000000);
	spi = spi_client(f, 1, SPI_MODE_3 | SPI_TX_QUAD | SPI_NO_RX, 4000000);
	KUNIT_ASSERT_NOT_NULL(test, spi);
	KUNIT_EXPECT_EQ(test, spi->mode & SPI_TX_QUAD, 0);
	KUNIT_EXPECT_EQ(test, spi->max_speed_hz, 2000000);

	mode = spi->mode;
	log = test_log_start(test, &spi->dev);
	spi->mode = SPI_MODE_0;
	KUNIT_EXPECT_EQ(test, spi_setup(spi), -EINVAL);
	spi->mode = mode | SPI_CS_HIGH;
	KUNIT_EXPECT_EQ(test, spi_setup(spi), -EINVAL);
	spi->mode = mode | SPI_NO_CS;
	KUNIT_EXPECT_EQ(test, spi_setup(spi), -EINVAL);
	spi->mode = mode;
	spi->max_speed_hz = 1999999;
	KUNIT_EXPECT_EQ(test, spi_setup(spi), -EINVAL);
	spi->max_speed_hz = 2000000;
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "channel runs"), 0);
	/* the device times its chip select; a client cannot ask for more */
	spi->cs_setup.value = 10;
	spi->cs_setup.unit = SPI_DELAY_UNIT_NSECS;
	KUNIT_EXPECT_EQ(test, spi_setup(spi), -EINVAL);
	spi->cs_setup.value = 0;
	spi->cs_hold.value = 1;
	spi->cs_hold.unit = SPI_DELAY_UNIT_USECS;
	KUNIT_EXPECT_EQ(test, spi_setup(spi), -EINVAL);
	spi->cs_hold.value = 0;
	KUNIT_EXPECT_EQ(test, spi_setup(spi), 0);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "hold times"), 2);
	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, SPI_CH, -1), 0);
}

/* Request @n since log index @mark is @want */
#define SPI_EXPECT_SENT(f, mark, n, ...)                                 \
	do {                                                             \
		static const u8 _w[] = { __VA_ARGS__ };                  \
		struct obmf_fake *_f = (f);                              \
		int _n = (n);                                            \
		size_t _len;                                             \
		const u8 *_p = spi_sent(_f, mark, _n, &_len);            \
		KUNIT_EXPECT_EQ_MSG(_f->test, _len, sizeof(_w),          \
				    "request %d", _n);                   \
		KUNIT_EXPECT_MEMEQ_MSG(_f->test, _p, _w,                 \
				       min(_len, sizeof(_w)),            \
				       "request %d", _n);                \
	} while (0)

static int spi_sends(struct obmf_fake *f)
{
	return fake_count(f, FAKE_SEND, SPI_CH, -1);
}

/*
 * Each transfer is one request with its bytes; a message is one chip
 * select window on the client's chip select: the first request asserts,
 * the last releases, a cs_change in between releases and the next
 * asserts again, and the last transfer's cs_change is ignored
 */
static void obmf_test_spi_xfer(struct kunit *test)
{
	struct obmf_fake *f = spi_fake(test, 516, 0, 2, 1000000, 1000000);
	struct spi_device *spi = spi_client(f, 1, 0, 0);
	u8 tx[4] = { 0x9f, 1, 2, 3 }, rx[4], want[4];
	struct spi_transfer t[3] = {};
	unsigned int mark;
	int i, n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	for (i = 0; i < ARRAY_SIZE(want); i++)
		want[i] = i ^ 0x5a;

	t[0].tx_buf = tx;
	t[0].rx_buf = rx;
	t[0].len = 4;
	mark = f->nlog;
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, t, 1), 0);
	SPI_EXPECT_SENT(f, mark, 0, 0x73, 4, 0, 4, 0, 0x9f, 1, 2, 3);
	KUNIT_EXPECT_MEMEQ(test, rx, want, 4);

	memset(t, 0, sizeof(t));
	t[0].tx_buf = tx;
	t[0].len = 1;
	t[1].rx_buf = rx;
	t[1].len = 3;
	t[2].tx_buf = tx + 1;
	t[2].len = 2;
	memset(rx, 0, sizeof(rx));
	mark = f->nlog;
	n = spi_sends(f);
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, t, 3), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 3);
	SPI_EXPECT_SENT(f, mark, 0, 0x62, 1, 0, 0, 0, 0x9f);
	SPI_EXPECT_SENT(f, mark, 1, 0x41, 0, 0, 3, 0);
	SPI_EXPECT_SENT(f, mark, 2, 0x52, 2, 0, 0, 0, 1, 2);
	KUNIT_EXPECT_MEMEQ(test, rx, want, 3);

	t[1].cs_change = 1;
	t[2].cs_change = 1;
	mark = f->nlog;
	n = spi_sends(f);
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, t, 3), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 3);
	SPI_EXPECT_SENT(f, mark, 0, 0x62, 1, 0, 0, 0, 0x9f);
	SPI_EXPECT_SENT(f, mark, 1, 0x51, 0, 0, 3, 0);
	SPI_EXPECT_SENT(f, mark, 2, 0x72, 2, 0, 0, 0, 1, 2);

	/* the next message asserts afresh */
	mark = f->nlog;
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, t, 1), 0);
	SPI_EXPECT_SENT(f, mark, 0, 0x72, 1, 0, 0, 0, 0x9f);
}

/*
 * Messages whose chip select moves or delays the channel cannot follow
 * are -EINVAL with nothing sent, and no line logged (user space can send
 * them at will); a transfer without data in the middle, with only a
 * delay, is kept
 */
static void obmf_test_spi_refused(struct kunit *test)
{
	static const char *const what[] = {
		"cs_off",
		"word delay",
		"delay on the last",
		"delay on cs_change",
		"first without data",
		"last without data",
		"cs_change without data",
		"delay after cs_change",
	};
	struct obmf_fake *f = spi_fake(test, 516, 0, 1, 1000000, 1000000);
	struct spi_device *spi = spi_client(f, 0, 0, 0);
	struct spi_transfer t[3];
	struct test_log *log;
	u8 buf[1] = { 0x05 };
	int i, n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	log = test_log_start(test, &spi->dev);
	for (i = 0; i <= ARRAY_SIZE(what); i++) {
		memset(t, 0, sizeof(t));
		for (n = 0; n < 3; n++) {
			t[n].tx_buf = buf;
			t[n].len = 1;
		}
		switch (i) {
		case 0:
			t[1].cs_off = 1;
			break;
		case 1:
			t[0].word_delay.value = 1;
			break;
		case 2:
			t[2].delay.value = 1;
			break;
		case 3:
			t[1].cs_change = 1;
			t[1].delay.value = 1;
			break;
		case 4:
			t[0].tx_buf = NULL;
			t[0].len = 0;
			break;
		case 5:
			t[2].tx_buf = NULL;
			break;
		case 6:
			t[1].tx_buf = NULL;
			t[1].len = 0;
			t[1].cs_change = 1;
			break;
		case 7:
			t[0].cs_change = 1;
			t[1].tx_buf = NULL;
			t[1].len = 0;
			t[1].delay.value = 1;
			break;
		default:
			t[1].tx_buf = NULL;
			t[1].len = 0;
			t[1].delay.value = 1;
		}
		n = spi_sends(f);
		if (i < ARRAY_SIZE(what)) {
			KUNIT_EXPECT_EQ_MSG(test, spi_sync_transfer(spi, t, 3),
					    -EINVAL, "%s", what[i]);
			KUNIT_EXPECT_EQ_MSG(test, spi_sends(f), n, "%s",
					    what[i]);
		} else {
			KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, t, 3), 0);
			KUNIT_EXPECT_EQ(test, spi_sends(f), n + 2);
		}
	}
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "refused"), 0);
}

/*
 * A transfer is at most the request limit less 5, the response limit
 * less 3 (here 37, the lower: the limits count the header) and 1/20 of
 * the clock in bytes; the SPI core cuts a longer one with the chip
 * select held
 */
static void obmf_test_spi_limits(struct kunit *test)
{
	struct obmf_fake *f =
		spi_fake_lim(test, NULL, 64, 44, 0, 1, 1000000, 1000000);
	struct spi_device *spi = spi_client(f, 0, 0, 0);
	struct spi_transfer t = {};
	unsigned int mark;
	size_t len;
	u8 *buf;
	int n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	buf = kunit_kzalloc(test, 64, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	KUNIT_EXPECT_EQ(test, spi_max_transfer_size(spi), 37);

	t.tx_buf = buf;
	t.len = 37;
	n = spi_sends(f);
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, &t, 1), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 1);
	t.len = 38;
	mark = f->nlog;
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, &t, 1), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 3);
	KUNIT_EXPECT_EQ(test, spi_sent(f, mark, 0, &len)[0], 0x22);
	KUNIT_EXPECT_EQ(test, spi_sent(f, mark, 1, &len)[0], 0x12);

	t.tx_buf = NULL;
	t.rx_buf = buf;
	t.len = 38;
	mark = f->nlog;
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, &t, 1), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 5);
	SPI_EXPECT_SENT(f, mark, 0, 0x21, 0, 0, 37, 0);
	SPI_EXPECT_SENT(f, mark, 1, 0x11, 0, 0, 1, 0);
	KUNIT_EXPECT_EQ(test, buf[36], 36 ^ 0x5a);
	KUNIT_EXPECT_EQ(test, buf[37], 0x5a);

	/* 200 Hz: 10 bytes a transfer */
	f = spi_fake(test, 516, 0, 1, 200, 400);
	spi = spi_client(f, 0, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, spi);
	KUNIT_EXPECT_EQ(test, spi_max_transfer_size(spi), 10);
	t.tx_buf = buf;
	t.rx_buf = NULL;
	t.len = 25;
	t.speed_hz = 0;
	mark = f->nlog;
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, &t, 1), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), 3);
	KUNIT_EXPECT_EQ(test, spi_sent(f, mark, 2, &len)[1], 5);
}

/*
 * A failed transfer ends the message with its errno; then one write
 * without data releases the chip select, and the failed command is not
 * sent again. A reply of the wrong size is -EPROTO, counted.
 */
static void obmf_test_spi_errors(struct kunit *test)
{
	static const struct {
		u8 status;
		int err;
	} st[] = {
		{ OBMF_STATUS_INVALID, -EIO },
		{ OBMF_STATUS_TIMEOUT, -ETIMEDOUT },
		{ OBMF_STATUS_BUSY, -EBUSY },
		{ OBMF_STATUS_PERMANENT_ERROR, -EIO },
		{ OBMF_STATUS_UNKNOWN_CHANNEL, -EIO },
		{ OBMF_STATUS_SIZE_UNSUPPORTED, -EMSGSIZE },
		{ 0x40, -EIO },
		{ 0x7f, -EIO },
	};
	struct obmf_fake *f = spi_fake(test, 516, 0, 2, 1000000, 1000000);
	struct spi_device *spi = spi_client(f, 1, 0, 0);
	struct fake_step s = { .how = FAKE_INLINE,
			       .channel = -1,
			       .echo = true };
	u8 buf[2] = { 0x03, 0x00 };
	struct spi_transfer t[2] = {
		{ .tx_buf = buf, .len = 2 },
		{ .rx_buf = buf, .len = 2 },
	};
	unsigned int mark;
	int i, n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	for (i = 0; i < ARRAY_SIZE(st); i++) {
		s.status = st[i].status;
		fake_push(f, &s);
		mark = f->nlog;
		n = spi_sends(f);
		KUNIT_EXPECT_EQ_MSG(test, spi_sync_transfer(spi, t, 2),
				    st[i].err, "status %#x", st[i].status);
		KUNIT_EXPECT_EQ_MSG(test, spi_sends(f), n + 2, "status %#x",
				    st[i].status);
		SPI_EXPECT_SENT(f, mark, 1, 0x52, 0, 0, 0, 0);
	}

	/* a write answered with a read size */
	s.status = 0;
	s.plen = 2;
	s.payload[0] = 2;
	fake_push(f, &s);
	mark = f->nlog;
	n = spi_sends(f);
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, t, 2), -EPROTO);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 2);
	SPI_EXPECT_SENT(f, mark, 1, 0x52, 0, 0, 0, 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, SPI_CH), OBMF_CH_PROTOCOL_ERRORS),
			1);

	/* a read answered short, and one whose size is not the read's */
	s.plen = 3;
	s.payload[0] = 2;
	fake_push(f, &s);
	n = spi_sends(f);
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, &t[1], 1), -EPROTO);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 2);
	s.plen = 4;
	s.payload[0] = 1;
	fake_push(f, &s);
	n = spi_sends(f);
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, &t[1], 1), -EPROTO);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, SPI_CH), OBMF_CH_PROTOCOL_ERRORS),
			3);

	/* a failed release is logged; the next message asserts again */
	s.status = OBMF_STATUS_INVALID;
	s.plen = 0;
	fake_push(f, &s);
	fake_push(f, &s);
	n = spi_sends(f);
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, t, 2), -EIO);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 2);
	mark = f->nlog;
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, t, 2), 0);
	SPI_EXPECT_SENT(f, mark, 0, 0x62, 2, 0, 0, 0, 0x03, 0x00);
}

/*
 * No answer: -ETIMEDOUT, and the release goes out after the core's hold
 * for the late answer
 */
static void obmf_test_spi_timeout(struct kunit *test)
{
	struct obmf_fake *f = spi_fake(test, 516, 0, 1, 1000000, 1000000);
	struct spi_device *spi = spi_client(f, 0, 0, 0);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	u8 buf[1] = { 0x05 };
	struct spi_transfer t = { .tx_buf = buf, .rx_buf = buf, .len = 1 };
	unsigned int mark;
	int n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	fake_push(f, &s);
	mark = f->nlog;
	n = spi_sends(f);
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, &t, 1), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 2);
	SPI_EXPECT_SENT(f, mark, 0, 0x33, 1, 0, 1, 0, 0x05);
	SPI_EXPECT_SENT(f, mark, 1, 0x12, 0, 0, 0, 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, SPI_CH), OBMF_CH_TIMEOUTS), 1);
}

static int spi_one(struct spi_device *spi)
{
	u8 buf[1] = { 0x05 };
	struct spi_transfer t = { .tx_buf = buf, .len = 1 };

	return spi_sync_transfer(spi, &t, 1);
}

/*
 * Unbinding the driver from the channel, with a client and the device
 * answering, removes the client and sends nothing; bound again, the
 * channel serves a new client
 */
static void obmf_test_spi_unbind(struct kunit *test)
{
	struct obmf_fake *f = spi_fake(test, 516, 0, 1, 1000000, 1000000);
	struct spi_device *spi = spi_client(f, 0, 0, 0);
	struct device *dev = &chan(f, SPI_CH)->fdev->adev.dev;
	int n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	KUNIT_EXPECT_EQ(test, spi_one(spi), 0);
	n = spi_sends(f);
	device_release_driver(dev);
	KUNIT_EXPECT_FALSE(test, device_is_registered(&spi->dev));
	KUNIT_EXPECT_EQ(test, spi_sends(f), n);
	KUNIT_ASSERT_EQ(test, device_attach(dev), 1);
	spi = spi_client(f, 0, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, spi);
	KUNIT_EXPECT_EQ(test, spi_one(spi), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 1);
}

struct spi_queued {
	struct spi_message m;
	struct spi_transfer t;
	u8 buf[1];
	struct completion done;
};

static void spi_queued_done(void *data)
{
	complete(data);
}

/*
 * Unbinding the driver while the device has stopped answering, with two
 * messages an in-kernel client queued: the one waiting for its answer
 * ends -ETIMEDOUT at its timeout, the release after it is sent after the
 * hold and fails on its own wait, with one line, and the next message,
 * whose client has gone, ends -ESHUTDOWN at once; the unbind ends inside
 * the SPI core's 5 s wait for an idle queue
 */
static void spi_unbind_silent(struct kunit *test, struct obmf_fake *f)
{
	static const int want[2] = { -ETIMEDOUT, -ESHUTDOWN };
	struct spi_device *spi = spi_client(f, 0, 0, 0);
	struct spi_controller *ctlr = spi_ctlr(f);
	struct device *dev = &chan(f, SPI_CH)->fdev->adev.dev;
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	unsigned int mark = f->nlog;
	struct test_log *log;
	struct spi_queued *q;
	ktime_t start;
	s64 ms;
	int i, n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	log = test_log_start(test, &spi->dev);
	test_log_also(log, &ctlr->dev);
	/*
	 * Not freed by KUnit: a case that stops while the SPI core still
	 * holds a message leaks it rather than freeing it under the core
	 */
	q = kzalloc_objs(*q, 2, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, q);
	for (i = 0; i < 4; i++)
		fake_push(f, &s);
	n = spi_sends(f);
	for (i = 0; i < 2; i++) {
		q[i].t.tx_buf = q[i].buf;
		q[i].t.len = 1;
		init_completion(&q[i].done);
		spi_message_init_with_transfers(&q[i].m, &q[i].t, 1);
		q[i].m.complete = spi_queued_done;
		q[i].m.context = &q[i].done;
		KUNIT_ASSERT_EQ(test, spi_async(spi, &q[i].m), 0);
	}
	/* the first request is out, waiting for its answer */
	for (i = 0; i < 50 && spi_sends(f) == n; i++)
		msleep(20);
	KUNIT_ASSERT_EQ(test, spi_sends(f), n + 1);

	start = ktime_get();
	device_release_driver(dev);
	ms = ktime_ms_delta(ktime_get(), start);
	KUNIT_EXPECT_LT(test, ms, 4500);
	for (i = 0; i < 2; i++) {
		KUNIT_EXPECT_TRUE(test, completion_done(&q[i].done));
		/* the messages' memory must outlive a failed check */
		if (!wait_for_completion_timeout(&q[i].done, 10 * HZ))
			KUNIT_FAIL_AND_ABORT(test, "message %d never ended", i);
		KUNIT_EXPECT_EQ(test, q[i].m.status, want[i]);
	}
	kfree(q);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 2);
	SPI_EXPECT_SENT(f, mark, 1, 0x12, 0, 0, 0, 0);
	KUNIT_EXPECT_FALSE(test, device_is_registered(&spi->dev));
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, "not released", 1, 1000),
			1);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "destroying queue"), 0);
}

static void obmf_test_spi_unbind_silent(struct kunit *test)
{
	spi_unbind_silent(test, spi_fake(test, 516, 0, 1, 1000000, 1000000));
}

/*
 * The same with a device that takes no data: every send takes its whole
 * timeout and fails, the SPI channel's too, while @n other channels send
 * requests that wait @wait_ms in a loop. The two messages end as above,
 * within the same bound, however long the other channels' sends take.
 */
static void spi_unbind_busy(struct kunit *test, int n, unsigned int wait_ms)
{
	static const int chs[SPI_EXTRA] = { 2, 3, 4, 5, 6, 7 };
	struct obmf_fake *f;
	struct test_busy *b;

	f = spi_fake_n(test, NULL, 516, 516, 0, 1, 1000000, 1000000, n);
	WRITE_ONCE(f->send_sleep_ret, -ETIMEDOUT);
	WRITE_ONCE(f->send_sleep_ms, OBMF_MAX_TIMEOUT_MS);
	b = test_busy_start(f, chs, n, wait_ms);
	spi_unbind_silent(test, f);
	test_busy_end(f, b, n);
	WRITE_ONCE(f->send_sleep_ms, 0);
	WRITE_ONCE(f->send_sleep_ret, 0);
}

/* Five other channels at the default wait */
static void obmf_test_spi_unbind_busy(struct kunit *test)
{
	spi_unbind_busy(test, 5, 0);
}

/* One other channel at the longest wait, as a serial port after timeouts */
static void obmf_test_spi_unbind_long_send(struct kunit *test)
{
	spi_unbind_busy(test, 1, OBMF_MAX_TIMEOUT_MS);
}

#define SPI_SLOW_N 8

/* A message of up to SPI_SLOW_N one-byte writes */
struct spi_slow_msg {
	struct spi_message m;
	struct spi_transfer t[SPI_SLOW_N];
	u8 buf[SPI_SLOW_N];
	struct completion done;
};

/* Every answer from an hrtimer, 100 ms inside the request's wait */
static void spi_slow(struct obmf_fake *f, bool slow)
{
	guard(spinlock_irqsave)
		(&f->lock);
	f->dflt.how = slow ? FAKE_TIMER : FAKE_INLINE;
	f->dflt.delay_ms = slow ? OBMF_DEFAULT_TIMEOUT_MS - 100 : 0;
}

/*
 * @nmsg messages of @nxfer transfers queued on @spi. Not freed by KUnit:
 * a case that stops while the SPI core still holds a message leaks it
 * rather than freeing it under the core.
 */
static struct spi_slow_msg *
spi_slow_queue(struct kunit *test, struct spi_device *spi, int nmsg, int nxfer)
{
	struct spi_slow_msg *q = kzalloc_objs(*q, nmsg, GFP_KERNEL);
	int i, j;

	KUNIT_ASSERT_NOT_NULL(test, q);
	for (i = 0; i < nmsg; i++) {
		for (j = 0; j < nxfer; j++) {
			q[i].buf[j] = j;
			q[i].t[j].tx_buf = &q[i].buf[j];
			q[i].t[j].len = 1;
		}
		init_completion(&q[i].done);
		spi_message_init_with_transfers(&q[i].m, q[i].t, nxfer);
		q[i].m.complete = spi_queued_done;
		q[i].m.context = &q[i].done;
		KUNIT_ASSERT_EQ(test, spi_async(spi, &q[i].m), 0);
	}
	return q;
}

/* Every message ended; a case aborts, leaking them, if one never does */
static void spi_slow_wait(struct kunit *test, struct spi_slow_msg *q, int nmsg)
{
	int i;

	for (i = 0; i < nmsg; i++)
		if (!wait_for_completion_timeout(&q[i].done, 10 * HZ))
			KUNIT_FAIL_AND_ABORT(test, "message %d never ended", i);
}

/* Waits for the first request of the channel after @n */
static void spi_wait_out(struct obmf_fake *f, int n)
{
	int i;

	for (i = 0; i < 50 && spi_sends(f) == n; i++)
		msleep(20);
	KUNIT_ASSERT_EQ(f->test, spi_sends(f), n + 1);
}

/*
 * Unbinding with a device that answers each request just inside its
 * wait and @nmsg messages of @nxfer transfers an in-kernel client queued,
 * the first request out: once the SPI core has removed the client, what
 * is left fails at once without being sent, so the queue drains well
 * inside the core's wait, with no timeout and no core line. @iface
 * unbinds the interface with the device present instead of the driver.
 * The messages are checked by the caller.
 */
static struct spi_slow_msg *spi_unbind_slow(struct kunit *test,
					    struct obmf_fake *f, bool iface,
					    int nmsg, int nxfer, int *sends)
{
	struct spi_device *spi = spi_client(f, 0, 0, 0);
	struct spi_controller *ctlr = spi_ctlr(f);
	struct obmf_channel *ch = chan(f, SPI_CH);
	struct device *dev = &ch->fdev->adev.dev;
	long timeouts = ch_cnt(ch, OBMF_CH_TIMEOUTS);
	struct spi_slow_msg *q;
	struct test_log *log;
	ktime_t start;
	int n, failed;
	s64 ms;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	log = test_log_start(test, iface ? f->tp.dev : &spi->dev);
	test_log_also(log, &ctlr->dev);
	n = spi_sends(f);
	spi_slow(f, true);
	q = spi_slow_queue(test, spi, nmsg, nxfer);
	spi_wait_out(f, n);

	start = ktime_get();
	if (iface)
		fake_unbind(f);
	else
		device_release_driver(dev);
	ms = ktime_ms_delta(ktime_get(), start);
	KUNIT_EXPECT_LT(test, ms, 2500);
	KUNIT_EXPECT_TRUE(test, completion_done(&q[nmsg - 1].done));
	spi_slow_wait(test, q, nmsg);
	KUNIT_EXPECT_FALSE(test, device_is_registered(&spi->dev));
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "destroying queue"), 0);
	if (iface) {
		KUNIT_EXPECT_EQ(test,
				test_log_lines(test, log, "device stopped"), 0);
	} else {
		KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_TIMEOUTS), timeouts);
		KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "not released"),
				0);
		/* the SPI core's line for each message that failed */
		failed = test_log_lines(test, log, "SPI transfer failed");
		KUNIT_EXPECT_EQ(test, failed, nmsg == 1 ? 1 : nmsg - 1);
	}
	*sends = spi_sends(f) - n;
	return q;
}

/* Eight messages of one transfer: the first answered, seven -ESHUTDOWN */
static void spi_unbind_slow_msgs(struct kunit *test, bool iface)
{
	struct obmf_fake *f = spi_fake(test, 516, 0, 1, 1000000, 1000000);
	struct spi_slow_msg *q;
	int i, sends;

	q = spi_unbind_slow(test, f, iface, SPI_SLOW_N, 1, &sends);
	KUNIT_EXPECT_EQ(test, sends, 1);
	KUNIT_EXPECT_EQ(test, q[0].m.status, 0);
	for (i = 1; i < SPI_SLOW_N; i++)
		KUNIT_EXPECT_EQ_MSG(test, q[i].m.status, -ESHUTDOWN,
				    "message %d", i);
	kfree(q);
}

static void obmf_test_spi_unbind_slow(struct kunit *test)
{
	spi_unbind_slow_msgs(test, false);
}

/* The same through the interface's unbind, which stops no timeout here */
static void obmf_test_spi_iface_unbind_slow(struct kunit *test)
{
	spi_unbind_slow_msgs(test, true);
}

/*
 * One message of eight transfers, one chip select window: the first is
 * answered and keeps the chip select, so the next sends only the release
 * that de-asserts it; the message ends -ESHUTDOWN after one transfer
 */
static void obmf_test_spi_unbind_slow_long(struct kunit *test)
{
	struct obmf_fake *f = spi_fake(test, 516, 0, 1, 1000000, 1000000);
	unsigned int mark = f->nlog;
	struct spi_slow_msg *q;
	int sends;

	q = spi_unbind_slow(test, f, false, 1, SPI_SLOW_N, &sends);
	KUNIT_EXPECT_EQ(test, sends, 2);
	KUNIT_EXPECT_EQ(test, q[0].m.status, -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, q[0].m.actual_length, 1);
	kfree(q);
	SPI_EXPECT_SENT(f, mark, 0, 0x22, 1, 0, 0, 0, 0);
	SPI_EXPECT_SENT(f, mark, 1, 0x12, 0, 0, 0, 0);
}

/*
 * One of two clients removed while its messages wait behind one in
 * flight, the device answering slowly: its leftovers fail -ESHUTDOWN
 * with nothing sent, the other client's message goes out, and a new
 * client on the removed one's chip select is served
 */
static void obmf_test_spi_client_removed(struct kunit *test)
{
	struct obmf_fake *f = spi_fake(test, 516, 0, 2, 1000000, 1000000);
	struct spi_device *spi0 = spi_client(f, 0, 0, 0);
	struct spi_device *spi1 = spi_client(f, 1, 0, 0);
	unsigned int mark = f->nlog;
	struct spi_slow_msg *q0, *q1;
	int n;

	KUNIT_ASSERT_NOT_NULL(test, spi0);
	KUNIT_ASSERT_NOT_NULL(test, spi1);
	n = spi_sends(f);
	spi_slow(f, true);
	q0 = spi_slow_queue(test, spi0, 3, 1);
	q1 = spi_slow_queue(test, spi1, 1, 1);
	spi_wait_out(f, n);
	spi_unregister_device(spi0);
	spi_slow_wait(test, q0, 3);
	spi_slow_wait(test, q1, 1);
	KUNIT_EXPECT_EQ(test, q0[0].m.status, 0);
	KUNIT_EXPECT_EQ(test, q0[1].m.status, -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, q0[2].m.status, -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, q1[0].m.status, 0);
	kfree(q0);
	kfree(q1);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 2);
	/* chip select 0, then 1 */
	SPI_EXPECT_SENT(f, mark, 0, 0x32, 1, 0, 0, 0, 0);
	SPI_EXPECT_SENT(f, mark, 1, 0x72, 1, 0, 0, 0, 0);

	spi_slow(f, false);
	spi0 = spi_client(f, 0, 0, 0);
	KUNIT_ASSERT_NOT_NULL(test, spi0);
	KUNIT_EXPECT_EQ(test, spi_one(spi0), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 3);
}

struct spi_call {
	struct work_struct work;
	struct spi_device *spi;
	int ret;
	struct completion done;
};

static void spi_call_work(struct work_struct *w)
{
	struct spi_call *c = container_of(w, struct spi_call, work);

	c->ret = spi_one(c->spi);
	complete(&c->done);
}

/*
 * Unplug with a client bound and its transfer waiting: the transfer ends
 * -ENODEV at once, no release is sent, and the client is removed
 */
static void obmf_test_spi_unplug(struct kunit *test)
{
	struct obmf_fake *f = spi_fake(test, 516, 0, 1, 1000000, 1000000);
	struct spi_device *spi = spi_client(f, 0, 0, 0);
	struct fake_step s = { .how = FAKE_NEVER, .channel = -1 };
	struct spi_call *c;
	unsigned int sends;
	int n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c);
	c->spi = spi;
	init_completion(&c->done);
	INIT_WORK(&c->work, spi_call_work);
	fake_push(f, &s);
	n = spi_sends(f);
	sends = f->sends;
	queue_work(f->callwq, &c->work);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	f->attached = false;
	fake_unbind(f);
	KUNIT_ASSERT_TRUE(test, test_wait(&c->done));
	flush_work(&c->work);
	KUNIT_EXPECT_EQ(test, c->ret, -ENODEV);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 1);
	KUNIT_EXPECT_FALSE(test, device_is_registered(&spi->dev));
}

/*
 * While suspended a message is -ESHUTDOWN from the SPI core with nothing
 * sent; after resume it goes out
 */
static void obmf_test_spi_suspend(struct kunit *test)
{
	struct obmf_fake *f = spi_fake(test, 516, 0, 1, 1000000, 1000000);
	struct spi_device *spi = spi_client(f, 0, 0, 0);
	struct device *dev = &chan(f, SPI_CH)->fdev->adev.dev;
	const struct dev_pm_ops *pm = dev->driver->pm;
	int n;

	KUNIT_ASSERT_NOT_NULL(test, spi);
	if (!pm || !pm->suspend)
		kunit_skip(test, "no system sleep");
	n = spi_sends(f);
	KUNIT_ASSERT_EQ(test, pm->suspend(dev), 0);
	KUNIT_EXPECT_EQ(test, spi_one(spi), -ESHUTDOWN);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n);
	KUNIT_ASSERT_EQ(test, pm->resume(dev), 0);
	KUNIT_EXPECT_EQ(test, spi_one(spi), 0);
	KUNIT_EXPECT_EQ(test, spi_sends(f), n + 1);
}

#if IS_ENABLED(CONFIG_SPI_OBMF_SPIDEV)
static const struct software_node sw_spidev = { .name = "obmf-test-spidev" };

static const struct software_node sw_spidev_spi = {
	.name = "spi@1",
	.parent = &sw_spidev,
	.properties = sw_reg1,
};

static const struct software_node sw_spidev_flash = {
	.name = "flash@0",
	.parent = &sw_spidev_spi,
};

static const struct software_node *const sw_spidev_nodes[] = {
	&sw_spidev,
	&sw_spidev_spi,
	&sw_spidev_flash,
	NULL,
};

static int spi_count_spidev(struct device *dev, void *data)
{
	struct spi_device *spi = to_spi_device(dev);

	if (!strcmp(spi->modalias, "spidev") && dev->driver &&
	    !strcmp(dev->driver->name, "spidev"))
		(*(int *)data)++;
	return 0;
}

static int spi_spidevs(struct spi_controller *ctlr)
{
	int n = 0;

	device_for_each_child(&ctlr->dev, &n, spi_count_spidev);
	return n;
}

/*
 * Whether the kernel command line sets spi_obmf.auto_bind_spidev, the
 * only way to set it for the built-in driver these cases run against
 */
static bool spi_auto_bind_set(struct kunit *test)
{
	char *args, *param, *val;
	bool on = false, b;

	args = kunit_kstrdup(test, saved_command_line, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, args);
	while (*args) {
		args = next_arg(args, &param, &val);
		if (!parameq(param, "spi_obmf.auto_bind_spidev"))
			continue;
		on = !val || (!kstrtobool(val, &b) && b);
	}
	return on;
}

/*
 * With auto_bind_spidev set: spidev bound on each chip select of a
 * channel the firmware describes no client for, gone at unbind; none on
 * a channel whose firmware node has a client. Skipped only when spidev
 * is not registered or the parameter is off: with both, a binding that
 * adds no spidev fails.
 */
static void obmf_test_spi_spidev(struct kunit *test)
{
	struct obmf_fake *f;
	struct spi_controller *ctlr;
	int ret;

	if (!driver_find("spidev", &spi_bus_type) || !spi_auto_bind_set(test))
		kunit_skip(test,
			   "needs spidev and spi_obmf.auto_bind_spidev=1");
	f = spi_fake(test, 516, 0, 3, 1000000, 1000000);
	ctlr = spi_ctlr(f);
	KUNIT_ASSERT_NOT_NULL(test, ctlr);
	KUNIT_EXPECT_EQ(test, spi_spidevs(ctlr), 3);
	device_release_driver(&chan(f, SPI_CH)->fdev->adev.dev);
	KUNIT_EXPECT_EQ(test, spi_spidevs(ctlr), 0);

	ret = kunit_software_node_register_node_group(test, sw_spidev_nodes);
	KUNIT_ASSERT_EQ(test, ret, 0);
	f = spi_fake_lim(test, &sw_spidev, 516, 516, 0, 2, 1000000, 1000000);
	ctlr = spi_ctlr(f);
	KUNIT_ASSERT_NOT_NULL(test, ctlr);
	KUNIT_EXPECT_PTR_EQ(test, dev_fwnode(&ctlr->dev),
			    software_node_fwnode(&sw_spidev_spi));
	KUNIT_EXPECT_EQ(test, spi_spidevs(ctlr), 0);
}
#endif

static struct kunit_case obmf_spi_cases[] = {
	KUNIT_CASE(obmf_test_spi_probe),
	KUNIT_CASE(obmf_test_spi_setup),
	KUNIT_CASE(obmf_test_spi_xfer),
	KUNIT_CASE(obmf_test_spi_refused),
	KUNIT_CASE(obmf_test_spi_limits),
	KUNIT_CASE(obmf_test_spi_errors),
	KUNIT_CASE_SLOW(obmf_test_spi_timeout),
	KUNIT_CASE(obmf_test_spi_unbind),
	KUNIT_CASE_SLOW(obmf_test_spi_unbind_silent),
	KUNIT_CASE_SLOW(obmf_test_spi_unbind_busy),
	KUNIT_CASE_SLOW(obmf_test_spi_unbind_long_send),
	KUNIT_CASE_SLOW(obmf_test_spi_unbind_slow),
	KUNIT_CASE_SLOW(obmf_test_spi_unbind_slow_long),
	KUNIT_CASE_SLOW(obmf_test_spi_iface_unbind_slow),
	KUNIT_CASE_SLOW(obmf_test_spi_client_removed),
	KUNIT_CASE(obmf_test_spi_unplug),
	KUNIT_CASE(obmf_test_spi_suspend),
#if IS_ENABLED(CONFIG_SPI_OBMF_SPIDEV)
	KUNIT_CASE(obmf_test_spi_spidev),
#endif
	{}
};

/* test->priv: the log of the cases that count lines */
static int obmf_spi_test_init(struct kunit *test)
{
	struct test_log *log = kunit_kzalloc(test, sizeof(*log), GFP_KERNEL);

	if (!log)
		return -ENOMEM;
	test_log_init(test, log);
	return 0;
}

static struct kunit_suite obmf_spi_suite = {
	.name = "obmf-spi",
	.suite_init = obmf_test_fn_suite_init,
	.suite_exit = obmf_test_fn_suite_exit,
	.init = obmf_spi_test_init,
	.exit = test_log_exit,
	.test_cases = obmf_spi_cases,
};

kunit_test_suite(obmf_spi_suite);

#endif /* IS_BUILTIN(CONFIG_SPI_OBMF) */

/* The serial driver on the fake transport; only when built in */
#if IS_BUILTIN(CONFIG_OBMF_TTY)

/* OBMF-ICP 1.0.0, 5.15.2, Table 78 */
#define TTY_CFG_LEN 7
#define TTY_HZ 115200
#define TTY_RTS_CTS 0x20
#define TTY_LINE (CSIZE | CSTOPB | PARENB | PARODD | CMSPAR | CRTSCTS)

/* Channel @nr's line configuration, Table 78 */
static void tty_cfg(struct obmf_fake *f, int nr, u32 hz, u8 data, u8 stop,
		    u8 line)
{
	u8 *c = f->mem + fake_ch_off(f, nr) + FAKE_CH_HDR_LEN;

	put_unaligned_le32(hz, c);
	c[4] = data;
	c[5] = stop;
	c[6] = line;
}

/* Whether a driver is bound to channel @nr's function device */
static bool tty_bound(struct obmf_fake *f, int nr)
{
	struct obmf_fdev *fdev = chan(f, nr)->fdev;

	KUNIT_ASSERT_NOT_NULL(f->test, fdev);
	return fdev->adev.dev.driver;
}

/* A tty opened in the kernel, the way speakup opens one */
struct tty_k {
	struct tty_struct *tty;
	bool open;
};

static void tty_k_close(void *data)
{
	struct tty_k *k = data;

	if (!k->tty)
		return;
	tty_lock(k->tty);
	if (k->open)
		k->tty->ops->close(k->tty, NULL);
	tty_unlock(k->tty);
	tty_kclose(k->tty);
	k->tty = NULL;
}

/* Channel @nr's tty, installed but not opened: the port stays closed */
static struct tty_k *tty_k_get(struct obmf_fake *f, int nr)
{
	struct kunit *test = f->test;
	struct obmf_fdev *fdev = chan(f, nr)->fdev;
	struct device *dev;
	struct tty_k *k;
	dev_t devt;

	KUNIT_ASSERT_NOT_NULL(test, fdev);
	dev = device_find_any_child(&fdev->adev.dev);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	devt = dev->devt;
	put_device(dev);
	k = kunit_kzalloc(test, sizeof(*k), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, k);
	k->tty = tty_kopen_exclusive(devt);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, k->tty);
	tty_unlock(k->tty);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, tty_k_close, k),
			0);
	return k;
}

static struct ktermios tty_k_termios(struct tty_k *k)
{
	struct ktermios t;

	down_read(&k->tty->termios_rwsem);
	t = k->tty->termios;
	up_read(&k->tty->termios_rwsem);
	return t;
}

/*
 * Table 78: a line speed of 0 gets no tty, as the driver divides by it;
 * data bits 9, stop bits 3 or parity 7 give a tty at 8N1, which reports
 * the speed; a configuration shorter than Table 78 gets no tty.
 * Expected: "line speed 0" for channel 1, "unsupported line ..., using
 * 8N1" for channels 3 to 5, "configuration data too short" for 6.
 */
static void obmf_test_tty_line(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "s1", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "s2", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "s3", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "s4", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "s5", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "s6", 516, 0,
		  TTY_CFG_LEN - 1 },
	};
	static const struct {
		u32 hz;
		u8 data, stop, line;
		tcflag_t cflag;
	} cfg[] = {
		{ 0, 8, 1, 0, 0 },
		{ TTY_HZ, 7, 2, 2, CS7 | CSTOPB | PARENB },
		{ TTY_HZ, 9, 1, 0, CS8 },
		{ TTY_HZ, 8, 3, 0, CS8 },
		{ TTY_HZ, 8, 1, 7, CS8 },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct ktermios t;
	int i;

	fake_layout(f, c, ARRAY_SIZE(c));
	for (i = 0; i < ARRAY_SIZE(cfg); i++)
		tty_cfg(f, i + 1, cfg[i].hz, cfg[i].data, cfg[i].stop,
			cfg[i].line);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_FALSE(test, tty_bound(f, 1));
	for (i = 1; i < ARRAY_SIZE(cfg); i++) {
		KUNIT_ASSERT_TRUE_MSG(test, tty_bound(f, i + 1), "channel %d",
				      i + 1);
		t = tty_k_termios(tty_k_get(f, i + 1));
		KUNIT_EXPECT_EQ_MSG(test, t.c_cflag & TTY_LINE, cfg[i].cflag,
				    "channel %d", i + 1);
		KUNIT_EXPECT_EQ_MSG(test, tty_termios_baud_rate(&t), TTY_HZ,
				    "channel %d", i + 1);
	}
	KUNIT_EXPECT_FALSE(test, tty_bound(f, 6));
}

/*
 * The tty reports CRTSCTS for Table 78 flow control 2
 * (RTS/CTS) only, also with the 8N1 fallback (channel 5, parity 7), and
 * leaves the input flags as the tty core set them, so never forces IXON
 * or IXOFF. Expected: "unsupported line ..., using 8N1" for channel 5.
 */
static void obmf_test_tty_flow(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "none", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "xon", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "rts", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "dsr", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "rts-8n1", 516, 0,
		  TTY_CFG_LEN },
	};
	static const struct {
		u8 line;
		tcflag_t cflag;
	} cfg[] = {
		{ 0x00, CS8 },
		{ 0x10, CS8 },
		{ TTY_RTS_CTS, CS8 | CRTSCTS },
		{ 0x30, CS8 },
		{ TTY_RTS_CTS | 7, CS8 | CRTSCTS },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct ktermios t;
	int i;

	fake_layout(f, c, ARRAY_SIZE(c));
	for (i = 0; i < ARRAY_SIZE(cfg); i++)
		tty_cfg(f, i + 1, TTY_HZ, 8, 1, cfg[i].line);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	for (i = 0; i < ARRAY_SIZE(cfg); i++) {
		KUNIT_ASSERT_TRUE_MSG(test, tty_bound(f, i + 1), "channel %d",
				      i + 1);
		t = tty_k_termios(tty_k_get(f, i + 1));
		KUNIT_EXPECT_EQ_MSG(test, t.c_cflag & TTY_LINE, cfg[i].cflag,
				    "channel %d", i + 1);
		KUNIT_EXPECT_EQ_MSG(test, t.c_iflag, tty_std_termios.c_iflag,
				    "channel %d", i + 1);
	}
}

/* OBMF-ICP 1.0.0, 5.5.1, Table 22 */
#define TTY_EV_GENERATE_BREAK BIT(0)
#define TTY_EV_BREAK BIT(1)
#define TTY_EV_OVERRUN BIT(2)
#define TTY_EV_CARRIER_DOWN BIT(3)
/*
 * Characters in one write, so that fake_inject() takes it; not a divisor
 * of a flip buffer's size, so a full buffer has a few bytes left
 */
#define TTY_CHUNK 60
/* MIN_TTYB_SIZE: the least flip buffer memory a port may be limited to */
#define TTY_FLIP_MIN 256

/* Channel 1: serial at 115200 8N1 */
static const struct test_chan tty_chans[] = {
	{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart", 516, 0, TTY_CFG_LEN },
};

static struct obmf_fake *tty_setup(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);

	fake_layout(f, tty_chans, ARRAY_SIZE(tty_chans));
	tty_cfg(f, 1, TTY_HZ, 8, 1, 0);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_ASSERT_TRUE(test, tty_bound(f, 1));
	return f;
}

/*
 * Opens the port, without echo, so the line discipline sends nothing; it
 * is closed after a failed open too, as tty_open() does
 */
static void tty_k_open(struct kunit *test, struct tty_k *k)
{
	struct ktermios t;
	int ret;

	tty_lock(k->tty);
	k->open = true;
	ret = k->tty->ops->open(k->tty, NULL);
	if (!ret) {
		t = tty_k_termios(k);
		t.c_lflag &= ~ECHO;
		tty_set_termios(k->tty, &t);
	}
	tty_unlock(k->tty);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_TRUE(test, tty_port_initialized(k->tty->port));
}

/* The last close of the port, then an open */
static void tty_k_reopen(struct kunit *test, struct tty_k *k)
{
	tty_lock(k->tty);
	k->tty->ops->close(k->tty, NULL);
	k->open = false;
	tty_unlock(k->tty);
	tty_k_open(test, k);
}

static void tty_k_clocal(struct tty_k *k, bool on)
{
	struct ktermios t = tty_k_termios(k);

	if (on)
		t.c_cflag |= CLOCAL;
	else
		t.c_cflag &= ~CLOCAL;
	tty_set_termios(k->tty, &t);
}

static struct serial_icounter_struct tty_k_icount(struct kunit *test,
						  struct tty_k *k)
{
	struct serial_icounter_struct ic = {};

	KUNIT_EXPECT_EQ(test, k->tty->ops->get_icount(k->tty, &ic), 0);
	return ic;
}

/* The transmit counters the serial driver keeps beside the icount */
struct tty_cnt {
	unsigned long dropped;
	unsigned long stalls;
};

static struct tty_cnt tty_k_cnt(struct tty_k *k)
{
	struct tty_cnt c;

	obmf_tty_kunit_counts(k->tty, &c.dropped, &c.stalls);
	return c;
}

/*
 * A device write on channel 1 with events @ev and @n characters; returns
 * the answer, of which there is exactly one
 */
static struct fake_rec *tty_write(struct obmf_fake *f, u8 ev, size_t n)
{
	int answers = fake_count(f, FAKE_SEND, 1, 1);
	u8 w[3 + TTY_CHUNK];
	size_t i;

	w[0] = ev;
	put_unaligned_le16(n, w + 1);
	for (i = 0; i < n; i++)
		w[3 + i] = 'a' + i % 26;
	fake_inject(f, 1, 0, w, 3 + n);
	flush_workqueue(f->core->wq);
	KUNIT_ASSERT_EQ(f->test, fake_count(f, FAKE_SEND, 1, 1), answers + 1);
	return last_answer(f, 1);
}

/* Whether answer @r is Table 23's {@nack, @count} */
static bool tty_answer(struct fake_rec *r, u8 nack, u16 count)
{
	return r->msg[1] == (OBMF_HDR_RQRESP | OBMF_STATUS_SUCCESS) &&
	       r->len == OBMF_HDR_LEN + 3 && r->msg[4] == nack &&
	       get_unaligned_le16(r->msg + 5) == count;
}

/*
 * The tty core has no interface that holds a flip buffer full or shows
 * what it holds, so the helpers below and the cases that use them read
 * its internals: port->buf.head and .tail, and each buffer's next, read,
 * used, size and flags, the tail room as __tty_buffer_request_room()
 * counts it, and port->buf.mem_limit; they wait for port->buf.work and
 * tty->hangup_work with flush_work(). tty_buffer_set_limit() is called
 * after the buffer's first use, which its kernel-doc rules out; it only
 * sets the limit the next allocation checks.
 */
KUNIT_DEFINE_ACTION_WRAPPER(tty_buf_unlock, tty_buffer_unlock_exclusive,
			    struct tty_port *);

/*
 * The bytes the flip buffer holds, its work held off; the first @max go
 * to @c and @fl unless they are NULL
 */
static int tty_held(struct tty_port *port, u8 *c, u8 *fl, int max)
{
	struct tty_buffer *b;
	unsigned int i;
	int n = 0;

	for (b = port->buf.head; b; b = b->next) {
		for (i = b->read; i < b->used; i++, n++) {
			if (!c || n >= max)
				continue;
			c[n] = *char_buf_ptr(b, i);
			fl[n] = b->flags ? *flag_buf_ptr(b, i) : TTY_NORMAL;
		}
	}
	return n;
}

/* The bytes left in the flip buffer's last buffer, as the tty core counts */
static int tty_tail_room(struct tty_port *port)
{
	struct tty_buffer *b = port->buf.tail;

	return (b->flags ? 1 : 2) * b->size - b->used;
}

/*
 * Tables 22 and 23: with the flip buffer full (its work held off and
 * its memory limited), a device write is NACKed with count 0, counted in
 * buf_overrun and not acted on, again when sent again; so is one whose
 * characters fit but not with the flag characters of its events. Carrier
 * down alone needs no room and counts dcd, with no hangup under CLOCAL;
 * a refused write with carrier down hangs nothing up, without CLOCAL
 * either. Nothing refused reaches the flip buffer. Once the buffer
 * drains, the write is taken whole and its events counted once.
 */
static void obmf_test_tty_full(struct kunit *test)
{
	struct obmf_fake *f = tty_setup(test);
	struct tty_k *k = tty_k_get(f, 1);
	struct serial_icounter_struct ic;
	struct tty_port *port;
	struct fake_rec *r;
	int limit, left, ret, i;

	tty_k_open(test, k);
	port = k->tty->port;
	limit = port->buf.mem_limit;
	KUNIT_ASSERT_EQ(test, tty_buffer_set_limit(port, TTY_FLIP_MIN), 0);
	tty_buffer_lock_exclusive(port);
	ret = kunit_add_action_or_reset(test, tty_buf_unlock, port);
	KUNIT_ASSERT_EQ(test, ret, 0);
	for (i = 0; i < 32; i++) {
		r = tty_write(f, 0, TTY_CHUNK);
		if (r->msg[4])
			break;
		KUNIT_EXPECT_TRUE(test, tty_answer(r, 0, TTY_CHUNK));
	}
	KUNIT_ASSERT_LT(test, i, 32);
	KUNIT_EXPECT_GT(test, i, 0);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, FAKE_SERIAL_NACK, 0));
	ic = tty_k_icount(test, k);
	KUNIT_EXPECT_EQ(test, ic.rx, i * TTY_CHUNK);
	KUNIT_EXPECT_EQ(test, ic.buf_overrun, 1);

	left = tty_tail_room(port);
	KUNIT_ASSERT_GE(test, left, 2);
	KUNIT_ASSERT_LT(test, left, TTY_CHUNK);

	r = tty_write(f, TTY_EV_BREAK | TTY_EV_OVERRUN, TTY_CHUNK);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, FAKE_SERIAL_NACK, 0));
	r = tty_write(f, TTY_EV_BREAK | TTY_EV_OVERRUN, left - 1);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, FAKE_SERIAL_NACK, 0));
	r = tty_write(f, TTY_EV_CARRIER_DOWN, 0);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, 0, 0));
	ic = tty_k_icount(test, k);
	KUNIT_EXPECT_EQ(test, ic.rx, i * TTY_CHUNK);
	KUNIT_EXPECT_EQ(test, ic.buf_overrun, 3);
	KUNIT_EXPECT_EQ(test, ic.brk, 0);
	KUNIT_EXPECT_EQ(test, ic.overrun, 0);
	KUNIT_EXPECT_EQ(test, ic.dcd, 1);
	KUNIT_EXPECT_TRUE(test, tty_port_initialized(port));

	tty_k_clocal(k, false);
	r = tty_write(f, TTY_EV_CARRIER_DOWN, TTY_CHUNK);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, FAKE_SERIAL_NACK, 0));
	flush_work(&k->tty->hangup_work);
	KUNIT_EXPECT_TRUE(test, tty_port_initialized(port));
	tty_k_clocal(k, true);
	ic = tty_k_icount(test, k);
	KUNIT_EXPECT_EQ(test, ic.buf_overrun, 4);
	KUNIT_EXPECT_EQ(test, ic.dcd, 1);
	KUNIT_EXPECT_EQ(test, tty_held(port, NULL, NULL, 0), i * TTY_CHUNK);

	kunit_release_action(test, tty_buf_unlock, port);
	flush_work(&port->buf.work);
	KUNIT_EXPECT_EQ(test, tty_buffer_set_limit(port, limit), 0);
	r = tty_write(f, TTY_EV_BREAK | TTY_EV_OVERRUN, TTY_CHUNK);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, 0, TTY_CHUNK));
	ic = tty_k_icount(test, k);
	KUNIT_EXPECT_EQ(test, ic.rx, (i + 1) * TTY_CHUNK);
	KUNIT_EXPECT_EQ(test, ic.buf_overrun, 4);
	KUNIT_EXPECT_EQ(test, ic.brk, 1);
	KUNIT_EXPECT_EQ(test, ic.overrun, 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_DEFAULT_ANSWERS), 0);
}

/*
 * With the port open and CLOCAL clear, carrier down is
 * acknowledged, counts dcd and hangs the tty up, which closes the port
 */
static void obmf_test_tty_hangup(struct kunit *test)
{
	struct obmf_fake *f = tty_setup(test);
	struct tty_k *k = tty_k_get(f, 1);
	struct serial_icounter_struct ic;
	struct fake_rec *r;

	tty_k_open(test, k);
	tty_k_clocal(k, false);
	r = tty_write(f, TTY_EV_CARRIER_DOWN, 0);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, 0, 0));
	flush_work(&k->tty->hangup_work);
	KUNIT_EXPECT_TRUE(test, test_bit(TTY_HUPPED, &k->tty->flags));
	KUNIT_EXPECT_FALSE(test, tty_port_initialized(k->tty->port));
	ic = tty_k_icount(test, k);
	KUNIT_EXPECT_EQ(test, ic.dcd, 1);
	KUNIT_EXPECT_EQ(test, ic.rx, 0);
	/* As for a hung-up file, the close does nothing more */
	if (test_bit(TTY_HUPPED, &k->tty->flags))
		k->open = false;
}

/*
 * A transport error that stops the device hangs the open tty
 * up, which closes the port, and the port refuses the next open with
 * -EIO
 */
static void obmf_test_tty_failed(struct kunit *test)
{
	struct obmf_fake *f = tty_setup(test);
	struct tty_k *k = tty_k_get(f, 1);
	int ret;

	tty_k_open(test, k);
	obmf_core_fatal(f->core, -EPROTO);
	flush_work(&f->core->fail_work);
	flush_work(&k->tty->hangup_work);
	KUNIT_EXPECT_TRUE(test, test_bit(TTY_HUPPED, &k->tty->flags));
	KUNIT_EXPECT_FALSE(test, tty_port_initialized(k->tty->port));
	/* As for a hung-up file, the close does nothing more */
	if (test_bit(TTY_HUPPED, &k->tty->flags))
		k->open = false;

	/* The tty core closes a port whose open failed */
	tty_lock(k->tty);
	k->open = true;
	ret = k->tty->ops->open(k->tty, NULL);
	tty_unlock(k->tty);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_FALSE(test, tty_port_initialized(k->tty->port));
}

/*
 * Device writes sent while the first waits for the driver take
 * the reserve and the queue; the next is refused with the default answer,
 * a NACK of count 0, counted in rx_dropped and reported, and every write
 * is answered once. Expected: "channel 1: device request queue full,
 * request refused".
 */
static void obmf_test_tty_queue_full(struct kunit *test)
{
	static const u8 w[] = { 0, 1, 0, 'a' };
	int n = OBMF_DREQ_RESERVE + OBMF_DREQ_QUEUE_MAX + 1, i, nacks = 0;
	struct obmf_fake *f = tty_setup(test);
	struct obmf_channel *ch = chan(f, 1);
	struct test_log *log = test_log_start(test, f->core->dev);
	struct tty_k *k = tty_k_get(f, 1);

	tty_k_open(test, k);

	/* Expectations only while the lock is held */
	mutex_lock(&ch->dispatch_lock);
	for (i = 0; i < n; i++)
		fake_inject(f, 1, 0, w, sizeof(w));
	KUNIT_EXPECT_EQ(test, dreq_out(ch), n - 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 1);
	mutex_unlock(&ch->dispatch_lock);
	flush_workqueue(f->core->wq);

	KUNIT_EXPECT_EQ(test, fake_count(f, FAKE_SEND, 1, 1), n);
	i = -1;
	while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 1)) >= 0)
		nacks += tty_answer(&f->log[i], FAKE_SERIAL_NACK, 0);
	KUNIT_EXPECT_EQ(test, nacks, 1);
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).rx, n - 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_RX_DROPPED), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_QUEUE_FULL), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DREQ_MAX_DEPTH), n - 1);
	KUNIT_EXPECT_EQ(test, dreq_out(ch), 0);
	n = test_log_lines(test, log, "queue full, request refused");
	KUNIT_EXPECT_EQ(test, n, 1);
}

/*
 * Table 23: while the port is closed, a device write with
 * characters is NACKed with count 0 and not acted on, and not counted in
 * buf_overrun; one with events only is acknowledged and its events
 * counted, with no flag characters. Once the port is open, the write sent
 * again is taken, and its characters and flag characters reach the flip
 * buffer once. The buffer is held while the port is closed and found
 * empty, as the line discipline opened with the tty would take anything
 * pushed then; it is let go across the open, as the tty core takes the
 * tty lock before the buffer's.
 */
static void obmf_test_tty_closed(struct kunit *test)
{
	static const u8 ev = TTY_EV_BREAK | TTY_EV_OVERRUN |
			     TTY_EV_CARRIER_DOWN;
	static const u8 c_want[] = { 0, 0, 'a', 'b', 'c' };
	static const u8 fl_want[] = { TTY_BREAK, TTY_OVERRUN, TTY_NORMAL,
				      TTY_NORMAL, TTY_NORMAL };
	struct obmf_fake *f = tty_setup(test);
	struct tty_k *k = tty_k_get(f, 1);
	struct serial_icounter_struct ic;
	u8 c[8], fl[8];
	struct fake_rec *r;
	int ret, n;

	KUNIT_ASSERT_FALSE(test, tty_port_initialized(k->tty->port));
	tty_buffer_lock_exclusive(k->tty->port);
	ret = kunit_add_action_or_reset(test, tty_buf_unlock, k->tty->port);
	KUNIT_ASSERT_EQ(test, ret, 0);
	r = tty_write(f, ev, 3);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, FAKE_SERIAL_NACK, 0));
	ic = tty_k_icount(test, k);
	KUNIT_EXPECT_EQ(test, ic.rx, 0);
	KUNIT_EXPECT_EQ(test, ic.brk, 0);
	KUNIT_EXPECT_EQ(test, ic.overrun, 0);
	KUNIT_EXPECT_EQ(test, ic.dcd, 0);
	KUNIT_EXPECT_EQ(test, ic.buf_overrun, 0);
	r = tty_write(f, ev, 0);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, 0, 0));
	ic = tty_k_icount(test, k);
	KUNIT_EXPECT_EQ(test, ic.brk, 1);
	KUNIT_EXPECT_EQ(test, ic.overrun, 1);
	KUNIT_EXPECT_EQ(test, ic.dcd, 1);
	KUNIT_EXPECT_EQ(test, ic.buf_overrun, 0);
	KUNIT_EXPECT_EQ(test, tty_held(k->tty->port, NULL, NULL, 0), 0);
	kunit_release_action(test, tty_buf_unlock, k->tty->port);

	tty_k_open(test, k);
	tty_buffer_lock_exclusive(k->tty->port);
	ret = kunit_add_action_or_reset(test, tty_buf_unlock, k->tty->port);
	KUNIT_ASSERT_EQ(test, ret, 0);
	r = tty_write(f, ev, 3);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, 0, 3));
	ic = tty_k_icount(test, k);
	KUNIT_EXPECT_EQ(test, ic.rx, 3);
	KUNIT_EXPECT_EQ(test, ic.brk, 2);
	KUNIT_EXPECT_EQ(test, ic.overrun, 2);
	KUNIT_EXPECT_EQ(test, ic.dcd, 2);
	KUNIT_EXPECT_EQ(test, ic.buf_overrun, 0);
	KUNIT_EXPECT_TRUE(test, tty_port_initialized(k->tty->port));
	n = tty_held(k->tty->port, c, fl, sizeof(c));
	KUNIT_ASSERT_EQ(test, n, sizeof(c_want));
	KUNIT_EXPECT_MEMEQ(test, c, c_want, n);
	KUNIT_EXPECT_MEMEQ(test, fl, fl_want, n);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_DEFAULT_ANSWERS), 0);
	kunit_release_action(test, tty_buf_unlock, k->tty->port);
}

/* The serial driver's tries of a NACKed Generate Break and their spacing */
#define TTY_BREAK_TRIES 20
#define TTY_BREAK_DELAY_MS 10

/*
 * Tables 22 and 23: a Generate Break the device NACKs is sent again; after two
 * NACKs it is acknowledged and the break succeeds, each of the three
 * requests a Generate Break with count 0, and one NACKed every
 * time fails with -EIO after the last try. Ending the break sends
 * nothing: the device sets its length.
 */
static void obmf_test_tty_break(struct kunit *test)
{
	struct obmf_fake *f = tty_setup(test);
	struct tty_k *k = tty_k_get(f, 1);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.plen = 3,
		.payload = { FAKE_SERIAL_NACK },
	};
	struct fake_rec *r;
	unsigned int sends;
	int i, n = 0;
	const u8 *p;
	ktime_t t;

	tty_k_open(test, k);
	fake_push(f, &s);
	fake_push(f, &s);
	s.payload[0] = 0;
	fake_push(f, &s);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	t = ktime_get();
	KUNIT_EXPECT_EQ(test, k->tty->ops->break_ctl(k->tty, -1), 0);
	KUNIT_EXPECT_GE(test, ktime_ms_delta(ktime_get(), t),
			2 * TTY_BREAK_DELAY_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 3);
	while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 0)) >= 0) {
		r = &f->log[i];
		p = r->msg + OBMF_HDR_LEN;
		KUNIT_EXPECT_EQ_MSG(test, r->len, (size_t)OBMF_HDR_LEN + 3,
				    "request %d", n);
		KUNIT_EXPECT_EQ_MSG(test, p[0], TTY_EV_GENERATE_BREAK,
				    "request %d", n);
		KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le16(p + 1), 0,
				    "request %d", n);
		n++;
	}
	KUNIT_EXPECT_EQ(test, n, 3);
	KUNIT_EXPECT_EQ(test, k->tty->ops->break_ctl(k->tty, 0), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 3);

	s.payload[0] = FAKE_SERIAL_NACK;
	scoped_guard(spinlock_irqsave, &f->lock)
		f->dflt = s;
	sends = READ_ONCE(f->sends);
	KUNIT_EXPECT_EQ(test, k->tty->ops->break_ctl(k->tty, -1), -EIO);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + TTY_BREAK_TRIES);
}

/*
 * A break the device answers with a timeout status, Table 4 0x02 or the
 * line timeout 0x40, fails -ETIMEDOUT, as one the transport times out
 * does, and is not sent again. TCSBRK returns what break_ctl() returns
 * (send_break() in drivers/tty/tty_io.c).
 */
static void obmf_test_tty_break_timeout(struct kunit *test)
{
	static const u8 status[] = { OBMF_STATUS_TIMEOUT,
				     OBMF_STATUS_SERIAL_LINE_TIMEOUT };
	struct obmf_fake *f = tty_setup(test);
	struct tty_k *k = tty_k_get(f, 1);
	struct fake_step s = { .how = FAKE_INLINE, .channel = -1 };
	unsigned int sends;
	int i;

	tty_k_open(test, k);
	for (i = 0; i < ARRAY_SIZE(status); i++) {
		s.status = status[i];
		fake_push(f, &s);
		sends = READ_ONCE(f->sends);
		KUNIT_EXPECT_EQ_MSG(test, k->tty->ops->break_ctl(k->tty, -1),
				    -ETIMEDOUT, "status %#x", status[i]);
		KUNIT_EXPECT_EQ_MSG(test, READ_ONCE(f->sends), sends + 1,
				    "status %#x", status[i]);
	}
	fake_push(f, &(struct fake_step){ .send_ret = -ETIMEDOUT });
	KUNIT_EXPECT_EQ(test, k->tty->ops->break_ctl(k->tty, -1), -ETIMEDOUT);
}

#define TTY_SLOW_HZ 300
/*
 * Characters at TTY_SLOW_HZ whose time on the wire, twice over, fits the
 * core's longest wait less the default timeout, ten bits a character
 */
#define TTY_SLOW_CHARS                                                   \
	(TTY_SLOW_HZ * (OBMF_MAX_TIMEOUT_MS - OBMF_DEFAULT_TIMEOUT_MS) / \
	 (2 * 10 * MSEC_PER_SEC))
#define TTY_SLOW_LEN 200

/*
 * On a slow line, a request carries no more characters than
 * its wait for the answer has time for within the core's limit, fewer
 * than the channel's request size allows; the rest follows in the next
 */
static void obmf_test_tty_slow(struct kunit *test)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.plen = 3,
	};
	u8 buf[TTY_SLOW_LEN];
	struct fake_rec *r;
	int i, n = 0, t, total = 0;
	struct tty_k *k;
	ssize_t ret;
	u16 c;

	fake_layout(f, tty_chans, ARRAY_SIZE(tty_chans));
	tty_cfg(f, 1, TTY_SLOW_HZ, 8, 1, 0);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_ASSERT_TRUE(test, tty_bound(f, 1));
	k = tty_k_get(f, 1);
	tty_k_open(test, k);
	scoped_guard(spinlock_irqsave, &f->lock)
		f->dflt = s;
	i = fake_nlog(f) - 1;
	memset(buf, 'x', sizeof(buf));
	ret = k->tty->ops->write(k->tty, buf, sizeof(buf));
	KUNIT_ASSERT_EQ(test, ret, TTY_SLOW_LEN);
	for (t = 0; t < 50 && tty_k_icount(test, k).tx < TTY_SLOW_LEN; t++)
		msleep(20);
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, TTY_SLOW_LEN);
	while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 0)) >= 0) {
		r = &f->log[i];
		c = get_unaligned_le16(r->msg + OBMF_HDR_LEN + 1);
		KUNIT_EXPECT_LE_MSG(test, c, TTY_SLOW_CHARS, "request %d", n);
		total += c;
		n++;
	}
	KUNIT_EXPECT_EQ(test, total, TTY_SLOW_LEN);
	KUNIT_EXPECT_EQ(test, n, DIV_ROUND_UP(TTY_SLOW_LEN, TTY_SLOW_CHARS));
}

/*
 * The nodes each function device below @parent had when it was added;
 * the uevent's OF_* values come from @of_node
 */
static struct {
	struct device *parent;
	struct fwnode_handle *node[3];
	struct device_node *of_node[3];
} tty_node_added;

static int tty_node_notify(struct notifier_block *nb, unsigned long action,
			   void *data)
{
	struct device *dev = data;
	struct obmf_fdev *fdev;

	if (action != BUS_NOTIFY_ADD_DEVICE ||
	    dev->parent != tty_node_added.parent)
		return NOTIFY_DONE;
	fdev = to_obmf_fdev(to_auxiliary_dev(dev));
	if (fdev->info->number < ARRAY_SIZE(tty_node_added.node)) {
		tty_node_added.node[fdev->info->number] = dev_fwnode(dev);
		tty_node_added.of_node[fdev->info->number] = dev_of_node(dev);
	}
	return NOTIFY_DONE;
}

static struct notifier_block tty_node_nb = {
	.notifier_call = tty_node_notify,
};

static void tty_node_unwatch(void *bus)
{
	bus_unregister_notifier(bus, &tty_node_nb);
}

/*
 * A serial channel of either role takes its node as an I2C channel does.
 * The node is its function device's when the bus adds that device, the
 * step whose uevent udev sees. The tty below it has no node, and its
 * parent is the function device, which udev rules import the node from.
 */
static void obmf_test_tty_node(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "p", 516, 0,
		  TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_SYMMETRIC, "s", 516, 0,
		  TTY_CFG_LEN },
	};
	static const char *const paths[] = {
		"/obmf-test-tty/serial@1",
		"/obmf-test-tty/serial@2",
	};
	struct obmf_fake *any = tty_setup(test);
	const struct bus_type *bus;
	struct fwnode_handle *node;
	struct device_node *np;
	struct obmf_fdev *fdev;
	struct obmf_fake *f;
	struct device *tty;
	int i, nr;

	/* auxiliary_bus_type is static: take it from a bound function device */
	bus = chan(any, 1)->fdev->adev.dev.bus;
	KUNIT_ASSERT_EQ(test, of_overlay_apply_kunit(test, obmf_test_nodes), 0);
	f = fake_create(test, FAKE_MAX_MSG, 0);
	device_set_node(f->tp.dev,
			of_fwnode_handle(of_test_node(test, "/obmf-test-tty")));
	memset(&tty_node_added, 0, sizeof(tty_node_added));
	tty_node_added.parent = f->tp.dev;
	KUNIT_ASSERT_EQ(test, bus_register_notifier(bus, &tty_node_nb), 0);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, tty_node_unwatch,
						  (void *)bus),
			0);
	fake_layout(f, c, ARRAY_SIZE(c));
	for (i = 0; i < ARRAY_SIZE(c); i++)
		tty_cfg(f, i + 1, TTY_HZ, 8, 1, 0);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);

	for (i = 0; i < ARRAY_SIZE(c); i++) {
		nr = i + 1;
		np = of_test_node(test, paths[i]);
		node = of_fwnode_handle(np);
		KUNIT_EXPECT_PTR_EQ_MSG(test, tty_node_added.node[nr], node,
					"channel %d", nr);
		KUNIT_EXPECT_PTR_EQ_MSG(test, tty_node_added.of_node[nr], np,
					"channel %d", nr);
		KUNIT_ASSERT_TRUE_MSG(test, tty_bound(f, nr), "channel %d", nr);
		fdev = chan(f, nr)->fdev;
		KUNIT_EXPECT_PTR_EQ_MSG(test, dev_fwnode(&fdev->adev.dev), node,
					"channel %d", nr);
		tty = device_find_any_child(&fdev->adev.dev);
		KUNIT_ASSERT_NOT_NULL_MSG(test, tty, "channel %d", nr);
		KUNIT_EXPECT_NE_MSG(test, MAJOR(tty->devt), 0, "channel %d",
				    nr);
		KUNIT_EXPECT_NULL_MSG(test, dev_fwnode(tty), "channel %d", nr);
		KUNIT_EXPECT_NULL_MSG(test, dev_of_node(tty), "channel %d", nr);
		KUNIT_EXPECT_PTR_EQ_MSG(test, tty->parent, &fdev->adev.dev,
					"channel %d", nr);
		put_device(tty);
	}
}

/* Characters in one request on tty_tx_chans' channel */
#define TTY_TX_MAX 16
#define TTY_TX_LEN 40
/* The serial driver's transmit FIFO */
#define TTY_FIFO 4096

/* Channel 1: serial at 115200 8N1, TTY_TX_MAX characters a request */
static const struct test_chan tty_tx_chans[] = {
	{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart",
	  OBMF_HDR_LEN + 3 + TTY_TX_MAX, 0, TTY_CFG_LEN },
};

/* The channels at most after the serial one, of a type no driver takes */
#define TTY_EXTRA 5

/* Channel 1 of tty_tx_chans, its tty open, then @extra IPMI channels */
static struct tty_k *tty_tx_setup_n(struct kunit *test, struct obmf_fake **fp,
				    int extra)
{
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct test_chan c[1 + TTY_EXTRA] = { tty_tx_chans[0] };
	struct tty_k *k;
	int i;

	KUNIT_ASSERT_LE(test, extra, TTY_EXTRA);
	for (i = 1; i <= extra; i++)
		c[i] = (struct test_chan){ OBMF_TYPE_IPMI, OBMF_ROLE_PRODUCER,
					   "ipmi", 516 };
	fake_layout(f, c, 1 + extra);
	tty_cfg(f, 1, TTY_HZ, 8, 1, 0);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_ASSERT_TRUE(test, tty_bound(f, 1));
	k = tty_k_get(f, 1);
	tty_k_open(test, k);
	*fp = f;
	return k;
}

static struct tty_k *tty_tx_setup(struct kunit *test, struct obmf_fake **fp)
{
	return tty_tx_setup_n(test, fp, 0);
}

/* Waits up to @ms for @tx characters sent in all and none left */
static void tty_tx_wait_ms(struct kunit *test, struct tty_k *k, u32 tx,
			   unsigned int ms)
{
	unsigned int t;

	for (t = 0; t < ms / 20; t++) {
		if (tty_k_icount(test, k).tx == tx &&
		    !k->tty->ops->chars_in_buffer(k->tty))
			break;
		msleep(20);
	}
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, tx);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 0);
}

static void tty_tx_wait(struct kunit *test, struct tty_k *k, u32 tx)
{
	tty_tx_wait_ms(test, k, tx, 1000);
}

/* Request @r carries Operation/Event 0 and the @n characters at @src */
static void tty_tx_check(struct kunit *test, struct fake_rec *r, const u8 *src,
			 u16 n, int nr)
{
	const u8 *p = r->msg + OBMF_HDR_LEN;

	KUNIT_ASSERT_EQ_MSG(test, r->len, (size_t)OBMF_HDR_LEN + 3 + n,
			    "request %d", nr);
	KUNIT_EXPECT_EQ_MSG(test, p[0], 0, "request %d", nr);
	KUNIT_EXPECT_EQ_MSG(test, get_unaligned_le16(p + 1), n, "request %d",
			    nr);
	KUNIT_EXPECT_MEMEQ_MSG(test, p + 3, src, n, "request %d", nr);
}

/*
 * Written characters go out in order, TTY_TX_MAX a request,
 * each request with Operation/Event 0 and its Character Count. One NACKed
 * with count 0 is sent again, and one partly accepted again from the
 * first character not accepted. Characters answered busy or with the
 * request timeout status are sent again. Those whose request fails with
 * another error status are sent once and dropped, counted as dropped and
 * not in tx, and the next write goes out; so are those whose answer
 * accepts more than was sent, a protocol error. Expected: "dropped 4
 * bytes: -EIO" and "dropped 4 bytes: -EPROTO".
 */
static void obmf_test_tty_tx(struct kunit *test)
{
	static const struct {
		u16 off, n;
	} want[] = {
		{ 0, TTY_TX_MAX },  { 0, TTY_TX_MAX },	{ 0, TTY_TX_MAX },
		{ 16, TTY_TX_MAX }, { 21, TTY_TX_MAX }, { 37, 3 },
	};
	struct fake_step ack = {
		.how = FAKE_INLINE,
		.channel = -1,
		.plen = 3,
	};
	struct fake_step s = ack;
	u8 buf[TTY_TX_LEN];
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	int i, n = 0;
	long errs;

	k = tty_tx_setup(test, &f);
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = 'a' + i % 26;
	scoped_guard(spinlock_irqsave, &f->lock)
		f->dflt = ack;
	s.payload[0] = FAKE_SERIAL_NACK;
	fake_push(f, &s);
	fake_push(f, &s);
	fake_push(f, &ack);
	s.payload[1] = 5;
	fake_push(f, &s);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, TTY_TX_LEN),
			TTY_TX_LEN);
	tty_tx_wait(test, k, TTY_TX_LEN);
	while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 0)) >= 0 &&
	       n < ARRAY_SIZE(want)) {
		tty_tx_check(test, &f->log[i], buf + want[n].off, want[n].n, n);
		n++;
	}
	KUNIT_EXPECT_EQ(test, n, ARRAY_SIZE(want));
	KUNIT_EXPECT_LT(test, i, 0);

	s = ack;
	s.plen = 0;
	s.status = OBMF_STATUS_BUSY;
	fake_push(f, &s);
	s.status = OBMF_STATUS_TIMEOUT;
	fake_push(f, &s);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait(test, k, TTY_TX_LEN + 4);
	msleep(50);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 3);
	n = 0;
	while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 0)) >= 0)
		tty_tx_check(test, &f->log[i], buf, 4, n++);
	KUNIT_EXPECT_EQ(test, n, 3);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);

	s.status = OBMF_STATUS_INVALID;
	fake_push(f, &s);
	sends = READ_ONCE(f->sends);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait(test, k, TTY_TX_LEN + 4);
	msleep(50);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 4);

	s = ack;
	s.payload[1] = 5;
	fake_push(f, &s);
	errs = ch_cnt(chan(f, 1), OBMF_CH_PROTOCOL_ERRORS);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait(test, k, TTY_TX_LEN + 4);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_PROTOCOL_ERRORS),
			errs + 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 8);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait(test, k, TTY_TX_LEN + 8);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 8);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 0);
}

/*
 * While a request is in flight its characters still count in
 * chars_in_buffer, and write_room is the rest of the FIFO. flush_buffer
 * then drops the characters not yet sent; those in flight count in tx
 * once the device accepts them. Characters written after the flush and
 * before that answer go out after it; nothing else is sent. A flush
 * counts nothing as dropped.
 */
static void obmf_test_tty_tx_flush(struct kunit *test)
{
	struct fake_step s = {
		.how = FAKE_WORK,
		.delay_ms = 200,
		.channel = -1,
		.plen = 3,
	};
	static const u8 late[] = "late";
	u8 buf[TTY_TX_LEN];
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	int i;

	k = tty_tx_setup(test, &f);
	memset(buf, 'x', sizeof(buf));
	fake_push(f, &s);
	s.how = FAKE_INLINE;
	scoped_guard(spinlock_irqsave, &f->lock)
		f->dflt = s;
	sends = READ_ONCE(f->sends);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, TTY_TX_LEN),
			TTY_TX_LEN);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), TTY_TX_LEN);
	KUNIT_EXPECT_EQ(test, k->tty->ops->write_room(k->tty),
			TTY_FIFO - TTY_TX_LEN);
	k->tty->ops->flush_buffer(k->tty);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, late, 4), 4);
	tty_tx_wait(test, k, TTY_TX_MAX + 4);
	msleep(50);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 2);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	tty_tx_check(test, &f->log[i], late, 4, 1);
	KUNIT_EXPECT_EQ(test, k->tty->ops->write_room(k->tty), TTY_FIFO);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/*
 * Two flushes while one request is in flight, each followed
 * by a write: after the answer only the write after the second flush
 * goes out, and nothing counts as dropped.
 */
static void obmf_test_tty_tx_flush_twice(struct kunit *test)
{
	static const u8 a[] = "abcd", b[] = "ef", c[] = "gh";
	struct fake_step s = {
		.how = FAKE_WORK,
		.delay_ms = 200,
		.channel = -1,
		.plen = 3,
	};
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	int i;

	k = tty_tx_setup(test, &f);
	fake_push(f, &s);
	s.how = FAKE_INLINE;
	scoped_guard(spinlock_irqsave, &f->lock)
		f->dflt = s;
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, a, 4), 4);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	k->tty->ops->flush_buffer(k->tty);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, b, 2), 2);
	k->tty->ops->flush_buffer(k->tty);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, c, 2), 2);
	tty_tx_wait(test, k, 6);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	tty_tx_check(test, &f->log[i], a, 4, 0);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	tty_tx_check(test, &f->log[i], c, 2, 1);
	KUNIT_EXPECT_LT(test, fake_find(f, i + 1, FAKE_SEND, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/*
 * A flush while a request is in flight that the device then
 * accepts in part, 2 of 4, and a write after the flush: the 2 count in
 * tx, the other 2 go, not counted as dropped, and the write goes out
 * alone.
 */
static void obmf_test_tty_tx_flush_partial(struct kunit *test)
{
	static const u8 a[] = "abcd", c[] = "xy";
	struct fake_step s = {
		.how = FAKE_WORK,
		.delay_ms = 200,
		.channel = -1,
		.plen = 3,
		.payload = { FAKE_SERIAL_NACK, 2 },
	};
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	int i;

	k = tty_tx_setup(test, &f);
	fake_push(f, &s);
	s = (struct fake_step){ .how = FAKE_INLINE, .channel = -1, .plen = 3 };
	scoped_guard(spinlock_irqsave, &f->lock)
		f->dflt = s;
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, a, 4), 4);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	k->tty->ops->flush_buffer(k->tty);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, c, 2), 2);
	tty_tx_wait(test, k, 4);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	tty_tx_check(test, &f->log[i], a, 4, 0);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	tty_tx_check(test, &f->log[i], c, 2, 1);
	KUNIT_EXPECT_LT(test, fake_find(f, i + 1, FAKE_SEND, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/* The serial driver's delay between tries of a refused request */
#define TTY_RETRY_MS 10
/* NACKs after which the serial driver dropped the characters before */
#define TTY_OLD_TRIES 20
/* How long the refusing devices below refuse */
#define TTY_REFUSE_MS 1000
/* Twice the time on the wire of 4 characters at TTY_HZ */
#define TTY_WIRE4_MS (2 * DIV_ROUND_UP(4 * 10 * MSEC_PER_SEC, TTY_HZ))

static void tty_dflt(struct obmf_fake *f, const struct fake_step *s)
{
	guard(spinlock_irqsave)
		(&f->lock);
	f->dflt = *s;
}

static const struct fake_step tty_ack = {
	.how = FAKE_INLINE,
	.channel = -1,
	.plen = 3,
};

static const struct fake_step tty_nack = {
	.how = FAKE_INLINE,
	.channel = -1,
	.plen = 3,
	.payload = { FAKE_SERIAL_NACK },
};

/* How long the serial driver keeps a refused head before it warns */
#define TTY_STALL_MS 5000

static int obmf_tty_test_init(struct kunit *test)
{
	struct test_log *log;

	log = kunit_kzalloc(test, sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;
	test_log_init(test, log);
	return 0;
}

/* The log lines of channel 1's function device, from now on */
static struct test_log *tty_log_start(struct obmf_fake *f)
{
	return test_log_start(f->test, &chan(f, 1)->fdev->adev.dev);
}

/* Every logged request from @i on carries the @n characters at @src */
static int tty_tx_same(struct kunit *test, struct obmf_fake *f, int i,
		       const u8 *src, u16 n)
{
	int nr = 0;

	while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 0)) >= 0)
		tty_tx_check(test, &f->log[i], src, n, nr++);
	return nr;
}

/*
 * Characters the device NACKs with count 0 stay in the FIFO
 * and are sent again, TTY_RETRY_MS apart, with no limit on the tries.
 * After TTY_OLD_TRIES NACKs, the bound at which they were dropped before,
 * the next try is sent and accepted. Nothing is logged.
 */
static void obmf_test_tty_nack_bound(struct kunit *test)
{
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	u8 buf[4];
	ktime_t t;
	int i, n;

	k = tty_tx_setup(test, &f);
	memcpy(buf, "abcd", sizeof(buf));
	tty_dflt(f, &tty_ack);
	for (i = 0; i < TTY_OLD_TRIES; i++)
		fake_push(f, &tty_nack);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	t = ktime_get();
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, sizeof(buf)),
			sizeof(buf));
	tty_tx_wait_ms(test, k, sizeof(buf), 4 * TTY_REFUSE_MS);
	KUNIT_EXPECT_GE(test, ktime_ms_delta(ktime_get(), t),
			TTY_OLD_TRIES * TTY_RETRY_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + TTY_OLD_TRIES + 1);
	n = tty_tx_same(test, f, i, buf, sizeof(buf));
	KUNIT_EXPECT_EQ(test, n, TTY_OLD_TRIES + 1);
}

/*
 * A device that NACKs every request for TTY_REFUSE_MS, then
 * accepts: while it refuses, the characters stay counted in
 * chars_in_buffer and out of write_room, none counts in tx, and the
 * tries come no faster than TTY_RETRY_MS apart. Then every character
 * is sent once more, in order, and counts in tx once: none is dropped.
 * A refusal shorter than TTY_STALL_MS is neither logged nor counted as a
 * stall.
 */
static void obmf_test_tty_nack_long(struct kunit *test)
{
	unsigned int sends, tries;
	struct fake_rec *rec[8];
	u8 buf[TTY_TX_LEN];
	struct obmf_fake *f;
	struct tty_k *k;
	struct test_log *log;
	int i, n;
	s64 ms;
	ktime_t t;

	k = tty_tx_setup(test, &f);
	log = tty_log_start(f);
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = 'a' + i % 26;
	scoped_guard(spinlock_irqsave, &f->lock) {
		f->dflt = tty_nack;
		f->nolog = true;
	}
	sends = READ_ONCE(f->sends);
	t = ktime_get();
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, TTY_TX_LEN),
			TTY_TX_LEN);
	msleep(TTY_REFUSE_MS);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), TTY_TX_LEN);
	KUNIT_EXPECT_EQ(test, k->tty->ops->write_room(k->tty),
			TTY_FIFO - TTY_TX_LEN);
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, 0);
	scoped_guard(spinlock_irqsave, &f->lock) {
		tries = f->sends - sends;
		ms = ktime_ms_delta(ktime_get(), t);
		f->nolog = false;
		i = f->nlog - 1;
	}
	KUNIT_EXPECT_GT(test, tries, TTY_OLD_TRIES);
	KUNIT_EXPECT_LE(test, tries, ms / TTY_RETRY_MS + 1);
	/* Tries logged before the device accepts carry the first request */
	tty_dflt(f, &tty_ack);
	tty_tx_wait(test, k, TTY_TX_LEN);
	n = 0;
	while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 0)) >= 0 &&
	       n < ARRAY_SIZE(rec))
		rec[n++] = &f->log[i];
	KUNIT_ASSERT_GE(test, n, 3);
	KUNIT_EXPECT_LT(test, i, 0);
	for (i = 0; i < n - 2; i++)
		tty_tx_check(test, rec[i], buf, TTY_TX_MAX, i);
	tty_tx_check(test, rec[n - 2], buf + TTY_TX_MAX, TTY_TX_MAX, n - 2);
	tty_tx_check(test, rec[n - 1], buf + 2 * TTY_TX_MAX,
		     TTY_TX_LEN - 2 * TTY_TX_MAX, n - 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 0);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "transmit stalled"), 0);
}

/*
 * Characters whose request times out stay in the FIFO and are
 * sent again: after two requests the device does not answer, which
 * double the wait twice, and after more than TTY_OLD_TRIES answered with
 * the line timeout status, which keep it. After a reopen, which ends
 * that longer wait, an answer that comes after the timeout is counted
 * late and dropped, and the next try waits for it: the device gets those
 * characters twice, they count in tx once. Nothing is logged or counted
 * as dropped.
 */
static void obmf_test_tty_timeout(struct kunit *test)
{
	struct fake_step s = tty_ack;
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	long to, late;
	u8 buf[12];
	int i, n, last;

	k = tty_tx_setup(test, &f);
	for (i = 0; i < sizeof(buf); i++)
		buf[i] = 'a' + i;
	tty_dflt(f, &tty_ack);
	s.how = FAKE_NEVER;
	fake_push(f, &s);
	fake_push(f, &s);
	to = ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait_ms(test, k, 4, 6 * OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 3);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to + 2);
	n = tty_tx_same(test, f, i, buf, 4);
	KUNIT_EXPECT_EQ(test, n, 3);

	s = tty_ack;
	s.status = OBMF_STATUS_SERIAL_LINE_TIMEOUT;
	s.plen = 0;
	tty_dflt(f, &s);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf + 4, 4), 4);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + TTY_OLD_TRIES + 1));
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, 4);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 4);
	tty_dflt(f, &tty_ack);
	tty_tx_wait(test, k, 8);
	n = tty_tx_same(test, f, i, buf + 4, 4);
	KUNIT_EXPECT_GT(test, n, TTY_OLD_TRIES + 1);
	KUNIT_ASSERT_LT(test, fake_nlog(f), FAKE_LOG_LEN);
	last = -1;
	while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 0)) >= 0)
		last = i;
	KUNIT_ASSERT_GE(test, last, 0);
	KUNIT_EXPECT_EQ(test, f->log[last].timeout_ms,
			4 * OBMF_DEFAULT_TIMEOUT_MS + TTY_WIRE4_MS);

	tty_k_reopen(test, k);
	s = tty_ack;
	s.how = FAKE_WORK;
	s.delay_ms = OBMF_DEFAULT_TIMEOUT_MS + 300;
	fake_push(f, &s);
	to = ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS);
	late = ch_cnt(chan(f, 1), OBMF_CH_RX_LATE);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf + 8, 4), 4);
	tty_tx_wait_ms(test, k, 12, 2 * s.delay_ms);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to + 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_RX_LATE), late + 1);
	n = tty_tx_same(test, f, i, buf + 8, 4);
	KUNIT_EXPECT_EQ(test, n, 2);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/*
 * Characters whose request the device answers with an error
 * that is neither busy nor a timeout are dropped: tx_dropped counts
 * them, and the drop is counted as an event and logged once on the
 * function device; the next write goes out. Expected: "dropped 4 bytes:
 * -EIO".
 */
static void obmf_test_tty_drop_report(struct kunit *test)
{
	static const u8 buf[] = "abcdefgh";
	static const char text[] = "dropped 4 bytes: -EIO";
	struct fake_step s = tty_ack;
	struct test_log *log;
	struct obmf_fake *f;
	struct tty_k *k;
	long ev;

	k = tty_tx_setup(test, &f);
	log = tty_log_start(f);
	tty_dflt(f, &tty_ack);
	s.status = OBMF_STATUS_PERMANENT_ERROR;
	s.plen = 0;
	fake_push(f, &s);
	ev = ev_cnt(f->core, OBMF_EV_TX_DROPPED);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	/* The event is counted before the line is logged */
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, text, 1, 2000), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_TX_DROPPED), ev + 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 4);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 0);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf + 4, 4), 4);
	tty_tx_wait(test, k, 4);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 4);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_TX_DROPPED), ev + 1);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "dropped"), 1);
}

/*
 * The port's transmit counters are in debugfs while it is
 * bound, as obmf_tty/<tty>/tx_dropped and tx_stalls: after a drop,
 * tx_dropped there counts the characters. Expected: "dropped 4 bytes:
 * -EIO".
 */
static void obmf_test_tty_debugfs(struct kunit *test)
{
	static const u8 buf[] = "abcd";
	struct fake_step s = tty_ack;
	struct dentry *root, *dir, *d;
	struct obmf_fake *f;
	struct tty_k *k;
	char name[64];
	int t;

	if (!IS_ENABLED(CONFIG_DEBUG_FS))
		kunit_skip(test, "needs CONFIG_DEBUG_FS");
	k = tty_tx_setup(test, &f);
	tty_dflt(f, &tty_ack);
	s.status = OBMF_STATUS_PERMANENT_ERROR;
	s.plen = 0;
	fake_push(f, &s);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	for (t = 0; t < 100 && tty_k_cnt(k).dropped != 4; t++)
		msleep(20);
	KUNIT_ASSERT_EQ(test, tty_k_cnt(k).dropped, 4);

	root = debugfs_lookup("obmf_tty", NULL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, root);
	strscpy(name, tty_name(k->tty));
	dir = debugfs_lookup(name, root);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dir);
	d = debugfs_lookup("tx_dropped", dir);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, d);
	KUNIT_EXPECT_EQ(test, *(unsigned long *)d_inode(d)->i_private, 4);
	dput(d);
	d = debugfs_lookup("tx_stalls", dir);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, d);
	KUNIT_EXPECT_EQ(test, *(unsigned long *)d_inode(d)->i_private, 0);
	dput(d);
	dput(dir);

	fake_unbind(f);
	k->open = false;
	tty_k_close(k);
	dir = debugfs_lookup(name, root);
	KUNIT_EXPECT_NULL(test, dir);
	dput(dir);
	dput(root);
}

/*
 * An unbind while the port is open and a request waits for an
 * answer that never comes hangs the tty up at once and returns within one
 * request wait
 */
static void obmf_test_tty_unbind_open(struct kunit *test)
{
	static const u8 buf[] = "abcd";
	struct fake_step s = tty_ack;
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	ktime_t t;

	k = tty_tx_setup(test, &f);
	tty_dflt(f, &tty_ack);
	s.how = FAKE_NEVER;
	fake_push(f, &s);
	sends = READ_ONCE(f->sends);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	t = ktime_get();
	fake_unbind(f);
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t),
			OBMF_DEFAULT_TIMEOUT_MS + 500);
	KUNIT_EXPECT_TRUE(test, test_bit(TTY_HUPPED, &k->tty->flags));
	KUNIT_EXPECT_FALSE(test, tty_port_initialized(k->tty->port));
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 1);
	k->open = false;
	tty_k_close(k);
}

/*
 * While the device refuses, a write that fills the FIFO is
 * taken whole, write_room falls to 0 and the next write takes nothing;
 * tcdrain waits, for its timeout. Once the device accepts, the FIFO
 * empties, write_room comes back and tcdrain returns.
 */
static void obmf_test_tty_refused_room(struct kunit *test)
{
	struct obmf_fake *f;
	struct tty_k *k;
	u8 *buf;
	ktime_t t;

	k = tty_tx_setup(test, &f);
	buf = kunit_kzalloc(test, TTY_FIFO, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, buf);
	memset(buf, 'x', TTY_FIFO);
	scoped_guard(spinlock_irqsave, &f->lock) {
		f->dflt = tty_nack;
		f->nolog = true;
	}
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, TTY_FIFO),
			TTY_FIFO);
	KUNIT_EXPECT_EQ(test, k->tty->ops->write_room(k->tty), 0);
	KUNIT_EXPECT_EQ(test, k->tty->ops->write(k->tty, buf, 1), 0);
	t = ktime_get();
	tty_wait_until_sent(k->tty, msecs_to_jiffies(200));
	KUNIT_EXPECT_GE(test, ktime_ms_delta(ktime_get(), t), 150);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), TTY_FIFO);
	KUNIT_EXPECT_EQ(test, k->tty->ops->write_room(k->tty), 0);
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, 0);

	tty_dflt(f, &tty_ack);
	t = ktime_get();
	tty_wait_until_sent(k->tty, msecs_to_jiffies(5 * TTY_REFUSE_MS));
	KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t),
			5 * TTY_REFUSE_MS);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 0);
	KUNIT_EXPECT_EQ(test, k->tty->ops->write_room(k->tty), TTY_FIFO);
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, TTY_FIFO);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

enum tty_stop {
	TTY_STOP_FLUSH,
	TTY_STOP_HANGUP,
	TTY_STOP_CLOSE,
	TTY_STOP_UNBIND
};

/*
 * A device that refuses every request, stopped by @how while
 * it does: the characters not sent go, nothing is sent after, and none
 * counts in tx. The flush comes while a request waits for its NACK, and
 * tcdrain returns once it comes. Only the close logs the characters it
 * discards and counts them as dropped; a hangup has flushed them.
 */
static void tty_refused_stop(struct kunit *test, enum tty_stop how)
{
	static const char closed[] = "characters not sent at close";
	struct fake_step s = tty_nack;
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	u8 buf[TTY_TX_LEN];
	struct test_log *log;
	ktime_t t;
	int i;

	k = tty_tx_setup(test, &f);
	log = tty_log_start(f);
	memset(buf, 'x', sizeof(buf));
	if (how == TTY_STOP_FLUSH) {
		s.how = FAKE_WORK;
		s.delay_ms = 100;
	}
	scoped_guard(spinlock_irqsave, &f->lock) {
		f->dflt = s;
		f->nolog = true;
	}
	sends = READ_ONCE(f->sends);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, TTY_TX_LEN),
			TTY_TX_LEN);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 3));
	switch (how) {
	case TTY_STOP_FLUSH:
		k->tty->ops->flush_buffer(k->tty);
		/* The FIFO is reset when the answer comes */
		KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty),
				TTY_TX_LEN);
		t = ktime_get();
		tty_wait_until_sent(k->tty, msecs_to_jiffies(TTY_REFUSE_MS));
		KUNIT_EXPECT_LT(test, ktime_ms_delta(ktime_get(), t),
				TTY_REFUSE_MS);
		break;
	case TTY_STOP_HANGUP:
		tty_vhangup(k->tty);
		k->open = false;
		break;
	case TTY_STOP_CLOSE:
		k->tty->port->closing_wait = msecs_to_jiffies(200);
		t = ktime_get();
		tty_lock(k->tty);
		k->tty->ops->close(k->tty, NULL);
		k->open = false;
		tty_unlock(k->tty);
		KUNIT_EXPECT_GE(test, ktime_ms_delta(ktime_get(), t), 150);
		KUNIT_EXPECT_FALSE(test, tty_port_initialized(k->tty->port));
		break;
	case TTY_STOP_UNBIND:
		fake_unbind(f);
		k->open = false;
		break;
	}
	/* The request in flight, if any, is answered, then its characters go */
	for (i = 0; i < 50 && k->tty->ops->chars_in_buffer(k->tty); i++)
		msleep(TTY_RETRY_MS);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 0);
	KUNIT_EXPECT_EQ(test, k->tty->ops->write_room(k->tty), TTY_FIFO);
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, 0);
	sends = READ_ONCE(f->sends);
	msleep(10 * TTY_RETRY_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends);
	KUNIT_EXPECT_EQ_MSG(test, tty_k_cnt(k).dropped,
			    how == TTY_STOP_CLOSE ? TTY_TX_LEN : 0, "stop %d",
			    how);
	KUNIT_EXPECT_EQ_MSG(test, test_log_lines(test, log, closed),
			    how == TTY_STOP_CLOSE, "stop %d", how);
}

static void obmf_test_tty_refused_stop(struct kunit *test)
{
	tty_refused_stop(test, TTY_STOP_FLUSH);
	tty_refused_stop(test, TTY_STOP_HANGUP);
	tty_refused_stop(test, TTY_STOP_CLOSE);
	tty_refused_stop(test, TTY_STOP_UNBIND);
}

/*
 * A close while a request is in flight. Once it is answered,
 * one line counts every character the close discarded that the device
 * did not take, those in the request too: all of them when the device
 * does not answer, all but 2 when it accepts 2 of the request; they
 * count as dropped. After a reopen, the next answer logs and counts
 * nothing. A hangup while a request is in flight logs and counts
 * nothing.
 */
static void obmf_test_tty_close_try(struct kunit *test)
{
	static const char closed[] = "characters not sent at close";
	unsigned int sends, r;
	struct fake_step s;
	u8 buf[TTY_TX_LEN];
	struct obmf_fake *f;
	struct test_log *log;
	struct tty_k *k;
	char want[48];

	memset(buf, 'x', sizeof(buf));
	for (r = 0; r < 3; r++) {
		k = tty_tx_setup(test, &f);
		log = tty_log_start(f);
		tty_dflt(f, &tty_ack);
		if (r == 1) {
			s = tty_nack;
			s.how = FAKE_WORK;
			s.delay_ms = 500;
			s.payload[1] = 2;
		} else {
			s = tty_ack;
			s.how = FAKE_NEVER;
		}
		fake_push(f, &s);
		sends = READ_ONCE(f->sends);
		KUNIT_ASSERT_EQ(test,
				k->tty->ops->write(k->tty, buf, TTY_TX_LEN),
				TTY_TX_LEN);
		KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
		if (r == 2)
			break;
		k->tty->port->closing_wait = msecs_to_jiffies(100);
		tty_lock(k->tty);
		k->tty->ops->close(k->tty, NULL);
		k->open = false;
		tty_unlock(k->tty);
		KUNIT_EXPECT_FALSE(test, tty_port_initialized(k->tty->port));
		KUNIT_EXPECT_EQ(test, test_log_lines(test, log, closed), 0);
		snprintf(want, sizeof(want), ": %d %s",
			 r ? TTY_TX_LEN - 2 : TTY_TX_LEN, closed);
		KUNIT_EXPECT_EQ_MSG(test,
				    test_log_wait(test, log, want, 1,
						  2 * OBMF_DEFAULT_TIMEOUT_MS),
				    1, "round %u", r);
		KUNIT_EXPECT_EQ(test, test_log_lines(test, log, closed), 1);
		KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 0);
		KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, r ? 2 : 0);
		KUNIT_EXPECT_EQ_MSG(test, tty_k_cnt(k).dropped,
				    r ? TTY_TX_LEN - 2 : TTY_TX_LEN, "round %u",
				    r);
		tty_k_open(test, k);
		KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
		tty_tx_wait(test, k, r ? 6 : 4);
		KUNIT_EXPECT_EQ(test, test_log_lines(test, log, closed), 1);
		KUNIT_EXPECT_EQ_MSG(test, tty_k_cnt(k).dropped,
				    r ? TTY_TX_LEN - 2 : TTY_TX_LEN, "round %u",
				    r);
	}
	/* The last round hangs up instead */
	tty_vhangup(k->tty);
	k->open = false;
	msleep(OBMF_DEFAULT_TIMEOUT_MS + 300);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, closed), 0);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 0);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/*
 * Characters the device keeps refusing get one warning once
 * they have waited TTY_STALL_MS, within a second, naming why and how
 * many wait, and none after while the refusal goes on, also when its
 * reason changes; none for a shorter refusal. Once the device takes
 * them, the next stall gets its own warning: busy, then a timeout
 * status, then no answer, for which it comes with the first timeout
 * after TTY_STALL_MS, later as each timeout doubles the next wait. Each
 * warning counts one stall; nothing counts as dropped. The same text
 * logged by another device is not counted.
 */
static void obmf_test_tty_stall(struct kunit *test)
{
	static const char refused[] =
		"transmit stalled: refused, 4 characters waiting";
	static const char busy[] =
		"transmit stalled: busy, 4 characters waiting";
	static const char status[] =
		"transmit stalled: timeout status, 4 characters waiting";
	static const char quiet[] =
		"transmit stalled: no answer, 4 characters waiting";
	unsigned int wait = TTY_STALL_MS + 3 * OBMF_DEFAULT_TIMEOUT_MS;
	struct fake_step s = tty_ack;
	struct obmf_fake *f;
	struct tty_k *k;
	struct test_log *log;
	u8 buf[4];

	k = tty_tx_setup(test, &f);
	log = tty_log_start(f);
	dev_warn(f->tp.dev, "%s\n", refused);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, refused), 0);
	memcpy(buf, "abcd", sizeof(buf));
	scoped_guard(spinlock_irqsave, &f->lock) {
		f->dflt = tty_nack;
		f->nolog = true;
	}
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	msleep(TTY_STALL_MS - 1000);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 0);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "transmit stalled"), 0);
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, refused, 1, 2000), 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 1);
	s.status = OBMF_STATUS_BUSY;
	s.plen = 0;
	tty_dflt(f, &s);
	msleep(1000);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "transmit stalled"), 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 1);
	tty_dflt(f, &tty_ack);
	tty_tx_wait(test, k, 4);

	tty_dflt(f, &s);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, busy, 1, wait), 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 2);
	tty_dflt(f, &tty_ack);
	tty_tx_wait(test, k, 8);

	s.status = OBMF_STATUS_TIMEOUT;
	tty_dflt(f, &s);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, status, 1, wait), 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 3);
	tty_dflt(f, &tty_ack);
	tty_tx_wait(test, k, 12);

	s = tty_ack;
	s.how = FAKE_NEVER;
	tty_dflt(f, &s);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	/* Tries end 1, 4 and 9 s after the write: waits of 1, 2 and 4 s */
	wait = TTY_STALL_MS + 6 * OBMF_DEFAULT_TIMEOUT_MS;
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, quiet, 1, wait), 1);
	tty_dflt(f, &tty_ack);
	tty_tx_wait_ms(test, k, 16, 3 * OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "transmit stalled"), 4);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 4);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/*
 * A flush ends a stall: characters written after it, under
 * the same refusal, get their own warning TTY_STALL_MS later, counted as
 * a second stall. The flush counts nothing as dropped.
 */
static void obmf_test_tty_stall_flush(struct kunit *test)
{
	static const char stalled[] = "transmit stalled: refused";
	unsigned int wait = TTY_STALL_MS + 2000;
	struct obmf_fake *f;
	struct tty_k *k;
	struct test_log *log;
	u8 buf[4];

	k = tty_tx_setup(test, &f);
	log = tty_log_start(f);
	memcpy(buf, "abcd", sizeof(buf));
	scoped_guard(spinlock_irqsave, &f->lock) {
		f->dflt = tty_nack;
		f->nolog = true;
	}
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	KUNIT_ASSERT_EQ(test, test_log_wait(test, log, stalled, 1, wait), 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 1);
	k->tty->ops->flush_buffer(k->tty);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	msleep(TTY_STALL_MS - 1000);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, stalled), 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 1);
	KUNIT_EXPECT_EQ(test, test_log_wait(test, log, stalled, 2, 2000), 2);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 2);
	tty_dflt(f, &tty_ack);
	tty_tx_wait(test, k, 4);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/*
 * A flush while a request is in flight that the device then
 * refuses starts no stall: TTY_STALL_MS later, characters the device
 * refuses once get no warning, and no stall is counted.
 */
static void obmf_test_tty_stall_flush_try(struct kunit *test)
{
	static const u8 buf[] = "abcd";
	struct fake_step s = tty_nack;
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	struct test_log *log;

	k = tty_tx_setup(test, &f);
	log = tty_log_start(f);
	tty_dflt(f, &tty_ack);
	s.how = FAKE_WORK;
	s.delay_ms = 200;
	fake_push(f, &s);
	sends = READ_ONCE(f->sends);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	k->tty->ops->flush_buffer(k->tty);
	msleep(TTY_STALL_MS + 500);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 0);
	fake_push(f, &tty_nack);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait(test, k, 4);
	msleep(100);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).stalls, 0);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, "transmit stalled"), 0);
}

/*
 * A write while a request waits for its NACK does not cut
 * the back-off short: the next try comes TTY_RETRY_MS after the NACK,
 * and carries both writes.
 */
static void obmf_test_tty_backoff(struct kunit *test)
{
	static const u8 buf[] = "abcdefgh";
	struct fake_step s = tty_nack;
	struct obmf_fake *f;
	struct tty_k *k;
	int i, rx, tx;

	k = tty_tx_setup(test, &f);
	tty_dflt(f, &tty_ack);
	s.how = FAKE_WORK;
	s.delay_ms = 50;
	fake_push(f, &s);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	msleep(s.delay_ms / 2);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf + 4, 4), 4);
	tty_tx_wait(test, k, 8);
	rx = fake_find(f, i + 1, FAKE_RX, 1, -1);
	KUNIT_ASSERT_GE(test, rx, 0);
	tx = fake_find(f, rx + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, tx, 0);
	KUNIT_EXPECT_GE(test, ktime_ms_delta(f->log[tx].t, f->log[rx].t),
			TTY_RETRY_MS / 2);
	tty_tx_check(test, &f->log[tx], buf, 8, 1);
	KUNIT_EXPECT_LT(test, fake_find(f, tx + 1, FAKE_SEND, 1, 0), 0);
}

/*
 * System suspend stops the tries of a refused head, and a
 * write while the core is quiesced sends nothing; the characters stay.
 * Resume, the core first, tries again, and they go out.
 */
static void obmf_test_tty_suspend(struct kunit *test)
{
	const struct dev_pm_ops *pm;
	struct obmf_fake *f;
	unsigned int sends;
	struct device *dev;
	struct tty_k *k;
	u8 buf[4];

	if (!IS_ENABLED(CONFIG_PM_SLEEP))
		kunit_skip(test, "needs CONFIG_PM_SLEEP");
	k = tty_tx_setup(test, &f);
	dev = &chan(f, 1)->fdev->adev.dev;
	pm = dev->driver->pm;
	KUNIT_ASSERT_NOT_NULL(test, pm);
	memcpy(buf, "abcd", sizeof(buf));
	scoped_guard(spinlock_irqsave, &f->lock) {
		f->dflt = tty_nack;
		f->nolog = true;
	}
	sends = READ_ONCE(f->sends);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 3));
	KUNIT_ASSERT_EQ(test, pm->suspend(dev), 0);
	sends = READ_ONCE(f->sends);
	msleep(5 * TTY_RETRY_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends);
	KUNIT_ASSERT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	msleep(5 * TTY_RETRY_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends);
	KUNIT_EXPECT_EQ(test, k->tty->ops->chars_in_buffer(k->tty), 8);
	tty_dflt(f, &tty_ack);
	KUNIT_ASSERT_EQ(test, obmf_core_resume(f->core), 0);
	KUNIT_ASSERT_EQ(test, pm->resume(dev), 0);
	tty_tx_wait(test, k, 8);
}

/*
 * An unbind while a refused head waits for its next try, and
 * the last close of the tty right after, which frees the port: no try
 * runs after. Its timer firing on freed memory fails only under KASAN,
 * and no report is made on the test's own thread: the gate reads the
 * log.
 */
static void obmf_test_tty_unbind_backoff(struct kunit *test)
{
	unsigned int sends, late = 0;
	struct obmf_fake *f;
	struct tty_k *k;
	u8 buf[4];
	int i;

	memcpy(buf, "abcd", sizeof(buf));
	for (i = 0; i < 10; i++) {
		k = tty_tx_setup(test, &f);
		scoped_guard(spinlock_irqsave, &f->lock) {
			f->dflt = tty_nack;
			f->nolog = true;
		}
		sends = READ_ONCE(f->sends);
		KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
		KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 2));
		fake_unbind(f);
		k->open = false;
		tty_k_close(k);
		sends = READ_ONCE(f->sends);
		msleep(3 * TTY_RETRY_MS);
		late += READ_ONCE(f->sends) - sends;
	}
	KUNIT_EXPECT_EQ(test, late, 0);
}

/*
 * No request carries a tag. The first try of "abcd" is
 * answered after its timeout; the second try waits for that answer,
 * which is counted late and dropped, and takes its own. The device NACKs
 * the first try of "efgh" in time, and it is sent again. Each block
 * counts in tx once and nothing is lost.
 */
static void obmf_test_tty_shift(struct kunit *test)
{
	struct fake_step s = tty_ack;
	unsigned int sends, to, late;
	struct obmf_fake *f;
	struct tty_k *k;
	u8 buf[8];
	int i, n;

	k = tty_tx_setup(test, &f);
	tty_dflt(f, &tty_ack);
	memcpy(buf, "abcdefgh", sizeof(buf));
	s.how = FAKE_WORK;
	s.delay_ms = OBMF_DEFAULT_TIMEOUT_MS + 300;
	fake_push(f, &s);
	s.delay_ms = 300;
	fake_push(f, &s);
	s = tty_nack;
	s.how = FAKE_WORK;
	s.delay_ms = 300;
	fake_push(f, &s);
	to = ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS);
	late = ch_cnt(chan(f, 1), OBMF_CH_RX_LATE);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait_ms(test, k, 4, 3 * OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_ASSERT_EQ(test, READ_ONCE(f->sends), sends + 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_RX_LATE), late + 1);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf + 4, 4), 4);
	tty_tx_wait_ms(test, k, 8, 2 * OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 4);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to + 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_RX_LATE), late + 1);
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).tx, 8);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
	for (n = 0; n < 4; n++) {
		i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
		KUNIT_ASSERT_GE(test, i, 0);
		tty_tx_check(test, &f->log[i], buf + (n / 2) * 4, 4, n);
	}
}

/* How late the slow device below answers */
#define TTY_SLOW_MS 1500

/*
 * Each timeout doubles the wait for the answer of the
 * requests after it, which keep it: a device that answers TTY_SLOW_MS
 * late gets one repeat of the first request, which waits for the late
 * answer, and none of the next; a break waits as long. The late answer
 * is counted late, and nothing counts as dropped.
 */
static void obmf_test_tty_wait_double(struct kunit *test)
{
	static const u8 buf[] = "abcdefgh";
	struct fake_step s = tty_ack;
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	long to, late;
	int i, j;
	s64 gap;

	k = tty_tx_setup(test, &f);
	s.how = FAKE_WORK;
	s.delay_ms = TTY_SLOW_MS;
	tty_dflt(f, &s);
	to = ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS);
	late = ch_cnt(chan(f, 1), OBMF_CH_RX_LATE);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait_ms(test, k, 4, 3 * TTY_SLOW_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 2);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to + 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_RX_LATE), late + 1);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	j = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, j, 0);
	KUNIT_EXPECT_EQ(test, f->log[i].timeout_ms,
			OBMF_DEFAULT_TIMEOUT_MS + TTY_WIRE4_MS);
	KUNIT_EXPECT_EQ(test, f->log[j].timeout_ms,
			2 * OBMF_DEFAULT_TIMEOUT_MS + TTY_WIRE4_MS);
	/* The repeat waited for the late answer, not the whole hold */
	gap = ktime_ms_delta(f->log[j].t, f->log[i].t);
	KUNIT_EXPECT_GE(test, gap, TTY_SLOW_MS - jiffies_to_msecs(2));
	KUNIT_EXPECT_LT(test, gap, 2 * OBMF_DEFAULT_TIMEOUT_MS);

	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf + 4, 4), 4);
	tty_tx_wait_ms(test, k, 8, 2 * TTY_SLOW_MS);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + 1);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	KUNIT_EXPECT_EQ(test, f->log[i].timeout_ms,
			2 * OBMF_DEFAULT_TIMEOUT_MS + TTY_WIRE4_MS);

	i = fake_nlog(f) - 1;
	KUNIT_EXPECT_EQ(test, k->tty->ops->break_ctl(k->tty, 1), 0);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	KUNIT_EXPECT_EQ(test, f->log[i].timeout_ms,
			2 * OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to + 1);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/*
 * Five other channels whose sends each take their whole timeout and
 * fail, as on a device that takes no data for them: the port's writes
 * go out and are answered, and only its own timeouts would double its
 * wait, so the wait stays the default one
 */
static void obmf_test_tty_wait_busy(struct kunit *test)
{
	static const int chs[] = { 2, 3, 4, 5, 6 };
	static const u8 buf[] = "abcdefgh";
	int n = ARRAY_SIZE(chs);
	struct obmf_fake *f;
	struct test_busy *b;
	struct tty_k *k;
	long to;
	int i, j;

	k = tty_tx_setup_n(test, &f, n);
	tty_dflt(f, &tty_ack);
	to = ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS);
	WRITE_ONCE(f->send_sleep_chans, GENMASK(6, 2));
	WRITE_ONCE(f->send_sleep_ret, -ETIMEDOUT);
	WRITE_ONCE(f->send_sleep_ms, OBMF_MAX_TIMEOUT_MS);
	b = test_busy_start(f, chs, n, 0);
	for (j = 0; j < 2; j++) {
		i = fake_nlog(f) - 1;
		KUNIT_ASSERT_EQ(test,
				k->tty->ops->write(k->tty, buf + 4 * j, 4), 4);
		tty_tx_wait(test, k, 4 * (j + 1));
		i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
		KUNIT_ASSERT_GE(test, i, 0);
		KUNIT_EXPECT_EQ(test, f->log[i].timeout_ms,
				OBMF_DEFAULT_TIMEOUT_MS + TTY_WIRE4_MS);
	}
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
	test_busy_end(f, b, n);
	WRITE_ONCE(f->send_sleep_ms, 0);
	WRITE_ONCE(f->send_sleep_ret, 0);
	WRITE_ONCE(f->send_sleep_chans, 0);
}

/*
 * The wait a timeout doubled stays for the writes after it
 * until the port is closed; the next open starts again from
 * OBMF_DEFAULT_TIMEOUT_MS
 */
static void obmf_test_tty_wait_reopen(struct kunit *test)
{
	static const u8 buf[] = "abcdefgh";
	struct fake_step s = tty_ack;
	struct obmf_fake *f;
	struct tty_k *k;
	int i;

	k = tty_tx_setup(test, &f);
	tty_dflt(f, &tty_ack);
	s.how = FAKE_NEVER;
	fake_push(f, &s);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait_ms(test, k, 4, 4 * OBMF_DEFAULT_TIMEOUT_MS);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf + 4, 4), 4);
	tty_tx_wait(test, k, 8);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	KUNIT_EXPECT_EQ(test, f->log[i].timeout_ms,
			2 * OBMF_DEFAULT_TIMEOUT_MS + TTY_WIRE4_MS);

	tty_k_reopen(test, k);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait(test, k, 12);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	KUNIT_EXPECT_EQ(test, f->log[i].timeout_ms,
			OBMF_DEFAULT_TIMEOUT_MS + TTY_WIRE4_MS);
	KUNIT_EXPECT_LT(test, fake_find(f, i + 1, FAKE_SEND, 1, 0), 0);
}

/* More tries than the doublings from the default wait to the limit */
#define TTY_STATUS_TRIES 8

/*
 * A device that answers every try at once with the timeout status, or
 * with the line timeout, has answered in time: every try waits as long
 * as the first, and none counts as a timeout
 */
static void obmf_test_tty_wait_status(struct kunit *test)
{
	static const u8 st[] = { OBMF_STATUS_TIMEOUT,
				 OBMF_STATUS_SERIAL_LINE_TIMEOUT };
	static const u8 buf[] = "abcd";
	struct fake_step s = tty_ack;
	unsigned int sends;
	struct obmf_fake *f;
	struct tty_k *k;
	int i, j, n;
	long to;

	k = tty_tx_setup(test, &f);
	to = ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS);
	s.plen = 0;
	for (j = 0; j < ARRAY_SIZE(st); j++) {
		s.status = st[j];
		tty_dflt(f, &s);
		sends = READ_ONCE(f->sends);
		i = fake_nlog(f) - 1;
		KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
		KUNIT_ASSERT_TRUE(test,
				  fake_wait_sends(f, sends + TTY_STATUS_TRIES));
		tty_dflt(f, &tty_ack);
		tty_tx_wait(test, k, 4 * (j + 1));
		KUNIT_ASSERT_LT(test, fake_nlog(f), FAKE_LOG_LEN);
		n = 0;
		while ((i = fake_find(f, i + 1, FAKE_SEND, 1, 0)) >= 0) {
			KUNIT_EXPECT_EQ_MSG(test, f->log[i].timeout_ms,
					    OBMF_DEFAULT_TIMEOUT_MS +
						    TTY_WIRE4_MS,
					    "status %#x, try %d", st[j], n);
			n++;
		}
		KUNIT_EXPECT_GT(test, n, TTY_STATUS_TRIES);
	}
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/*
 * How long after the host's wait the device below answers: past the
 * timer wheel's slack on that wait, within the core's hold after it
 */
#define TTY_LATE_MS 500

/*
 * A device that answers every try only after the host has stopped
 * waiting doubles the wait each time, up to OBMF_MAX_TIMEOUT_MS: each
 * late answer is counted late and ends the hold, and the try after the
 * fourth timeout waits the limit and is answered
 */
static void obmf_test_tty_wait_cap(struct kunit *test)
{
	static const u8 buf[] = "abcd";
	struct fake_step s = tty_ack;
	unsigned int sends, wait;
	struct obmf_fake *f;
	struct tty_k *k;
	long to, late;
	int i, n = 0;

	k = tty_tx_setup(test, &f);
	tty_dflt(f, &tty_ack);
	s.how = FAKE_WORK;
	for (wait = OBMF_DEFAULT_TIMEOUT_MS; wait < OBMF_MAX_TIMEOUT_MS;
	     wait *= 2) {
		s.delay_ms = wait + TTY_WIRE4_MS + TTY_LATE_MS;
		fake_push(f, &s);
		n++;
	}
	to = ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS);
	late = ch_cnt(chan(f, 1), OBMF_CH_RX_LATE);
	sends = READ_ONCE(f->sends);
	i = fake_nlog(f) - 1;
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, 4), 4);
	tty_tx_wait_ms(test, k, 4, 2 * wait);
	KUNIT_EXPECT_EQ(test, READ_ONCE(f->sends), sends + n + 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to + n);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_RX_LATE), late + n);
	for (wait = OBMF_DEFAULT_TIMEOUT_MS; n >= 0; n--, wait *= 2) {
		i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
		KUNIT_ASSERT_GE(test, i, 0);
		KUNIT_EXPECT_EQ(test, f->log[i].timeout_ms,
				min(wait + TTY_WIRE4_MS, OBMF_MAX_TIMEOUT_MS));
	}
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);
}

/* A slow line, and a write that spends 2 s on it */
#define TTY_CAP_HZ 1200
#define TTY_CAP_CHARS 240
#define TTY_CAP_WIRE_MS \
	(2 * DIV_ROUND_UP(TTY_CAP_CHARS * 10 * MSEC_PER_SEC, TTY_CAP_HZ))
/*
 * Timeouts in a row: the wait doubles from OBMF_DEFAULT_TIMEOUT_MS, so
 * an unsigned int that is not held at the limit is 0 after 29 of them
 */
#define TTY_CAP_TIMEOUTS 30

static const struct test_chan tty_cap_chans[] = {
	{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "uart",
	  OBMF_HDR_LEN + 3 + TTY_CAP_CHARS, 0, TTY_CFG_LEN },
};

/*
 * On a slow line the wait of a long write passes OBMF_MAX_TIMEOUT_MS
 * before the doubled wait does. The transport times out the first
 * TTY_CAP_TIMEOUTS tries or more at once; each doubles the wait, which
 * the core caps for the long write from the fourth on, and which stays
 * at the limit, which a break then waits.
 */
static void obmf_test_tty_wait_slow(struct kunit *test)
{
	static const unsigned int want[] = { 5000, 6000, 8000 };
	struct fake_step out = { .send_ret = -ETIMEDOUT };
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	unsigned int sends, n;
	u8 buf[TTY_CAP_CHARS];
	struct tty_k *k;
	long to, txe;
	int i;

	BUILD_BUG_ON(TTY_CAP_WIRE_MS != 4000);
	BUILD_BUG_ON(OBMF_DEFAULT_TIMEOUT_MS != 1000 ||
		     OBMF_MAX_TIMEOUT_MS != 10000);
	fake_layout(f, tty_cap_chans, ARRAY_SIZE(tty_cap_chans));
	tty_cfg(f, 1, TTY_CAP_HZ, 8, 1, 0);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_ASSERT_TRUE(test, tty_bound(f, 1));
	k = tty_k_get(f, 1);
	tty_k_open(test, k);
	/* More steps than the fake queues: the default times out instead */
	tty_dflt(f, &out);
	memset(buf, 'a', sizeof(buf));
	to = ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS);
	txe = ch_cnt(chan(f, 1), OBMF_CH_TX_ERRORS);
	i = fake_nlog(f) - 1;
	sends = READ_ONCE(f->sends);
	KUNIT_ASSERT_EQ(test, k->tty->ops->write(k->tty, buf, sizeof(buf)),
			sizeof(buf));
	for (n = 0; n < 250; n++) {
		if (READ_ONCE(f->sends) >= sends + TTY_CAP_TIMEOUTS)
			break;
		msleep(20);
	}
	tty_dflt(f, &tty_ack);
	tty_tx_wait(test, k, sizeof(buf));
	sends = READ_ONCE(f->sends) - sends;
	KUNIT_ASSERT_GT(test, sends, TTY_CAP_TIMEOUTS);
	for (n = 0; n < sends; n++) {
		i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
		KUNIT_ASSERT_GE(test, i, 0);
		KUNIT_EXPECT_EQ_MSG(test, f->log[i].timeout_ms,
				    n < ARRAY_SIZE(want) ? want[n] :
							   OBMF_MAX_TIMEOUT_MS,
				    "try %u", n);
	}
	KUNIT_EXPECT_LT(test, fake_find(f, i + 1, FAKE_SEND, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TX_ERRORS),
			txe + sends - 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_TIMEOUTS), to);
	KUNIT_EXPECT_EQ(test, tty_k_cnt(k).dropped, 0);

	KUNIT_EXPECT_EQ(test, k->tty->ops->break_ctl(k->tty, 1), 0);
	i = fake_find(f, i + 1, FAKE_SEND, 1, 0);
	KUNIT_ASSERT_GE(test, i, 0);
	KUNIT_EXPECT_EQ(test, f->log[i].timeout_ms, OBMF_MAX_TIMEOUT_MS);
}

/*
 * A device write with the overrun event counts overrun and
 * pushes a 0x00 with TTY_OVERRUN before its character, with no event and
 * no line from the driver
 */
static void obmf_test_tty_overrun(struct kunit *test)
{
	struct obmf_fake *f = tty_setup(test);
	struct tty_k *k = tty_k_get(f, 1);
	struct test_log *log = tty_log_start(f);
	struct tty_port *port;
	struct fake_rec *r;
	u8 c[2], fl[2];
	long ev;
	int ret;

	tty_k_open(test, k);
	port = k->tty->port;
	tty_buffer_lock_exclusive(port);
	ret = kunit_add_action_or_reset(test, tty_buf_unlock, port);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ev = er_total(f->core, OBMF_EV_NR);
	r = tty_write(f, TTY_EV_OVERRUN, 1);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, 0, 1));
	KUNIT_EXPECT_EQ(test, tty_k_icount(test, k).overrun, 1);
	KUNIT_ASSERT_EQ(test, tty_held(port, c, fl, 2), 2);
	KUNIT_EXPECT_EQ(test, c[0], 0);
	KUNIT_EXPECT_EQ(test, fl[0], TTY_OVERRUN);
	KUNIT_EXPECT_EQ(test, c[1], 'a');
	KUNIT_EXPECT_EQ(test, fl[1], TTY_NORMAL);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_NR), ev);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, ""), 0);
	kunit_release_action(test, tty_buf_unlock, port);
}

/*
 * While the port is closed, a device write with characters is
 * NACKed with count 0 by the driver, with no event and no line
 */
static void obmf_test_tty_closed_quiet(struct kunit *test)
{
	struct obmf_fake *f = tty_setup(test);
	struct test_log *log = tty_log_start(f);
	long ev = er_total(f->core, OBMF_EV_NR);
	struct fake_rec *r;

	r = tty_write(f, 0, 4);
	KUNIT_EXPECT_TRUE(test, tty_answer(r, FAKE_SERIAL_NACK, 0));
	KUNIT_EXPECT_EQ(test, ch_cnt(chan(f, 1), OBMF_CH_DEFAULT_ANSWERS), 0);
	KUNIT_EXPECT_EQ(test, er_total(f->core, OBMF_EV_NR), ev);
	KUNIT_EXPECT_EQ(test, test_log_lines(test, log, ""), 0);
}

/*
 * A serial channel whose request limit leaves no room for a character
 * gets no tty, and one with room for one gets one. Expected: "request
 * limit 3 too small" for channel 1.
 */
static void obmf_test_tty_limit(struct kunit *test)
{
	static const struct test_chan c[] = {
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "s1", OBMF_HDR_LEN + 3,
		  0, TTY_CFG_LEN },
		{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "s2", OBMF_HDR_LEN + 4,
		  0, TTY_CFG_LEN },
	};
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	int i;

	fake_layout(f, c, ARRAY_SIZE(c));
	for (i = 0; i < ARRAY_SIZE(c); i++)
		tty_cfg(f, i + 1, TTY_HZ, 8, 1, 0);
	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_EXPECT_FALSE(test, tty_bound(f, 1));
	KUNIT_EXPECT_TRUE(test, tty_bound(f, 2));
}

static struct kunit_case obmf_tty_cases[] = {
	KUNIT_CASE(obmf_test_tty_line),
	KUNIT_CASE(obmf_test_tty_flow),
	KUNIT_CASE(obmf_test_tty_full),
	KUNIT_CASE(obmf_test_tty_hangup),
	KUNIT_CASE(obmf_test_tty_queue_full),
	KUNIT_CASE(obmf_test_tty_failed),
	KUNIT_CASE(obmf_test_tty_closed),
	KUNIT_CASE(obmf_test_tty_break),
	KUNIT_CASE(obmf_test_tty_break_timeout),
	KUNIT_CASE(obmf_test_tty_slow),
	KUNIT_CASE(obmf_test_tty_node),
	KUNIT_CASE(obmf_test_tty_tx),
	KUNIT_CASE(obmf_test_tty_tx_flush),
	KUNIT_CASE(obmf_test_tty_tx_flush_twice),
	KUNIT_CASE(obmf_test_tty_tx_flush_partial),
	KUNIT_CASE(obmf_test_tty_nack_bound),
	KUNIT_CASE_SLOW(obmf_test_tty_nack_long),
	KUNIT_CASE_SLOW(obmf_test_tty_timeout),
	KUNIT_CASE(obmf_test_tty_drop_report),
	KUNIT_CASE(obmf_test_tty_debugfs),
	KUNIT_CASE(obmf_test_tty_unbind_open),
	KUNIT_CASE(obmf_test_tty_refused_room),
	KUNIT_CASE_SLOW(obmf_test_tty_refused_stop),
	KUNIT_CASE_SLOW(obmf_test_tty_close_try),
	KUNIT_CASE_SLOW(obmf_test_tty_stall),
	KUNIT_CASE_SLOW(obmf_test_tty_stall_flush),
	KUNIT_CASE_SLOW(obmf_test_tty_stall_flush_try),
	KUNIT_CASE(obmf_test_tty_backoff),
	KUNIT_CASE(obmf_test_tty_suspend),
	KUNIT_CASE(obmf_test_tty_unbind_backoff),
	KUNIT_CASE_SLOW(obmf_test_tty_shift),
	KUNIT_CASE_SLOW(obmf_test_tty_wait_double),
	KUNIT_CASE_SLOW(obmf_test_tty_wait_busy),
	KUNIT_CASE_SLOW(obmf_test_tty_wait_reopen),
	KUNIT_CASE_SLOW(obmf_test_tty_wait_cap),
	KUNIT_CASE(obmf_test_tty_wait_status),
	KUNIT_CASE(obmf_test_tty_wait_slow),
	KUNIT_CASE(obmf_test_tty_overrun),
	KUNIT_CASE(obmf_test_tty_closed_quiet),
	KUNIT_CASE(obmf_test_tty_limit),
	{}
};

static struct kunit_suite obmf_tty_suite = {
	.name = "obmf-tty",
	.suite_init = obmf_test_fn_suite_init,
	.suite_exit = obmf_test_fn_suite_exit,
	.init = obmf_tty_test_init,
	.exit = test_log_exit,
	.test_cases = obmf_tty_cases,
};

kunit_test_suite(obmf_tty_suite);

#endif /* IS_BUILTIN(CONFIG_OBMF_TTY) */

/* The GPIO driver on the fake transport; only when built in */
#if IS_BUILTIN(CONFIG_GPIO_OBMF)

#define CH_GPIO_ONLY 1

/* A Table 77 entry; the fields not given 0 */
struct gt_line {
	u16 index;
	const char *name;
	u8 dir;
	u8 dflt;
	u8 drive;
	u8 bias;
};

/* Channel 1 only: GPIO, @limit bytes each way, @n lines in Table 76 form */
static struct obmf_fake *gt_create(struct kunit *test, u16 limit,
				   const struct gt_line *l, int n)
{
	const size_t esz = sizeof(struct obmf_gpio_cfg_entry);
	struct test_chan c = { OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, "gpio",
			       limit, 0, OBMF_GPIO_COUNT_LEN + n * esz };
	struct obmf_fake *f = fake_create(test, FAKE_MAX_MSG, 0);
	struct obmf_gpio_cfg_entry *e;
	u8 *cfg;
	int i;

	fake_layout(f, &c, 1);
	cfg = f->mem + fake_ch_off(f, CH_GPIO_ONLY) + FAKE_CH_HDR_LEN;
	memset(cfg, 0, c.config_len);
	put_unaligned_le16(n, cfg);
	e = (struct obmf_gpio_cfg_entry *)(cfg + OBMF_GPIO_COUNT_LEN);
	for (i = 0; i < n; i++) {
		e[i].index = cpu_to_le16(l[i].index);
		if (l[i].name)
			memcpy(e[i].line.name, l[i].name,
			       min(strlen(l[i].name), sizeof(e[i].line.name)));
		e[i].line.direction = l[i].dir;
		e[i].line.default_output = l[i].dflt;
		e[i].line.drive = l[i].drive;
		e[i].line.bias = l[i].bias;
	}
	return f;
}

/* The configuration of channel 1, and its length in the header */
static u8 *gt_cfg(struct obmf_fake *f, u32 len)
{
	u8 *h = f->mem + fake_ch_off(f, CH_GPIO_ONLY);

	put_unaligned_le32(len, h + 0x1c);
	return h + FAKE_CH_HDR_LEN;
}

static int gt_match(struct gpio_chip *gc, const void *data)
{
	return gc->parent == data;
}

KUNIT_DEFINE_ACTION_WRAPPER(gt_put_gdev, gpio_device_put,
			    struct gpio_device *);

/* Bind; the chip the GPIO driver registered on channel 1, or NULL */
static struct gpio_chip *gt_bind(struct obmf_fake *f)
{
	struct kunit *test = f->test;
	struct gpio_device *gdev;
	struct obmf_channel *ch;

	KUNIT_ASSERT_EQ(test, fake_bind(f), 0);
	KUNIT_ASSERT_EQ(test, obmf_core_state(f->core), OBMF_LIVE);
	ch = chan(f, CH_GPIO_ONLY);
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	gdev = gpio_device_find(&ch->fdev->adev.dev, gt_match);
	if (!gdev)
		return NULL;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, gt_put_gdev, gdev),
			0);
	return gpio_device_get_chip(gdev);
}

KUNIT_DEFINE_ACTION_WRAPPER(gt_free_desc, gpiochip_free_own_desc,
			    struct gpio_desc *);

static struct gpio_desc *gt_desc(struct kunit *test, struct gpio_chip *gc,
				 unsigned int hw, enum gpiod_flags flags)
{
	struct gpio_desc *d;

	d = gpiochip_request_own_desc(gc, hw, "kunit",
				      GPIO_LOOKUP_FLAGS_DEFAULT, flags);
	if (!IS_ERR(d))
		KUNIT_ASSERT_EQ(test,
				kunit_add_action_or_reset(test, gt_free_desc, d),
				0);
	return d;
}

/* A reply of the echo and @n entries */
static void gt_reply(struct obmf_fake *f, u8 status, const u16 *ent, int n)
{
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.echo = true,
		.status = status,
		.plen = 2 * n,
	};
	int i;

	for (i = 0; i < n; i++)
		put_unaligned_le16(ent[i], s.payload + 2 * i);
	fake_push(f, &s);
}

/* The payload of request @nth (from 0) on channel 1 */
static const u8 *gt_sent(struct obmf_fake *f, int nth, size_t *len)
{
	int i = -1;

	do
		i = fake_find(f, i + 1, FAKE_SEND, CH_GPIO_ONLY, 0);
	while (i >= 0 && nth--);
	KUNIT_ASSERT_GE(f->test, i, 0);
	*len = f->log[i].len - OBMF_HDR_LEN;
	return f->log[i].msg + OBMF_HDR_LEN;
}

static int gt_sends(struct obmf_fake *f)
{
	return fake_count(f, FAKE_SEND, CH_GPIO_ONLY, 0);
}

/*
 * gpiod_set_value_cansleep() and the chip's set() and set_multiple()
 * with their result. Before v6.17 they return void, and a case that
 * checks the result is skipped.
 */
static int gt_set_value(struct kunit *test, struct gpio_desc *desc, int value)
{
#ifdef OBMF_COMPAT_GPIO_SET_VOID
	kunit_skip(test, "gpiod_set_value() returns void on this kernel");
	return 0;
#else
	return gpiod_set_value_cansleep(desc, value);
#endif
}

static int gt_set(struct kunit *test, struct gpio_chip *gc, unsigned int hw,
		  int value)
{
#ifdef OBMF_COMPAT_GPIO_SET_VOID
	kunit_skip(test, "set() returns void on this kernel");
	return 0;
#else
	return gc->set(gc, hw, value);
#endif
}

static int gt_set_multiple(struct kunit *test, struct gpio_chip *gc,
			   unsigned long *mask, unsigned long *bits)
{
#ifdef OBMF_COMPAT_GPIO_SET_VOID
	kunit_skip(test, "set_multiple() returns void on this kernel");
	return 0;
#else
	return gc->set_multiple(gc, mask, bits);
#endif
}

#define GT_FULL_NAME "line-name-of-thirty-two-bytes-xx"

/*
 * Table 77: a line per listed index, the others holes; a direction,
 * default output, drive or bias out of range leaves the line out. A name
 * fills the 32 bytes without a NUL or is left out when it has a control
 * character. Nothing is sent at probe.
 */
static void obmf_test_gpio_lines(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in0", 1 },
		{ 1, "out1", 0, 1 },
		{ 3, GT_FULL_NAME, 0, 0, 1 },
		{ 4, "dir2", 2 },
		{ 5, "bad\tname", 1, 0, 2, 2 },
		{ 6, "bias3", 1, 0, 0, 3 },
		{ 7, "dflt2", 0, 2 },
		{ 8, "drive3", 0, 0, 3 },
	};
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	static const bool valid[] = { 1, 1, 0, 1, 0, 1 };
	int i;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_EXPECT_EQ(test, gc->ngpio, 6);
	KUNIT_EXPECT_TRUE(test, gc->can_sleep);
	KUNIT_EXPECT_STREQ(test, gc->label, "obmf-test_gpio");
	for (i = 0; i < ARRAY_SIZE(valid); i++)
		KUNIT_EXPECT_EQ_MSG(test, gpiochip_line_is_valid(gc, i),
				    valid[i], "line %d", i);
	KUNIT_ASSERT_NOT_NULL(test, gc->names);
	KUNIT_EXPECT_STREQ(test, gc->names[0], "in0");
	KUNIT_EXPECT_STREQ(test, gc->names[1], "out1");
	KUNIT_EXPECT_NULL(test, gc->names[2]);
	KUNIT_EXPECT_STREQ(test, gc->names[3], GT_FULL_NAME);
	KUNIT_EXPECT_NULL(test, gc->names[5]);
	KUNIT_EXPECT_EQ(test, gc->get_direction(gc, 0), GPIO_LINE_DIRECTION_IN);
	KUNIT_EXPECT_EQ(test, gc->get_direction(gc, 1),
			GPIO_LINE_DIRECTION_OUT);
	KUNIT_EXPECT_EQ(test, gc->get_direction(gc, 3),
			GPIO_LINE_DIRECTION_OUT);
	KUNIT_EXPECT_EQ(test, gc->get_direction(gc, 5), GPIO_LINE_DIRECTION_IN);
	KUNIT_EXPECT_EQ(test, PTR_ERR(gt_desc(test, gc, 2, GPIOD_ASIS)),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 0);
}

/*
 * Get Values reads an input line; an output reads back the value last
 * set, from GPIO_DEFAULT_OUTPUT before any Set, with nothing sent. Set
 * Values carries the line and its value; a Set of an input is refused
 * before the wire.
 */
static void obmf_test_gpio_get_set(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in", 1 },
		{ 1, "out", 0, 1 },
	};
	static const u8 get[] = { OBMF_GPIO_GET_VALUES, 0x00, 0x00 };
	static const u8 set[] = { OBMF_GPIO_SET_VALUES, 0x01, 0x00 };
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct gpio_desc *in, *out;
	const u8 *p;
	size_t len;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	in = gt_desc(test, gc, 0, GPIOD_ASIS);
	out = gt_desc(test, gc, 1, GPIOD_ASIS);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));

	gt_reply(f, 0, (const u16[]){ 0x1000 }, 1);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), 1);
	p = gt_sent(f, 0, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(get));
	KUNIT_EXPECT_MEMEQ(test, p, get, sizeof(get));
	gt_reply(f, 0, (const u16[]){ 0x0000 }, 1);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), 0);

	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 1);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 2);
	gt_reply(f, 0, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set_value(test, out, 0), 0);
	p = gt_sent(f, 2, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(set));
	KUNIT_EXPECT_MEMEQ(test, p, set, sizeof(set));
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 0);

	KUNIT_EXPECT_EQ(test, gt_set(test, gc, 0, 1), -EPERM);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 3);
}

/*
 * A Set the device refuses keeps the value last set; each Table 17 GPIO
 * status gives its errno; a Get or Set that times out gives -ETIMEDOUT
 */
static void obmf_test_gpio_refused(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in", 1 },
		{ 1, "out", 0, 1 },
	};
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct gpio_desc *in, *out;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	in = gt_desc(test, gc, 0, GPIOD_ASIS);
	out = gt_desc(test, gc, 1, GPIOD_ASIS);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));

	gt_reply(f, OBMF_STATUS_GPIO_INVALID_OPERATION, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set_value(test, out, 0), -EPERM);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 1);
	gt_reply(f, OBMF_STATUS_GPIO_INDEX_UNSUPPORTED, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set_value(test, out, 0), -EINVAL);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 1);
	gt_reply(f, OBMF_STATUS_GPIO_INDEX_UNSUPPORTED, NULL, 0);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EINVAL);
	gt_reply(f, OBMF_STATUS_GPIO_IRQ_UNSUPPORTED, NULL, 0);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EOPNOTSUPP);

	fake_push(f, &(struct fake_step){ .how = FAKE_NEVER, .channel = -1 });
	KUNIT_EXPECT_EQ(test, gt_set_value(test, out, 0), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 1);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 5);
}

/*
 * A timeout status (Table 4 0x02) on a Get, a change of direction or a
 * Set reaches gpiolib's users as -ETIMEDOUT, as no answer does, and the
 * output keeps the value last set. Set returns void before v6.17, so
 * there only the change of direction checks a Set.
 */
static void obmf_test_gpio_timeout_status(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in", 1 },
		{ 1, "out", 0, 1 },
	};
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	unsigned long mask = BIT(0), bits = 0;
	struct gpio_desc *in, *out;
	int sends = 3;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	in = gt_desc(test, gc, 0, GPIOD_ASIS);
	out = gt_desc(test, gc, 1, GPIOD_ASIS);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));

	gt_reply(f, OBMF_STATUS_TIMEOUT, NULL, 0);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -ETIMEDOUT);
	gt_reply(f, OBMF_STATUS_TIMEOUT, NULL, 0);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), -ETIMEDOUT);
	gt_reply(f, OBMF_STATUS_TIMEOUT, NULL, 0);
	KUNIT_EXPECT_EQ(test, gpiod_direction_output(out, 0), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 1);
#ifndef OBMF_COMPAT_GPIO_SET_VOID
	mask = BIT(1);
	gt_reply(f, OBMF_STATUS_TIMEOUT, NULL, 0);
	KUNIT_EXPECT_EQ(test, gpiod_set_value_cansleep(out, 0), -ETIMEDOUT);
	gt_reply(f, OBMF_STATUS_TIMEOUT, NULL, 0);
	KUNIT_EXPECT_EQ(test, gc->set_multiple(gc, &mask, &bits), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 1);
	sends += 2;
#endif
	KUNIT_EXPECT_EQ(test, gt_sends(f), sends);
}

/*
 * Inside the driver a timeout status stays -ETIME: the core returns it,
 * and the failed Set's report carries it, while the GPIO user gets
 * -ETIMEDOUT
 */
static void obmf_test_gpio_timeout_report(struct kunit *test)
{
	static const struct gt_line l[] = { { 0, "out", 0, 1 } };
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct gpio_desc *out;
	struct er_ctx *x;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	test->priv = kunit_kzalloc(test, sizeof(*x), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, test->priv);
	x = er_start(test);
	out = gt_desc(test, gc, 0, GPIOD_ASIS);
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));

	gt_reply(f, OBMF_STATUS_TIMEOUT, NULL, 0);
	KUNIT_EXPECT_EQ(test, er_req(chan(f, CH_GPIO_ONLY), 0), -ETIME);
	KUNIT_EXPECT_EQ(test, x->n, 0);

	gt_reply(f, OBMF_STATUS_TIMEOUT, NULL, 0);
	KUNIT_EXPECT_EQ(test, gpiod_direction_output(out, 0), -ETIMEDOUT);
	KUNIT_ASSERT_EQ(test, x->n, 1);
	KUNIT_EXPECT_EQ(test, x->rec[0].ev, OBMF_EV_GPIO_SET);
	KUNIT_EXPECT_EQ(test, x->rec[0].ch, CH_GPIO_ONLY);
	KUNIT_EXPECT_EQ(test, x->rec[0].a, 0);
	KUNIT_EXPECT_EQ(test, x->rec[0].b, -ETIME);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_SET), 1);
}

/*
 * A Set the device takes is no event. One that fails, here busy, is one
 * event of its channel with the line and the errno, or line -1 for a Set
 * of several lines. A Set of an input, refused here or by the device
 * (-EPERM), and one that finds the device gone (-ENODEV) are none.
 */
static void obmf_test_gpio_set_report(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in", 1 },
		{ 1, "out", 0, 1 },
	};
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct fake_step gone = { .send_ret = -ENODEV };
	unsigned long mask = BIT(1), bits = 0;
	struct er_ctx *x;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	test->priv = kunit_kzalloc(test, sizeof(*x), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, test->priv);
	x = er_start(test);

	gt_reply(f, 0, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set(test, gc, 1, 1), 0);
	gt_reply(f, 0, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), 0);
	KUNIT_EXPECT_EQ(test, x->n, 0);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_SET), 0);

	gt_reply(f, OBMF_STATUS_BUSY, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set(test, gc, 1, 0), -EBUSY);
	gt_reply(f, OBMF_STATUS_BUSY, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), -EBUSY);
	KUNIT_ASSERT_EQ(test, x->n, 2);
	KUNIT_EXPECT_EQ(test, x->rec[0].ev, OBMF_EV_GPIO_SET);
	KUNIT_EXPECT_EQ(test, x->rec[0].ch, CH_GPIO_ONLY);
	KUNIT_EXPECT_EQ(test, x->rec[0].a, 1);
	KUNIT_EXPECT_EQ(test, x->rec[0].b, -EBUSY);
	KUNIT_EXPECT_EQ(test, x->rec[1].ev, OBMF_EV_GPIO_SET);
	KUNIT_EXPECT_EQ(test, x->rec[1].a, -1);
	KUNIT_EXPECT_EQ(test, x->rec[1].b, -EBUSY);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_SET), 2);

	KUNIT_EXPECT_EQ(test, gt_set(test, gc, 0, 1), -EPERM);
	mask = BIT(0) | BIT(1);
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), -EPERM);
	mask = BIT(1);
	gt_reply(f, OBMF_STATUS_GPIO_INVALID_OPERATION, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set(test, gc, 1, 0), -EPERM);
	gt_reply(f, OBMF_STATUS_GPIO_INVALID_OPERATION, NULL, 0);
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), -EPERM);
	fake_push(f, &gone);
	KUNIT_EXPECT_EQ(test, gt_set(test, gc, 1, 0), -ENODEV);
	fake_push(f, &gone);
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), -ENODEV);
	KUNIT_EXPECT_EQ(test, x->n, 2);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_SET), 2);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 8);
}

/*
 * An output reads the value last set only while the device
 * works: suspended, a Get of it alone or with other outputs is -EAGAIN;
 * after the fatal stop -EIO. Nothing is sent.
 */
static void obmf_test_gpio_failed_get(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "out-0", 0, 1 },
		{ 1, "out-1", 0, 0 },
	};
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	unsigned long mask = BIT(0) | BIT(1), bits = 0;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_EXPECT_EQ(test, gc->get(gc, 0), 1);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), 0);
	KUNIT_EXPECT_EQ(test, bits, BIT(0));

	KUNIT_ASSERT_EQ(test, obmf_core_quiesce(f->core), 0);
	KUNIT_EXPECT_EQ(test, gc->get(gc, 0), -EAGAIN);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), -EAGAIN);
	KUNIT_ASSERT_EQ(test, obmf_core_resume(f->core), 0);
	KUNIT_EXPECT_EQ(test, gc->get(gc, 1), 0);

	obmf_core_fatal(f->core, -EPROTO);
	KUNIT_EXPECT_EQ(test, gc->get(gc, 0), -EIO);
	KUNIT_EXPECT_EQ(test, gc->get(gc, 1), -EIO);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), -EIO);
	mask = BIT(1);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), -EIO);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 0);
}

/*
 * Table 19: a Get answered with another line, a line beyond the chip, a
 * value above 1, the wrong count of entries or the wrong echo is -EPROTO
 * and a protocol error; so is a Set answered with entries
 */
static void obmf_test_gpio_bad_reply(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in", 1 },
		{ 1, "out", 0, 1 },
	};
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct obmf_channel *ch = chan(f, CH_GPIO_ONLY);
	struct fake_step s = {
		.how = FAKE_INLINE,
		.channel = -1,
		.plen = 3,
		.payload = { OBMF_GPIO_SET_VALUES, 0x00, 0x10 },
	};
	struct gpio_desc *in, *out;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	in = gt_desc(test, gc, 0, GPIOD_ASIS);
	out = gt_desc(test, gc, 1, GPIOD_ASIS);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));

	gt_reply(f, 0, (const u16[]){ 0x1001 }, 1);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EPROTO);
	gt_reply(f, 0, (const u16[]){ 0x0040 }, 1);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EPROTO);
	gt_reply(f, 0, (const u16[]){ 0x0fff }, 1);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EPROTO);
	gt_reply(f, 0, (const u16[]){ 0x2000 }, 1);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EPROTO);
	gt_reply(f, 0, (const u16[]){ 0x1000, 0x1000 }, 2);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EPROTO);
	gt_reply(f, 0, NULL, 0);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EPROTO);
	fake_push(f, &s);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -EPROTO);
	gt_reply(f, 0, (const u16[]){ 0x0001 }, 1);
	KUNIT_EXPECT_EQ(test, gt_set_value(test, out, 0), -EPROTO);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 1);
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_PROTOCOL_ERRORS), 8);
	gt_reply(f, 0, (const u16[]){ 0x1000 }, 1);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), 1);
}

/*
 * The direction is fixed: asking an output for input or an input for
 * output is -EINVAL with nothing sent; an output's own direction sets its
 * value
 */
static void obmf_test_gpio_direction(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in", 1 },
		{ 1, "out", 0, 0 },
	};
	static const u8 set[] = { OBMF_GPIO_SET_VALUES, 0x01, 0x10 };
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct gpio_desc *in, *out;
	const u8 *p;
	size_t len;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_EXPECT_EQ(test, PTR_ERR(gt_desc(test, gc, 0, GPIOD_OUT_LOW)),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, PTR_ERR(gt_desc(test, gc, 1, GPIOD_IN)),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 0);

	in = gt_desc(test, gc, 0, GPIOD_IN);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));
	KUNIT_EXPECT_EQ(test, gt_sends(f), 0);
	gt_reply(f, 0, NULL, 0);
	out = gt_desc(test, gc, 1, GPIOD_OUT_HIGH);
	KUNIT_ASSERT_FALSE(test, IS_ERR(out));
	p = gt_sent(f, 0, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(set));
	KUNIT_EXPECT_MEMEQ(test, p, set, sizeof(set));
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(out), 1);
}

/*
 * Three lines a message (11-byte limit). A Get of inputs and outputs reads
 * the inputs only, split 3 + 2, the entries in any order; a Set carries
 * the masked lines only, split 3 + 1. A failed second message keeps the
 * values of the first. A reply that repeats a line or names one not asked
 * for is -EPROTO, and the next Get is matched afresh.
 */
static void obmf_test_gpio_multiple(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "i0", 1 }, { 1, "i1", 1 }, { 2, "i2", 1 },
		{ 3, "i3", 1 }, { 4, "i4", 1 }, { 5, "o5", 0, 1 },
		{ 6, "o6", 0, 0 }, { 7, "o7", 0, 1 }, { 8, "o8", 0, 0 },
	};
	static const u8 get0[] = { OBMF_GPIO_GET_VALUES, 0x00, 0x00, 0x01,
				   0x00, 0x02, 0x00 };
	static const u8 get1[] = { OBMF_GPIO_GET_VALUES, 0x03, 0x00, 0x04,
				   0x00 };
	static const u8 set0[] = { OBMF_GPIO_SET_VALUES, 0x05, 0x00, 0x06,
				   0x10, 0x07, 0x00 };
	static const u8 set1[] = { OBMF_GPIO_SET_VALUES, 0x08, 0x10 };
	static const u8 set2[] = { OBMF_GPIO_SET_VALUES, 0x05, 0x10, 0x07,
				   0x10 };
	struct obmf_fake *f = gt_create(test, 11, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	unsigned long mask, bits;
	const u8 *p;
	size_t len;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_ASSERT_EQ(test, chan(f, CH_GPIO_ONLY)->info.quirks.gpio_per_get,
			3);

	gt_reply(f, 0, (const u16[]){ 0x1002, 0x0000, 0x1001 }, 3);
	gt_reply(f, 0, (const u16[]){ 0x1004, 0x0003 }, 2);
	mask = 0x7f;
	bits = BIT(7);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), 0);
	KUNIT_EXPECT_EQ(test, bits, BIT(7) | BIT(5) | BIT(4) | BIT(2) | BIT(1));
	KUNIT_ASSERT_EQ(test, gt_sends(f), 2);
	p = gt_sent(f, 0, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(get0));
	KUNIT_EXPECT_MEMEQ(test, p, get0, sizeof(get0));
	p = gt_sent(f, 1, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(get1));
	KUNIT_EXPECT_MEMEQ(test, p, get1, sizeof(get1));

	gt_reply(f, 0, NULL, 0);
	gt_reply(f, 0, NULL, 0);
	mask = 0x1e0;
	bits = BIT(8) | BIT(6);
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), 0);
	KUNIT_ASSERT_EQ(test, gt_sends(f), 4);
	p = gt_sent(f, 2, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(set0));
	KUNIT_EXPECT_MEMEQ(test, p, set0, sizeof(set0));
	p = gt_sent(f, 3, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(set1));
	KUNIT_EXPECT_MEMEQ(test, p, set1, sizeof(set1));

	gt_reply(f, 0, NULL, 0);
	mask = BIT(7) | BIT(5);
	bits = BIT(7) | BIT(5);
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), 0);
	KUNIT_ASSERT_EQ(test, gt_sends(f), 5);
	p = gt_sent(f, 4, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(set2));
	KUNIT_EXPECT_MEMEQ(test, p, set2, sizeof(set2));

	gt_reply(f, 0, NULL, 0);
	gt_reply(f, OBMF_STATUS_GPIO_INVALID_OPERATION, NULL, 0);
	mask = 0x1e0;
	bits = 0;
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), -EPERM);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 7);
	bits = 0;
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), 0);
	KUNIT_EXPECT_EQ(test, bits, BIT(8));

	mask = BIT(5) | BIT(0);
	KUNIT_EXPECT_EQ(test, gt_set_multiple(test, gc, &mask, &bits), -EPERM);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 7);

	mask = 0x3;
	gt_reply(f, 0, (const u16[]){ 0x0000, 0x0000 }, 2);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), -EPROTO);
	gt_reply(f, 0, (const u16[]){ 0x0000, 0x0002 }, 2);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), -EPROTO);
	gt_reply(f, 0, (const u16[]){ 0x0001, 0x1000 }, 2);
	bits = 0;
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), 0);
	KUNIT_EXPECT_EQ(test, bits, BIT(0));
	KUNIT_EXPECT_EQ(test, gt_sends(f), 10);
}

/* Bind @f; the GPIO driver must not take the channel */
static void gt_refused(struct kunit *test, struct obmf_fake *f,
		       const char *what)
{
	KUNIT_EXPECT_NULL_MSG(test, gt_bind(f), "%s", what);
	KUNIT_EXPECT_NULL_MSG(test, chan(f, CH_GPIO_ONLY)->fdev->adev.dev.driver,
			      "%s", what);
	KUNIT_EXPECT_EQ_MSG(test, gt_sends(f), 0, "%s", what);
}

/*
 * Table 76: a configuration shorter than GPIO_COUNT, a count above 4096
 * or longer than the data, an index with reserved bits or listed twice,
 * no line, or no line in range refuses the channel; so does a capacity
 * of no line. Expected: "configuration data too short", "GPIO_COUNT 4097
 * above 4096", "configuration data too short for 2 lines", "GPIO_INDEX
 * 0x1001 out of range", "GPIO_INDEX 1 listed twice", "1 lines out of
 * range, left out", "channel limits 1/1 too small".
 */
static void obmf_test_gpio_config(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "a", 1 },
		{ 1, "b", 0 },
	};
	static const struct gt_line twice[] = {
		{ 0, "a", 1 },
		{ 1, "b", 0 },
		{ 1, "c", 1 },
	};
	static const struct gt_line range[] = {
		{ 0, "a", 2 },
	};
	struct obmf_fake *f;
	u8 *cfg;

	f = gt_create(test, 516, l, 0);
	gt_cfg(f, 0);
	gt_refused(test, f, "config_size 0");

	f = gt_create(test, 516, l, 0);
	gt_cfg(f, 1);
	gt_refused(test, f, "config_size 1");

	f = gt_create(test, 516, l, 0);
	gt_refused(test, f, "GPIO_COUNT 0");

	f = gt_create(test, 516, l, ARRAY_SIZE(l));
	put_unaligned_le16(OBMF_GPIO_LINES + 1, gt_cfg(f, 2 + 2 * 50));
	gt_refused(test, f, "GPIO_COUNT 4097");

	f = gt_create(test, 516, l, ARRAY_SIZE(l));
	gt_cfg(f, 2 + 50 + 49);
	gt_refused(test, f, "short table");

	f = gt_create(test, 516, l, ARRAY_SIZE(l));
	cfg = gt_cfg(f, 2 + 2 * 50);
	put_unaligned_le16(0x1001, cfg + 2 + 50);
	gt_refused(test, f, "reserved index bits");

	f = gt_create(test, 516, twice, ARRAY_SIZE(twice));
	gt_refused(test, f, "index listed twice");

	f = gt_create(test, 516, range, ARRAY_SIZE(range));
	gt_refused(test, f, "no line in range");

	f = gt_create(test, 5, l, ARRAY_SIZE(l));
	gt_refused(test, f, "capacity 0");
	KUNIT_EXPECT_EQ(test, chan(f, CH_GPIO_ONLY)->info.quirks.gpio_per_get,
			0);
}

/* A Get run from a work item, so the test can unbind while it waits */
struct gt_get {
	struct work_struct work;
	struct gpio_desc *desc;
	int ret;
	struct completion done;
};

static void gt_get_work(struct work_struct *w)
{
	struct gt_get *g = container_of(w, struct gt_get, work);

	g->ret = gpiod_get_value_cansleep(g->desc);
	complete(&g->done);
}

static void gt_get_flush(void *data)
{
	struct gt_get *g = data;

	flush_work(&g->work);
}

static struct gt_get *gt_get_start(struct obmf_fake *f, struct gpio_desc *d)
{
	struct kunit *test = f->test;
	struct gt_get *g;

	g = kunit_kzalloc(test, sizeof(*g), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, g);
	g->desc = d;
	init_completion(&g->done);
	INIT_WORK(&g->work, gt_get_work);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, gt_get_flush, g),
			0);
	queue_work(f->callwq, &g->work);
	return g;
}

/*
 * Unbind with a line held and a Get in flight: the Get ends when it times
 * out; the held line then reads -ENODEV with nothing sent, and is freed
 * after the chip is gone
 */
static void obmf_test_gpio_unbind(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in", 1 },
		{ 1, "out", 0, 1 },
	};
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct gpio_desc *in;
	unsigned int sends;
	struct gt_get *g;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	in = gt_desc(test, gc, 0, GPIOD_ASIS);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in));

	fake_push(f, &(struct fake_step){ .how = FAKE_NEVER, .channel = -1 });
	sends = f->sends;
	g = gt_get_start(f, in);
	KUNIT_ASSERT_TRUE(test, fake_wait_sends(f, sends + 1));
	fake_unbind(f);
	KUNIT_ASSERT_TRUE(test, test_wait(&g->done));
	KUNIT_EXPECT_EQ(test, g->ret, -ETIMEDOUT);

	sends = gt_sends(f);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(in), -ENODEV);
	KUNIT_EXPECT_EQ(test, gt_sends(f), sends);
	kunit_release_action(test, gt_free_desc, in);
}

/*
 * The chip label keeps the channel name: a 32-byte device name is cut so
 * that "<device name>_gpio" fits the 31 bytes the GPIO uAPI reports
 */
static void obmf_test_gpio_label(struct kunit *test)
{
	static const struct gt_line l[] = { { 0, "in", 1 } };
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc;

	memcpy(f->mem + 6, "0123456789abcdef0123456789abcdef", 32);
	gc = gt_bind(f);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_EXPECT_STREQ(test, gc->label, "0123456789abcdef0123456789_gpio");
	KUNIT_EXPECT_EQ(test, gt_sends(f), 0);
}

static int gt_answers(struct obmf_fake *f)
{
	return fake_count(f, FAKE_SEND, CH_GPIO_ONLY, 1);
}

/* A consumer's interrupt of a line; with @f, the answers sent at its call */
struct gt_irq {
	atomic_t calls;
	int irq;
	struct obmf_fake *f;
	int answers;
};

static irqreturn_t gt_irq_hard(int irq, void *data)
{
	return IRQ_HANDLED;
}

static irqreturn_t gt_irq_thread(int irq, void *data)
{
	struct gt_irq *g = data;

	if (g->f)
		g->answers = gt_answers(g->f);
	atomic_inc(&g->calls);
	return IRQ_HANDLED;
}

static void gt_free_irq(void *data)
{
	struct gt_irq *g = data;

	free_irq(g->irq, g);
}

/* Request the threaded interrupt of @d with the trigger in @flags */
static int gt_irq_request(struct kunit *test, struct gpio_desc *d,
			  unsigned long flags, struct gt_irq **gp)
{
	struct gt_irq *g = kunit_kzalloc(test, sizeof(*g), GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, g);
	*gp = g;
	g->irq = gpiod_to_irq(d);
	KUNIT_ASSERT_GT(test, g->irq, 0);
	ret = request_threaded_irq(g->irq, NULL, gt_irq_thread,
				   IRQF_ONESHOT | flags, "kunit", g);
	if (!ret)
		KUNIT_ASSERT_EQ(test,
				kunit_add_action_or_reset(test, gt_free_irq, g),
				0);
	return ret;
}

/* Inject GPIO command @cmd with @n entries and deliver it */
static void gt_dreq(struct obmf_fake *f, u8 cmd, const u16 *ent, int n)
{
	u8 req[1 + 2 * 4];
	int i;

	req[0] = cmd;
	for (i = 0; i < n; i++)
		put_unaligned_le16(ent[i], req + 1 + 2 * i);
	fake_inject(f, CH_GPIO_ONLY, 0, req, 1 + 2 * n);
	flush_workqueue(f->core->wq);
}

/* The last answer on channel 1: @status and the echo of @cmd */
static void gt_answer(struct obmf_fake *f, u8 status, u8 cmd, const char *what)
{
	struct fake_rec *r = last_answer(f, CH_GPIO_ONLY);

	KUNIT_EXPECT_EQ_MSG(f->test, r->msg[1], OBMF_HDR_RQRESP | status, "%s",
			    what);
	KUNIT_EXPECT_EQ_MSG(f->test, r->len, OBMF_HDR_LEN + 1, "%s", what);
	KUNIT_EXPECT_EQ_MSG(f->test, r->msg[4], cmd, "%s", what);
}

/*
 * Table 18: an input's edge interrupt is configured with Set Interrupt
 * Config only when the irq core syncs: rising when requested, both edges
 * on a new type, disable when an unlazy disable masks it, both again on
 * enable, disable when freed. gpiolib sees the line as an enabled
 * interrupt only while it is unmasked. A level type, an output line, a
 * hole or a request with no thread handler is refused with nothing sent.
 */
static void obmf_test_gpio_irq_request(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in0", 1 },
		{ 1, "out1", 0, 1 },
		{ 3, "in3", 1 },
	};
	static const u8 want[][3] = {
		{ OBMF_GPIO_SET_INTERRUPT_CONFIG, 0x00, 0x30 },
		{ OBMF_GPIO_SET_INTERRUPT_CONFIG, 0x00, 0x50 },
		{ OBMF_GPIO_SET_INTERRUPT_CONFIG, 0x00, 0x00 },
		{ OBMF_GPIO_SET_INTERRUPT_CONFIG, 0x00, 0x50 },
		{ OBMF_GPIO_SET_INTERRUPT_CONFIG, 0x03, 0x40 },
		{ OBMF_GPIO_SET_INTERRUPT_CONFIG, 0x00, 0x00 },
	};
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct gpio_desc *in0, *out1, *in3;
	struct gt_irq *a, *b;
	const u8 *p;
	size_t len;
	int i, irq, ret;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	f->dflt.plen = 0;
	in0 = gt_desc(test, gc, 0, GPIOD_IN);
	out1 = gt_desc(test, gc, 1, GPIOD_ASIS);
	in3 = gt_desc(test, gc, 3, GPIOD_IN);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in0));
	KUNIT_ASSERT_FALSE(test, IS_ERR(out1));
	KUNIT_ASSERT_FALSE(test, IS_ERR(in3));

	KUNIT_EXPECT_EQ(test, gc->to_irq(gc, 2), -ENXIO);
	KUNIT_EXPECT_EQ(test,
			gt_irq_request(test, out1, IRQF_TRIGGER_RISING, &b),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, gt_irq_request(test, in3, IRQF_TRIGGER_HIGH, &b),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, gt_irq_request(test, in3, IRQF_TRIGGER_LOW, &b),
			-EINVAL);
	irq = gpiod_to_irq(in3);
	KUNIT_ASSERT_GT(test, irq, 0);
	ret = request_irq(irq, gt_irq_hard, IRQF_TRIGGER_RISING, "kunit", NULL);
	if (!ret)
		free_irq(irq, NULL);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 0);

	KUNIT_ASSERT_EQ(test,
			gt_irq_request(test, in0, IRQF_TRIGGER_RISING, &a), 0);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 1);
	KUNIT_EXPECT_EQ(test, gpiod_direction_output(in0, 0), -EIO);
	KUNIT_EXPECT_EQ(test, irq_set_irq_type(a->irq, IRQ_TYPE_EDGE_BOTH), 0);
	KUNIT_EXPECT_EQ(test, irq_set_irq_type(a->irq, IRQ_TYPE_LEVEL_HIGH),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 2);
	irq_set_status_flags(a->irq, IRQ_DISABLE_UNLAZY);
	disable_irq(a->irq);
	KUNIT_EXPECT_EQ(test, gpiod_direction_output(in0, 0), -EINVAL);
	enable_irq(a->irq);
	KUNIT_EXPECT_EQ(test, gpiod_direction_output(in0, 0), -EIO);
	KUNIT_ASSERT_EQ(test,
			gt_irq_request(test, in3, IRQF_TRIGGER_FALLING, &b), 0);
	kunit_release_action(test, gt_free_irq, a);
	KUNIT_EXPECT_EQ(test, gpiod_direction_output(in0, 0), -EINVAL);

	KUNIT_ASSERT_EQ(test, gt_sends(f), ARRAY_SIZE(want));
	for (i = 0; i < ARRAY_SIZE(want); i++) {
		p = gt_sent(f, i, &len);
		KUNIT_ASSERT_EQ(test, len, sizeof(want[i]));
		KUNIT_EXPECT_MEMEQ_MSG(test, p, want[i], sizeof(want[i]),
				       "send %d", i);
	}
}

/*
 * Table 18: an Interrupt Notify is answered success before it runs the
 * handler of each line it lists, the last line (n - 1) too; a Datagram
 * runs them with no answer. A line at or past the chip (n, 64: the word
 * after the chip's bitmap, so that a missing bound is seen under KASAN,
 * 4095), a hole, data not 0, an odd length or no entry makes the request
 * malformed: answered invalid, a Datagram not at all, and none of its
 * lines handled. A line with no interrupt requested, or one freed, is
 * counted in gpio_irq_unused. Another command gets the core's answer.
 */
static void obmf_test_gpio_irq_notify(struct kunit *test)
{
	static const struct gt_line l[] = {
		{ 0, "in0", 1 },
		{ 1, "out1", 0, 1 },
		{ 3, "in3", 1 },
	};
	static const struct {
		const char *what;
		u8 len;
		u8 req[5];
	} bad[] = {
		{ "line n", 3, { OBMF_GPIO_INTERRUPT_NOTIFY, 0x04, 0x00 } },
		{ "line 64", 3, { OBMF_GPIO_INTERRUPT_NOTIFY, 0x40, 0x00 } },
		{ "line 4095", 3, { OBMF_GPIO_INTERRUPT_NOTIFY, 0xff, 0x0f } },
		{ "hole", 3, { OBMF_GPIO_INTERRUPT_NOTIFY, 0x02, 0x00 } },
		{ "data 1", 3, { OBMF_GPIO_INTERRUPT_NOTIFY, 0x00, 0x10 } },
		{ "line 0, line n",
		  5,
		  { OBMF_GPIO_INTERRUPT_NOTIFY, 0x00, 0x00, 0x04, 0x00 } },
		{ "line n, line 0",
		  5,
		  { OBMF_GPIO_INTERRUPT_NOTIFY, 0x04, 0x00, 0x00, 0x00 } },
		{ "odd length", 4, { OBMF_GPIO_INTERRUPT_NOTIFY, 0x00, 0x00 } },
		{ "no entry", 1, { OBMF_GPIO_INTERRUPT_NOTIFY } },
	};
	static const u8 dgram[] = { OBMF_GPIO_INTERRUPT_NOTIFY_DATAGRAM, 0x04,
				    0x00 };
	static const u8 other[] = { OBMF_GPIO_SET_INTERRUPT_CONFIG, 0x00,
				    0x30 };
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct obmf_channel *ch = chan(f, CH_GPIO_ONLY);
	struct gpio_desc *in0, *in3;
	struct gt_irq *a, *b;
	long bad0;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	f->dflt.plen = 0;
	in0 = gt_desc(test, gc, 0, GPIOD_IN);
	in3 = gt_desc(test, gc, 3, GPIOD_IN);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in0));
	KUNIT_ASSERT_FALSE(test, IS_ERR(in3));
	KUNIT_ASSERT_EQ(test,
			gt_irq_request(test, in0, IRQF_TRIGGER_RISING, &a), 0);
	KUNIT_ASSERT_EQ(test,
			gt_irq_request(test, in3, IRQF_TRIGGER_RISING, &b), 0);
	b->f = f;

	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0003 }, 1);
	gt_answer(f, OBMF_STATUS_SUCCESS, OBMF_GPIO_INTERRUPT_NOTIFY, "n - 1");
	KUNIT_EXPECT_EQ(test, atomic_read(&b->calls), 1);
	KUNIT_EXPECT_EQ(test, b->answers, 1);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0000, 0x0003 },
		2);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&b->calls), 2);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY_DATAGRAM, (const u16[]){ 0x0000 },
		1);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 2);
	KUNIT_EXPECT_EQ(test, gt_answers(f), 2);

	bad0 = if_cnt(f->core, OBMF_IF_RX_MALFORMED);
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		fake_inject(f, CH_GPIO_ONLY, 0, bad[i].req, bad[i].len);
		flush_workqueue(f->core->wq);
		KUNIT_EXPECT_EQ_MSG(test, gt_answers(f), 3 + i, "%s",
				    bad[i].what);
		gt_answer(f, OBMF_STATUS_INVALID, OBMF_GPIO_INTERRUPT_NOTIFY,
			  bad[i].what);
	}
	fake_inject(f, CH_GPIO_ONLY, 0, dgram, sizeof(dgram));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, gt_answers(f), 2 + ARRAY_SIZE(bad));
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_RX_MALFORMED),
			bad0 + ARRAY_SIZE(bad) + 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_BAD_DREQ),
			ARRAY_SIZE(bad) + 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 2);
	KUNIT_EXPECT_EQ(test, atomic_read(&b->calls), 2);

	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0001 }, 1);
	gt_answer(f, OBMF_STATUS_SUCCESS, OBMF_GPIO_INTERRUPT_NOTIFY, "output");
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ_UNUSED), 1);
	kunit_release_action(test, gt_free_irq, b);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0003 }, 1);
	gt_answer(f, OBMF_STATUS_SUCCESS, OBMF_GPIO_INTERRUPT_NOTIFY, "freed");
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ_UNUSED), 2);
	KUNIT_EXPECT_EQ(test, atomic_read(&b->calls), 2);

	fake_inject(f, CH_GPIO_ONLY, 0, other, sizeof(other));
	flush_workqueue(f->core->wq);
	gt_answer(f, OBMF_STATUS_INVALID, OBMF_GPIO_SET_INTERRUPT_CONFIG,
		  "other");
	KUNIT_EXPECT_EQ(test, ch_cnt(ch, OBMF_CH_DEFAULT_ANSWERS), 1);
	KUNIT_EXPECT_EQ(test, if_cnt(f->core, OBMF_IF_RX_MALFORMED),
			bad0 + ARRAY_SIZE(bad) + 1);
}

/*
 * A Set Interrupt Config the device refuses, here busy, is an event with
 * the line and the errno; the request still succeeds, but the line is not
 * armed and its edge is counted unused. The next sync sends it again. One
 * that finds the device gone (-ENODEV) is no event and keeps the trigger
 * the device has.
 */
static void obmf_test_gpio_irq_failed(struct kunit *test)
{
	static const struct gt_line l[] = { { 0, "in0", 1 } };
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct fake_step gone = { .send_ret = -ENODEV };
	struct gpio_desc *in0;
	struct er_ctx *x;
	struct gt_irq *a;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	f->dflt.plen = 0;
	test->priv = kunit_kzalloc(test, sizeof(*x), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, test->priv);
	x = er_start(test);
	in0 = gt_desc(test, gc, 0, GPIOD_IN);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in0));

	gt_reply(f, OBMF_STATUS_BUSY, NULL, 0);
	KUNIT_ASSERT_EQ(test,
			gt_irq_request(test, in0, IRQF_TRIGGER_RISING, &a), 0);
	KUNIT_ASSERT_EQ(test, x->n, 1);
	KUNIT_EXPECT_EQ(test, x->rec[0].ev, OBMF_EV_GPIO_IRQ);
	KUNIT_EXPECT_EQ(test, x->rec[0].ch, CH_GPIO_ONLY);
	KUNIT_EXPECT_EQ(test, x->rec[0].a, 0);
	KUNIT_EXPECT_EQ(test, x->rec[0].b, -EBUSY);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ), 1);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0000 }, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 0);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ_UNUSED), 1);

	KUNIT_EXPECT_EQ(test, irq_set_irq_type(a->irq, IRQ_TYPE_EDGE_RISING),
			0);
	KUNIT_EXPECT_EQ(test, gt_sends(f), 2);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0000 }, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 1);

	fake_push(f, &gone);
	KUNIT_EXPECT_EQ(test, irq_set_irq_type(a->irq, IRQ_TYPE_EDGE_BOTH), 0);
	KUNIT_EXPECT_EQ(test, x->n, 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ), 1);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0000 }, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 2);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ_UNUSED), 1);
}

/*
 * Unbind with an interrupt requested: the device requests stop first, the
 * chip frees the interrupt as it goes, and the consumer's free_irq()
 * afterwards finds nothing left
 */
static void obmf_test_gpio_irq_unbind(struct kunit *test)
{
	static const struct gt_line l[] = { { 0, "in0", 1 } };
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct gpio_desc *in0;
	struct gt_irq *a;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	f->dflt.plen = 0;
	in0 = gt_desc(test, gc, 0, GPIOD_IN);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in0));
	KUNIT_ASSERT_EQ(test,
			gt_irq_request(test, in0, IRQF_TRIGGER_RISING, &a), 0);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0000 }, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 1);

	fake_unbind(f);
	KUNIT_EXPECT_NULL(test, irq_get_irq_data(a->irq));
	kunit_release_action(test, gt_free_irq, a);
	kunit_release_action(test, gt_free_desc, in0);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 1);
}

/*
 * A USB reset or a reset-resume unbinds the interface and binds it again,
 * here with the device gone for the disable sent as the chip goes. The
 * new chip has nothing armed: an edge is counted unused until the
 * consumer requests the line again, which sends Set Interrupt Config with
 * the trigger again; a Notify then runs the new handler only.
 */
static void obmf_test_gpio_irq_rebind(struct kunit *test)
{
	static const struct gt_line l[] = { { 0, "in0", 1 } };
	static const u8 rising[] = { OBMF_GPIO_SET_INTERRUPT_CONFIG, 0x00,
				     0x30 };
	struct obmf_fake *f = gt_create(test, 516, l, ARRAY_SIZE(l));
	struct gpio_chip *gc = gt_bind(f);
	struct gpio_desc *in0;
	struct gt_irq *a, *b;
	const u8 *p;
	size_t len;
	int sends;

	KUNIT_ASSERT_NOT_NULL(test, gc);
	f->dflt.plen = 0;
	in0 = gt_desc(test, gc, 0, GPIOD_IN);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in0));
	KUNIT_ASSERT_EQ(test,
			gt_irq_request(test, in0, IRQF_TRIGGER_RISING, &a), 0);
	KUNIT_ASSERT_EQ(test, gt_sends(f), 1);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0000 }, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 1);

	f->dflt.send_ret = -ENODEV;
	fake_unbind(f);
	f->dflt.send_ret = 0;
	kunit_release_action(test, gt_free_irq, a);
	kunit_release_action(test, gt_free_desc, in0);

	gc = gt_bind(f);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	sends = gt_sends(f);
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0000 }, 1);
	gt_answer(f, OBMF_STATUS_SUCCESS, OBMF_GPIO_INTERRUPT_NOTIFY,
		  "not armed");
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ_UNUSED), 1);

	in0 = gt_desc(test, gc, 0, GPIOD_IN);
	KUNIT_ASSERT_FALSE(test, IS_ERR(in0));
	KUNIT_ASSERT_EQ(test,
			gt_irq_request(test, in0, IRQF_TRIGGER_RISING, &b), 0);
	KUNIT_ASSERT_EQ(test, gt_sends(f), sends + 1);
	p = gt_sent(f, sends, &len);
	KUNIT_ASSERT_EQ(test, len, sizeof(rising));
	KUNIT_EXPECT_MEMEQ(test, p, rising, sizeof(rising));
	gt_dreq(f, OBMF_GPIO_INTERRUPT_NOTIFY, (const u16[]){ 0x0000 }, 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&b->calls), 1);
	KUNIT_EXPECT_EQ(test, atomic_read(&a->calls), 1);
	KUNIT_EXPECT_EQ(test, ev_cnt(f->core, OBMF_EV_GPIO_IRQ_UNUSED), 1);
}

static struct kunit_case obmf_gpio_cases[] = {
	KUNIT_CASE(obmf_test_gpio_lines),
	KUNIT_CASE(obmf_test_gpio_get_set),
	KUNIT_CASE(obmf_test_gpio_refused),
	KUNIT_CASE(obmf_test_gpio_timeout_status),
	KUNIT_CASE(obmf_test_gpio_timeout_report),
	KUNIT_CASE(obmf_test_gpio_set_report),
	KUNIT_CASE(obmf_test_gpio_failed_get),
	KUNIT_CASE(obmf_test_gpio_bad_reply),
	KUNIT_CASE(obmf_test_gpio_direction),
	KUNIT_CASE(obmf_test_gpio_multiple),
	KUNIT_CASE(obmf_test_gpio_config),
	KUNIT_CASE(obmf_test_gpio_unbind),
	KUNIT_CASE(obmf_test_gpio_label),
	KUNIT_CASE(obmf_test_gpio_irq_request),
	KUNIT_CASE(obmf_test_gpio_irq_notify),
	KUNIT_CASE(obmf_test_gpio_irq_failed),
	KUNIT_CASE(obmf_test_gpio_irq_unbind),
	KUNIT_CASE(obmf_test_gpio_irq_rebind),
	{}
};

static struct kunit_suite obmf_gpio_suite = {
	.name = "obmf-gpio",
	.suite_init = obmf_test_fn_suite_init,
	.suite_exit = obmf_test_fn_suite_exit,
	.test_cases = obmf_gpio_cases,
};

kunit_test_suite(obmf_gpio_suite);

#endif /* IS_BUILTIN(CONFIG_GPIO_OBMF) */
