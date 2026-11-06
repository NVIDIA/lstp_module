// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: the USB transport
 *
 * One OBMF-ICP message per bulk transfer in each direction. Two bulk IN
 * transfers are kept queued; one bulk OUT transfer is in flight at a time.
 */

#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/errno.h>
#include <linux/fault-inject.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/usb.h>
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
 * @tx_urb: the bulk OUT transfer
 * @zlp: a zero-length packet ends a bulk OUT message that fills its last
 *	packet
 * @tx_lock: one bulk OUT transfer at a time
 * @tx_done: @tx_urb completed
 * @recover: resubmits bulk IN after a stall, a transient error or a failed
 *	submission
 * @errors: consecutive bulk IN errors
 * @flags: OBMF_USB_*
 * @fail_in: fault injection on bulk IN
 * @fail_out: fault injection on bulk OUT
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
	struct urb *tx_urb;
	bool zlp;
	struct mutex tx_lock; /* tx_urb */
	struct completion tx_done;
	struct delayed_work recover;
	atomic_t errors;
	unsigned long flags;
	struct obmf_usb_fault fail_in;
	struct obmf_usb_fault fail_out;
};

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

/* Count a bulk IN error; below the bound, try again a little later */
static void obmf_usb_retry(struct obmf_usb *u)
{
	if (atomic_inc_return(&u->errors) < OBMF_USB_MAX_ERRORS) {
		mod_delayed_work(system_dfl_wq, &u->recover,
				 msecs_to_jiffies(OBMF_USB_RETRY_MS));
		return;
	}
	if (!test_and_set_bit(OBMF_USB_FAILED, &u->flags))
		obmf_core_fatal(u->tp.core, -EIO);
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
		set_bit(i, &u->rx_idle);
		set_bit(OBMF_USB_STALL_IN, &u->flags);
		obmf_usb_retry(u);
		return;
	default:
		/* Wait: on an unplug these come before the disconnect does */
		obmf_count(u->tp.core, OBMF_IF_RX_URB_ERRORS);
		set_bit(i, &u->rx_idle);
		obmf_usb_retry(u);
		return;
	}
	obmf_usb_rx_submit(u, i, GFP_ATOMIC);
}

