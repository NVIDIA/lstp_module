// SPDX-License-Identifier: GPL-2.0-only
/*
 * MMIO channel for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#include <linux/build_bug.h>
#include <linux/cleanup.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/kref.h>
#include <linux/limits.h>
#include <linux/miscdevice.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <linux/usb/lstp.h>

static DEFINE_IDA(lstp_mmio_ida);

enum lstp_mmio_cmd {
	LSTP_MMIO_CMD_READ = 0x00,
	LSTP_MMIO_CMD_WRITE = 0x01,
};

struct lstp_mmio_read_req {
	__le32 offset;
	__le16 length;
} __packed;

struct lstp_mmio_write_req {
	__le32 offset;
	u8 data[];
} __packed;

/* One per bind; the bind and each open fd hold @ref. */
struct lstp_mmio_ctx {
	struct kref ref;
	struct lstp_channel *ch; /* pinned for the life of @ctx */
	struct miscdevice miscdev;
	int ida_id;
	/* held across the gone test and the device I/O; teardown waits on it once @stopped is up */
	struct mutex lock;
	u8 *xfer_buf;
	bool stopped;
};

/*
 * The ida id, and the /dev/lstp_mmio_<n> name built from it, stay taken until the last of
 * the unbind and the last close, so a rebind with an fd still open comes back under another
 * number. Partial-init safe: a failed lstp_mmio_init() unwinds through it as well.
 */
static void lstp_mmio_ctx_release(struct kref *ref)
{
	struct lstp_mmio_ctx *ctx = container_of(ref, struct lstp_mmio_ctx, ref);
	struct lstp_channel *ch = ctx->ch;

	if (ctx->ida_id >= 0)
		ida_free(&lstp_mmio_ida, ctx->ida_id);
	kfree(ctx->miscdev.name);
	mutex_destroy(&ctx->lock);
	kfree(ctx->xfer_buf);
	kfree(ctx);
	lstp_channel_put(ch);
}

/* Devres action for the bind's reference, booked ahead of lstp_mmio_deregister(). */
static void lstp_mmio_ctx_put(void *data)
{
	struct lstp_mmio_ctx *ctx = data;

	kref_put(&ctx->ref, lstp_mmio_ctx_release);
}

static struct lstp_mmio_ctx *lstp_mmio_file_ctx(struct file *file)
{
	return file->private_data;
}

static int lstp_mmio_validate_range(loff_t caller_offset, size_t count)
{
	if (caller_offset < 0 || caller_offset > U32_MAX)
		return -EINVAL;
	if ((u64)count > (u64)U32_MAX - caller_offset + 1)
		return -EINVAL;

	return 0;
}

/* @stopped is per bind and never cleared; see Documentation/driver-api/usb/lstp.rst. */
static bool lstp_mmio_gone(struct lstp_mmio_ctx *ctx)
{
	return READ_ONCE(ctx->stopped) || lstp_channel_disconnected(ctx->ch);
}

static int lstp_mmio_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct lstp_mmio_ctx *ctx = container_of(miscdev, struct lstp_mmio_ctx, miscdev);

	if (lstp_mmio_gone(ctx))
		return -ENODEV;

	/*
	 * The fd holds @ctx and @ctx its channel pin, so neither dangles past an unbind or a
	 * disconnect. misc_mtx spans this ->open and misc_deregister(), which the bind's @ref
	 * outlasts, so this cannot race the final put. I/O is still refused through @stopped.
	 */
	kref_get(&ctx->ref);
	file->private_data = ctx;
	return 0;
}

