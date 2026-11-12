// SPDX-License-Identifier: GPL-2.0
/*
 * Serial port on an OBMF-ICP serial channel
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kfifo.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/serial.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/tty_port.h>
#include <linux/unaligned.h>
#include <linux/usb/obmf.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#define OBMF_TTY_MINORS 256

/* OBMF-ICP 1.0.0, 5.5.1, Table 22: Operation/Event */
#define OBMF_TTY_EV_GENERATE_BREAK BIT(0)
#define OBMF_TTY_EV_BREAK BIT(1)
#define OBMF_TTY_EV_OVERRUN BIT(2)
#define OBMF_TTY_EV_CARRIER_DOWN BIT(3)
/* Operation/Event and Character Count */
#define OBMF_TTY_HEAD 3

/* OBMF-ICP 1.0.0, 5.5.2, Table 23 */
#define OBMF_TTY_RESP_NACK BIT(0)
#define OBMF_TTY_RESP_LEN 3

/* OBMF-ICP 1.0.0, 5.15.2, Table 78 */
struct obmf_tty_cfg {
	__le32 speed_hz;
	u8 data_bits;
	u8 stop_bits;
	u8 line_control;
} __packed;

#define OBMF_TTY_PARITY GENMASK(3, 0)
#define OBMF_TTY_FLOW GENMASK(7, 4)
#define OBMF_TTY_RTS_CTS 2

#define OBMF_TTY_FIFO_SIZE 4096
/* NACKs before a break fails */
#define OBMF_TTY_NACK_RETRIES 20
/* Between tries of a request the device refused or did not answer */
#define OBMF_TTY_RETRY_DELAY_MS 10
/* How long the head of the FIFO stays refused before a warning */
#define OBMF_TTY_STALL_MS 5000

/**
 * struct obmf_tty - one serial channel
 * @port: the tty port; its reference count owns this structure
 * @fdev: the function device, NULL once it is being removed; read without
 *	@req_lock by .shutdown, which teardown's hangup runs before that
 * @tx_work: sends @fifo to the device, delayed while the device refuses
 * @req_lock: serialises host requests and @req; covers @fdev
 * @lock: covers @fifo, the tx_ members and the counters
 * @fifo: bytes written and not yet accepted by the device
 * @req: request buffer, Character Count and Character Data
 * @max_chars: most characters in one request
 * @tx_inflight: bytes at the head of @fifo in the request being sent
 * @tx_flush: bytes at the head of @fifo discarded while a request was in
 *	flight; they leave @fifo when it is answered
 * @tx_flush_drop: what @fifo held when a close discarded it while a
 *	request was in flight; the answer drops and reports those of them
 *	the device does not accept
 * @tx_stalled: the head of @fifo has not moved since @tx_since
 * @tx_warned: the stall was reported
 * @tx_since: jiffies of the first try that kept the head
 * @tx_wait: how long a request waits for its answer beyond twice its
 *	time on the wire: OBMF_DEFAULT_TIMEOUT_MS at each open, doubled
 *	after each timeout up to OBMF_MAX_TIMEOUT_MS
 * @icount: line counters for TIOCGICOUNT
 * @tx_dropped: bytes dropped after an error or at close
 * @tx_stalls: stalls reported
 * @dbg: debugfs directory with @tx_dropped and @tx_stalls
 * @speed: UART_SPEED_HZ
 * @cflag: CSIZE, CSTOPB, PARENB, PARODD, CMSPAR and CRTSCTS from Table 78
 * @minor: tty index, U32_MAX until allocated
 * @gone: install and activate refuse the port
 */
struct obmf_tty {
	struct tty_port port;
	struct obmf_fdev *fdev;
	struct delayed_work tx_work;
	struct mutex req_lock; /* host requests, req, fdev */
	spinlock_t lock; /* fifo, tx_*, counters */
	DECLARE_KFIFO(fifo, u8, OBMF_TTY_FIFO_SIZE);
	u8 *req;
	size_t max_chars;
	size_t tx_inflight;
	size_t tx_flush;
	size_t tx_flush_drop;
	bool tx_stalled;
	bool tx_warned;
	unsigned long tx_since;
	unsigned int tx_wait;
	struct async_icount icount;
	unsigned long tx_dropped;
	unsigned long tx_stalls;
	struct dentry *dbg;
	u32 speed;
	tcflag_t cflag;
	u32 minor;
	bool gone;
};

