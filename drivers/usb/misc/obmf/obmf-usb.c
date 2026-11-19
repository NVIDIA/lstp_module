// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: the USB transport
 *
 * One OBMF-ICP message per bulk transfer in each direction. Two bulk IN
 * transfers are kept queued. Each send submits its own bulk OUT transfer
 * and waits for it: the channels are independent (OBMF-ICP over USB 1.0,
 * 6), and the endpoint's queue keeps the transfers in order.
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fault-inject.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "obmf-core.h"
#include "obmf-dialect.h"
#ifdef CONFIG_OBMF_LSTP
#include "obmf_usb_lstp.h"
#endif

/* OBMF-ICP over USB 1.0, 5.1 */
#define OBMF_USB_SUBCLASS 0x09
#define OBMF_USB_PROTOCOL 0x01

/* OBMF-ICP over USB 1.0, 5.2 */
#define OBMF_USB_FUNCTIONAL 0x01
#define OBMF_USB_VERSION_MAJOR 0x01

struct obmf_usb_func_desc {
	u8 length;
	u8 descriptor_type;
	u8 descriptor_subtype;
	u8 multimessage_support;
	__le16 max_wr_transfer_size;
	__le16 max_rd_transfer_size;
	__le16 max_wr_interrupt_size;
	__le16 max_rd_interrupt_size;
	__le16 bcd_obmf_version;
} __packed;

/* Channel 0 needs 64-byte payloads */
#define OBMF_USB_MIN_TRANSFER 64

#define OBMF_USB_RX_URBS 2
#define OBMF_USB_MAX_ERRORS 10
#define OBMF_USB_RETRY_MS 250

enum {
	OBMF_USB_STOPPED,
	OBMF_USB_QUIESCED,
	OBMF_USB_STALL_IN,
	OBMF_USB_STALL_OUT,
	OBMF_USB_FAILED,
};

/**
 * struct obmf_usb_fault - an error injected into completed transfers
 * @attr: when to inject, in debugfs fail_bulk_in/ or fail_bulk_out/
 * @error: the errno a transfer that succeeded completes with instead
 */
struct obmf_usb_fault {
#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS
	struct fault_attr attr;
	u32 error;
#endif
};

/**
 * struct obmf_usb - one OBMF-ICP USB interface
 * @tp: the transport, the interface's driver data
 * @intf: the interface
 * @udev: its device
 * @in_pipe: bulk IN
 * @out_pipe: bulk OUT
 * @rx_urb: bulk IN transfers
 * @rx_len: size of each bulk IN buffer
 * @rx_idle: bit per bulk IN transfer not queued
 * @zlp: a zero-length packet ends a bulk OUT message that fills its last
 *	packet
 * @tx_anchor: the bulk OUT transfers submitted and not completed
 * @send_lock: a send's check of @flags, its count and its submission,
 *	against the stop, the quiesce and the end of a bulk OUT halt
 * @tx_count: sends past that check and not returned
 * @tx_idle: woken when @tx_count drops to 0
 * @recover: clears a bulk OUT halt; resubmits bulk IN after a stall, a
 *	transient error or a failed submission
 * @errors: consecutive bulk IN errors
 * @out_errors: consecutive failed clears of a bulk OUT halt, in @recover
 * @flags: OBMF_USB_*
 * @fail_in: fault injection on bulk IN
 * @fail_out: fault injection on bulk OUT
 * @release: the release after a failed discovery, allocated at probe so
 *	that it cannot fail
 * @released: @release is queued and frees itself
 */
struct obmf_usb {
	struct obmf_transport tp;
	struct usb_interface *intf;
	struct usb_device *udev;
	unsigned int in_pipe;
	unsigned int out_pipe;
	struct urb *rx_urb[OBMF_USB_RX_URBS];
	size_t rx_len;
	unsigned long rx_idle;
	bool zlp;
	struct usb_anchor tx_anchor;
	spinlock_t send_lock; /* tx_count */
	unsigned int tx_count;
	wait_queue_head_t tx_idle;
	struct delayed_work recover;
	atomic_t errors;
	unsigned int out_errors;
	unsigned long flags;
	struct obmf_usb_fault fail_in;
	struct obmf_usb_fault fail_out;
	struct obmf_usb_release *release;
	bool released;
};

/*
 * The release of an interface whose discovery failed. It takes the USB
 * device's lock, which every unbind holds while it waits for the bind
 * work, so it runs from obmf_usb_wq with its own interface reference.
 */
struct obmf_usb_release {
	struct work_struct work;
	struct usb_interface *intf;
};

static struct workqueue_struct *obmf_usb_wq;
static struct usb_driver obmf_usb_driver;

static struct obmf_usb *to_obmf_usb(struct obmf_transport *tp)
{
	return container_of(tp, struct obmf_usb, tp);
}

#ifdef CONFIG_FAULT_INJECTION_DEBUG_FS
static void obmf_usb_fault_init(struct obmf_usb_fault *f)
{
	f->attr = (struct fault_attr)FAULT_ATTR_INITIALIZER;
	f->error = EPROTO;
}

static void obmf_usb_fault_dir(struct obmf_usb_fault *f, const char *name,
			       struct dentry *parent)
{
	struct dentry *dir;

	dir = fault_create_debugfs_attr(name, parent, &f->attr);
	if (!IS_ERR(dir))
		debugfs_create_u32("error", 0600, dir, &f->error);
}

/* After the bind: the core's debugfs directory exists then */
static void obmf_usb_fault_add(struct obmf_usb *u)
{
	struct dentry *parent = u->tp.core->dbg;

	if (!parent)
		return;
	obmf_usb_fault_dir(&u->fail_in, "fail_bulk_in", parent);
	obmf_usb_fault_dir(&u->fail_out, "fail_bulk_out", parent);
}

static void obmf_usb_fault(struct obmf_usb_fault *f, struct urb *urb)
{
	u32 error = READ_ONCE(f->error);

	if (!urb->status && error && should_fail(&f->attr, 1))
		urb->status = -(int)error;
}
#else
static void obmf_usb_fault_init(struct obmf_usb_fault *f)
{
}

static void obmf_usb_fault_add(struct obmf_usb *u)
{
}

static void obmf_usb_fault(struct obmf_usb_fault *f, struct urb *urb)
{
}
#endif