static void obmf_usb_recover(struct work_struct *work)
{
	struct obmf_usb *u =
		container_of(to_delayed_work(work), struct obmf_usb, recover);
	int i, ret;

	if (test_bit(OBMF_USB_FAILED, &u->flags))
		return;
	if (test_and_clear_bit(OBMF_USB_STALL_IN, &u->flags)) {
		for (i = 0; i < OBMF_USB_RX_URBS; i++)
			usb_kill_urb(u->rx_urb[i]);
		ret = usb_clear_halt(u->udev, u->in_pipe);
		if (ret) {
			dev_dbg(&u->intf->dev, "clear halt failed: %d\n", ret);
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

static void obmf_usb_tx_complete(struct urb *urb)
{
	struct obmf_usb *u = urb->context;

	obmf_usb_fault(&u->fail_out, urb);
	complete(&u->tx_done);
}

static int obmf_usb_tx_status(struct obmf_usb *u, size_t len)
{
	struct urb *urb = u->tx_urb;
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
		usb_clear_halt(u->udev, u->out_pipe);
		return -EPIPE;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
	case -ENODEV:
		return -ENODEV;
	}
	obmf_count(core, OBMF_IF_TX_URB_ERRORS);
	return -EIO;
}

/* A message is never sent twice: a failed transfer is the caller's error */
static int obmf_usb_send(struct obmf_transport *tp, const void *buf, size_t len,
			 unsigned int timeout_ms)
{
	struct obmf_usb *u = to_obmf_usb(tp);
	struct urb *urb = u->tx_urb;
	int ret;

	if (len > tp->max_tx_msg)
		return -EMSGSIZE;

	guard(mutex)(&u->tx_lock);
	if (test_bit(OBMF_USB_STOPPED, &u->flags))
		return -ENODEV;
	if (test_bit(OBMF_USB_QUIESCED, &u->flags))
		return -EAGAIN;

	memcpy(urb->transfer_buffer, buf, len);
	urb->transfer_buffer_length = len;
	reinit_completion(&u->tx_done);
	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret == -EPERM || ret == -ENODEV)
		return -ENODEV;
	if (ret) {
		obmf_count(tp->core, OBMF_IF_TX_URB_ERRORS);
		return -EIO;
	}
	if (wait_for_completion_timeout(&u->tx_done,
					msecs_to_jiffies(timeout_ms)))
		return obmf_usb_tx_status(u, len);

	usb_kill_urb(urb);
	if (urb->status != -ENOENT)
		return obmf_usb_tx_status(u, len);
	if (test_bit(OBMF_USB_STOPPED, &u->flags))
		return -ENODEV;
	obmf_count(tp->core, OBMF_IF_TX_URB_ERRORS);
	return -ETIMEDOUT;
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

	set_bit(OBMF_USB_STOPPED, &u->flags);
	disable_delayed_work_sync(&u->recover);
	for (i = 0; i < OBMF_USB_RX_URBS; i++)
		usb_poison_urb(u->rx_urb[i]);
	usb_poison_urb(u->tx_urb);
}

static void obmf_usb_quiesce(struct obmf_transport *tp)
{
	struct obmf_usb *u = to_obmf_usb(tp);
	int i;

	set_bit(OBMF_USB_QUIESCED, &u->flags);
	disable_delayed_work_sync(&u->recover);
	for (i = 0; i < OBMF_USB_RX_URBS; i++)
		usb_kill_urb(u->rx_urb[i]);
	/* Wait for a send that started before OBMF_USB_QUIESCED was set */
	mutex_lock(&u->tx_lock);
	mutex_unlock(&u->tx_lock);
}

static int obmf_usb_resume_rx(struct obmf_transport *tp)
{
	struct obmf_usb *u = to_obmf_usb(tp);

	clear_bit(OBMF_USB_QUIESCED, &u->flags);
	clear_bit(OBMF_USB_STALL_IN, &u->flags);
	atomic_set(&u->errors, 0);
	enable_delayed_work(&u->recover);
	/* A failed core stays failed until the interface is bound again */
	if (test_bit(OBMF_USB_FAILED, &u->flags))
		return 0;
	return obmf_usb_start(tp);
}

static const struct obmf_transport_ops obmf_usb_ops = {
	.start = obmf_usb_start,
	.stop = obmf_usb_stop,
	.quiesce = obmf_usb_quiesce,
	.resume = obmf_usb_resume_rx,
	.send = obmf_usb_send,
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
	if (u->tx_urb)
		kfree(u->tx_urb->transfer_buffer);
	usb_free_urb(u->tx_urb);
	mutex_destroy(&u->tx_lock);
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
	u->tx_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!u->tx_urb)
		return -ENOMEM;
	buf = kmalloc(u->tp.max_tx_msg, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	usb_fill_bulk_urb(u->tx_urb, u->udev, u->out_pipe, buf,
			  u->tp.max_tx_msg, obmf_usb_tx_complete, u);
	/* OBMF-ICP over USB 1.0, 6.1: a ZLP ends a full-packet message */
	if (u->zlp)
		u->tx_urb->transfer_flags |= URB_ZERO_PACKET;
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

	struct obmf_usb *u __free(obmf_usb) = kzalloc_obj(*u);
	if (!u)
		return -ENOMEM;
	u->intf = intf;
	u->udev = interface_to_usbdev(intf);
	u->in_pipe = usb_rcvbulkpipe(u->udev, usb_endpoint_num(in));
	u->out_pipe = usb_sndbulkpipe(u->udev, usb_endpoint_num(out));
	mutex_init(&u->tx_lock);
	init_completion(&u->tx_done);
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

	obmf_debugfs_init(usb_debug_root);
	ret = usb_register(&obmf_usb_driver);
	if (ret)
		obmf_debugfs_exit();
	return ret;
}
module_init(obmf_usb_init);

static void __exit obmf_usb_exit(void)
{
	usb_deregister(&obmf_usb_driver);
	obmf_debugfs_exit();
}
module_exit(obmf_usb_exit);

MODULE_DESCRIPTION("OBMF-ICP over USB");
MODULE_LICENSE("GPL");

#if IS_ENABLED(CONFIG_OBMF_KUNIT_TEST)
#include <kunit/test.h>

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
 * A dialect with one of encode and decode, or with nothing set.
 * Expected: one WARN for each of the two checks, unless an earlier test
 * has already hit it.
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

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		proto.dialect = &bad[i];
		KUNIT_EXPECT_EQ_MSG(test, obmf_usb_probe(intf, &id), -EINVAL,
				    "table %d", i);
	}
}

#endif /* CONFIG_OBMF_DIALECTS */
static struct kunit_case obmf_usb_cases[] = {
	KUNIT_CASE(obmf_usb_test_no_proto),
	KUNIT_CASE(obmf_usb_test_limits),
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