static ssize_t lstp_mmio_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct lstp_mmio_ctx *ctx = lstp_mmio_file_ctx(file);
	loff_t caller_offset = *ppos;
	size_t done = 0;
	ssize_t ret = lstp_mmio_validate_range(caller_offset, count);

	if (ret)
		return ret;

	mutex_lock(&ctx->lock);
	if (lstp_mmio_gone(ctx)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	struct lstp_channel *ch = ctx->ch;
	struct lstp_mmio_read_req req;

	/* Probe enforces bulk_rx_size >= LSTP_USB_EP_MIN_SIZE; anchor the fixed request here. */
	BUILD_BUG_ON(sizeof(struct lstp_header) + sizeof(req) > LSTP_USB_EP_MIN_SIZE);

	/* A length of zero is a valid READ, so a count of zero goes to the device too. */
	if (!count) {
		req = (struct lstp_mmio_read_req){
			.offset = cpu_to_le32((u32)caller_offset),
			.length = cpu_to_le16(0),
		};

		ret = lstp_request(ch, LSTP_MMIO_CMD_READ, &req, sizeof(req), NULL, 0, NULL, NULL);
		goto out_unlock;
	}

	while (done < count) {
		size_t chunk = min3(count - done, ch->max_rx_payload, (size_t)U16_MAX);

		/* Teardown lands while we hold @lock; stop at a chunk edge. */
		if (lstp_mmio_gone(ctx)) {
			ret = done ?: -ENODEV;
			goto out;
		}

		if (signal_pending(current)) {
			ret = done ?: -ERESTARTSYS;
			goto out;
		}

		req = (struct lstp_mmio_read_req){
			.offset = cpu_to_le32((u32)(caller_offset + done)),
			.length = cpu_to_le16((u16)chunk),
		};

		ret = lstp_request(ch, LSTP_MMIO_CMD_READ, &req, sizeof(req), ctx->xfer_buf, chunk,
				   NULL, NULL);
		if (ret) {
			ret = done ?: ret;
			goto out;
		}

		if (copy_to_user(buf + done, ctx->xfer_buf, chunk)) {
			ret = done ?: -EFAULT;
			goto out;
		}

		done += chunk;
	}

	ret = done;

out:
	if (done)
		*ppos = caller_offset + done;
out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

static ssize_t lstp_mmio_write(struct file *file, const char __user *buf, size_t count,
			       loff_t *ppos)
{
	struct lstp_mmio_ctx *ctx = lstp_mmio_file_ctx(file);
	loff_t caller_offset = *ppos;
	size_t done = 0;
	ssize_t ret = lstp_mmio_validate_range(caller_offset, count);

	if (ret)
		return ret;

	mutex_lock(&ctx->lock);
	if (lstp_mmio_gone(ctx)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	struct lstp_channel *ch = ctx->ch;
	struct lstp_mmio_write_req *req = (struct lstp_mmio_write_req *)ctx->xfer_buf;

	/* Probe enforces bulk_tx_size >= LSTP_USB_EP_MIN_SIZE; anchor the fixed request here. */
	BUILD_BUG_ON(sizeof(struct lstp_header) + sizeof(*req) > LSTP_USB_EP_MIN_SIZE);

	/*
	 * A request carrying only the offset is a valid zero-length WRITE, so a count of zero
	 * goes to the device; what it does there is device-specific, not necessarily a no-op.
	 */
	if (!count) {
		req->offset = cpu_to_le32((u32)caller_offset);

		ret = lstp_request(ch, LSTP_MMIO_CMD_WRITE, req, sizeof(*req), NULL, 0, NULL, NULL);
		goto out_unlock;
	}

	while (done < count) {
		size_t chunk = min(count - done, ch->max_tx_payload - sizeof(*req));

		/* Teardown lands while we hold @lock; stop at a chunk edge. */
		if (lstp_mmio_gone(ctx)) {
			ret = done ?: -ENODEV;
			goto out;
		}

		if (signal_pending(current)) {
			ret = done ?: -ERESTARTSYS;
			goto out;
		}

		if (copy_from_user(req->data, buf + done, chunk)) {
			ret = done ?: -EFAULT;
			goto out;
		}

		req->offset = cpu_to_le32((u32)(caller_offset + done));

		ret = lstp_request(ch, LSTP_MMIO_CMD_WRITE, req, struct_size(req, data, chunk),
				   NULL, 0, NULL, NULL);
		if (ret) {
			ret = done ?: ret;
			goto out;
		}

		done += chunk;
	}

	ret = done;

out:
	if (done)
		*ppos = caller_offset + done;
out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

static loff_t lstp_mmio_llseek(struct file *file, loff_t offset, int whence)
{
	return fixed_size_llseek(file, offset, whence, U32_MAX);
}

static int lstp_mmio_release(struct inode *inode, struct file *file)
{
	struct lstp_mmio_ctx *ctx = lstp_mmio_file_ctx(file);

	kref_put(&ctx->ref, lstp_mmio_ctx_release);

	return 0;
}

static const struct file_operations lstp_mmio_fops = {
	.owner = THIS_MODULE,
	.open = lstp_mmio_open,
	.read = lstp_mmio_read,
	.write = lstp_mmio_write,
	.llseek = lstp_mmio_llseek,
	.release = lstp_mmio_release,
};

/* MMIO handles no unsolicited packets, so the context never goes through @ch->priv. */
static struct lstp_mmio_ctx *lstp_mmio_init(struct lstp_channel *ch)
{
	struct lstp_mmio_ctx *ctx = kzalloc_obj(*ctx);
	int ret;

	if (!ctx)
		return ERR_PTR(-ENOMEM);

	/*
	 * Dropping @ref is the only unwind from here, and the release tolerates the partial
	 * states below because @ida_id starts at -1 and the pointers start NULL.
	 */
	kref_init(&ctx->ref);
	ctx->ch = lstp_channel_get(ch);
	ctx->ida_id = -1;
	mutex_init(&ctx->lock);

	ctx->xfer_buf = kmalloc(max(min_t(size_t, ch->max_rx_payload, U16_MAX), ch->max_tx_payload),
				GFP_KERNEL);
	if (!ctx->xfer_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	ctx->ida_id = ida_alloc(&lstp_mmio_ida, GFP_KERNEL);
	if (ctx->ida_id < 0) {
		ret = ctx->ida_id;
		goto err_put;
	}

	ctx->miscdev.name = kasprintf(GFP_KERNEL, "lstp_mmio_%d", ctx->ida_id);
	if (!ctx->miscdev.name) {
		ret = -ENOMEM;
		goto err_put;
	}

	ctx->miscdev.minor = MISC_DYNAMIC_MINOR;
	ctx->miscdev.fops = &lstp_mmio_fops;
	ctx->miscdev.parent = ch->dev;
	ctx->miscdev.mode = 0600;

	dev_dbg(ch->dev, "%s: ch_%d: Initialized as %s\n", __func__, ch->ch_id, ch->display_name);
	return ctx;

err_put:
	kref_put(&ctx->ref, lstp_mmio_ctx_release);
	return ERR_PTR(ret);
}

static void lstp_mmio_deregister(void *data)
{
	struct lstp_mmio_ctx *ctx = data;

	/*
	 * Flag before mutex, or the devres unwind would wait out a transfer's whole chunk loop,
	 * one USB round trip per chunk for a length userspace chose. The loop bails at its next
	 * chunk and the empty lock section waits only for that, so no transfer is cut mid-chunk.
	 */
	WRITE_ONCE(ctx->stopped, true);
	scoped_guard(mutex, &ctx->lock) {
	}

	misc_deregister(&ctx->miscdev);
}

static int lstp_mmio_start(struct lstp_channel *ch, struct lstp_mmio_ctx *ctx)
{
	int ret = misc_register(&ctx->miscdev);

	if (ret) {
		dev_err(ch->dev, "%s: ch_%d: Could not register miscdevice (%pe)\n", __func__,
			ch->ch_id, ERR_PTR(ret));
		return ret;
	}

	ret = devm_add_action_or_reset(ch->dev, lstp_mmio_deregister, ctx);
	if (ret)
		return ret;

	dev_info(ch->dev, "%s: ch_%d: Started as %s\n", __func__, ch->ch_id, ch->display_name);
	return 0;
}

/* The context reference is booked before the miscdev's action, so it drops after the node. */
static int lstp_mmio_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(auxdev);
	const struct lstp_channel_init_info *info = dev_get_platdata(&auxdev->dev);

	if (!info)
		return -ENODEV;

	struct lstp_mmio_ctx *ctx = lstp_mmio_init(ch);

	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	int ret = devm_add_action_or_reset(ch->dev, lstp_mmio_ctx_put, ctx);

	if (ret)
		return ret;

	return lstp_mmio_start(ch, ctx);
}

static const struct auxiliary_device_id lstp_mmio_id_table[] = { { .name = "lstp.mmio" }, {} };
MODULE_DEVICE_TABLE(auxiliary, lstp_mmio_id_table);

static struct lstp_auxiliary_driver lstp_mmio_drv = {
	.adrv = {
		.name = "mmio",
		.probe = lstp_mmio_probe,
		.id_table = lstp_mmio_id_table,
	},
};
module_lstp_auxiliary_driver(lstp_mmio_drv);

MODULE_IMPORT_NS("LSTP");
MODULE_DESCRIPTION("LSTP MMIO driver");
MODULE_LICENSE("GPL");