/* Below the bound of @errors in a row, try again a little later */
static void obmf_usb_retry_after(struct obmf_usb *u, unsigned int errors)
{
	if (errors < OBMF_USB_MAX_ERRORS) {
		mod_delayed_work(system_dfl_wq, &u->recover,
				 msecs_to_jiffies(OBMF_USB_RETRY_MS));
		return;
	}
	if (!test_and_set_bit(OBMF_USB_FAILED, &u->flags))
		obmf_core_fatal(u->tp.core, -EIO);
}

/* Count a bulk IN error */
static void obmf_usb_retry(struct obmf_usb *u)
{
	obmf_usb_retry_after(u, atomic_inc_return(&u->errors));
}

static void obmf_usb_rx_submit(struct obmf_usb *u, int i, gfp_t gfp)
{
	int ret;

	if (test_bit(OBMF_USB_STOPPED, &u->flags) ||
	    test_bit(OBMF_USB_QUIESCED, &u->flags) ||
	    test_bit(OBMF_USB_FAILED, &u->flags))
		return;
	clear_bit(i, &u->rx_idle);
	ret = usb_submit_urb(u->rx_urb[i], gfp);
	if (!ret)
		return;
	set_bit(i, &u->rx_idle);
	if (ret == -EPERM || ret == -ENODEV)
		return;
	obmf_count(u->tp.core, OBMF_IF_RX_URB_ERRORS);
	obmf_usb_retry(u);
}

/* Every completion is one of ours: the last URB if none of the others */
static int obmf_usb_rx_index(struct obmf_usb *u, struct urb *urb)
{
	int i;

	for (i = 0; i < OBMF_USB_RX_URBS - 1; i++)
		if (u->rx_urb[i] == urb)
			break;
	return i;
}

static void obmf_usb_rx_complete(struct urb *urb)
{
	struct obmf_usb *u = urb->context;
	int i = obmf_usb_rx_index(u, urb);

	obmf_usb_fault(&u->fail_in, urb);
	switch (urb->status) {
	case 0:
		atomic_set(&u->errors, 0);
		obmf_core_rx(u->tp.core, urb->transfer_buffer,
			     urb->actual_length);
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		set_bit(i, &u->rx_idle);
		return;
	case -EPIPE:
		/* OBMF-ICP over USB 1.0, 8.2: clear the halt, then go on */
		obmf_count(u->tp.core, OBMF_IF_STALLS);
		obmf_report(u->tp.core, &u->intf->dev, OBMF_EV_IN_STALL, -1, 0,
			    0);
		set_bit(i, &u->rx_idle);
		set_bit(OBMF_USB_STALL_IN, &u->flags);
		obmf_usb_retry(u);
		return;
	default:
		/* Wait: on an unplug these come before the disconnect does */
		dev_dbg_ratelimited(&u->intf->dev, "bulk in error: %pe\n",
				    ERR_PTR(urb->status));
		obmf_count(u->tp.core, OBMF_IF_RX_URB_ERRORS);
		set_bit(i, &u->rx_idle);
		obmf_usb_retry(u);
		return;
	}
	obmf_usb_rx_submit(u, i, GFP_ATOMIC);
}

/*
 * OBMF-ICP over USB 1.0, 8.2: clear a bulk OUT halt. Sends fail with
 * -EPIPE from the stall until the clear succeeds, and the transfers
 * queued behind the stalled one are cancelled first, so the endpoint is
 * idle while its halt is cleared. A failed clear is tried again on its
 * own count: a bulk IN that works does not reset it.
 */
static void obmf_usb_clear_out(struct obmf_usb *u)
{
	int ret;

	/* Wait for a send that checked the flags before the stall */
	spin_lock(&u->send_lock);
	spin_unlock(&u->send_lock);
	usb_poison_anchored_urbs(&u->tx_anchor);
	WARN_ON_ONCE(!usb_anchor_empty(&u->tx_anchor));
	ret = usb_clear_halt(u->udev, u->out_pipe);
	usb_unpoison_anchored_urbs(&u->tx_anchor);
	if (ret) {
		if (ret != -ENODEV)
			obmf_report(u->tp.core, &u->intf->dev,
				    OBMF_EV_OUT_CLEAR_HALT, -1, ret, 0);
		obmf_count(u->tp.core, OBMF_IF_TX_URB_ERRORS);
		obmf_usb_retry_after(u, ++u->out_errors);
		return;
	}
	u->out_errors = 0;
	scoped_guard(spinlock, &u->send_lock)
		clear_bit(OBMF_USB_STALL_OUT, &u->flags);
}

static void obmf_usb_recover(struct work_struct *work)
{
	struct obmf_usb *u =
		container_of(to_delayed_work(work), struct obmf_usb, recover);
	int i, ret;

	if (test_bit(OBMF_USB_STALL_OUT, &u->flags) &&
	    !test_bit(OBMF_USB_FAILED, &u->flags))
		obmf_usb_clear_out(u);
	if (test_bit(OBMF_USB_FAILED, &u->flags))
		return;
	if (test_and_clear_bit(OBMF_USB_STALL_IN, &u->flags)) {
		for (i = 0; i < OBMF_USB_RX_URBS; i++)
			usb_kill_urb(u->rx_urb[i]);
		ret = usb_clear_halt(u->udev, u->in_pipe);
		if (ret) {
			if (ret != -ENODEV)
				obmf_report(u->tp.core, &u->intf->dev,
					    OBMF_EV_IN_CLEAR_HALT, -1, ret, 0);
			obmf_count(u->tp.core, OBMF_IF_RX_URB_ERRORS);
			set_bit(OBMF_USB_STALL_IN, &u->flags);
			obmf_usb_retry(u);
			return;
		}
	}
	for (i = 0; i < OBMF_USB_RX_URBS; i++)
		if (test_bit(i, &u->rx_idle))
			obmf_usb_rx_submit(u, i, GFP_KERNEL);
}

/* One send's bulk OUT transfer, on the sender's stack */
struct obmf_usb_tx {
	struct obmf_usb *u;
	struct completion done;
};

static void obmf_usb_tx_complete(struct urb *urb)
{
	struct obmf_usb_tx *tx = urb->context;
	struct obmf_usb *u = tx->u;

	obmf_usb_fault(&u->fail_out, urb);
	/* Set before the sender wakes: no later send reaches the endpoint */
	if (urb->status == -EPIPE)
		set_bit(OBMF_USB_STALL_OUT, &u->flags);
	complete(&tx->done);
}

