// SPDX-License-Identifier: GPL-2.0
/*
 * Serial port on an OBMF-ICP serial channel
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kfifo.h>
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

#define OBMF_TTY_FIFO_SIZE 4096
/* Zero-count NACKs before the bytes in flight are dropped */
#define OBMF_TTY_NACK_RETRIES 20
#define OBMF_TTY_NACK_DELAY_MS 10

/**
 * struct obmf_tty - one serial channel
 * @port: the tty port; its reference count owns this structure
 * @fdev: the function device, NULL once it is being removed
 * @tx_work: sends @fifo to the device
 * @req_lock: serialises host requests and @req; covers @fdev
 * @lock: covers @fifo, @tx_inflight, @tx_flush and the counters
 * @fifo: bytes written and not yet accepted by the device
 * @req: request buffer, Character Count and Character Data
 * @max_chars: most characters in one request
 * @tx_inflight: bytes at the head of @fifo in the request being sent
 * @tx_flush: flush_buffer ran while a request was in flight
 * @icount: line counters for TIOCGICOUNT
 * @tx_dropped: bytes dropped after the last NACK or an error
 * @rx_dropped: bytes received while the port was closed
 * @speed: UART_SPEED_HZ
 * @cflag: CSIZE, CSTOPB, PARENB, PARODD and CMSPAR from Table 78
 * @minor: tty index, U32_MAX until allocated
 * @gone: install refuses the port
 */
struct obmf_tty {
	struct tty_port port;
	struct obmf_fdev *fdev;
	struct work_struct tx_work;
	struct mutex req_lock; /* host requests, req, fdev */
	spinlock_t lock; /* fifo, tx_inflight, tx_flush, counters */
	DECLARE_KFIFO(fifo, u8, OBMF_TTY_FIFO_SIZE);
	u8 *req;
	size_t max_chars;
	size_t tx_inflight;
	bool tx_flush;
	struct async_icount icount;
	unsigned long tx_dropped;
	unsigned long rx_dropped;
	u32 speed;
	tcflag_t cflag;
	u32 minor;
	bool gone;
};

static struct tty_driver *obmf_tty_driver;
/* Covers the minor table and @gone for install */
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
 * the count the device accepted.
 */
static int obmf_tty_send(struct obmf_tty *p, u8 ev, size_t n, size_t *done)
{
	u8 resp[OBMF_TTY_RESP_LEN];
	unsigned int ms;
	size_t len;
	u16 acc;
	int ret;

	/* Twice the time on the wire, ten bits a character */
	ms = OBMF_DEFAULT_TIMEOUT_MS +
	     2 * DIV_ROUND_UP(n * 10 * MSEC_PER_SEC, p->speed);
	put_unaligned_le16(n, p->req);
	ret = obmf_request(p->fdev, ev, p->req, 2 + n, resp, sizeof(resp), &len,
			   ms);
	if (ret)
		return ret;
	if (len != sizeof(resp) || get_unaligned_le16(resp + 1) > n) {
		obmf_fdev_protocol_error(p->fdev);
		return -EPROTO;
	}
	acc = get_unaligned_le16(resp + 1);
	*done = (resp[0] & OBMF_TTY_RESP_NACK) ? acc : n;
	return 0;
}

enum { OBMF_TTY_TX_IDLE, OBMF_TTY_TX_MORE, OBMF_TTY_TX_RETRY };

/* One request from the head of @fifo */
static int obmf_tty_tx_one(struct obmf_tty *p, unsigned int *nacks)
{
	int next = OBMF_TTY_TX_MORE;
	bool drop = false;
	size_t n, done = 0;
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

	ret = obmf_tty_send(p, 0, n, &done);
	if (ret == -EAGAIN) {
		/* Quiesced core: obmf_tty_resume() sends the bytes */
		next = OBMF_TTY_TX_IDLE;
	} else if (!ret && !done && ++*nacks < OBMF_TTY_NACK_RETRIES) {
		next = OBMF_TTY_TX_RETRY;
	} else if (ret || !done) {
		drop = true;
		done = n;
	}

	scoped_guard(spinlock_irqsave, &p->lock) {
		p->tx_inflight = 0;
		if (p->tx_flush) {
			kfifo_reset_out(&p->fifo);
			p->tx_flush = false;
		} else {
			for (n = done; n; n--)
				kfifo_skip(&p->fifo);
		}
		if (drop)
			p->tx_dropped += done;
		else
			p->icount.tx += done;
	}
	if (drop)
		dev_warn_ratelimited(&p->fdev->adev.dev,
				     "dropped %zu bytes: %d\n", done,
				     ret ?: -EIO);
	if (done) {
		*nacks = 0;
		tty_port_tty_wakeup(&p->port);
	}
	return next;
}

static void obmf_tty_tx_work(struct work_struct *work)
{
	struct obmf_tty *p = container_of(work, struct obmf_tty, tx_work);
	unsigned int nacks = 0;
	int next;

	for (;;) {
		next = obmf_tty_tx_one(p, &nacks);
		if (next == OBMF_TTY_TX_IDLE)
			return;
		if (next == OBMF_TTY_TX_RETRY)
			msleep(OBMF_TTY_NACK_DELAY_MS);
	}
}

