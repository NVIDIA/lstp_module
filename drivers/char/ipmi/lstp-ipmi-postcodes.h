/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * IPMI postcodes support for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#ifndef __LSTP_IPMI_POSTCODES_H
#define __LSTP_IPMI_POSTCODES_H

#include <linux/types.h>

#include <linux/usb/lstp.h>

#ifdef CONFIG_SEPARATE_LSTP_IPMI_POSTCODES

#include <linux/miscdevice.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/uaccess.h>

/*
 * Postcode message (IPMI OEM Group Extension):
 * byte 0 netfn/lun (netfn 0x2C = Group Extension), byte 1 cmd 0x02,
 * byte 2 group 0xAE, bytes 3-11 nine bytes of postcode data.
 */
#define LSTP_IPMI_POST_NETFN 0x2C
#define LSTP_IPMI_POST_CMD 0x02
#define LSTP_IPMI_POST_GROUP 0xAE
#define LSTP_IPMI_POST_MIN_LEN 3 /* netfn + cmd + group */
#define LSTP_IPMI_POST_CODE_SIZE 9 /* postcode data length */
#define LSTP_IPMI_POST_CODE_OFFSET 3 /* offset to postcode data */
#define LSTP_IPMI_POST_BUFFER_SIZE 1024

struct lstp_ipmi_postcodes {
	struct miscdevice miscdev;
	spinlock_t lock; /* protects @fifo and @running */
	wait_queue_head_t wait_queue;
	struct kfifo fifo; /* postcode bytes pending read() */
	bool running; /* exclusive-open flag */
	bool stopped; /* node torn down; see lstp_ipmi_postcodes_stopped() */
	struct lstp_channel *ch; /* pinned by the embedding context */
	struct kref *owner_ref; /* the embedding context's; held by each open fd */
	void (*owner_release)(struct kref *ref);
};

static inline struct lstp_ipmi_postcodes *lstp_ipmi_postcodes_to_ctx(struct file *file)
{
	return container_of(file->private_data, struct lstp_ipmi_postcodes, miscdev);
}

/* @stopped is per bind and never cleared; see Documentation/driver-api/usb/lstp.rst. */
static inline bool lstp_ipmi_postcodes_stopped(struct lstp_ipmi_postcodes *post)
{
	return READ_ONCE(post->stopped) || lstp_channel_disconnected(post->ch);
}

static int lstp_ipmi_postcodes_open(struct inode *inode, struct file *file)
{
	struct lstp_ipmi_postcodes *post = lstp_ipmi_postcodes_to_ctx(file);
	int ret = 0;

	spin_lock_irq(&post->lock);
	if (lstp_ipmi_postcodes_stopped(post))
		ret = -ENODEV;
	else if (post->running)
		ret = -EBUSY;
	else
		post->running = true;
	spin_unlock_irq(&post->lock);

	if (ret)
		return ret;

	kref_get(post->owner_ref);

	return 0;
}

static ssize_t lstp_ipmi_postcodes_read(struct file *file, char __user *buf, size_t count,
					loff_t *ppos)
{
	struct lstp_ipmi_postcodes *post = lstp_ipmi_postcodes_to_ctx(file);
	u8 tmp_buf[LSTP_IPMI_POST_CODE_SIZE];
	unsigned long flags;

	/*
	 * The ring carries whole postcodes in a byte fifo, so a partial read
	 * would leave the tail of one behind and every later read would
	 * straddle two. Refuse instead of desynchronising the stream.
	 */
	if (count < LSTP_IPMI_POST_CODE_SIZE)
		return -EINVAL;

	/* Before the O_NONBLOCK test, so a gone node never reads as merely empty. */
	if (lstp_ipmi_postcodes_stopped(post))
		return -ENODEV;

	if (kfifo_is_empty(&post->fifo)) {
		ssize_t ret;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(post->wait_queue,
					       !kfifo_is_empty(&post->fifo) ||
						       lstp_ipmi_postcodes_stopped(post));
		if (ret == -ERESTARTSYS)
			return ret;
		if (lstp_ipmi_postcodes_stopped(post))
			return -ENODEV;
	}

	/* copy_to_user() may fault, so drain to a kernel buffer under the lock. */
	spin_lock_irqsave(&post->lock, flags);
	unsigned int copied = kfifo_out(&post->fifo, tmp_buf, LSTP_IPMI_POST_CODE_SIZE);

	spin_unlock_irqrestore(&post->lock, flags);

	if (copied == 0)
		return -EAGAIN;

	if (copy_to_user(buf, tmp_buf, copied))
		return -EFAULT;

	return copied;
}

static int lstp_ipmi_postcodes_release(struct inode *inode, struct file *file)
{
	struct lstp_ipmi_postcodes *post = lstp_ipmi_postcodes_to_ctx(file);

	spin_lock_irq(&post->lock);
	post->running = false;
	spin_unlock_irq(&post->lock);

	/* May free @post with its embedding context. */
	kref_put(post->owner_ref, post->owner_release);

	return 0;
}