/*
 * A transfer that another path cancelled, or refused at submission: the
 * stop, or the clear of a bulk OUT halt
 */
static int obmf_usb_tx_cancelled(struct obmf_usb *u)
{
	return test_bit(OBMF_USB_STOPPED, &u->flags) ? -ENODEV : -EPIPE;
}

static int obmf_usb_tx_status(struct obmf_usb *u, struct urb *urb, size_t len)
{
	struct obmf_core *core = u->tp.core;

	switch (urb->status) {
	case 0:
		if (urb->actual_length == len) {
			obmf_count(core, OBMF_IF_TX_MESSAGES);
			return 0;
		}
		break;
	case -EPIPE:
		obmf_count(core, OBMF_IF_STALLS);
		obmf_report(core, &u->intf->dev, OBMF_EV_OUT_STALL, -1, 0, 0);
		mod_delayed_work(system_dfl_wq, &u->recover, 0);
		return -EPIPE;
	case -ENOENT:
		return obmf_usb_tx_cancelled(u);
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return -ENODEV;
	}
	dev_dbg(&u->intf->dev, "bulk out failed: %pe, %u of %zu bytes\n",
		ERR_PTR(urb->status), urb->actual_length, len);
	obmf_count(core, OBMF_IF_TX_URB_ERRORS);
	return -EIO;
}

/*
 * A submission the USB core refused. With the anchor poisoned, the error
 * of the path that poisoned it; for lack of memory, -ENOMEM.
 */
static int obmf_usb_tx_refused(struct obmf_usb *u, int ret)
{
	switch (ret) {
	case -EPERM:
		return obmf_usb_tx_cancelled(u);
	case -ENODEV:
		return -ENODEV;
	}
	dev_dbg(&u->intf->dev, "bulk out submit failed: %pe\n", ERR_PTR(ret));
	obmf_count(u->tp.core, OBMF_IF_TX_URB_ERRORS);
	return ret == -ENOMEM ? -ENOMEM : -EIO;
}

/*
 * Under @send_lock, so that the stop, the quiesce and the clear of a
 * halt find every send that passed the check counted and its transfer
 * submitted. GFP_ATOMIC, as the lock is held: the poison of the anchor
 * spins while a transfer is on it and not yet submitted.
 */
static int obmf_usb_tx_submit(struct obmf_usb *u, struct urb *urb)
{
	int ret;

	guard(spinlock)(&u->send_lock);
	if (test_bit(OBMF_USB_STOPPED, &u->flags))
		return -ENODEV;
	if (test_bit(OBMF_USB_QUIESCED, &u->flags))
		return -EAGAIN;
	/* Until the recovery work has cleared the halt */
	if (test_bit(OBMF_USB_STALL_OUT, &u->flags))
		return -EPIPE;

	usb_anchor_urb(urb, &u->tx_anchor);
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (!ret) {
		u->tx_count++;
		return 0;
	}
	usb_unanchor_urb(urb);
	return obmf_usb_tx_refused(u, ret);
}

static int obmf_usb_tx_wait(struct obmf_usb *u, struct obmf_usb_tx *tx,
			    struct urb *urb, size_t len,
			    unsigned int timeout_ms)
{
	if (wait_for_completion_timeout(&tx->done,
					msecs_to_jiffies(timeout_ms)))
		return obmf_usb_tx_status(u, urb, len);

	usb_kill_urb(urb);
	if (urb->status != -ENOENT)
		return obmf_usb_tx_status(u, urb, len);
	if (test_bit(OBMF_USB_STOPPED, &u->flags))
		return -ENODEV;
	dev_dbg(&u->intf->dev, "bulk out timed out after %u ms\n", timeout_ms);
	obmf_count(u->tp.core, OBMF_IF_TX_URB_ERRORS);
	return -ETIMEDOUT;
}

static bool obmf_usb_tx_idle(struct obmf_usb *u)
{
	guard(spinlock)(&u->send_lock);
	return !u->tx_count;
}

/*
 * Each send submits its own transfer, waits for it at most @timeout_ms
 * and then cancels it, whatever the other senders do. Once the stop has
 * begun it sends nothing and fails with -ENODEV, once the quiesce has
 * with -EAGAIN, and while a bulk OUT halt is not cleared with -EPIPE. A
 * message is never sent twice: a failed transfer is the caller's error.
 */
static int obmf_usb_send(struct obmf_transport *tp, const void *buf, size_t len,
			 unsigned int timeout_ms)
{
	struct obmf_usb *u = to_obmf_usb(tp);
	struct obmf_usb_tx tx = { .u = u };
	struct urb *urb;
	void *msg;
	bool idle;
	int ret;

	if (len > tp->max_tx_msg)
		return -EMSGSIZE;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;
	msg = kmemdup(buf, len, GFP_KERNEL);
	if (!msg) {
		usb_free_urb(urb);
		return -ENOMEM;
	}
	init_completion(&tx.done);
	usb_fill_bulk_urb(urb, u->udev, u->out_pipe, msg, len,
			  obmf_usb_tx_complete, &tx);
	urb->transfer_flags |= URB_FREE_BUFFER;
	/* OBMF-ICP over USB 1.0, 6.1: a ZLP ends a full-packet message */
	if (u->zlp)
		urb->transfer_flags |= URB_ZERO_PACKET;

	ret = obmf_usb_tx_submit(u, urb);
	if (!ret) {
		ret = obmf_usb_tx_wait(u, &tx, urb, len, timeout_ms);
		scoped_guard(spinlock, &u->send_lock)
			idle = !--u->tx_count;
		if (idle)
			wake_up(&u->tx_idle);
	}
	usb_free_urb(urb);
	return ret;
}

static int obmf_usb_start(struct obmf_transport *tp)
{
	struct obmf_usb *u = to_obmf_usb(tp);
	int i, ret;

	for (i = 0; i < OBMF_USB_RX_URBS; i++) {
		clear_bit(i, &u->rx_idle);
		ret = usb_submit_urb(u->rx_urb[i], GFP_KERNEL);
		if (ret) {
			set_bit(i, &u->rx_idle);
			return ret;
		}
	}
	return 0;
}

