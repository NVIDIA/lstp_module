// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: message layer
 *
 * One request in flight per channel, matched to its response by the
 * channel number. Requests the device sends are queued per channel and
 * delivered to the bound function driver, or answered by the core with
 * the channel type's default answer.
 */

#include <kunit/static_stub.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "obmf-core.h"
#include "obmf-dialect.h"

/*
 * OBMF-ICP 1.0.0, 5.2, Table 4 and the channel-specific tables. A timeout
 * the device reports is -ETIME, so that it differs from -ETIMEDOUT, the
 * host's wait for the answer running out.
 */
static int obmf_status_errno(u8 type, u8 status)
{
	switch (status) {
	case OBMF_STATUS_SUCCESS:
		return 0;
	case OBMF_STATUS_TIMEOUT:
		return -ETIME;
	case OBMF_STATUS_BUSY:
		return -EBUSY;
	case OBMF_STATUS_UNKNOWN_CHANNEL:
		return -ECHRNG;
	case OBMF_STATUS_SIZE_UNSUPPORTED:
		return -EMSGSIZE;
	case OBMF_STATUS_INVALID:
	case OBMF_STATUS_PERMANENT_ERROR:
		return -EIO;
	}
	if (status < 0x40)
		return -EIO;

	switch (type) {
	case OBMF_TYPE_I2C_CONTROLLER:
		switch (status) {
		case OBMF_STATUS_I2C_TIMEOUT:
			return -ETIME;
		case OBMF_STATUS_I2C_ARB_LOST:
			return -EAGAIN;
		case OBMF_STATUS_I2C_BUS_BUSY:
			return -EBUSY;
		case OBMF_STATUS_I2C_NACK:
			return -ENXIO;
		}
		break;
	case OBMF_TYPE_I3C:
		switch (status) {
		case OBMF_STATUS_I3C_TIMEOUT:
			return -ETIME;
		case OBMF_STATUS_I3C_ARB_LOST:
			return -EAGAIN;
		case OBMF_STATUS_I3C_BUS_BUSY:
			return -EBUSY;
		case OBMF_STATUS_I3C_BAD_CCC_REPLY:
			return -EPROTO;
		}
		break;
	case OBMF_TYPE_SERIAL:
		if (status == OBMF_STATUS_SERIAL_LINE_TIMEOUT)
			return -ETIME;
		break;
	case OBMF_TYPE_GPIO:
		switch (status) {
		case OBMF_STATUS_GPIO_INDEX_UNSUPPORTED:
			return -EINVAL;
		case OBMF_STATUS_GPIO_IRQ_UNSUPPORTED:
			return -EOPNOTSUPP;
		case OBMF_STATUS_GPIO_INVALID_OPERATION:
			return -EPERM;
		}
		break;
	case OBMF_TYPE_CONFIG:
	case OBMF_TYPE_MMIO:
		/* OBMF-ICP 1.0.0, 5.3, Table 7 */
		if (status == 0x40)
			return -EINVAL;
		if (status == 0x41)
			return -EACCES;
		break;
	case OBMF_TYPE_IO:
		if (status == OBMF_STATUS_IO_OUT_OF_RANGE)
			return -EINVAL;
		if (status == OBMF_STATUS_IO_ACCESS_DENIED)
			return -EACCES;
		break;
	case OBMF_TYPE_MEMDEV:
		switch (status) {
		case OBMF_STATUS_MEMDEV_ACCESS_DENIED:
			return -EACCES;
		case OBMF_STATUS_MEMDEV_SIZE_UNSUPPORTED:
		case OBMF_STATUS_MEMDEV_ERASE_MISALIGNED:
			return -EINVAL;
		case OBMF_STATUS_MEMDEV_WRITE_PROTECTED:
			return -EROFS;
		}
		break;
	}
	return -EIO;
}

/*
 * Bits of payload byte 0 a response echoes from its request; 0 for the
 * types whose response does not start with an echo.
 */
static u8 obmf_echo_mask(u8 type)
{
	switch (type) {
	case OBMF_TYPE_I2C_CONTROLLER:
		return GENMASK(6, 0);
	case OBMF_TYPE_GPIO:
	case OBMF_TYPE_IPMI:
	case OBMF_TYPE_SPI:
		return GENMASK(3, 0);
	}
	return 0;
}

/* OBMF-ICP 1.0.0, 5.3, Table 5: the transaction and a reserved byte */
#define OBMF_MMIO_HEAD_LEN 2
#define OBMF_MMIO_TRANSACTION GENMASK(2, 0)

static bool obmf_is_mmio(u8 type)
{
	return type == OBMF_TYPE_CONFIG || type == OBMF_TYPE_MMIO;
}

/*
 * Table 5 lays its head over every MMIO payload, while Tables 9 and 11
 * give a response as the data alone, or nothing. Both forms are taken;
 * a head must echo the transaction. The length is the request's.
 */
static bool obmf_mmio_strip(struct obmf_channel *ch, u8 cmd, const u8 **body,
			    size_t *len, size_t want)
{
	struct obmf_core *core = ch->core;
	int form;

	if (*len == want + OBMF_MMIO_HEAD_LEN &&
	    ((*body)[0] & OBMF_MMIO_TRANSACTION) ==
		    (cmd & OBMF_MMIO_TRANSACTION)) {
		*body += OBMF_MMIO_HEAD_LEN;
		*len = want;
		form = OBMF_MMIO_FORM_HEAD;
	} else if (*len == want) {
		form = OBMF_MMIO_FORM_DATA;
	} else {
		return false;
	}
	if (want && xchg(&core->mmio_form, form) != form)
		dev_dbg(core->dev, "MMIO read responses: %s\n",
			form == OBMF_MMIO_FORM_HEAD ? "2-byte head, then data" :
						      "data only");
	return true;
}

int obmf_core_state(struct obmf_core *core)
{
	return READ_ONCE(core->state);
}

static int obmf_state_errno(int state)
{
	switch (state) {
	case OBMF_INIT:
	case OBMF_LIVE:
	case OBMF_UNBINDING:
		return 0;
	case OBMF_QUIESCED:
		return -EAGAIN;
	case OBMF_FAILED:
		return -EIO;
	}
	return -ENODEV;
}

/**
 * obmf_fdev_errno - what a request on the function device fails with now
 * @fdev: the function device
 *
 * Any context. Return: 0 while requests go out, -EAGAIN while the device
 * is suspended, -EIO after a transport error stopped it, -ENODEV once it
 * is gone.
 */
int obmf_fdev_errno(struct obmf_fdev *fdev)
{
	return obmf_state_errno(READ_ONCE(fdev->ch->core->state));
}
EXPORT_SYMBOL_NS_GPL(obmf_fdev_errno, "OBMF");

/* The failed operation of every published driver, once, after the stop */
static void obmf_core_fail_work(struct work_struct *work)
{
	struct obmf_core *core =
		container_of(work, struct obmf_core, fail_work);
	struct obmf_channel *ch;
	int i;

	/* Pairs with obmf_bus_publish(): the table is complete */
	if (!smp_load_acquire(&core->table))
		return;
	for (i = 1; i < OBMF_NR_CHANNELS; i++) {
		ch = core->chans[i];
		if (!ch)
			continue;
		guard(mutex)
			(&ch->dispatch_lock);
		if (ch->ops && ch->ops->failed)
			ch->ops->failed(ch->publisher);
	}
}