static struct tty_driver *obmf_tty_driver;
static struct dentry *obmf_tty_dbg;
/* Covers the minor table and @gone for install and activate */
static DEFINE_MUTEX(obmf_tty_lock);
static DEFINE_XARRAY_ALLOC(obmf_tty_minors);

static void obmf_tty_put(struct obmf_tty *p)
{
	if (p)
		tty_port_put(&p->port);
}

DEFINE_FREE(obmf_tty, struct obmf_tty *, obmf_tty_put(_T))

/*
 * Send Operation/Event @ev with the first @n bytes of @req; *@done gets
 * the count the device accepted and *@nack its NACK bit.
 */
static int obmf_tty_send(struct obmf_tty *p, u8 ev, size_t n, size_t *done,
			 bool *nack)
{
	u8 resp[OBMF_TTY_RESP_LEN];
	unsigned int ms;
	size_t len;
	u16 acc;
	int ret;

	scoped_guard(spinlock_irqsave, &p->lock)
		ms = p->tx_wait;
	/* Twice the time on the wire, ten bits a character */
	ms += 2 * DIV_ROUND_UP(n * 10 * MSEC_PER_SEC, p->speed);
	put_unaligned_le16(n, p->req);
	ret = obmf_request(p->fdev, ev, p->req, 2 + n, resp, sizeof(resp), &len,
			   ms);
	if (ret == -ETIMEDOUT) {
		guard(spinlock_irqsave)
			(&p->lock);
		p->tx_wait = min(2 * p->tx_wait, OBMF_MAX_TIMEOUT_MS);
	}
	if (ret)
		return ret;
	if (len != sizeof(resp) || get_unaligned_le16(resp + 1) > n) {
		obmf_fdev_protocol_error(p->fdev);
		return -EPROTO;
	}
	acc = get_unaligned_le16(resp + 1);
	*nack = resp[0] & OBMF_TTY_RESP_NACK;
	*done = *nack ? acc : n;
	return 0;
}

enum { OBMF_TTY_TX_IDLE, OBMF_TTY_TX_MORE, OBMF_TTY_TX_RETRY };

/*
 * Under @lock, after a try that @kept the head of @fifo or not: whether
 * the head has now been kept for OBMF_TTY_STALL_MS, reported once a stall.
 * A discard ends the stall, also one during the try, whose answer then
 * drops the head.
 */
static bool obmf_tty_stalled(struct obmf_tty *p, bool kept)
{
	if (!kept || p->tx_flush) {
		p->tx_stalled = false;
		return false;
	}
	if (!p->tx_stalled) {
		p->tx_stalled = true;
		p->tx_warned = false;
		p->tx_since = jiffies;
		return false;
	}
	if (p->tx_warned ||
	    time_before(jiffies,
			p->tx_since + msecs_to_jiffies(OBMF_TTY_STALL_MS)))
		return false;
	p->tx_warned = true;
	p->tx_stalls++;
	return true;
}

static void obmf_tty_warn_stall(struct obmf_tty *p, int ret,
				unsigned int waiting)
{
	const char *why = "refused";

	if (ret == -EBUSY)
		why = "busy";
	else if (ret)
		why = "timed out";
	dev_warn_ratelimited(&p->fdev->adev.dev,
			     "transmit stalled: %s, %u characters waiting\n",
			     why, waiting);
}

static void obmf_tty_warn_close(struct obmf_tty *p, size_t n)
{
	dev_warn_ratelimited(&p->fdev->adev.dev,
			     "%zu characters not sent at close, discarded\n",
			     n);
}

/*
 * One request from the head of @fifo. Bytes leave @fifo when the device
 * accepts them. A NACK of none, busy (OBMF-ICP 1.0.0, 5.2, Table 4: 0x03)
 * or a timeout keeps them for another try: no answer, Table 4 0x02, or
 * the line timeout (5.5, Table 21: 0x40). Any other error drops them.
 * No request carries a tag: the core holds the next try until an answer
 * that comes after the timeout arrives, or for one default timeout, so
 * the device may get the bytes twice. Each timeout doubles the wait of
 * the requests after it, up to OBMF_MAX_TIMEOUT_MS, until the next open,
 * so a device that is slower than the wait gets one repeat, not one per
 * request. An answer later than the hold answers the next try, whose own
 * answer the request after it can take, so bytes can also be lost. A
 * head kept for OBMF_TTY_STALL_MS gets one warning, then none until the
 * device accepts it.
 */