static void obmf_usb_stop(struct obmf_transport *tp)
{
	struct obmf_usb *u = to_obmf_usb(tp);
	int i;

	scoped_guard(spinlock, &u->send_lock)
		set_bit(OBMF_USB_STOPPED, &u->flags);
	/* A clear of a halt in progress unpoisons the anchor at its end */
	disable_delayed_work_sync(&u->recover);
	for (i = 0; i < OBMF_USB_RX_URBS; i++)
		usb_poison_urb(u->rx_urb[i]);
	usb_poison_anchored_urbs(&u->tx_anchor);
}

static void obmf_usb_quiesce(struct obmf_transport *tp)
{
	struct obmf_usb *u = to_obmf_usb(tp);
	int i;

	scoped_guard(spinlock, &u->send_lock)
		set_bit(OBMF_USB_QUIESCED, &u->flags);
	disable_delayed_work_sync(&u->recover);
	for (i = 0; i < OBMF_USB_RX_URBS; i++)
		usb_kill_urb(u->rx_urb[i]);
	/* Each send in flight ends within its own timeout */
	wait_event(u->tx_idle, obmf_usb_tx_idle(u));
}

static int obmf_usb_resume_rx(struct obmf_transport *tp)
{
	struct obmf_usb *u = to_obmf_usb(tp);
	int ret;

	clear_bit(OBMF_USB_QUIESCED, &u->flags);
	clear_bit(OBMF_USB_STALL_IN, &u->flags);
	atomic_set(&u->errors, 0);
	enable_delayed_work(&u->recover);
	/* A failed core stays failed until the interface is bound again */
	if (test_bit(OBMF_USB_FAILED, &u->flags))
		return 0;
	ret = obmf_usb_start(tp);
	/* The quiesce may have cancelled the clear of a bulk OUT halt */
	if (!ret && test_bit(OBMF_USB_STALL_OUT, &u->flags))
		mod_delayed_work(system_dfl_wq, &u->recover, 0);
	return ret;
}

static void obmf_usb_release_work(struct work_struct *work)
{
	struct obmf_usb_release *r =
		container_of(work, struct obmf_usb_release, work);
	struct usb_interface *intf = r->intf;
	struct usb_device *udev = interface_to_usbdev(intf);
	struct obmf_transport *tp;

	usb_lock_device(udev);
	/* Only the binding that asked: an unbind and a bind may come first */
	if (intf->dev.driver == &obmf_usb_driver.driver) {
		tp = usb_get_intfdata(intf);
		if (tp && READ_ONCE(to_obmf_usb(tp)->released)) {
			dev_dbg(&intf->dev,
				"released after a failed discovery\n");
			usb_driver_release_interface(&obmf_usb_driver, intf);
		}
	}
	usb_unlock_device(udev);
	usb_put_intf(intf);
	kfree(r);
}

static void obmf_usb_release_intf(struct obmf_transport *tp)
{
	struct obmf_usb *u = to_obmf_usb(tp);

	u->release->intf = usb_get_intf(u->intf);
	WRITE_ONCE(u->released, true);
	queue_work(obmf_usb_wq, &u->release->work);
}

static const struct obmf_transport_ops obmf_usb_ops = {
	.start = obmf_usb_start,
	.stop = obmf_usb_stop,
	.quiesce = obmf_usb_quiesce,
	.resume = obmf_usb_resume_rx,
	.send = obmf_usb_send,
	.release = obmf_usb_release_intf,
};

static const struct obmf_usb_func_desc *
obmf_usb_func_desc(struct usb_host_interface *alt)
{
	const struct obmf_usb_func_desc *d;
	unsigned char *p = alt->extra;
	int left = alt->extralen;

	while (left >= 2 && p[0] >= 2 && p[0] <= left) {
		d = (const struct obmf_usb_func_desc *)p;
		if (p[1] == USB_DT_CS_INTERFACE && d->length >= sizeof(*d) &&
		    d->descriptor_subtype == OBMF_USB_FUNCTIONAL)
			return d;
		left -= p[0];
		p += p[0];
	}
	return NULL;
}

static void obmf_usb_free(struct obmf_usb *u)
{
	int i;

	if (!u)
		return;
	for (i = 0; i < OBMF_USB_RX_URBS; i++) {
		if (u->rx_urb[i])
			kfree(u->rx_urb[i]->transfer_buffer);
		usb_free_urb(u->rx_urb[i]);
	}
	/* A queued release frees itself */
	if (!u->released)
		kfree(u->release);
	kfree(u);
}

DEFINE_FREE(obmf_usb, struct obmf_usb *, obmf_usb_free(_T))