/*
 * Every state edge writes the state, wakes @state_wq, then takes each
 * slot's rx_lock, so a waiter that armed its slot before the edge is seen
 * by @err. An edge to a
 * state that fails requests also ends a hold of obmf_slot_wait_late().
 */
static void obmf_core_wake(struct obmf_core *core, int err)
{
	bool fails = err || obmf_state_errno(READ_ONCE(core->state));
	struct obmf_channel *ch;
	int i;

	wake_up_all(&core->state_wq);
	for (i = 0; i < OBMF_NR_CHANNELS; i++) {
		ch = core->ch0;
		if (i) {
			/* Pairs with obmf_bus_publish(): the table is complete */
			if (!smp_load_acquire(&core->table))
				break;
			ch = core->chans[i];
		}
		if (!ch)
			continue;
		guard(spinlock_irqsave)(&ch->slot.rx_lock);
		if (err && ch->slot.armed) {
			ch->slot.armed = false;
			ch->slot.err = err;
			complete(&ch->slot.done);
		} else if (fails) {
			obmf_slot_late(&ch->slot);
		}
	}
}

static void obmf_core_enter(struct obmf_core *core, int state, int err)
{
	WRITE_ONCE(core->state, state);
	obmf_core_wake(core, err);
}

/* The edge from @from to @to, if @core is still in @from */
static bool obmf_core_move(struct obmf_core *core, int from, int to, int err)
{
	if (cmpxchg(&core->state, from, to) != from)
		return false;
	obmf_core_wake(core, err);
	return true;
}

void obmf_core_fatal(struct obmf_core *core, int err)
{
	int state = READ_ONCE(core->state);

	do {
		if (state == OBMF_FAILED || state == OBMF_STOPPING ||
		    state == OBMF_STOPPED)
			return;
	} while (!try_cmpxchg(&core->state, &state, OBMF_FAILED));
	obmf_count(core, OBMF_IF_FATAL_ERRORS);
	obmf_report_bounded(core, core->dev, OBMF_EV_FATAL, -1, err, 0);
	obmf_core_wake(core, -EIO);
	/* Any context: the bulk IN completion stops the device too */
	queue_work(core->wq, &core->fail_work);
}

/**
 * obmf_report_log - log one event of obmf_report()
 * @dev: the device the line names
 * @ev: the event
 * @ch: its channel, or -1
 * @a: its first value
 * @b: its second value
 * @rs: the call site's ratelimit state, NULL for an event the state
 *	machine bounds
 * @func: the call site, named in the line on suppressed events
 *
 * Any context. A KUnit case can redirect it, so it sees the events raised
 * on its own thread, an interrupt taken on that thread included.
 */
void obmf_report_log(struct device *dev, enum obmf_event ev, int ch, long a,
		     long b, struct ratelimit_state *rs, const char *func)
{
	KUNIT_STATIC_STUB_REDIRECT(obmf_report_log, dev, ev, ch, a, b, rs,
				   func);

	if (rs && !___ratelimit(rs, func))
		return;
	switch (ev) {
	case OBMF_EV_FATAL:
		dev_err(dev, "transport failed: %pe, device stopped\n",
			ERR_PTR(a));
		break;
	case OBMF_EV_IN_STALL:
		dev_warn(dev, "bulk in stalled\n");
		break;
	case OBMF_EV_OUT_STALL:
		dev_warn(dev, "bulk out stalled\n");
		break;
	case OBMF_EV_IN_CLEAR_HALT:
		dev_warn(dev, "bulk in clear halt failed: %pe\n", ERR_PTR(a));
		break;
	case OBMF_EV_OUT_CLEAR_HALT:
		dev_warn(dev, "bulk out clear halt failed: %pe\n", ERR_PTR(a));
		break;
	case OBMF_EV_NO_RESPONSE:
		dev_warn(dev, "channel %d: no response in %ld ms\n", ch, a);
		break;
	case OBMF_EV_DEVICE_ERROR:
		dev_warn(dev, "channel %d: device error %#lx\n", ch, a);
		break;
	case OBMF_EV_BAD_RESPONSE:
		if (a < 0)
			dev_warn(dev, "channel %d: bad response\n", ch);
		else
			dev_warn(dev, "channel %d: bad response, %ld bytes\n",
				 ch, a);
		break;
	case OBMF_EV_RX_SHORT:
		dev_warn(dev, "malformed message: short, %ld bytes\n", a);
		break;
	case OBMF_EV_RX_OVERSIZE:
		dev_warn(dev, "malformed message: size over the transfer, %ld of %ld bytes\n",
			 a, b);
		break;
	case OBMF_EV_RX_TRAILING:
		dev_warn(dev, "malformed message: trailing bytes, %ld\n", a);
		break;
	case OBMF_EV_RX_NO_CHANNEL:
		dev_warn(dev, "message for unknown channel %d\n", ch);
		break;
	case OBMF_EV_BAD_DREQ:
		dev_warn(dev, "channel %d: bad device request, %ld bytes\n", ch,
			 a);
		break;
	case OBMF_EV_ANSWER_NOT_SENT:
		dev_warn(dev, "channel %d: answer not sent: %pe\n", ch,
			 ERR_PTR(a));
		break;
	case OBMF_EV_CH_HEALTH:
		dev_warn(dev, "channel %d: health %ld, error indicator %ld\n",
			 ch, a, b);
		break;
	case OBMF_EV_QUEUE_FULL:
		dev_warn(dev,
			 "channel %d: device request queue full, request %s\n",
			 ch, a ? "refused" : "dropped");
		break;
	case OBMF_EV_TX_DROPPED:
		dev_warn(dev, "dropped %ld bytes: %pe\n", a, ERR_PTR(b));
		break;
	case OBMF_EV_GPIO_SET:
		if (a < 0)
			dev_err(dev, "cannot set lines: %pe\n", ERR_PTR(b));
		else
			dev_err(dev, "cannot set line %ld: %pe\n", a,
				ERR_PTR(b));
		break;
	case OBMF_EV_GPIO_IRQ:
		dev_err(dev,
			"cannot configure the interrupt of line %ld: %pe\n", a,
			ERR_PTR(b));
		break;
	case OBMF_EV_GPIO_IRQ_UNUSED:
	case OBMF_EV_NR:
		break;
	}
}

/* Returns @core, or NULL once the core has stopped */
struct obmf_core *obmf_inflight_enter(struct obmf_core *core)
{
	guard(spinlock)(&core->inflight_lock);
	if (READ_ONCE(core->state) == OBMF_STOPPED)
		return NULL;
	core->inflight++;
	return core;
}

/* Takes the result of obmf_inflight_enter() */
void obmf_inflight_exit(struct obmf_core *core)
{
	if (!core)
		return;
	guard(spinlock)(&core->inflight_lock);
	if (!--core->inflight)
		wake_up(&core->inflight_wq);
}

static bool obmf_inflight_idle(struct obmf_core *core)
{
	guard(spinlock)(&core->inflight_lock);
	return !core->inflight;
}