static int obmf_tty_tx_one(struct obmf_tty *p)
{
	bool drop = false, stall, wake, nack;
	int next = OBMF_TTY_TX_MORE;
	size_t n, done = 0, closed;
	unsigned int waiting;
	int ret;

	guard(mutex)(&p->req_lock);
	if (!p->fdev)
		return OBMF_TTY_TX_IDLE;
	scoped_guard(spinlock_irqsave, &p->lock) {
		n = kfifo_out_peek(&p->fifo, p->req + 2, p->max_chars);
		p->tx_inflight = n;
	}
	if (!n)
		return OBMF_TTY_TX_IDLE;

	ret = obmf_tty_send(p, 0, n, &done, &nack);
	if (ret == -EAGAIN) {
		/* Quiesced core: obmf_tty_resume() sends the bytes */
		next = OBMF_TTY_TX_IDLE;
	} else if (ret == -ETIMEDOUT || ret == -EBUSY || (!ret && !done)) {
		next = OBMF_TTY_TX_RETRY;
	} else if (ret) {
		drop = true;
		done = n;
	}

	scoped_guard(spinlock_irqsave, &p->lock) {
		p->tx_inflight = 0;
		wake = done || p->tx_flush;
		stall = obmf_tty_stalled(p, next == OBMF_TTY_TX_RETRY);
		closed = p->tx_flush_drop ? p->tx_flush_drop - done : 0;
		p->tx_dropped += closed;
		kfifo_skip_count(&p->fifo, p->tx_flush ?: done);
		p->tx_flush = 0;
		p->tx_flush_drop = 0;
		if (drop)
			p->tx_dropped += done;
		else
			p->icount.tx += done;
		waiting = kfifo_len(&p->fifo);
	}
	/* -ENODEV: the device is gone and every write fails */
	if (drop && ret != -ENODEV)
		obmf_fdev_report(p->fdev, OBMF_EV_TX_DROPPED, done, ret);
	if (stall)
		obmf_tty_warn_stall(p, ret, waiting);
	if (closed)
		obmf_tty_warn_close(p, closed);
	if (wake)
		tty_port_tty_wakeup(&p->port);
	return next;
}

static void obmf_tty_tx_work(struct work_struct *work)
{
	struct obmf_tty *p =
		container_of(to_delayed_work(work), struct obmf_tty, tx_work);
	int next;

	do {
		next = obmf_tty_tx_one(p);
	} while (next == OBMF_TTY_TX_MORE);
	/* A write during the try queued this work with no delay */
	if (next == OBMF_TTY_TX_RETRY)
		mod_delayed_work(system_dfl_wq, &p->tx_work,
				 msecs_to_jiffies(OBMF_TTY_RETRY_DELAY_MS));
}

static ssize_t obmf_tty_write(struct tty_struct *tty, const u8 *buf,
			      size_t count)
{
	struct obmf_tty *p = tty->driver_data;
	unsigned int n;

	scoped_guard(spinlock_irqsave, &p->lock)
		n = kfifo_in(&p->fifo, buf, count);
	queue_delayed_work(system_dfl_wq, &p->tx_work, 0);
	return n;
}

static unsigned int obmf_tty_write_room(struct tty_struct *tty)
{
	struct obmf_tty *p = tty->driver_data;

	guard(spinlock_irqsave)(&p->lock);
	return kfifo_avail(&p->fifo);
}

/* The bytes in flight stay in @fifo until the device accepts them */
static unsigned int obmf_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct obmf_tty *p = tty->driver_data;

	guard(spinlock_irqsave)(&p->lock);
	return kfifo_len(&p->fifo);
}

/*
 * Drops the bytes not sent; with a request in flight, the bytes @fifo
 * holds now once it is answered, so those written later stay. A close
 * (@drop) counts what it drops in tx_dropped and returns the count; if
 * it is the first discard while a request is in flight, the answer does
 * both instead.
 */
static unsigned int obmf_tty_discard(struct obmf_tty *p, bool drop)
{
	unsigned int len, n;

	guard(spinlock_irqsave)(&p->lock);
	len = kfifo_len(&p->fifo);
	p->tx_stalled = false;
	if (!p->tx_inflight) {
		n = len;
		kfifo_reset_out(&p->fifo);
	} else if (p->tx_flush) {
		n = len - p->tx_flush;
		p->tx_flush = len;
	} else {
		p->tx_flush = len;
		if (drop)
			p->tx_flush_drop = len;
		return 0;
	}
	if (drop)
		p->tx_dropped += n;
	return n;
}