static int obmf_usb_alloc_urbs(struct obmf_usb *u)
{
	void *buf;
	int i;

	for (i = 0; i < OBMF_USB_RX_URBS; i++) {
		u->rx_urb[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!u->rx_urb[i])
			return -ENOMEM;
		buf = kmalloc(u->rx_len, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		usb_fill_bulk_urb(u->rx_urb[i], u->udev, u->in_pipe, buf,
				  u->rx_len, obmf_usb_rx_complete, u);
	}
	return 0;
}

static int obmf_usb_check(struct usb_interface *intf,
			  const struct obmf_usb_func_desc *d)
{
	u16 wr = le16_to_cpu(d->max_wr_transfer_size);
	u16 rd = le16_to_cpu(d->max_rd_transfer_size);
	u16 ver = le16_to_cpu(d->bcd_obmf_version);

	if (d->multimessage_support)
		return dev_err_probe(&intf->dev, -ENODEV,
				     "multimessage transfers not supported\n");
	if (ver >> 8 != OBMF_USB_VERSION_MAJOR)
		return dev_err_probe(&intf->dev, -ENODEV,
				     "unsupported OBMF-ICP version %x.%02x\n",
				     ver >> 8, ver & 0xff);
	if (wr < OBMF_USB_MIN_TRANSFER || rd < OBMF_USB_MIN_TRANSFER)
		return dev_err_probe(&intf->dev, -ENODEV,
				     "transfer sizes %u/%u too small\n", wr,
				     rd);
	return 0;
}

/*
 * The message limits and the bulk IN buffer: from the functional
 * descriptor, or from the bulk endpoints for one message per packet,
 * where a longer buffer would join two messages.
 */
static int obmf_usb_limits(struct usb_interface *intf,
			   const struct obmf_usb_profile *pf,
			   const struct usb_endpoint_descriptor *in,
			   const struct usb_endpoint_descriptor *out,
			   struct obmf_transport *tp, size_t *rx_len)
{
	const struct obmf_usb_func_desc *d;
	int ret;

	if (!pf->func_desc) {
		tp->max_tx_msg =
			min_t(size_t, usb_endpoint_maxp(out), pf->max_packet);
		tp->max_rx_msg =
			min_t(size_t, usb_endpoint_maxp(in), pf->max_packet);
		if (tp->max_tx_msg < OBMF_USB_MIN_TRANSFER ||
		    tp->max_rx_msg < OBMF_USB_MIN_TRANSFER)
			return dev_err_probe(&intf->dev, -ENODEV,
					     "packet sizes %zu/%zu too small\n",
					     tp->max_tx_msg, tp->max_rx_msg);
		*rx_len = tp->max_rx_msg;
		return 0;
	}

	d = obmf_usb_func_desc(intf->cur_altsetting);
	if (!d)
		return dev_err_probe(&intf->dev, -ENODEV,
				     "no OBMF-ICP functional descriptor\n");
	ret = obmf_usb_check(intf, d);
	if (ret)
		return ret;
	/*
	 * Whether the transfer sizes count the message header is open: send
	 * and ask for whole messages within them, accept a header more.
	 */
	tp->max_tx_msg = le16_to_cpu(d->max_wr_transfer_size);
	tp->max_rx_msg = le16_to_cpu(d->max_rd_transfer_size);
	*rx_len = tp->max_rx_msg + OBMF_HDR_LEN;
	return 0;
}

static int obmf_usb_probe(struct usb_interface *intf,
			  const struct usb_device_id *id)
{
	const struct obmf_usb_proto *proto = (const void *)id->driver_info;
	struct usb_endpoint_descriptor *in, *out;
	const struct obmf_usb_profile *pf;
	struct obmf_transport tp = {};
	struct obmf_core *core;
	size_t rx_len;
	int ret;

	/* A dynamic ID (new_id) without a reference ID has none */
	if (!proto)
		return -ENODEV;
	pf = proto->profile;

	ret = usb_find_common_endpoints(intf->cur_altsetting, &in, &out, NULL,
					NULL);
	if (ret)
		return dev_err_probe(&intf->dev, ret, "no bulk endpoints\n");
	ret = obmf_usb_limits(intf, pf, in, out, &tp, &rx_len);
	if (ret)
		return ret;

	struct obmf_usb *u __free(obmf_usb) = kzalloc_obj(*u, GFP_KERNEL);
	if (!u)
		return -ENOMEM;
	u->release = kzalloc_obj(*u->release, GFP_KERNEL);
	if (!u->release)
		return -ENOMEM;
	INIT_WORK(&u->release->work, obmf_usb_release_work);
	u->intf = intf;
	u->udev = interface_to_usbdev(intf);
	u->in_pipe = usb_rcvbulkpipe(u->udev, usb_endpoint_num(in));
	u->out_pipe = usb_sndbulkpipe(u->udev, usb_endpoint_num(out));
	init_usb_anchor(&u->tx_anchor);
	spin_lock_init(&u->send_lock);
	init_waitqueue_head(&u->tx_idle);
	INIT_DELAYED_WORK(&u->recover, obmf_usb_recover);
	obmf_usb_fault_init(&u->fail_in);
	obmf_usb_fault_init(&u->fail_out);

	u->tp.ops = &obmf_usb_ops;
	u->tp.dev = &intf->dev;
	u->tp.max_tx_msg = tp.max_tx_msg;
	u->tp.max_rx_msg = tp.max_rx_msg;
	u->tp.resp_timeout_floor_ms = pf->resp_timeout_floor_ms;
	/* An interface shares its device's companion, the port object */
	u->tp.acpi_adr = intf->cur_altsetting->desc.bInterfaceNumber;
	/* The channel nodes are below the interface's, found by its number */
	if (u->udev->dev.of_node && !intf->dev.of_node)
		dev_dbg(&intf->dev, "interface %u has no DT node\n",
			intf->cur_altsetting->desc.bInterfaceNumber);
	ret = obmf_dialect_attach(&u->tp, proto);
	if (ret)
		return ret;
	u->rx_len = rx_len;
	u->zlp = pf->zlp;

	ret = obmf_usb_alloc_urbs(u);
	if (ret)
		return ret;
	usb_set_intfdata(intf, &u->tp);

	core = obmf_core_create(&u->tp);
	if (!core)
		return -ENOMEM;

	ret = obmf_core_bind(core);
	if (ret) {
		obmf_core_unbind(core, false);
		return ret;
	}
	obmf_usb_fault_add(u);
	retain_and_null_ptr(u);
	return 0;
}

static void obmf_usb_disconnect(struct usb_interface *intf)
{
	struct obmf_transport *tp = usb_get_intfdata(intf);
	struct obmf_usb *u = to_obmf_usb(tp);

	obmf_core_unbind(tp->core, u->udev->state != USB_STATE_NOTATTACHED);
	obmf_usb_free(u);
}

static int obmf_usb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct obmf_transport *tp = usb_get_intfdata(intf);

	return obmf_core_quiesce(tp->core);
}

static int obmf_usb_resume(struct usb_interface *intf)
{
	struct obmf_transport *tp = usb_get_intfdata(intf);

	return obmf_core_resume(tp->core);
}

static const struct obmf_usb_profile obmf_usb_icp_profile = {
	.func_desc = true,
	.zlp = true,
};

static const struct obmf_usb_proto obmf_usb_icp = {
	.profile = &obmf_usb_icp_profile,
};

static const struct usb_device_id obmf_usb_ids[] = {
	{ USB_INTERFACE_INFO(USB_CLASS_MISC, OBMF_USB_SUBCLASS,
			     OBMF_USB_PROTOCOL),
	  .driver_info = (kernel_ulong_t)&obmf_usb_icp },
#ifdef CONFIG_OBMF_LSTP
	OBMF_LSTP_USB_IDS
#endif
	{}
};
MODULE_DEVICE_TABLE(usb, obmf_usb_ids);

static struct usb_driver obmf_usb_driver = {
	.name = "obmf",
	.probe = obmf_usb_probe,
	.disconnect = obmf_usb_disconnect,
	.suspend = obmf_usb_suspend,
	.resume = obmf_usb_resume,
	.id_table = obmf_usb_ids,
	.dev_groups = obmf_intf_groups,
	.soft_unbind = 1,
};