static void obmf_dreq_work(struct work_struct *work);

/*
 * @max_req: largest payload the core sends on the channel
 * @max_resp: largest response payload function drivers ask for
 * @rx_cap: largest payload the core accepts on the channel
 * All three are cut to the transport's limits, unless the device's own
 * limits are kept.
 */
struct obmf_channel *obmf_ch_alloc(struct obmf_core *core, u8 number,
				   size_t max_req, size_t max_resp,
				   size_t rx_cap)
{
	struct obmf_channel *ch __free(obmf_ch) = kzalloc_obj(*ch, GFP_KERNEL);
	const struct obmf_dialect *dialect = obmf_dialect(core);
	struct obmf_transport *tp = core->tp;
	struct obmf_dreq_rec *rec;
	int i;

	if (!ch)
		return NULL;

	ch->core = core;
	ch->info.number = number;
	if (!dialect || !dialect->keep_ch_limits) {
		max_req = min(max_req, tp->max_tx_msg - OBMF_HDR_LEN);
		max_resp = min(max_resp, tp->max_rx_msg - OBMF_HDR_LEN);
		rx_cap = min(rx_cap, tp->max_rx_msg);
	}
	ch->info.max_req = max_req;
	ch->info.max_resp = max_resp;
	rx_cap = max(rx_cap, ch->info.max_resp);

	mutex_init(&ch->slot.lock);
	spin_lock_init(&ch->slot.rx_lock);
	init_completion(&ch->slot.done);
	spin_lock_init(&ch->dreq_lock);
	INIT_LIST_HEAD(&ch->queue);
	INIT_LIST_HEAD(&ch->kept);
	INIT_WORK(&ch->work, obmf_dreq_work);
	mutex_init(&ch->dispatch_lock);

	ch->tx_cap = OBMF_HDR_LEN + ch->info.max_req;
	ch->tx_buf = kmalloc(ch->tx_cap, GFP_KERNEL);
	ch->slot.cap = rx_cap;
	ch->slot.buf = kmalloc(rx_cap, GFP_KERNEL);
	if (!ch->tx_buf || !ch->slot.buf)
		return NULL;
	if (obmf_dialect_ch_alloc(ch, rx_cap))
		return NULL;

	for (i = 0; i < OBMF_DREQ_RESERVE; i++) {
		rec = kzalloc_flex(*rec, buf, rx_cap);
		if (!rec)
			return NULL;
		rec->cap = rx_cap;
		rec->prealloc = true;
		ch->reserve[i] = rec;
	}
	return_ptr(ch);
}

static void obmf_rec_free(struct obmf_channel *ch, struct obmf_dreq_rec *rec)
{
	int i;

	ch->used--;
	if (!rec->prealloc) {
		kfree(rec);
		return;
	}
	for (i = 0; i < OBMF_DREQ_RESERVE; i++) {
		if (!ch->reserve[i]) {
			ch->reserve[i] = rec;
			return;
		}
	}
	WARN_ON_ONCE(1);
}

void obmf_ch_free(struct obmf_channel *ch)
{
	struct obmf_dreq_rec *rec, *tmp;
	int i;

	if (!ch)
		return;
	list_for_each_entry_safe(rec, tmp, &ch->queue, node) {
		list_del(&rec->node);
		obmf_ch_count(ch, OBMF_CH_RX_DROPPED);
		obmf_rec_free(ch, rec);
	}
	list_for_each_entry_safe(rec, tmp, &ch->kept, node) {
		list_del(&rec->node);
		obmf_rec_free(ch, rec);
	}
	for (i = 0; i < OBMF_DREQ_RESERVE; i++)
		kfree(ch->reserve[i]);
	obmf_dialect_ch_free(ch);
	kfree(ch->slot.buf);
	kfree(ch->tx_buf);
	kfree(ch->config);
	fwnode_handle_put(ch->fwnode);
	mutex_destroy(&ch->dispatch_lock);
	mutex_destroy(&ch->slot.lock);
	kfree(ch);
}

static int obmf_reply_status(struct obmf_channel *ch, int err)
{
	struct obmf_core *core = ch->core;

	obmf_ch_count(ch, OBMF_CH_STATUS_ERRORS);
	/* The device's own failure; every other status is an answer */
	if ((err == -EIO || err == -ECHRNG) && !obmf_core_discovering(core))
		obmf_report(core, core->dev, OBMF_EV_DEVICE_ERROR,
			    ch->info.number, ch->slot.status, 0);
	return err;
}

static int obmf_reply(struct obmf_channel *ch, u8 cmd, void *resp,
		      size_t resp_cap, size_t *resp_len)
{
	struct obmf_slot *slot = &ch->slot;
	struct obmf_core *core = ch->core;
	u8 mask = obmf_echo_mask(ch->info.type);
	const u8 *body = slot->buf;
	size_t len = slot->len;
	int err;

	err = obmf_dialect_status_errno(ch);
	if (err)
		return obmf_reply_status(ch, err);
	if (slot->status)
		return obmf_reply_status(ch, obmf_status_errno(ch->info.type,
							       slot->status));
	if (obmf_is_mmio(ch->info.type)) {
		if (!obmf_mmio_strip(ch, cmd, &body, &len, resp_cap))
			goto proto;
	} else if (mask) {
		if (!len || (body[0] & mask) != (cmd & mask))
			goto proto;
		body++;
		len--;
	}
	if (len > resp_cap || (!resp_len && len != resp_cap))
		goto proto;
	if (len)
		memcpy(resp, body, len);
	if (resp_len)
		*resp_len = len;
	return 0;

proto:
	obmf_ch_count(ch, OBMF_CH_PROTOCOL_ERRORS);
	if (!obmf_core_discovering(core))
		obmf_report(core, core->dev, OBMF_EV_BAD_RESPONSE,
			    ch->info.number, slot->len, 0);
	else if (!ch->info.number)
		dev_dbg(core->dev,
			"channel 0: %zu bytes, %zu or %zu expected\n",
			slot->len, resp_cap, resp_cap + OBMF_MMIO_HEAD_LEN);
	return -EPROTO;
}

/*
 * A device that stops answering during a software unbind would hold every
 * function driver's remove for a timeout per request or answer: the first
 * timeout stops the core instead. One function driver's unbind leaves the
 * core live, and each of its requests ends on its own timeout.
 */
static void obmf_core_timed_out(struct obmf_core *core)
{
	if (obmf_core_move(core, OBMF_UNBINDING, OBMF_STOPPING, -ENODEV))
		dev_warn(core->dev,
			 "no response during unbind, device stopped\n");
}

static unsigned int obmf_wait_ms(struct obmf_core *core,
				 unsigned int timeout_ms)
{
	unsigned int wait;

	wait = min(timeout_ms ?: OBMF_DEFAULT_TIMEOUT_MS, OBMF_MAX_TIMEOUT_MS);
	return max(wait, core->tp->resp_timeout_floor_ms);
}

/*
 * No request carries a tag: after a reply timeout the next request waits
 * for the late reply, which is dropped, or one default timeout, so that
 * reply does not answer it.
 */