static void obmf_tty_flush_buffer(struct tty_struct *tty)
{
	obmf_tty_discard(tty->driver_data, false);
	tty_wakeup(tty);
}

/*
 * One Generate Break per "on"; the device sets its length. A NACKed
 * break is sent again, as written bytes are.
 */
static int obmf_tty_break_ctl(struct tty_struct *tty, int state)
{
	struct obmf_tty *p = tty->driver_data;
	unsigned int tries;
	size_t done;
	bool nack;
	int ret;

	if (!state)
		return 0;
	for (tries = 0; tries < OBMF_TTY_NACK_RETRIES; tries++) {
		if (tries)
			msleep(OBMF_TTY_RETRY_DELAY_MS);
		scoped_guard(mutex, &p->req_lock) {
			if (!p->fdev)
				return -ENODEV;
			ret = obmf_tty_send(p, OBMF_TTY_EV_GENERATE_BREAK, 0,
					    &done, &nack);
		}
		if (ret || !nack)
			return ret;
	}
	return -EIO;
}

/* The line runs as configured; CLOCAL, CREAD and HUPCL are the user's */
static void obmf_tty_set_hw(struct obmf_tty *p, struct tty_struct *tty)
{
	tty->termios.c_cflag &=
		~(CSIZE | CSTOPB | PARENB | PARODD | CMSPAR | CRTSCTS);
	tty->termios.c_cflag |= p->cflag;
	tty_encode_baud_rate(tty, p->speed, p->speed);
}

static void obmf_tty_set_termios(struct tty_struct *tty,
				 const struct ktermios *old)
{
	obmf_tty_set_hw(tty->driver_data, tty);
}

static int obmf_tty_install(struct tty_driver *driver, struct tty_struct *tty)
{
	struct obmf_tty *p;
	int ret;

	scoped_guard(mutex, &obmf_tty_lock) {
		p = xa_load(&obmf_tty_minors, tty->index);
		if (!p || p->gone || !tty_port_get(&p->port))
			return -ENODEV;
	}

	ret = tty_standard_install(driver, tty);
	if (ret) {
		tty_port_put(&p->port);
		return ret;
	}
	tty->driver_data = p;
	obmf_tty_set_hw(p, tty);
	return 0;
}

static void obmf_tty_cleanup(struct tty_struct *tty)
{
	struct obmf_tty *p = tty->driver_data;

	tty_port_put(&p->port);
}

static int obmf_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct obmf_tty *p = tty->driver_data;

	return tty_port_open(&p->port, tty, filp);
}

static void obmf_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct obmf_tty *p = tty->driver_data;

	tty_port_close(&p->port, tty, filp);
}

static void obmf_tty_hangup(struct tty_struct *tty)
{
	struct obmf_tty *p = tty->driver_data;

	tty_port_hangup(&p->port);
}

static int obmf_tty_get_icount(struct tty_struct *tty,
			       struct serial_icounter_struct *icount)
{
	struct obmf_tty *p = tty->driver_data;

	guard(spinlock_irqsave)(&p->lock);
	icount->rx = p->icount.rx;
	icount->tx = p->icount.tx;
	icount->brk = p->icount.brk;
	icount->overrun = p->icount.overrun;
	icount->dcd = p->icount.dcd;
	icount->buf_overrun = p->icount.buf_overrun;
	return 0;
}

#if IS_ENABLED(CONFIG_OBMF_KUNIT_TEST)
/* For the KUnit cases: the counters get_icount() does not report */
void obmf_tty_kunit_counts(struct tty_struct *tty, unsigned long *dropped,
			   unsigned long *stalls)
{
	struct obmf_tty *p = tty->driver_data;

	guard(spinlock_irqsave)(&p->lock);
	*dropped = p->tx_dropped;
	*stalls = p->tx_stalls;
}
#endif