static int __init obmf_usb_init(void)
{
	int ret;

	/* Freezable, as the hub's: a release waits for the system to wake */
	obmf_usb_wq = alloc_workqueue("obmf-usb", WQ_FREEZABLE | WQ_UNBOUND, 0);
	if (!obmf_usb_wq)
		return -ENOMEM;
	obmf_debugfs_init(usb_debug_root);
	ret = usb_register(&obmf_usb_driver);
	if (ret) {
		obmf_debugfs_exit();
		destroy_workqueue(obmf_usb_wq);
	}
	return ret;
}
module_init(obmf_usb_init);

static void __exit obmf_usb_exit(void)
{
	usb_deregister(&obmf_usb_driver);
	/* A release queued before the last unbind runs in this module */
	destroy_workqueue(obmf_usb_wq);
	/*
	 * A device link to a function device may still hold its last
	 * reference, put later from the device-link workqueue; the release
	 * runs in this module.
	 */
	device_link_wait_removal();
	obmf_debugfs_exit();
}
module_exit(obmf_usb_exit);

MODULE_DESCRIPTION("OBMF-ICP over USB");
MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_OBMF_KUNIT_TEST)
#include <kunit/test.h>
#include <linux/iopoll.h>

#include "obmf-kunit.h"

static struct usb_interface *obmf_usb_test_intf(struct kunit *test)
{
	struct usb_interface *intf =
		kunit_kzalloc(test, sizeof(*intf), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, intf);
	intf->cur_altsetting =
		kunit_kzalloc(test, sizeof(*intf->cur_altsetting), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, intf->cur_altsetting);
	return intf;
}

/* An ID without a protocol, as new_id makes without a reference ID */
static void obmf_usb_test_no_proto(struct kunit *test)
{
	const struct usb_device_id id = {};

	KUNIT_EXPECT_EQ(test, obmf_usb_probe(obmf_usb_test_intf(test), &id),
			-ENODEV);
}

/*
 * Without a functional descriptor: packets below the minimum transfer
 * refused in either direction, larger ones cut to the profile's packet
 */
static void obmf_usb_test_limits(struct kunit *test)
{
	static const struct obmf_usb_profile pf = { .max_packet = 512 };
	struct usb_interface *intf = obmf_usb_test_intf(test);
	struct usb_endpoint_descriptor in = {}, out = {};
	struct obmf_transport tp = {};
	size_t rx_len = 0;

	in.wMaxPacketSize = cpu_to_le16(OBMF_USB_MIN_TRANSFER);
	out.wMaxPacketSize = cpu_to_le16(OBMF_USB_MIN_TRANSFER);
	KUNIT_EXPECT_EQ(test,
			obmf_usb_limits(intf, &pf, &in, &out, &tp, &rx_len), 0);
	KUNIT_EXPECT_EQ(test, tp.max_tx_msg, OBMF_USB_MIN_TRANSFER);
	KUNIT_EXPECT_EQ(test, tp.max_rx_msg, OBMF_USB_MIN_TRANSFER);
	KUNIT_EXPECT_EQ(test, rx_len, OBMF_USB_MIN_TRANSFER);

	out.wMaxPacketSize = cpu_to_le16(OBMF_USB_MIN_TRANSFER - 1);
	KUNIT_EXPECT_EQ(test,
			obmf_usb_limits(intf, &pf, &in, &out, &tp, &rx_len),
			-ENODEV);
	out.wMaxPacketSize = cpu_to_le16(OBMF_USB_MIN_TRANSFER);
	in.wMaxPacketSize = cpu_to_le16(OBMF_USB_MIN_TRANSFER - 1);
	KUNIT_EXPECT_EQ(test,
			obmf_usb_limits(intf, &pf, &in, &out, &tp, &rx_len),
			-ENODEV);

	in.wMaxPacketSize = cpu_to_le16(1024);
	out.wMaxPacketSize = cpu_to_le16(1024);
	KUNIT_EXPECT_EQ(test,
			obmf_usb_limits(intf, &pf, &in, &out, &tp, &rx_len), 0);
	KUNIT_EXPECT_EQ(test, tp.max_tx_msg, 512);
	KUNIT_EXPECT_EQ(test, tp.max_rx_msg, 512);
	KUNIT_EXPECT_EQ(test, rx_len, 512);
}

/*
 * An EF/09/01 interface matches the OBMF-ICP class entry for any
 * VID/PID, one that other entries list too; another
 * class, subclass or protocol matches no entry. The USB core matches no
 * interface class of a device whose device class is vendor-specific, so
 * the devices here have device class 0 or EF (Miscellaneous).
 */
static void obmf_usb_test_class_entry(struct kunit *test)
{
	static const struct {
		u16 vid, pid;
		u8 class;
	} devs[] = {
		{ 0x0000, 0x0000, 0 },
		{ 0x1d6b, 0x0104, USB_CLASS_MISC },
		{ 0x0955, 0xcf11, 0 },
		{ 0xffff, 0xffff, USB_CLASS_MISC },
	};
	static const u8 other[][3] = {
		{ USB_CLASS_MISC, OBMF_USB_SUBCLASS, OBMF_USB_PROTOCOL + 1 },
		{ USB_CLASS_MISC, OBMF_USB_SUBCLASS + 1, OBMF_USB_PROTOCOL },
		{ USB_CLASS_MISC, OBMF_USB_SUBCLASS - 1, OBMF_USB_PROTOCOL },
		{ USB_CLASS_VENDOR_SPEC, OBMF_USB_SUBCLASS, OBMF_USB_PROTOCOL },
		{ USB_CLASS_MISC, OBMF_USB_SUBCLASS, 0 },
	};
	struct usb_interface *intf = obmf_usb_test_intf(test);
	struct usb_interface_descriptor *d = &intf->cur_altsetting->desc;
	struct usb_device *udev =
		kunit_kzalloc(test, sizeof(*udev), GFP_KERNEL);
	const struct usb_device_id *id;
	const struct obmf_usb_proto *proto;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, udev);
	intf->dev.parent = &udev->dev;
	for (i = 0; i < ARRAY_SIZE(devs); i++) {
		udev->descriptor.idVendor = cpu_to_le16(devs[i].vid);
		udev->descriptor.idProduct = cpu_to_le16(devs[i].pid);
		udev->descriptor.bDeviceClass = devs[i].class;
		d->bInterfaceClass = USB_CLASS_MISC;
		d->bInterfaceSubClass = OBMF_USB_SUBCLASS;
		d->bInterfaceProtocol = OBMF_USB_PROTOCOL;
		id = usb_match_id(intf, obmf_usb_ids);
		KUNIT_ASSERT_PTR_EQ_MSG(test, id, &obmf_usb_ids[0], "%04x:%04x",
					devs[i].vid, devs[i].pid);
		proto = (const struct obmf_usb_proto *)id->driver_info;
		KUNIT_EXPECT_PTR_EQ(test, proto, &obmf_usb_icp);
	}
	KUNIT_EXPECT_EQ(test, USB_CLASS_MISC, 0xef);
	KUNIT_EXPECT_EQ(test, OBMF_USB_SUBCLASS, 0x09);
	KUNIT_EXPECT_EQ(test, OBMF_USB_PROTOCOL, 0x01);

	udev->descriptor.idVendor = cpu_to_le16(0x1d6b);
	udev->descriptor.idProduct = cpu_to_le16(0x0104);
	udev->descriptor.bDeviceClass = 0;
	for (i = 0; i < ARRAY_SIZE(other); i++) {
		d->bInterfaceClass = other[i][0];
		d->bInterfaceSubClass = other[i][1];
		d->bInterfaceProtocol = other[i][2];
		KUNIT_EXPECT_NULL_MSG(test, usb_match_id(intf, obmf_usb_ids),
				      "%02x/%02x/%02x", other[i][0],
				      other[i][1], other[i][2]);
	}
}