static ssize_t obmf_tty_write(struct tty_struct *tty, const u8 *buf,
			      size_t count)
{
	struct obmf_tty *p = tty->driver_data;
	unsigned int n;

	scoped_guard(spinlock_irqsave, &p->lock)
		n = kfifo_in(&p->fifo, buf, count);
	queue_work(system_dfl_wq, &p->tx_work);
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

static void obmf_tty_flush_buffer(struct tty_struct *tty)
{
	struct obmf_tty *p = tty->driver_data;

	scoped_guard(spinlock_irqsave, &p->lock) {
		if (p->tx_inflight)
			p->tx_flush = true;
		else
			kfifo_reset_out(&p->fifo);
	}
	tty_wakeup(tty);
}

/* One Generate Break per "on"; the device sets its length */
static int obmf_tty_break_ctl(struct tty_struct *tty, int state)
{
	struct obmf_tty *p = tty->driver_data;
	size_t done;

	if (!state)
		return 0;
	guard(mutex)(&p->req_lock);
	if (!p->fdev)
		return -ENODEV;
	return obmf_tty_send(p, OBMF_TTY_EV_GENERATE_BREAK, 0, &done);
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
	return 0;
}

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

static const struct tty_port_operations obmf_tty_port_ops = {
	.destruct = obmf_tty_destruct,
};

/*
 * A device write is taken whole or not at all, and dropped while the
 * port is closed, as a UART with no reader.
 */
static int obmf_tty_device_request(struct obmf_fdev *fdev,
				   struct obmf_dreq *dreq)
{
	struct obmf_tty *p = auxiliary_get_drvdata(&fdev->adev);
	u16 count = get_unaligned_le16(dreq->data);
	const u8 *chars = dreq->data + 2;
	u8 resp[OBMF_TTY_RESP_LEN] = {};
	bool open = tty_port_initialized(&p->port);
	bool push = false;

	scoped_guard(spinlock_irqsave, &p->lock) {
		if (dreq->cmd & OBMF_TTY_EV_BREAK)
			p->icount.brk++;
		if (dreq->cmd & OBMF_TTY_EV_OVERRUN)
			p->icount.overrun++;
		if (count && !open)
			p->rx_dropped += count;
	}
	if (open && (dreq->cmd & OBMF_TTY_EV_BREAK))
		push |= tty_insert_flip_char(&p->port, 0, TTY_BREAK);
	if (open && (dreq->cmd & OBMF_TTY_EV_OVERRUN))
		push |= tty_insert_flip_char(&p->port, 0, TTY_OVERRUN);

	if (open && count) {
		if (tty_buffer_request_room(&p->port, count) < count) {
			resp[0] = OBMF_TTY_RESP_NACK;
			count = 0;
		} else {
			tty_insert_flip_string(&p->port, chars, count);
			scoped_guard(spinlock_irqsave, &p->lock)
				p->icount.rx += count;
			push = true;
		}
	}
	if (push)
		tty_flip_buffer_push(&p->port);

	put_unaligned_le16(count, resp + 1);
	if (dreq->answer_due)
		obmf_respond(fdev, dreq, OBMF_STATUS_SUCCESS, resp,
			     sizeof(resp));
	return 0;
}

static const struct obmf_fdev_ops obmf_tty_fdev_ops = {
	.device_request = obmf_tty_device_request,
};

static void obmf_tty_teardown(void *data)
{
	struct obmf_tty *p = data;

	scoped_guard(mutex, &obmf_tty_lock)
		p->gone = true;
	tty_port_tty_vhangup(&p->port);
	disable_work_sync(&p->tx_work);
	/* Waits for a break request; none starts after this */
	scoped_guard(mutex, &p->req_lock)
		p->fdev = NULL;
	tty_unregister_device(obmf_tty_driver, p->minor);
	tty_port_put(&p->port);
}

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
	par = FIELD_GET(OBMF_TTY_PARITY, cfg->line_control);
	*speed = le32_to_cpu(cfg->speed_hz);
	if (!*speed || cfg->data_bits < 5 || cfg->data_bits > 8 ||
	    cfg->stop_bits < 1 || cfg->stop_bits > 2 ||
	    par >= ARRAY_SIZE(parity))
		return dev_err_probe(dev, -EINVAL,
				     "unsupported line %u Hz %u%u%u\n", *speed,
				     cfg->data_bits, par, cfg->stop_bits);
	*cflag = sizes[cfg->data_bits - 5] | parity[par];
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
	INIT_WORK(&p->tx_work, obmf_tty_tx_work);
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
	ret = devm_add_action_or_reset(dev, obmf_tty_teardown, no_free_ptr(p));
	if (ret)
		return ret;
	return devm_obmf_publish(fdev, &obmf_tty_fdev_ops);
}

static int obmf_tty_suspend(struct device *dev)
{
	struct obmf_tty *p = dev_get_drvdata(dev);

	cancel_work_sync(&p->tx_work);
	return 0;
}

/* Runs after the parent interface has resumed the core */
static int obmf_tty_resume(struct device *dev)
{
	struct obmf_tty *p = dev_get_drvdata(dev);

	queue_work(system_dfl_wq, &p->tx_work);
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

	ret = auxiliary_driver_register(&obmf_tty_aux_driver);
	if (ret) {
		tty_unregister_driver(driver);
		tty_driver_kref_put(driver);
	}
	return ret;
}
module_init(obmf_tty_init);

static void __exit obmf_tty_exit(void)
{
	auxiliary_driver_unregister(&obmf_tty_aux_driver);
	tty_unregister_driver(obmf_tty_driver);
	tty_driver_kref_put(obmf_tty_driver);
}
module_exit(obmf_tty_exit);

MODULE_DESCRIPTION("Serial port on an OBMF-ICP channel");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("OBMF");