static const struct tty_operations obmf_tty_ops = {
	.install = obmf_tty_install,
	.cleanup = obmf_tty_cleanup,
	.open = obmf_tty_open,
	.close = obmf_tty_close,
	.hangup = obmf_tty_hangup,
	.write = obmf_tty_write,
	.write_room = obmf_tty_write_room,
	.chars_in_buffer = obmf_tty_chars_in_buffer,
	.flush_buffer = obmf_tty_flush_buffer,
	.set_termios = obmf_tty_set_termios,
	.break_ctl = obmf_tty_break_ctl,
	.get_icount = obmf_tty_get_icount,
};

/* Frees the minor last, so a reused minor has no tty left */
static void obmf_tty_destruct(struct tty_port *port)
{
	struct obmf_tty *p = container_of(port, struct obmf_tty, port);

	if (p->minor != U32_MAX) {
		guard(mutex)(&obmf_tty_lock);
		xa_erase(&obmf_tty_minors, p->minor);
	}
	mutex_destroy(&p->req_lock);
	kfree(p);
}

/*
 * port->tty is set by now, so a teardown that has not set @gone yet will
 * hang this tty up; one that has may have found no tty to hang up. So
 * will a transport error that stops the device later; after one, every
 * request fails, and the open is refused.
 */
static int obmf_tty_activate(struct tty_port *port, struct tty_struct *tty)
{
	struct obmf_tty *p = container_of(port, struct obmf_tty, port);

	guard(mutex)
		(&obmf_tty_lock);
	if (p->gone)
		return -ENODEV;
	/* @fdev is set until @gone */
	if (obmf_fdev_errno(p->fdev) == -EIO)
		return -EIO;
	scoped_guard(spinlock_irqsave, &p->lock)
		p->tx_wait = OBMF_DEFAULT_TIMEOUT_MS;
	return 0;
}

/*
 * Hangup, or close once closing_wait is over: the bytes not sent go and
 * count as dropped. A hangup has flushed them already. With a request in
 * flight, its answer reports them, as the device may still take some.
 */
static void obmf_tty_shutdown(struct tty_port *port)
{
	struct obmf_tty *p = container_of(port, struct obmf_tty, port);
	unsigned int n = obmf_tty_discard(p, true);

	if (n)
		obmf_tty_warn_close(p, n);
}

static const struct tty_port_operations obmf_tty_port_ops = {
	.activate = obmf_tty_activate,
	.shutdown = obmf_tty_shutdown,
	.destruct = obmf_tty_destruct,
};

/*
 * A device write is taken whole or refused: NACKed with count 0 and not
 * acted on, so the device sends it again and it is acted on once. It is
 * refused while the port is closed if it has characters, so the device
 * keeps them, and when the flip buffer has no room for its characters and
 * the flag characters of its events. buf_overrun counts the refusals for
 * want of room, one per NACK, so again for a write sent again; no
 * character is lost. While the port is closed, the events of a write
 * without characters are counted only. Carrier down hangs the tty up
 * unless CLOCAL is set.
 */
static int obmf_tty_device_request(struct obmf_fdev *fdev,
				   struct obmf_dreq *dreq)
{
	struct obmf_tty *p = auxiliary_get_drvdata(&fdev->adev);
	u16 count = get_unaligned_le16(dreq->data);
	const u8 *chars = dreq->data + 2;
	u8 resp[OBMF_TTY_RESP_LEN] = {};
	bool open = tty_port_initialized(&p->port);
	bool brk = dreq->cmd & OBMF_TTY_EV_BREAK;
	bool overrun = dreq->cmd & OBMF_TTY_EV_OVERRUN;
	bool down = dreq->cmd & OBMF_TTY_EV_CARRIER_DOWN;
	size_t room = open ? count + brk + overrun : 0;
	bool nack = count && !open;

	if (room && tty_buffer_request_room(&p->port, room) < room) {
		nack = true;
		scoped_guard(spinlock_irqsave, &p->lock)
			p->icount.buf_overrun++;
	}
	if (nack) {
		resp[0] = OBMF_TTY_RESP_NACK;
		goto answer;
	}

	scoped_guard(spinlock_irqsave, &p->lock) {
		if (brk)
			p->icount.brk++;
		if (overrun)
			p->icount.overrun++;
		if (down)
			p->icount.dcd++;
		p->icount.rx += count;
	}
	if (room) {
		if (brk)
			tty_insert_flip_char(&p->port, 0, TTY_BREAK);
		if (overrun)
			tty_insert_flip_char(&p->port, 0, TTY_OVERRUN);
		tty_insert_flip_string(&p->port, chars, count);
		tty_flip_buffer_push(&p->port);
	}
	put_unaligned_le16(count, resp + 1);
answer:
	if (dreq->answer_due)
		obmf_respond(fdev, dreq, OBMF_STATUS_SUCCESS, resp,
			     sizeof(resp));
	if (open && down && !nack)
		tty_port_tty_hangup(&p->port, true);
	return 0;
}