#ifdef CONFIG_OBMF_DIALECTS
static ssize_t obmf_usb_test_encode(struct obmf_channel *ch, u8 *buf,
				    size_t len, size_t cap)
{
	return len;
}

static ssize_t obmf_usb_test_decode(struct obmf_channel *ch, const u8 *in,
				    size_t len, u8 *out, size_t cap,
				    u8 pending_cmd, unsigned long *levels)
{
	return -EPROTO;
}

/*
 * A dialect with one of encode and decode, or with nothing set: each of
 * the two checks warns once, inside obmf_test_warns()
 */
static void obmf_usb_test_bad_dialect(struct kunit *test)
{
	static const struct obmf_usb_profile pf = { .max_packet = 64 };
	static const struct obmf_dialect bad[] = {
		{ .encode = obmf_usb_test_encode },
		{ .decode = obmf_usb_test_decode },
		{},
	};
	struct usb_interface *intf = obmf_usb_test_intf(test);
	struct obmf_usb_proto proto = { .profile = &pf };
	const struct usb_device_id id = {
		.driver_info = (kernel_ulong_t)&proto,
	};
	struct usb_host_endpoint *ep;
	struct usb_device *udev;
	int i;

	ep = kunit_kcalloc(test, 2, sizeof(*ep), GFP_KERNEL);
	udev = kunit_kzalloc(test, sizeof(*udev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ep);
	KUNIT_ASSERT_NOT_NULL(test, udev);
	ep[0].desc.bEndpointAddress = USB_DIR_IN | 1;
	ep[1].desc.bEndpointAddress = USB_DIR_OUT | 2;
	for (i = 0; i < 2; i++) {
		ep[i].desc.bmAttributes = USB_ENDPOINT_XFER_BULK;
		ep[i].desc.wMaxPacketSize = cpu_to_le16(64);
	}
	intf->cur_altsetting->endpoint = ep;
	intf->cur_altsetting->desc.bNumEndpoints = 2;
	intf->dev.parent = &udev->dev;

	obmf_test_warns(test)
	{
		for (i = 0; i < ARRAY_SIZE(bad); i++) {
			proto.dialect = &bad[i];
			KUNIT_EXPECT_EQ_MSG(test, obmf_usb_probe(intf, &id),
					    -EINVAL, "table %d", i);
		}
	}
}

#endif /* CONFIG_OBMF_DIALECTS */
/* An interface with no USB device: the bulk OUT state of a probe */
static struct obmf_usb *obmf_usb_test_tx(struct kunit *test)
{
	struct obmf_usb *u = kunit_kzalloc(test, sizeof(*u), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, u);
	u->tp.core = kunit_kzalloc(test, sizeof(*u->tp.core), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, u->tp.core);
	u->intf = obmf_usb_test_intf(test);
	init_usb_anchor(&u->tx_anchor);
	spin_lock_init(&u->send_lock);
	init_waitqueue_head(&u->tx_idle);
	INIT_DELAYED_WORK(&u->recover, obmf_usb_recover);
	obmf_usb_fault_init(&u->fail_in);
	obmf_usb_fault_init(&u->fail_out);
	return u;
}

KUNIT_DEFINE_ACTION_WRAPPER(obmf_usb_test_free_urb, usb_free_urb, struct urb *);

/* A send's transfer with no device, which the USB core refuses */
static struct urb *obmf_usb_test_tx_urb(struct kunit *test,
					struct obmf_usb_tx *tx)
{
	struct urb *urb = usb_alloc_urb(0, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, urb);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, obmf_usb_test_free_urb,
						  urb),
			0);
	init_completion(&tx->done);
	usb_fill_bulk_urb(urb, NULL, 0, NULL, 0, obmf_usb_tx_complete, tx);
	return urb;
}

static long obmf_usb_test_tx_errors(struct obmf_usb *u)
{
	return atomic_long_read(&u->tp.core->cnt[OBMF_IF_TX_URB_ERRORS]);
}

/*
 * A send is refused, nothing submitted, by the stop with -ENODEV, by
 * the quiesce with -EAGAIN and while an OUT halt is marked with -EPIPE,
 * in that order. A submission the USB core refuses leaves nothing on
 * the anchor and nothing counted in flight; -ENOMEM is passed on.
 */