static __poll_t lstp_ipmi_postcodes_poll(struct file *file, poll_table *wait)
{
	struct lstp_ipmi_postcodes *post = lstp_ipmi_postcodes_to_ctx(file);

	poll_wait(file, &post->wait_queue, wait);
	if (lstp_ipmi_postcodes_stopped(post))
		return EPOLLHUP;
	if (!kfifo_is_empty(&post->fifo))
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static const struct file_operations lstp_ipmi_postcodes_fops = {
	.owner = THIS_MODULE,
	.open = lstp_ipmi_postcodes_open,
	.read = lstp_ipmi_postcodes_read,
	.release = lstp_ipmi_postcodes_release,
	.poll = lstp_ipmi_postcodes_poll,
};

/*
 * Build the postcodes context; the node is named "<base_name>-postcodes". An
 * open fd holds @owner_ref, the reference count of the context @post is
 * embedded in, and drops it through @owner_release. On failure, what was built
 * is left for _destroy().
 */
static inline int lstp_ipmi_postcodes_init(struct lstp_ipmi_postcodes *post, struct device *dev,
					   const char *base_name, struct lstp_channel *ch,
					   struct kref *owner_ref,
					   void (*owner_release)(struct kref *ref))
{
	memset(post, 0, sizeof(*post));

	int ret = kfifo_alloc(&post->fifo, LSTP_IPMI_POST_BUFFER_SIZE, GFP_KERNEL);

	if (ret)
		return ret;

	spin_lock_init(&post->lock);
	init_waitqueue_head(&post->wait_queue);

	post->miscdev.name = kasprintf(GFP_KERNEL, "%s-postcodes", base_name);
	if (!post->miscdev.name)
		return -ENOMEM;

	post->miscdev.minor = MISC_DYNAMIC_MINOR;
	post->miscdev.fops = &lstp_ipmi_postcodes_fops;
	post->miscdev.parent = dev;

	post->ch = ch;
	post->owner_ref = owner_ref;
	post->owner_release = owner_release;

	return 0;
}

/*
 * Devres action booked by _start(). @stopped before the wake, as in
 * lstp_ipmi_stop_and_deregister().
 */
static inline void lstp_ipmi_postcodes_stop(void *data)
{
	struct lstp_ipmi_postcodes *post = data;

	misc_deregister(&post->miscdev);
	WRITE_ONCE(post->stopped, true);
	wake_up_all(&post->wait_queue);
}

/*
 * Register the node, booking its teardown on the parent it was built for, so
 * it unwinds with the bind that registered it.
 */
static inline int lstp_ipmi_postcodes_start(struct lstp_ipmi_postcodes *post)
{
	int ret = misc_register(&post->miscdev);

	if (ret)
		return ret;

	return devm_add_action_or_reset(post->miscdev.parent, lstp_ipmi_postcodes_stop, post);
}

/* Free what _init() built, all or part, from a zeroed @post; no fds may remain. */
static inline void lstp_ipmi_postcodes_destroy(struct lstp_ipmi_postcodes *post)
{
	kfifo_free(&post->fifo);
	kfree(post->miscdev.name);
}

static inline bool lstp_ipmi_postcodes_is_postcode(const u8 *payload, u16 len)
{
	if (len < LSTP_IPMI_POST_MIN_LEN)
		return false;

	return ((payload[0] >> 2) == LSTP_IPMI_POST_NETFN && payload[1] == LSTP_IPMI_POST_CMD &&
		payload[2] == LSTP_IPMI_POST_GROUP);
}

/* Queue the 9 postcode bytes at LSTP_IPMI_POST_CODE_OFFSET; called from ops->irq_packet. */
static inline void lstp_ipmi_postcodes_send(struct lstp_ipmi_postcodes *post, const u8 *payload,
					    u16 len)
{
	unsigned long flags;

	if (len < (LSTP_IPMI_POST_CODE_OFFSET + LSTP_IPMI_POST_CODE_SIZE))
		return;

	spin_lock_irqsave(&post->lock, flags);

	/* Whole-ring reset on overflow, unlike the request fifo: only the newest matters. */
	if ((LSTP_IPMI_POST_BUFFER_SIZE - kfifo_len(&post->fifo)) < LSTP_IPMI_POST_CODE_SIZE)
		kfifo_reset(&post->fifo);

	unsigned int rc = kfifo_in(&post->fifo, &payload[LSTP_IPMI_POST_CODE_OFFSET],
				   LSTP_IPMI_POST_CODE_SIZE);
	if (rc != LSTP_IPMI_POST_CODE_SIZE) {
		kfifo_reset(&post->fifo);
		spin_unlock_irqrestore(&post->lock, flags);
		return;
	}

	spin_unlock_irqrestore(&post->lock, flags);

	wake_up_all(&post->wait_queue);
}

#else /* !CONFIG_SEPARATE_LSTP_IPMI_POSTCODES */

struct lstp_ipmi_postcodes {
	char __dummy; /* Avoid empty struct warnings */
};

static inline int lstp_ipmi_postcodes_init(struct lstp_ipmi_postcodes *post, struct device *dev,
					   const char *base_name, struct lstp_channel *ch,
					   struct kref *owner_ref,
					   void (*owner_release)(struct kref *ref))
{
	return 0;
}

static inline int lstp_ipmi_postcodes_start(struct lstp_ipmi_postcodes *post)
{
	return 0;
}

static inline void lstp_ipmi_postcodes_destroy(struct lstp_ipmi_postcodes *post)
{
}

static inline bool lstp_ipmi_postcodes_is_postcode(const u8 *payload, u16 len)
{
	return false;
}

static inline void lstp_ipmi_postcodes_send(struct lstp_ipmi_postcodes *post, const u8 *payload,
					    u16 len)
{
}

#endif /* CONFIG_SEPARATE_LSTP_IPMI_POSTCODES */

#endif /* __LSTP_IPMI_POSTCODES_H */