/* A transport error stopped the device: hang up, as on carrier down */
static void obmf_tty_failed(struct obmf_fdev *fdev)
{
	struct obmf_tty *p = auxiliary_get_drvdata(&fdev->adev);

	tty_port_tty_hangup(&p->port, false);
}

static const struct obmf_fdev_ops obmf_tty_fdev_ops = {
	.device_request = obmf_tty_device_request,
	.failed = obmf_tty_failed,
};

static void obmf_tty_teardown(void *data)
{
	struct obmf_tty *p = data;

	scoped_guard(mutex, &obmf_tty_lock)
		p->gone = true;
	tty_port_tty_vhangup(&p->port);
	disable_delayed_work_sync(&p->tx_work);
	/* Waits for a break request; none starts after this */
	scoped_guard(mutex, &p->req_lock)
		p->fdev = NULL;
	debugfs_remove(p->dbg);
	tty_unregister_device(obmf_tty_driver, p->minor);
	tty_port_put(&p->port);
}

/*
 * A speed of 0 is refused, as obmf_tty_send() divides by it; data bits,
 * stop bits or parity out of range are replaced by 8N1
 */
static int obmf_tty_cfg(struct device *dev,
			const struct obmf_channel_info *info, u32 *speed,
			tcflag_t *cflag)
{
	static const tcflag_t sizes[] = { CS5, CS6, CS7, CS8 };
	static const tcflag_t parity[] = {
		0,
		PARENB | PARODD,
		PARENB,
		PARENB | CMSPAR | PARODD,
		PARENB | CMSPAR,
	};
	const struct obmf_tty_cfg *cfg;
	u8 par;

	if (info->config_len < sizeof(*cfg))
		return dev_err_probe(dev, -EINVAL,
				     "configuration data too short\n");
	cfg = (const struct obmf_tty_cfg *)info->config;
	*speed = le32_to_cpu(cfg->speed_hz);
	if (!*speed)
		return dev_err_probe(dev, -EINVAL, "line speed 0\n");
	*cflag = 0;
	if (FIELD_GET(OBMF_TTY_FLOW, cfg->line_control) == OBMF_TTY_RTS_CTS)
		*cflag = CRTSCTS;
	par = FIELD_GET(OBMF_TTY_PARITY, cfg->line_control);
	if (cfg->data_bits < 5 || cfg->data_bits > 8 || cfg->stop_bits < 1 ||
	    cfg->stop_bits > 2 || par >= ARRAY_SIZE(parity)) {
		dev_warn(dev, "unsupported line %u%u%u, using 8N1\n",
			 cfg->data_bits, par, cfg->stop_bits);
		*cflag |= CS8;
		return 0;
	}
	*cflag |= sizes[cfg->data_bits - 5] | parity[par];
	if (cfg->stop_bits == 2)
		*cflag |= CSTOPB;
	return 0;
}