static void obmf_slot_wait_late(struct obmf_core *core, struct obmf_slot *slot)
{
	long left = 0;

	scoped_guard(spinlock_irqsave, &slot->rx_lock) {
		if (slot->held)
			left = (long)(slot->held_until - jiffies);
	}
	if (left > 0 && !obmf_state_errno(READ_ONCE(core->state)))
		wait_for_completion_timeout(&slot->done, left);
	scoped_guard(spinlock_irqsave, &slot->rx_lock)
		slot->held = false;
}

/*
 * Arms the slot, sends the @len bytes in tx_buf and waits for the reply,
 * under the slot lock. Returns 0 with the reply in the slot.
 */
int obmf_slot_xfer(struct obmf_channel *ch, u8 cmd, size_t len,
		   unsigned int timeout_ms)
{
	struct obmf_core *core = ch->core;
	struct obmf_slot *slot = &ch->slot;
	unsigned int wait = obmf_wait_ms(core, timeout_ms);
	unsigned long deadline;
	int ret, state;
	long left;

	if (READ_ONCE(slot->held))
		obmf_slot_wait_late(core, slot);
	scoped_guard(spinlock_irqsave, &slot->rx_lock) {
		ret = obmf_state_errno(READ_ONCE(core->state));
		if (!ret) {
			slot->armed = true;
			slot->cmd = cmd;
			slot->err = 0;
			slot->len = 0;
			reinit_completion(&slot->done);
		}
	}
	if (ret)
		return ret;

	obmf_ch_count(ch, OBMF_CH_TX_REQUESTS);
	deadline = jiffies + msecs_to_jiffies(wait);
	ret = core->tp->ops->send(core->tp, ch->tx_buf, len, wait);
	if (ret) {
		scoped_guard(spinlock_irqsave, &slot->rx_lock)
			slot->armed = false;
		obmf_ch_count(ch, OBMF_CH_TX_ERRORS);
		if (ret == -ETIMEDOUT)
			obmf_core_timed_out(core);
		return ret;
	}

	left = max_t(long, (long)(deadline - jiffies), 1);
	if (!wait_for_completion_timeout(&slot->done, left)) {
		deadline = jiffies + msecs_to_jiffies(OBMF_DEFAULT_TIMEOUT_MS);
		scoped_guard(spinlock_irqsave, &slot->rx_lock) {
			if (slot->armed) {
				slot->armed = false;
				slot->held = true;
				slot->held_until = deadline;
				ret = -ETIMEDOUT;
			}
		}
		if (ret) {
			obmf_ch_count(ch, OBMF_CH_TIMEOUTS);
			state = READ_ONCE(core->state);
			if (state == OBMF_LIVE || state == OBMF_QUIESCED)
				obmf_report(core, core->dev,
					    OBMF_EV_NO_RESPONSE,
					    ch->info.number, wait, 0);
			obmf_core_timed_out(core);
			return ret;
		}
	}
	/* Set under rx_lock; reported here, where the caller may sleep */
	if (slot->err == -EPROTO && !obmf_core_discovering(core))
		obmf_report(core, core->dev, OBMF_EV_BAD_RESPONSE,
			    ch->info.number, slot->len, 0);
	return slot->err;
}

int obmf_ch_request(struct obmf_channel *ch, u8 cmd, const void *req,
		    size_t req_len, void *resp, size_t resp_cap,
		    size_t *resp_len, unsigned int timeout_ms)
{
	struct obmf_core *core = ch->core;
	struct obmf_hdr *hdr = (struct obmf_hdr *)ch->tx_buf;
	size_t plen = 1 + req_len;
	size_t len = OBMF_HDR_LEN + plen;
	int ret;

	if (plen > ch->info.max_req)
		return -EMSGSIZE;

	CLASS(obmf_inflight, inflight)(core);
	if (!inflight)
		return -ENODEV;
	guard(mutex)(&ch->slot.lock);

	hdr->channel = ch->info.number;
	hdr->status = 0;
	hdr->size = cpu_to_le16(plen);
	ch->tx_buf[OBMF_HDR_LEN] = cmd;
	if (req_len)
		memcpy(ch->tx_buf + OBMF_HDR_LEN + 1, req, req_len);

	ret = obmf_dialect_encode_request(ch, &len);
	if (ret)
		return ret;

	ret = obmf_slot_xfer(ch, cmd, len, timeout_ms);
	if (!ret)
		ret = obmf_reply(ch, cmd, resp, resp_cap, resp_len);
	return ret;
}

/**
 * obmf_request - send a request on the function device's channel and wait
 * @fdev: the function device
 * @cmd: payload byte 0
 * @req: payload after byte 0
 * @req_len: length of @req
 * @resp: response payload after the echo byte on types that echo, and
 *	after the MMIO head where the device sends one
 * @resp_cap: size of @resp; on MMIO the length of every response
 * @resp_len: length of the response, or NULL if it must be @resp_cap
 * @timeout_ms: response wait, 0 for the default, capped at
 *	%OBMF_MAX_TIMEOUT_MS
 *
 * After a response timeout on the channel, the next request is sent once
 * the late response comes or %OBMF_DEFAULT_TIMEOUT_MS after the timeout,
 * so it can take longer than @timeout_ms. A request also waits for the
 * one in flight on its channel, as the channel carries one at a time.
 * Beyond these, it waits for no other channel's request: it ends within
 * its own wait whatever the device and the other channels do, also in
 * its driver's unbind, and an earlier timeout fails no later request.
 *
 * Sleeps. Returns 0, the errno of a nonzero response status (-ETIME for
 * a timeout the device reports), -ETIMEDOUT when no response came in
 * time, -EMSGSIZE for a request above the channel's limit, -EPROTO for a
 * malformed response, -EAGAIN while the device is suspended, -EIO after a
 * transport error stopped it, or -ENODEV once it is gone.
 */
int obmf_request(struct obmf_fdev *fdev, u8 cmd, const void *req,
		 size_t req_len, void *resp, size_t resp_cap, size_t *resp_len,
		 unsigned int timeout_ms)
{
	return obmf_ch_request(fdev->ch, cmd, req, req_len, resp, resp_cap,
			       resp_len, timeout_ms);
}
EXPORT_SYMBOL_NS_GPL(obmf_request, "OBMF");

static void obmf_rx_response(struct obmf_channel *ch, u8 status, const u8 *data,
			     size_t len)
{
	struct obmf_slot *slot = &ch->slot;

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
	slot->status = status;
	slot->len = len;
	if (len > slot->cap) {
		slot->err = -EPROTO;
		obmf_ch_count(ch, OBMF_CH_PROTOCOL_ERRORS);
	} else {
		memcpy(slot->buf, data, len);
	}
	obmf_ch_count(ch, OBMF_CH_RX_RESPONSES);
	complete(&slot->done);
}

/* OBMF-ICP 1.0.0, 5.5.2, Table 23 */
#define OBMF_SERIAL_RESP_NACK BIT(0)

static bool obmf_answer_due(const struct obmf_channel *ch, u8 cmd)
{
	return !(ch->info.type == OBMF_TYPE_GPIO &&
		 cmd == OBMF_GPIO_INTERRUPT_NOTIFY_DATAGRAM);
}