static void obmf_usb_test_tx_submit(struct kunit *test)
{
	static const struct {
		unsigned long flags;
		int ret;
	} cases[] = {
		{ BIT(OBMF_USB_STOPPED) | BIT(OBMF_USB_QUIESCED) |
			  BIT(OBMF_USB_STALL_OUT),
		  -ENODEV },
		{ BIT(OBMF_USB_QUIESCED) | BIT(OBMF_USB_STALL_OUT), -EAGAIN },
		{ BIT(OBMF_USB_STALL_OUT), -EPIPE },
		/* The USB core's refusal: no device */
		{ 0, -ENODEV },
	};
	struct obmf_usb *u = obmf_usb_test_tx(test);
	struct obmf_usb_tx tx = { .u = u };
	struct urb *urb = obmf_usb_test_tx_urb(test, &tx);
	int i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		u->flags = cases[i].flags;
		KUNIT_EXPECT_EQ_MSG(test, obmf_usb_tx_submit(u, urb),
				    cases[i].ret, "flags %#lx", cases[i].flags);
		KUNIT_EXPECT_TRUE_MSG(test, usb_anchor_empty(&u->tx_anchor),
				      "flags %#lx", cases[i].flags);
		KUNIT_EXPECT_EQ(test, u->tx_count, 0);
	}
	KUNIT_EXPECT_EQ(test, obmf_usb_test_tx_errors(u), 0);

	u->flags = 0;
	KUNIT_EXPECT_EQ(test, obmf_usb_tx_refused(u, -ENOMEM), -ENOMEM);
	KUNIT_EXPECT_EQ(test, obmf_usb_tx_refused(u, -EMSGSIZE), -EIO);
	KUNIT_EXPECT_EQ(test, obmf_usb_test_tx_errors(u), 2);
	/* A poisoned anchor: the error of the path that poisoned it */
	KUNIT_EXPECT_EQ(test, obmf_usb_tx_refused(u, -EPERM), -EPIPE);
	KUNIT_EXPECT_EQ(test, obmf_usb_tx_refused(u, -ENODEV), -ENODEV);
	set_bit(OBMF_USB_STOPPED, &u->flags);
	KUNIT_EXPECT_EQ(test, obmf_usb_tx_refused(u, -EPERM), -ENODEV);
	KUNIT_EXPECT_EQ(test, obmf_usb_test_tx_errors(u), 2);
}

/*
 * The completion marks an OUT halt before it wakes the sender, so that
 * no later send reaches the endpoint; a transfer that succeeded marks
 * nothing. A transfer another path cancelled is -EPIPE, or -ENODEV once
 * the stop has begun.
 */
static void obmf_usb_test_tx_complete(struct kunit *test)
{
	struct obmf_usb *u = obmf_usb_test_tx(test);
	struct obmf_usb_tx tx = { .u = u };
	struct urb *urb = obmf_usb_test_tx_urb(test, &tx);

	urb->status = 0;
	obmf_usb_tx_complete(urb);
	KUNIT_EXPECT_TRUE(test, completion_done(&tx.done));
	KUNIT_EXPECT_FALSE(test, test_bit(OBMF_USB_STALL_OUT, &u->flags));

	reinit_completion(&tx.done);
	urb->status = -EPIPE;
	obmf_usb_tx_complete(urb);
	KUNIT_EXPECT_TRUE(test, completion_done(&tx.done));
	KUNIT_EXPECT_TRUE(test, test_bit(OBMF_USB_STALL_OUT, &u->flags));

	KUNIT_EXPECT_EQ(test, obmf_usb_tx_cancelled(u), -EPIPE);
	set_bit(OBMF_USB_STOPPED, &u->flags);
	KUNIT_EXPECT_EQ(test, obmf_usb_tx_cancelled(u), -ENODEV);
}

struct obmf_usb_test_quiesce {
	struct work_struct work;
	struct obmf_usb *u;
	struct completion done;
};

static void obmf_usb_test_quiesce_fn(struct work_struct *work)
{
	struct obmf_usb_test_quiesce *q =
		container_of(work, struct obmf_usb_test_quiesce, work);

	obmf_usb_quiesce(&q->u->tp);
	complete(&q->done);
}

/*
 * The quiesce refuses new sends at once and returns only when the send
 * in flight has returned. Only expectations between the queueing and the
 * flush: the work must not outlive the case.
 */
static void obmf_usb_test_tx_quiesce(struct kunit *test)
{
	struct obmf_usb *u = obmf_usb_test_tx(test);
	struct obmf_usb_test_quiesce q = { .u = u };
	unsigned long left;
	bool idle;
	int set;

	u->tx_count = 1;
	init_completion(&q.done);
	INIT_WORK_ONSTACK(&q.work, obmf_usb_test_quiesce_fn);
	queue_work(system_dfl_wq, &q.work);
	KUNIT_EXPECT_EQ(test,
			read_poll_timeout(test_bit, set, set, 1000, 1000000,
					  false, OBMF_USB_QUIESCED, &u->flags),
			0);
	left = wait_for_completion_timeout(&q.done, msecs_to_jiffies(50));
	KUNIT_EXPECT_EQ(test, left, 0);

	/* The send returns, as obmf_usb_send() does */
	scoped_guard(spinlock, &u->send_lock)
		idle = !--u->tx_count;
	if (idle)
		wake_up(&u->tx_idle);
	left = wait_for_completion_timeout(&q.done, msecs_to_jiffies(1000));
	KUNIT_EXPECT_NE(test, left, 0);
	flush_work(&q.work);
	destroy_work_on_stack(&q.work);
}

/*
 * The stop marks the transport stopped, disables the recovery work and
 * poisons the anchor, so that a transfer submitted before it is
 * cancelled and none is submitted after it.
 */
static void obmf_usb_test_tx_stop(struct kunit *test)
{
	struct obmf_usb *u = obmf_usb_test_tx(test);

	obmf_usb_stop(&u->tp);
	KUNIT_EXPECT_TRUE(test, test_bit(OBMF_USB_STOPPED, &u->flags));
	KUNIT_EXPECT_TRUE(test, u->tx_anchor.poisoned);
	mod_delayed_work(system_dfl_wq, &u->recover, msecs_to_jiffies(60000));
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&u->recover));
	cancel_delayed_work_sync(&u->recover);
}

static struct kunit_case obmf_usb_cases[] = {
	KUNIT_CASE(obmf_usb_test_no_proto),
	KUNIT_CASE(obmf_usb_test_limits),
	KUNIT_CASE(obmf_usb_test_class_entry),
	KUNIT_CASE(obmf_usb_test_tx_submit),
	KUNIT_CASE(obmf_usb_test_tx_complete),
	KUNIT_CASE(obmf_usb_test_tx_quiesce),
	KUNIT_CASE(obmf_usb_test_tx_stop),
#ifdef CONFIG_OBMF_DIALECTS
	KUNIT_CASE(obmf_usb_test_bad_dialect),
#endif
	{}
};

static struct kunit_suite obmf_usb_suite = {
	.name = "obmf-usb",
	.test_cases = obmf_usb_cases,
};

kunit_test_suite(obmf_usb_suite);

#endif /* CONFIG_OBMF_KUNIT_TEST */