static int obmf_tty_probe(struct auxiliary_device *adev,
			  const struct auxiliary_device_id *id)
{
	struct obmf_fdev *fdev = to_obmf_fdev(adev);
	const struct obmf_channel_info *info = fdev->info;
	struct device *dev = &adev->dev;
	struct device *tty_dev;
	size_t max_chars;
	tcflag_t cflag;
	u32 speed;
	u64 wire;
	u8 *req;
	int ret;

	ret = devm_obmf_enable(fdev);
	if (ret)
		return ret;
	ret = obmf_tty_cfg(dev, info, &speed, &cflag);
	if (ret)
		return ret;
	if (info->max_req <= OBMF_TTY_HEAD)
		return dev_err_probe(dev, -EINVAL,
				     "request limit %zu too small\n",
				     info->max_req);
	max_chars = min_t(size_t, info->max_req - OBMF_TTY_HEAD, U16_MAX);
	/* So that obmf_tty_send()'s first wait fits the core's limit */
	wire = div_u64((u64)speed *
			       (OBMF_MAX_TIMEOUT_MS - OBMF_DEFAULT_TIMEOUT_MS),
		       2 * 10 * MSEC_PER_SEC);
	max_chars = clamp_t(u64, wire, 1, max_chars);
	req = devm_kzalloc(dev, 2 + max_chars, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	struct obmf_tty *p __free(obmf_tty) = kzalloc_obj(*p);
	if (!p)
		return -ENOMEM;
	tty_port_init(&p->port);
	p->port.ops = &obmf_tty_port_ops;
	p->minor = U32_MAX;
	p->fdev = fdev;
	INIT_DELAYED_WORK(&p->tx_work, obmf_tty_tx_work);
	mutex_init(&p->req_lock);
	spin_lock_init(&p->lock);
	INIT_KFIFO(p->fifo);
	p->req = req;
	p->max_chars = max_chars;
	p->speed = speed;
	p->cflag = cflag;

	ret = xa_alloc(&obmf_tty_minors, &p->minor, p,
		       XA_LIMIT(0, OBMF_TTY_MINORS - 1), GFP_KERNEL);
	if (ret) {
		p->minor = U32_MAX;
		return ret;
	}
	tty_dev = tty_port_register_device(&p->port, obmf_tty_driver, p->minor,
					   dev);
	if (IS_ERR(tty_dev))
		return PTR_ERR(tty_dev);

	auxiliary_set_drvdata(adev, p);
	p->dbg = debugfs_create_dir(dev_name(tty_dev), obmf_tty_dbg);
	debugfs_create_ulong("tx_dropped", 0444, p->dbg, &p->tx_dropped);
	debugfs_create_ulong("tx_stalls", 0444, p->dbg, &p->tx_stalls);
	ret = devm_add_action_or_reset(dev, obmf_tty_teardown, no_free_ptr(p));
	if (ret)
		return ret;
	return devm_obmf_publish(fdev, &obmf_tty_fdev_ops);
}

static int obmf_tty_suspend(struct device *dev)
{
	struct obmf_tty *p = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&p->tx_work);
	return 0;
}

/* Runs after the parent interface has resumed the core */
static int obmf_tty_resume(struct device *dev)
{
	struct obmf_tty *p = dev_get_drvdata(dev);

	queue_delayed_work(system_dfl_wq, &p->tx_work, 0);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(obmf_tty_pm, obmf_tty_suspend, obmf_tty_resume);

static const struct auxiliary_device_id obmf_tty_ids[] = {
	{ .name = "obmf.serial" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, obmf_tty_ids);

static struct auxiliary_driver obmf_tty_aux_driver = {
	.probe = obmf_tty_probe,
	.id_table = obmf_tty_ids,
	.driver.pm = pm_sleep_ptr(&obmf_tty_pm),
};

static int __init obmf_tty_init(void)
{
	struct tty_driver *driver;
	int ret;

	driver = tty_alloc_driver(OBMF_TTY_MINORS,
				  TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV |
					  TTY_DRIVER_RESET_SAVED_TERMIOS);
	if (IS_ERR(driver))
		return PTR_ERR(driver);
	driver->driver_name = "obmf_tty";
	driver->name = "ttyOBMF";
	driver->type = TTY_DRIVER_TYPE_SERIAL;
	driver->subtype = SERIAL_TYPE_NORMAL;
	driver->init_termios = tty_std_termios;
	driver->init_termios.c_cflag = B115200 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_set_operations(driver, &obmf_tty_ops);

	ret = tty_register_driver(driver);
	if (ret) {
		tty_driver_kref_put(driver);
		return ret;
	}
	obmf_tty_driver = driver;
	obmf_tty_dbg = debugfs_create_dir("obmf_tty", NULL);

	ret = auxiliary_driver_register(&obmf_tty_aux_driver);
	if (ret) {
		debugfs_remove(obmf_tty_dbg);
		tty_unregister_driver(driver);
		tty_driver_kref_put(driver);
	}
	return ret;
}
module_init(obmf_tty_init);

static void __exit obmf_tty_exit(void)
{
	auxiliary_driver_unregister(&obmf_tty_aux_driver);
	debugfs_remove(obmf_tty_dbg);
	tty_unregister_driver(obmf_tty_driver);
	tty_driver_kref_put(obmf_tty_driver);
}
module_exit(obmf_tty_exit);

MODULE_DESCRIPTION("Serial port on an OBMF-ICP channel");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("OBMF");