/* Shortest well-formed device request payload per type */
bool obmf_dreq_len_ok(const struct obmf_channel *ch, const u8 *data, size_t len)
{
	if (!len)
		return false;
	if (ch->info.type == OBMF_TYPE_SERIAL) {
		/* OBMF-ICP 1.0.0, 5.5.1, Table 22 */
		if (len < 3)
			return false;
		return get_unaligned_le16(data + 1) <= len - 3;
	}
	return true;
}

static int obmf_answer_xmit(struct obmf_channel *ch, u8 cmd, u8 status,
			    const void *data, size_t len)
{
	struct obmf_core *core = ch->core;
	struct obmf_transport *tp = core->tp;
	bool echo = obmf_echo_mask(ch->info.type);
	size_t plen = len + echo;
	size_t mlen = OBMF_HDR_LEN + plen;
	struct obmf_hdr *hdr;
	int state, ret;

	if (plen > ch->info.max_req)
		return -EMSGSIZE;
	/*
	 * No answer starts once the core stops: the channel works run until
	 * after the transport's stop. One that passed this check just before
	 * reaches the transport, which refuses it.
	 */
	state = READ_ONCE(core->state);
	if (state == OBMF_STOPPING || state == OBMF_STOPPED)
		return -ENODEV;

	u8 *msg __free(kfree) = kmalloc(mlen, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;
	hdr = (struct obmf_hdr *)msg;
	hdr->channel = ch->info.number;
	hdr->status = OBMF_HDR_RQRESP | (status & OBMF_HDR_STATUS);
	hdr->size = cpu_to_le16(plen);
	if (echo)
		msg[OBMF_HDR_LEN] = cmd;
	if (len)
		memcpy(msg + OBMF_HDR_LEN + echo, data, len);

	ret = obmf_dialect_encode_answer(ch, msg, &mlen);
	if (ret || !mlen)
		return ret;

	ret = tp->ops->send(tp, msg, mlen, OBMF_DEFAULT_TIMEOUT_MS);
	if (ret == -ETIMEDOUT)
		obmf_core_timed_out(core);
	else if (!ret)
		obmf_ch_count(ch, OBMF_CH_TX_ANSWERS);
	return ret;
}

/* No caller can act on a failure: report every one but the device's end */
static int obmf_send_answer(struct obmf_channel *ch, u8 cmd, u8 status,
			    const void *data, size_t len)
{
	int ret = obmf_answer_xmit(ch, cmd, status, data, len);

	if (ret && ret != -ENODEV) {
		obmf_ch_count(ch, OBMF_CH_TX_ERRORS);
		obmf_report(ch->core, ch->core->dev, OBMF_EV_ANSWER_NOT_SENT,
			    ch->info.number, ret, 0);
	}
	return ret;
}

/*
 * The answer for a device request no driver takes: a serial write is
 * refused with nothing accepted, a GPIO interrupt is acknowledged, a GPIO
 * command the device may not send is invalid (Table 16: a Producer
 * requests interrupts only; Table 18: commands 6 to 0xff are reserved),
 * and everything else is answered busy.
 */
static void obmf_default_answer(struct obmf_channel *ch, u8 cmd)
{
	static const u8 serial_nack[3] = { OBMF_SERIAL_RESP_NACK, 0, 0 };
	u8 status = OBMF_STATUS_BUSY;
	const void *body = NULL;
	size_t len = 0;

	switch (ch->info.type) {
	case OBMF_TYPE_SERIAL:
		status = OBMF_STATUS_SUCCESS;
		body = serial_nack;
		len = sizeof(serial_nack);
		break;
	case OBMF_TYPE_GPIO:
		if (cmd == OBMF_GPIO_INTERRUPT_NOTIFY)
			status = OBMF_STATUS_SUCCESS;
		else if (cmd > OBMF_GPIO_INTERRUPT_NOTIFY_DATAGRAM ||
			 ch->info.role == OBMF_ROLE_PRODUCER)
			status = OBMF_STATUS_INVALID;
		break;
	}
	obmf_ch_count(ch, OBMF_CH_DEFAULT_ANSWERS);
	obmf_send_answer(ch, cmd, status, body, len);
}

/*
 * The answer for a device request that could not be decoded: invalid,
 * with no body past an echo, so that the device does not send it again
 * as it would after a NACK
 */
static void obmf_invalid_answer(struct obmf_channel *ch)
{
	obmf_ch_count(ch, OBMF_CH_DEFAULT_ANSWERS);
	obmf_send_answer(ch, 0, OBMF_STATUS_INVALID, NULL, 0);
}

static void obmf_rec_put(struct obmf_channel *ch, struct obmf_dreq_rec *rec)
{
	guard(spinlock_irqsave)(&ch->dreq_lock);
	if (!rec->prealloc)
		ch->queued--;
	obmf_rec_free(ch, rec);
}

static struct obmf_dreq_rec *obmf_rec_get(struct obmf_channel *ch, bool due,
					  size_t len)
{
	struct obmf_dreq_rec *rec = NULL;
	int i;

	for (i = 0; due && i < OBMF_DREQ_RESERVE; i++) {
		if (ch->reserve[i] && ch->reserve[i]->cap >= len) {
			rec = ch->reserve[i];
			ch->reserve[i] = NULL;
			return rec;
		}
	}
	if (ch->queued < OBMF_DREQ_QUEUE_MAX) {
		rec = kmalloc_flex(*rec, buf, len, GFP_ATOMIC);
		if (rec) {
			rec->cap = len;
			rec->prealloc = false;
			ch->queued++;
			return rec;
		}
	}
	for (i = 0; !due && i < OBMF_DREQ_RESERVE; i++) {
		if (ch->reserve[i] && ch->reserve[i]->cap >= len) {
			rec = ch->reserve[i];
			ch->reserve[i] = NULL;
			return rec;
		}
	}
	return NULL;
}

/*
 * Queues a device request, or owes its answer and returns false when the
 * queue is full; @due tells whether it waits for one; under dreq_lock
 */
bool obmf_dreq_add(struct obmf_channel *ch, const u8 *data, size_t len,
		   bool *due)
{
	struct obmf_dreq_rec *rec;

	*due = obmf_answer_due(ch, data[0]);
	rec = obmf_rec_get(ch, *due, len - 1);
	if (rec) {
		rec->kept = false;
		rec->done = false;
		rec->dreq.cmd = data[0];
		rec->dreq.answer_due = *due;
		rec->dreq.data = rec->buf;
		rec->dreq.len = len - 1;
		memcpy(rec->buf, data + 1, len - 1);
		list_add_tail(&rec->node, &ch->queue);
		if (++ch->used >
		    atomic_long_read(&ch->cnt[OBMF_CH_DREQ_MAX_DEPTH]))
			atomic_long_set(&ch->cnt[OBMF_CH_DREQ_MAX_DEPTH],
					ch->used);
		return true;
	}
	if (*due) {
		ch->owed++;
		ch->owed_cmd = data[0];
	}
	return false;
}

/* Counts and reports a dropped device request; not under dreq_lock */
void obmf_dreq_full(struct obmf_channel *ch, bool due)
{
	obmf_ch_count(ch, OBMF_CH_RX_DROPPED);
	obmf_report(ch->core, ch->core->dev, OBMF_EV_QUEUE_FULL,
		    ch->info.number, due, 0);
}

/*
 * Whether every device request of the type waits for an answer: a
 * serial write (5.5.1, Table 22) and an IPMI request (5.9, Table 44).
 * On GPIO the command decides (5.4.1, Table 18).
 */
static bool obmf_type_answers(u8 type)
{
	return type == OBMF_TYPE_SERIAL || type == OBMF_TYPE_IPMI;
}

/*
 * Owes the invalid answer to a device request that cannot be decoded,
 * where the type answers every request; under dreq_lock
 */
void obmf_dreq_owe_invalid(struct obmf_channel *ch)
{
	if (obmf_type_answers(ch->info.type))
		ch->owed_invalid++;
}

static void obmf_rx_dreq(struct obmf_channel *ch, const u8 *data, size_t len)
{
	bool bad, queued = true, due = false;

	obmf_ch_count(ch, OBMF_CH_RX_DEVICE_REQUESTS);
	scoped_guard(spinlock_irqsave, &ch->dreq_lock) {
		bad = !obmf_dreq_len_ok(ch, data, len);
		if (bad)
			obmf_dreq_owe_invalid(ch);
		else
			queued = obmf_dreq_add(ch, data, len, &due);
	}

	if (bad) {
		obmf_count(ch->core, OBMF_IF_RX_MALFORMED);
		if (!obmf_core_discovering(ch->core))
			obmf_report(ch->core, ch->core->dev, OBMF_EV_BAD_DREQ,
				    ch->info.number, len, 0);
	} else if (!queued) {
		obmf_dreq_full(ch, due);
	}
	queue_work(ch->core->wq, &ch->work);
}

static void obmf_dreq_deliver(struct obmf_channel *ch,
			      struct obmf_dreq_rec *rec)
{
	int ret = -ENODEV;
	bool answered;

	scoped_guard(mutex, &ch->dispatch_lock) {
		if (ch->ops && ch->ops->device_request)
			ret = ch->ops->device_request(ch->publisher,
						      &rec->dreq);
	}

	scoped_guard(spinlock_irqsave, &ch->dreq_lock) {
		answered = rec->done;
		if (ret == -EINPROGRESS && !answered) {
			rec->kept = true;
			list_add_tail(&rec->node, &ch->kept);
			return;
		}
	}
	if (!answered && rec->dreq.answer_due) {
		if (!ret)
			dev_warn_once(ch->core->dev,
				      "channel %u: request not answered\n",
				      ch->info.number);
		obmf_default_answer(ch, rec->dreq.cmd);
	} else if (!answered && ret) {
		/* A datagram no driver took */
		obmf_ch_count(ch, OBMF_CH_RX_DROPPED);
	}
	obmf_rec_put(ch, rec);
}

static void obmf_dreq_work(struct work_struct *work)
{
	struct obmf_channel *ch = container_of(work, struct obmf_channel, work);
	struct obmf_dreq_rec *rec;
	bool owed, invalid;
	u8 cmd = 0;

	for (;;) {
		owed = false;
		invalid = false;
		scoped_guard(spinlock_irqsave, &ch->dreq_lock) {
			rec = list_first_entry_or_null(&ch->queue, typeof(*rec),
						       node);
			if (rec) {
				list_del(&rec->node);
			} else if (ch->owed) {
				ch->owed--;
				cmd = ch->owed_cmd;
				owed = true;
			} else if (ch->owed_invalid) {
				ch->owed_invalid--;
				invalid = true;
			}
		}

		if (rec)
			obmf_dreq_deliver(ch, rec);
		else if (owed)
			obmf_default_answer(ch, cmd);
		else if (invalid)
			obmf_invalid_answer(ch);
		else
			break;
	}
}

/* Ends a kept or in-delivery record; returns it if the caller frees it */
static struct obmf_dreq_rec *obmf_dreq_finish(struct obmf_channel *ch,
					      struct obmf_dreq *dreq)
{
	struct obmf_dreq_rec *rec =
		container_of(dreq, struct obmf_dreq_rec, dreq);

	guard(spinlock_irqsave)(&ch->dreq_lock);
	rec->done = true;
	if (!rec->kept)
		return NULL;
	list_del(&rec->node);
	return rec;
}

/**
 * obmf_respond - answer a device request
 * @fdev: the function device
 * @dreq: the request, from the device_request operation
 * @status: header status
 * @data: response payload after the echo byte on types that echo
 * @len: length of @data
 *
 * Sleeps. Ends @dreq whether or not the answer could be sent; called from
 * the device_request operation, @dreq stays readable until it returns.
 */
int obmf_respond(struct obmf_fdev *fdev, struct obmf_dreq *dreq, u8 status,
		 const void *data, size_t len)
{
	struct obmf_channel *ch = fdev->ch;
	struct obmf_dreq_rec *rec;
	int ret = -EINVAL;

	if (dreq->answer_due) {
		CLASS(obmf_inflight, inflight)(ch->core);

		ret = -ENODEV;
		if (inflight)
			ret = obmf_send_answer(ch, dreq->cmd, status, data,
					       len);
	}
	rec = obmf_dreq_finish(ch, dreq);
	if (rec)
		obmf_rec_put(ch, rec);
	return ret;
}
EXPORT_SYMBOL_NS_GPL(obmf_respond, "OBMF");

/**
 * obmf_dreq_drop - end a device request without an answer
 * @fdev: the function device
 * @dreq: the request
 */
void obmf_dreq_drop(struct obmf_fdev *fdev, struct obmf_dreq *dreq)
{
	struct obmf_channel *ch = fdev->ch;
	struct obmf_dreq_rec *rec;

	obmf_ch_count(ch, OBMF_CH_RX_DROPPED);
	rec = obmf_dreq_finish(ch, dreq);
	if (rec)
		obmf_rec_put(ch, rec);
}
EXPORT_SYMBOL_NS_GPL(obmf_dreq_drop, "OBMF");

/* Answer what an unbound driver kept, with the default answers */
void obmf_ch_answer_leftovers(struct obmf_channel *ch)
{
	struct obmf_dreq_rec *rec;

	for (;;) {
		scoped_guard(spinlock_irqsave, &ch->dreq_lock) {
			rec = list_first_entry_or_null(&ch->kept, typeof(*rec),
						       node);
			if (rec) {
				list_del(&rec->node);
				rec->kept = false;
				rec->done = true;
			}
		}
		if (!rec)
			break;
		dev_warn_once(ch->core->dev,
			      "channel %u: device request kept at unbind\n",
			      ch->info.number);
		if (rec->dreq.answer_due)
			obmf_default_answer(ch, rec->dreq.cmd);
		obmf_rec_put(ch, rec);
	}
}

static struct obmf_channel *obmf_core_lookup(struct obmf_core *core, u8 nr)
{
	if (!nr)
		return core->ch0;
	/* Pairs with obmf_bus_publish(): the table is complete */
	if (!smp_load_acquire(&core->table))
		return NULL;
	return core->chans[nr];
}

/**
 * obmf_core_rx - one message from the transport
 * @core: the core
 * @buf: the message, header included
 * @len: its length
 *
 * Any context.
 */
void obmf_core_rx(struct obmf_core *core, const u8 *buf, size_t len)
{
	const struct obmf_dialect *dialect = obmf_dialect(core);
	const struct obmf_hdr *hdr = (const struct obmf_hdr *)buf;
	struct obmf_channel *ch;
	size_t size;
	bool quiet;

	quiet = obmf_core_discovering(core);
	if (len < OBMF_HDR_LEN) {
		obmf_count(core, OBMF_IF_RX_MALFORMED);
		if (!quiet)
			obmf_report(core, core->dev, OBMF_EV_RX_SHORT, -1, len,
				    0);
		return;
	}
	size = le16_to_cpu(hdr->size);
	if (size > len - OBMF_HDR_LEN) {
		obmf_count(core, OBMF_IF_RX_MALFORMED);
		if (!quiet)
			obmf_report(core, core->dev, OBMF_EV_RX_OVERSIZE, -1,
				    size, len - OBMF_HDR_LEN);
		return;
	}
	/* Bytes after the payload the header gives are not part of it */
	if (size < len - OBMF_HDR_LEN) {
		obmf_count(core, OBMF_IF_RX_MALFORMED);
		if (!quiet)
			obmf_report(core, core->dev, OBMF_EV_RX_TRAILING, -1,
				    len - OBMF_HDR_LEN - size, 0);
	}
	obmf_count(core, OBMF_IF_RX_MESSAGES);

	ch = obmf_core_lookup(core, hdr->channel);
	if (!ch) {
		obmf_count(core, OBMF_IF_RX_NO_CHANNEL);
		if (!quiet)
			obmf_report(core, core->dev, OBMF_EV_RX_NO_CHANNEL,
				    hdr->channel, 0, 0);
		return;
	}
	if (dialect && dialect->decode) {
		obmf_dialect_rx(ch, buf, OBMF_HDR_LEN + size);
		return;
	}
	if (hdr->status & OBMF_HDR_RQRESP)
		obmf_rx_response(ch, hdr->status & OBMF_HDR_STATUS,
				 buf + OBMF_HDR_LEN, size);
	else
		obmf_rx_dreq(ch, buf + OBMF_HDR_LEN, size);
}

static void obmf_core_free(struct obmf_core *core)
{
	int i;

	if (!core)
		return;
	for (i = 1; i < OBMF_NR_CHANNELS; i++)
		obmf_ch_free(core->chans[i]);
	obmf_ch_free(core->ch0);
	if (core->wq)
		destroy_workqueue(core->wq);
	mutex_destroy(&core->ch0_lock);
	core->tp->core = NULL;
	kfree(core);
}

DEFINE_FREE(obmf_core, struct obmf_core *, obmf_core_free(_T))

static void obmf_core_bind_work(struct work_struct *work);

struct obmf_core *obmf_core_create(struct obmf_transport *tp)
{
	struct obmf_core *core __free(obmf_core) =
		kzalloc_obj(*core, GFP_KERNEL);
	const struct obmf_dialect *dialect;
	size_t resp, cap;

	if (!core)
		return NULL;

	core->tp = tp;
	core->dev = tp->dev;
	core->state = OBMF_INIT;
	spin_lock_init(&core->inflight_lock);
	init_waitqueue_head(&core->inflight_wq);
	init_waitqueue_head(&core->state_wq);
	mutex_init(&core->ch0_lock);

	/* OBMF-ICP 1.0.0, 5.15: channel 0 takes at least 64-byte payloads */
	core->ch0_chunk = min_t(size_t, 64, tp->max_rx_msg - OBMF_HDR_LEN);
	/* The slot takes a chunk and its MMIO head within the header margin */
	BUILD_BUG_ON(OBMF_MMIO_HEAD_LEN > OBMF_HDR_LEN);

	core->wq =
		alloc_workqueue("obmf-%s", WQ_UNBOUND, 0, dev_name(core->dev));
	if (!core->wq)
		return NULL;
	INIT_WORK(&core->fail_work, obmf_core_fail_work);
	INIT_WORK(&core->bind_work, obmf_core_bind_work);
	resp = core->ch0_chunk;
	cap = core->ch0_chunk + OBMF_HDR_LEN;
	dialect = obmf_dialect(core);
	if (dialect && dialect->ch0_whole_msgs) {
		resp = tp->max_rx_msg;
		cap = tp->max_rx_msg;
	}
	core->ch0 = obmf_ch_alloc(core, 0, tp->max_tx_msg - OBMF_HDR_LEN, resp,
				  cap);
	if (!core->ch0)
		return NULL;
	core->ch0->info.type = OBMF_TYPE_CONFIG;
	tp->core = core;
	return_ptr(core);
}

/* Room left at the end of the line for the count of the channels cut */
#define OBMF_INFO_MORE sizeof(" and 255 more")

static const char *const obmf_kinds[] = {
	[OBMF_TYPE_MMIO] = "MMIO",
	[OBMF_TYPE_GPIO] = "GPIO",
	[OBMF_TYPE_SERIAL] = "serial",
	[OBMF_TYPE_I2C_CONTROLLER] = "I2C",
	[OBMF_TYPE_I2C_TARGET] = "I2C-target",
	[OBMF_TYPE_I3C] = "I3C",
	[OBMF_TYPE_IPMI] = "IPMI",
	[OBMF_TYPE_SPI] = "SPI",
	[OBMF_TYPE_IO] = "IO",
	[OBMF_TYPE_MEMDEV] = "memory",
	[OBMF_TYPE_TPM] = "TPM",
	[OBMF_TYPE_RTC] = "RTC",
};

/*
 * The line of a bound device in @buf, OBMF_INFO_LEN bytes: the protocol
 * and its version, the device name, then the kind, number and name of
 * each channel but channel 0, marked when it got no function device. The
 * channels that do not fit are counted at the end instead.
 */
void obmf_core_info(struct obmf_core *core, char *buf)
{
	const struct obmf_dialect *dialect = obmf_dialect(core);
	const size_t room = OBMF_INFO_LEN - OBMF_INFO_MORE;
	char num[sizeof("type 0xff")];
	const char *sep = ": ", *kind;
	struct obmf_channel *ch;
	size_t len, n;
	int i, more = 0;
	u8 type;

	if (dialect && dialect->name)
		len = scnprintf(buf, room, "%s %u", dialect->name,
				core->version_major);
	else
		len = scnprintf(buf, room, "OBMF-ICP %x.%x",
				core->version_major, core->version_minor);
	len += scnprintf(buf + len, room - len, " device '%s'",
			 core->device_name);
	for (i = 1; i < OBMF_NR_CHANNELS; i++) {
		ch = core->chans[i];
		if (!ch)
			continue;
		if (!more) {
			type = ch->info.type;
			if (type < ARRAY_SIZE(obmf_kinds) && obmf_kinds[type]) {
				kind = obmf_kinds[type];
			} else {
				snprintf(num, sizeof(num), "type 0x%02x", type);
				kind = num;
			}
			n = snprintf(buf + len, room - len, "%s%s ch%d '%s'%s",
				     sep, kind, i, ch->info.name,
				     ch->fdev ? "" : " (no driver)");
			if (n < room - len) {
				len += n;
				sep = ", ";
				continue;
			}
		}
		more++;
	}
	if (more)
		scnprintf(buf + len, OBMF_INFO_LEN - len, " and %d more", more);
}

/**
 * obmf_bind_warn - log a configuration anomaly found by the bind work
 * @core: the core
 * @fmt: printf format
 *
 * A device may list 255 channels, each with its anomaly: the first
 * OBMF_BIND_WARN_MAX of a bind work run are logged, the others only
 * counted, and the run ends with how many it left out. Only the bind
 * work calls this, one run at a time, so the count takes no lock.
 */
void obmf_bind_warn(struct obmf_core *core, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (core->bind_warns++ >= OBMF_BIND_WARN_MAX)
		return;
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	dev_warn(core->dev, "%pV", &vaf);
	va_end(args);
}

/*
 * A core that failed before its function devices were added gives the
 * device up, once. The bind work and resume never run at once: quiesce
 * waits for the bind work, and resume queues it only out of QUIESCED.
 */
static void obmf_core_release(struct obmf_core *core)
{
	if (core->added || core->released ||
	    READ_ONCE(core->state) != OBMF_FAILED)
		return;
	core->released = true;
	core->tp->ops->release(core->tp);
}

/*
 * Discovery, the channel table and the function devices, out of probe: a
 * device can take seconds to answer. Unbind and suspend end a discovery
 * in progress by moving the core out of INIT, then wait for this work; a
 * resume runs it again, past a table already published. A discovery that
 * fails leaves the core failed, with no function device, and the transport
 * gives the device up, so that a new plug or bind runs it again.
 */
static void obmf_core_bind_work(struct work_struct *work)
{
	struct obmf_core *core =
		container_of(work, struct obmf_core, bind_work);
	char info[OBMF_INFO_LEN];
	int ret = 0;

	core->bind_warns = 0;
	if (!core->table) {
		ret = obmf_ctrl_discover(core);
		if (!ret)
			obmf_bus_publish(core);
	}
	if (!ret && !obmf_core_move(core, OBMF_INIT, OBMF_LIVE, 0))
		ret = obmf_state_errno(READ_ONCE(core->state));
	core->bind_err = ret;
	if (ret) {
		obmf_core_move(core, OBMF_INIT, OBMF_FAILED, 0);
		/*
		 * Not when an unbind or a suspend ended discovery; nothing
		 * leaves FAILED while this work runs
		 */
		obmf_core_release(core);
	} else {
		core->added = true;
		obmf_debugfs_add(core);
		obmf_bus_add(core);
		obmf_core_info(core, info);
		dev_info(core->dev, "%s\n", info);
	}
	if (core->bind_warns > OBMF_BIND_WARN_MAX)
		dev_warn(core->dev,
			 "%u more configuration warnings not logged\n",
			 core->bind_warns - OBMF_BIND_WARN_MAX);
}

/* Starts the transport; the bind work does the rest, after probe */
int obmf_core_bind(struct obmf_core *core)
{
	int ret;

	ret = core->tp->ops->start(core->tp);
	if (ret)
		return ret;
	queue_work(core->wq, &core->bind_work);
	return 0;
}

/* Process context, after discovery or with the transport stopped */
static struct obmf_channel *obmf_core_ch(struct obmf_core *core, int i)
{
	return i ? core->chans[i] : core->ch0;
}

/* Unbind steps 1 and 2: stop the transport, wake and drain the callers */
static void obmf_core_stop(struct obmf_core *core)
{
	obmf_core_enter(core, OBMF_STOPPED, -ENODEV);
	core->tp->ops->stop(core->tp);
	wait_event(core->inflight_wq, obmf_inflight_idle(core));
}

/* Unbind step 3, quiesce step 1 */
static void obmf_core_disable_works(struct obmf_core *core)
{
	struct obmf_channel *ch;
	int i;

	for (i = 0; i < OBMF_NR_CHANNELS; i++) {
		ch = obmf_core_ch(core, i);
		if (ch)
			disable_work_sync(&ch->work);
	}
}

/**
 * obmf_core_unbind - take the core down and free it
 * @core: the core
 * @attached: the device is still there: a software unbind
 *
 * A software unbind of a live core removes the function devices while
 * the transport still works, so their drivers can switch their channels
 * off; otherwise the transport stops first. A discovery in progress ends
 * at its next request, and the bind work is waited for first.
 */
void obmf_core_unbind(struct obmf_core *core, bool attached)
{
	obmf_core_move(core, OBMF_INIT, OBMF_STOPPING, -ENODEV);
	disable_work_sync(&core->bind_work);
	if (attached && obmf_core_move(core, OBMF_LIVE, OBMF_UNBINDING, 0)) {
		obmf_bus_del(core);
		obmf_core_stop(core);
		obmf_core_disable_works(core);
	} else {
		obmf_core_stop(core);
		obmf_core_disable_works(core);
		obmf_bus_del(core);
	}

	disable_work_sync(&core->fail_work);
	obmf_debugfs_del(core);
	obmf_core_free(core);
}

/*
 * A request holds its slot's lock until its reply or its timeout, so
 * taking each lock waits for the requests armed before the edge.
 */
static void obmf_core_drain_slots(struct obmf_core *core)
{
	struct obmf_channel *ch;
	int i;

	for (i = 0; i < OBMF_NR_CHANNELS; i++) {
		ch = obmf_core_ch(core, i);
		if (ch) {
			mutex_lock(&ch->slot.lock);
			mutex_unlock(&ch->slot.lock);
		}
	}
}

static void obmf_core_enable_works(struct obmf_core *core)
{
	struct obmf_channel *ch;
	int i;

	for (i = 0; i < OBMF_NR_CHANNELS; i++) {
		ch = obmf_core_ch(core, i);
		if (ch)
			enable_and_queue_work(core->wq, &ch->work);
	}
}

/*
 * Device requests that arrive while quiesced stay queued and are
 * delivered at resume; a request from a function driver fails -EAGAIN. A
 * discovery in progress ends at its next request and runs again at
 * resume.
 */
int obmf_core_quiesce(struct obmf_core *core)
{
	obmf_core_move(core, OBMF_INIT, OBMF_QUIESCED, -EAGAIN);
	flush_work(&core->bind_work);
	obmf_core_disable_works(core);
	if (obmf_core_move(core, OBMF_LIVE, OBMF_QUIESCED, 0))
		obmf_core_drain_slots(core);
	core->tp->ops->quiesce(core->tp);
	return 0;
}

int obmf_core_resume(struct obmf_core *core)
{
	int ret;

	ret = core->tp->ops->resume(core->tp);
	if (ret) {
		obmf_core_fatal(core, ret);
	} else if (obmf_core_move(core, OBMF_QUIESCED,
				  core->added ? OBMF_LIVE : OBMF_INIT, 0)) {
		obmf_core_enable_works(core);
		if (!core->added)
			queue_work(core->wq, &core->bind_work);
		return 0;
	}
	/*
	 * A failed core keeps its works disabled; unbind frees what is queued.
	 * One whose discovery the suspend ended gives the device up here.
	 */
	obmf_core_release(core);
	return ret;
}
