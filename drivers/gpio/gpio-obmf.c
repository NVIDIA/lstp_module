// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO lines on an OBMF-ICP channel
 *
 * The channel configuration lists the lines, each with a fixed direction.
 * Get Values reads input lines. An output line reads back the value last
 * set, from its default output on; the device is not asked. A request for
 * more lines than one message carries is split. The driver reads none of
 * the GPIO_STATUS bits.
 *
 * An input line is an interrupt source on its edges: Set Interrupt Config
 * tells the device the trigger, and the device reports an edge with an
 * Interrupt Notify or a Datagram. The handlers run nested in the thread
 * that takes the device's requests.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio/driver.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/usb/obmf.h>

/* OBMF-ICP 1.0.0, 5.15.2, Table 77 */
#define OBMF_GPIO_DIR_OUT 0
#define OBMF_GPIO_DIR_IN 1
#define OBMF_GPIO_DRIVE_MAX 2
#define OBMF_GPIO_BIAS_MAX 2

/* GPIO_MAX_NAME_SIZE of the GPIO uAPI, which cuts a longer chip label */
#define OBMF_GPIO_LABEL_SIZE 32

/**
 * struct obmf_gpio - one GPIO channel
 * @gc: the GPIO chip
 * @fdev: the function device
 * @lock: serializes the requests, guards @value, @pend, @rd, @req and @resp
 * @valid: the lines the driver uses
 * @out: the output lines
 * @value: the value last set on each output line
 * @pend: the lines of the Get in flight, each cleared as its entry comes back
 * @rd: lines to read, or values read
 * @per_get: lines one Get Values carries
 * @per_set: lines one Set Values carries
 * @req: request entries
 * @resp: response entries of a Get
 * @irq_lock: the irq bus lock, guards @irq_type and @irq_dev
 * @irq_unmasked: the lines whose interrupt is unmasked
 * @irq_armed: the lines the device may report, read without a lock
 * @irq_type: the Set Interrupt Config trigger of each line, 0 if none
 * @irq_dev: the trigger the device last took for each line
 * @label: the chip label
 */
struct obmf_gpio {
	struct gpio_chip gc;
	struct obmf_fdev *fdev;
	struct mutex lock; /* requests; value, pend, rd, req, resp */
	unsigned long *valid;
	unsigned long *out;
	unsigned long *value;
	unsigned long *pend;
	unsigned long *rd;
	u16 per_get;
	u16 per_set;
	__le16 *req;
	__le16 *resp;
	struct mutex irq_lock; /* irq bus lock; irq_type, irq_dev */
	unsigned long *irq_unmasked;
	unsigned long *irq_armed;
	u8 *irq_type;
	u8 *irq_dev;
	char label[OBMF_GPIO_LABEL_SIZE];
};

/* The value of each line of the @n entries goes to @bits */
static void obmf_gpio_apply(const __le16 *ent, unsigned int n,
			    unsigned long *bits)
{
	unsigned int k;
	u16 e;

	for (k = 0; k < n; k++) {
		e = le16_to_cpu(ent[k]);
		__assign_bit(FIELD_GET(OBMF_GPIO_INDEX, e), bits,
			     FIELD_GET(OBMF_GPIO_DATA, e));
	}
}

/*
 * One Get Values of the @n lines in @req, their values to @bits. Table 19:
 * the echo, then an entry per line with its index and value. Each line
 * comes back once, in any order.
 */
static int obmf_gpio_get_chunk(struct obmf_gpio *og, unsigned int n,
			       unsigned long *bits)
{
	unsigned int k, idx;
	int ret;
	u16 e;

	ret = obmf_request(og->fdev, OBMF_GPIO_GET_VALUES, og->req,
			   n * sizeof(*og->req), og->resp,
			   n * sizeof(*og->resp), NULL,
			   OBMF_DEFAULT_TIMEOUT_MS);
	if (ret)
		return ret;
	for (k = 0; k < n; k++)
		__set_bit(le16_to_cpu(og->req[k]), og->pend);
	for (k = 0; k < n; k++) {
		e = le16_to_cpu(og->resp[k]);
		idx = FIELD_GET(OBMF_GPIO_INDEX, e);
		if (FIELD_GET(OBMF_GPIO_DATA, e) > 1 || idx >= og->gc.ngpio ||
		    !__test_and_clear_bit(idx, og->pend))
			break;
	}
	if (k < n) {
		for (k = 0; k < n; k++)
			__clear_bit(le16_to_cpu(og->req[k]), og->pend);
		obmf_fdev_protocol_error(og->fdev);
		return -EPROTO;
	}
	obmf_gpio_apply(og->resp, n, bits);
	return 0;
}

/* Get Values of the lines in @mask, @per_get lines a message */
static int obmf_gpio_read(struct obmf_gpio *og, const unsigned long *mask,
			  unsigned long *bits)
{
	unsigned int i, n = 0;
	int ret;

	for_each_set_bit(i, mask, og->gc.ngpio) {
		og->req[n++] = cpu_to_le16(i);
		if (n < og->per_get)
			continue;
		ret = obmf_gpio_get_chunk(og, n, bits);
		if (ret)
			return ret;
		n = 0;
	}
	return n ? obmf_gpio_get_chunk(og, n, bits) : 0;
}

/*
 * One Set Values of the @n entries in @req; Table 19: the echo alone. The
 * values are kept only when the device took them.
 */
static int obmf_gpio_set_chunk(struct obmf_gpio *og, unsigned int n)
{
	int ret;

	ret = obmf_request(og->fdev, OBMF_GPIO_SET_VALUES, og->req,
			   n * sizeof(*og->req), NULL, 0, NULL,
			   OBMF_DEFAULT_TIMEOUT_MS);
	if (!ret)
		obmf_gpio_apply(og->req, n, og->value);
	return ret;
}

static int obmf_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct obmf_gpio *og = gpiochip_get_data(gc);
	int ret;

	guard(mutex)(&og->lock);
	/* The value last set is no answer while the device cannot be asked */
	ret = obmf_fdev_errno(og->fdev);
	if (ret)
		return ret;
	if (test_bit(offset, og->out))
		return test_bit(offset, og->value);
	og->req[0] = cpu_to_le16(offset);
	ret = obmf_gpio_get_chunk(og, 1, og->rd);
	return ret ?: test_bit(offset, og->rd);
}

/*
 * Callers may drop the errno of a Set (leds-gpio does), so a failure is
 * reported: of line @line, or of several lines when it is -1. A Set of a
 * line that is not an output, refused here or by the device (-EPERM), and
 * a device that is gone are not.
 */
static int obmf_gpio_set_done(struct obmf_gpio *og, long line, int ret)
{
	if (ret && ret != -ENODEV && ret != -EPERM)
		obmf_fdev_report(og->fdev, OBMF_EV_GPIO_SET, line, ret);
	return ret;
}

static int obmf_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct obmf_gpio *og = gpiochip_get_data(gc);

	guard(mutex)(&og->lock);
	if (!test_bit(offset, og->out))
		return -EPERM;
	og->req[0] = cpu_to_le16(offset | FIELD_PREP(OBMF_GPIO_DATA, !!value));
	return obmf_gpio_set_done(og, offset, obmf_gpio_set_chunk(og, 1));
}

#ifdef OBMF_COMPAT_GPIO_SET_VOID
static void obmf_gpio_set_void(struct gpio_chip *gc, unsigned int offset,
			       int value)
{
	obmf_gpio_set(gc, offset, value);
}
#endif

static int obmf_gpio_get_multiple(struct gpio_chip *gc, unsigned long *mask,
				  unsigned long *bits)
{
	struct obmf_gpio *og = gpiochip_get_data(gc);
	int ret;

	guard(mutex)(&og->lock);
	ret = obmf_fdev_errno(og->fdev);
	if (ret)
		return ret;
	bitmap_and(og->rd, mask, og->out, gc->ngpio);
	bitmap_replace(bits, bits, og->value, og->rd, gc->ngpio);
	bitmap_andnot(og->rd, mask, og->out, gc->ngpio);
	return obmf_gpio_read(og, og->rd, bits);
}

/* Set Values of the lines in @mask to @bits, @per_set lines a message */
static int obmf_gpio_write(struct obmf_gpio *og, const unsigned long *mask,
			   const unsigned long *bits)
{
	unsigned int i, n = 0;
	int ret;
	u16 e;

	for_each_set_bit(i, mask, og->gc.ngpio) {
		e = i | FIELD_PREP(OBMF_GPIO_DATA, test_bit(i, bits));
		og->req[n++] = cpu_to_le16(e);
		if (n < og->per_set)
			continue;
		ret = obmf_gpio_set_chunk(og, n);
		if (ret)
			return ret;
		n = 0;
	}
	return n ? obmf_gpio_set_chunk(og, n) : 0;
}

/* Only the lines in @mask */
static int obmf_gpio_set_multiple(struct gpio_chip *gc, unsigned long *mask,
				  unsigned long *bits)
{
	struct obmf_gpio *og = gpiochip_get_data(gc);

	guard(mutex)(&og->lock);
	if (!bitmap_subset(mask, og->out, gc->ngpio))
		return -EPERM;
	return obmf_gpio_set_done(og, -1, obmf_gpio_write(og, mask, bits));
}

#ifdef OBMF_COMPAT_GPIO_SET_VOID
static void obmf_gpio_set_multiple_void(struct gpio_chip *gc,
					unsigned long *mask,
					unsigned long *bits)
{
	obmf_gpio_set_multiple(gc, mask, bits);
}
#endif

static int obmf_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct obmf_gpio *og = gpiochip_get_data(gc);

	return test_bit(offset, og->out) ? GPIO_LINE_DIRECTION_OUT :
					   GPIO_LINE_DIRECTION_IN;
}

static int obmf_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct obmf_gpio *og = gpiochip_get_data(gc);

	return test_bit(offset, og->out) ? -EINVAL : 0;
}

static int obmf_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				      int value)
{
	struct obmf_gpio *og = gpiochip_get_data(gc);

	if (!test_bit(offset, og->out))
		return -EINVAL;
	return obmf_gpio_set(gc, offset, value);
}

static int obmf_gpio_init_valid_mask(struct gpio_chip *gc,
				     unsigned long *valid_mask,
				     unsigned int ngpios)
{
	struct obmf_gpio *og = gpiochip_get_data(gc);

	bitmap_copy(valid_mask, og->valid, ngpios);
	return 0;
}

#ifdef OBMF_COMPAT_GPIO_VALID_MASK_NEEDS_REQUEST
static int obmf_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	return gpiochip_line_is_valid(gc, offset) ? 0 : -EINVAL;
}
#endif

static void obmf_gpio_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct obmf_gpio *og = gpiochip_get_data(gc);
	irq_hw_number_t line = irqd_to_hwirq(d);

	clear_bit(line, og->irq_unmasked);
	clear_bit(line, og->irq_armed);
	gpiochip_disable_irq(gc, line);
}

static void obmf_gpio_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct obmf_gpio *og = gpiochip_get_data(gc);
	irq_hw_number_t line = irqd_to_hwirq(d);

	gpiochip_enable_irq(gc, line);
	set_bit(line, og->irq_unmasked);
}

/*
 * Edges only: Table 18 does not say when a level that stays is reported
 * again, so the level types are refused. A level type is added here; the
 * rest takes any trigger. Under the bus lock and desc->lock.
 */
static int obmf_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct obmf_gpio *og = gpiochip_get_data(gc);
	irq_hw_number_t line = irqd_to_hwirq(d);

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_RISING:
		og->irq_type[line] = OBMF_GPIO_IRQ_RISING;
		return 0;
	case IRQ_TYPE_EDGE_FALLING:
		og->irq_type[line] = OBMF_GPIO_IRQ_FALLING;
		return 0;
	case IRQ_TYPE_EDGE_BOTH:
		og->irq_type[line] = OBMF_GPIO_IRQ_BOTH;
		return 0;
	}
	return -EINVAL;
}

static void obmf_gpio_irq_bus_lock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct obmf_gpio *og = gpiochip_get_data(gc);

	mutex_lock(&og->irq_lock);
}

/*
 * The only place the device is configured: the line's trigger while it is
 * unmasked, else disable, sent when it differs from what the device last
 * took. A failure keeps that, so the next sync tries again; no caller
 * sees it, so it is reported, apart from a device that is gone. An
 * unmasked line is armed before the device can report it; one masked, or
 * whose trigger the device did not take, is not.
 */
static void obmf_gpio_irq_bus_sync_unlock(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct obmf_gpio *og = gpiochip_get_data(gc);
	irq_hw_number_t line = irqd_to_hwirq(d);
	u8 want = OBMF_GPIO_IRQ_DISABLE;
	__le16 ent;
	int ret;

	if (test_bit(line, og->irq_unmasked))
		want = og->irq_type[line];
	if (want)
		set_bit(line, og->irq_armed);
	if (want != og->irq_dev[line]) {
		ent = cpu_to_le16(line | FIELD_PREP(OBMF_GPIO_DATA, want));
		ret = obmf_request(og->fdev, OBMF_GPIO_SET_INTERRUPT_CONFIG,
				   &ent, sizeof(ent), NULL, 0, NULL,
				   OBMF_DEFAULT_TIMEOUT_MS);
		if (!ret)
			og->irq_dev[line] = want;
		else if (ret != -ENODEV)
			obmf_fdev_report(og->fdev, OBMF_EV_GPIO_IRQ, line, ret);
	}
	if (!want || !og->irq_dev[line])
		clear_bit(line, og->irq_armed);
	mutex_unlock(&og->irq_lock);
}

/* An output gives no interrupt */
static int obmf_gpio_irq_request_resources(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct obmf_gpio *og = gpiochip_get_data(gc);

	if (test_bit(irqd_to_hwirq(d), og->out))
		return -EINVAL;
	return gpiochip_irq_reqres(d);
}

static const struct irq_chip obmf_gpio_irq_chip = {
	.name = "obmf-gpio",
	.irq_mask = obmf_gpio_irq_mask,
	.irq_unmask = obmf_gpio_irq_unmask,
	.irq_set_type = obmf_gpio_irq_set_type,
	.irq_bus_lock = obmf_gpio_irq_bus_lock,
	.irq_bus_sync_unlock = obmf_gpio_irq_bus_sync_unlock,
	.irq_request_resources = obmf_gpio_irq_request_resources,
	.irq_release_resources = gpiochip_irq_relres,
	.flags = IRQCHIP_IMMUTABLE,
};

/*
 * An Interrupt Notify or a Datagram (Table 18): an entry for each line
 * that saw its edge, data 0. One bad entry makes the request malformed:
 * none is taken. A Notify is answered before the handlers run, as they
 * may read lines from a device that waits for the answer. An edge on a
 * line the device was not asked to report is counted and dropped. The
 * other commands get the core's answer. Takes neither lock.
 */
static int obmf_gpio_device_request(struct obmf_fdev *fdev,
				    struct obmf_dreq *dreq)
{
	struct obmf_gpio *og = auxiliary_get_drvdata(&fdev->adev);
	unsigned int n = dreq->len / 2, k, line, irq;
	bool bad = !n || dreq->len % 2;
	u16 e;

	if (dreq->cmd != OBMF_GPIO_INTERRUPT_NOTIFY &&
	    dreq->cmd != OBMF_GPIO_INTERRUPT_NOTIFY_DATAGRAM)
		return -EOPNOTSUPP;
	for (k = 0; k < n && !bad; k++) {
		e = get_unaligned_le16(dreq->data + 2 * k);
		line = FIELD_GET(OBMF_GPIO_INDEX, e);
		bad = FIELD_GET(OBMF_GPIO_DATA, e) || line >= og->gc.ngpio ||
		      !test_bit(line, og->valid);
	}
	if (bad)
		obmf_fdev_bad_request(fdev, 1 + dreq->len);
	if (dreq->answer_due)
		obmf_respond(fdev, dreq,
			     bad ? OBMF_STATUS_INVALID : OBMF_STATUS_SUCCESS,
			     NULL, 0);
	if (bad)
		return 0;

	for (k = 0; k < n; k++) {
		e = get_unaligned_le16(dreq->data + 2 * k);
		line = FIELD_GET(OBMF_GPIO_INDEX, e);
		irq = 0;
		if (test_bit(line, og->irq_armed))
			irq = irq_find_mapping(og->gc.irq.domain, line);
		if (irq)
			handle_nested_irq(irq);
		else
			obmf_fdev_count(fdev, OBMF_EV_GPIO_IRQ_UNUSED);
	}
	return 0;
}

static const struct obmf_fdev_ops obmf_gpio_ops = {
	.device_request = obmf_gpio_device_request,
};

static bool obmf_gpio_line_ok(const struct obmf_gpio_line_cfg *l)
{
	return l->direction <= OBMF_GPIO_DIR_IN && l->default_output <= 1 &&
	       l->drive <= OBMF_GPIO_DRIVE_MAX && l->bias <= OBMF_GPIO_BIAS_MAX;
}

/*
 * GPIO_NAME: UTF-8, NUL-padded, so 32 bytes without a NUL when full. A
 * name with a control character is left out.
 */
static const char *obmf_gpio_name(struct device *dev, const u8 *raw,
				  size_t size)
{
	size_t len = strnlen((const char *)raw, size), i;
	char *name;

	for (i = 0; i < len; i++)
		if (raw[i] < 0x20 || raw[i] == 0x7f)
			return NULL;
	if (!len)
		return NULL;
	name = devm_kzalloc(dev, len + 1, GFP_KERNEL);
	if (name)
		memcpy(name, raw, len);
	return name ?: ERR_PTR(-ENOMEM);
}

/*
 * Table 76: GPIO_COUNT, then a Table 77 entry for each line. A count above
 * 4096, an index with reserved bits set or one listed twice refuses the
 * channel. A line with a direction, default output, drive or bias out of
 * range is left out. The lines are numbered by their index; an index not
 * listed is a hole.
 */
static int obmf_gpio_parse(struct obmf_gpio *og, struct device *dev)
{
	const struct obmf_channel_info *info = og->fdev->info;
	const struct obmf_gpio_cfg_entry *e;
	unsigned int count, i, idx, bad = 0, ngpio = 0;
	const char **names;
	const char *name;

	BUILD_BUG_ON(sizeof(*e) != 50);

	if (info->config_len < OBMF_GPIO_COUNT_LEN)
		return dev_err_probe(dev, -EINVAL,
				     "configuration data too short\n");
	count = get_unaligned_le16(info->config);
	if (count > OBMF_GPIO_LINES)
		return dev_err_probe(dev, -EINVAL, "GPIO_COUNT %u above %u\n",
				     count, OBMF_GPIO_LINES);
	if (info->config_len < OBMF_GPIO_COUNT_LEN + count * sizeof(*e))
		return dev_err_probe(dev, -EINVAL,
				     "configuration data too short for %u lines\n",
				     count);
	e = (const void *)(info->config + OBMF_GPIO_COUNT_LEN);

	unsigned long *seen __free(bitmap) =
		bitmap_zalloc(OBMF_GPIO_LINES, GFP_KERNEL);
	if (!seen)
		return -ENOMEM;
	for (i = 0; i < count; i++) {
		idx = le16_to_cpu(e[i].index);
		if (idx >= OBMF_GPIO_LINES)
			return dev_err_probe(dev, -EINVAL,
					     "GPIO_INDEX %#x out of range\n",
					     idx);
		if (__test_and_set_bit(idx, seen))
			return dev_err_probe(dev, -EINVAL,
					     "GPIO_INDEX %u listed twice\n",
					     idx);
		if (obmf_gpio_line_ok(&e[i].line))
			ngpio = max(ngpio, idx + 1);
		else
			bad++;
	}
	if (bad)
		dev_warn(dev, "%u lines out of range, left out\n", bad);
	if (!ngpio) {
		dev_dbg(dev, "no GPIO lines\n");
		return -ENODEV;
	}

	og->valid = devm_bitmap_zalloc(dev, ngpio, GFP_KERNEL);
	og->out = devm_bitmap_zalloc(dev, ngpio, GFP_KERNEL);
	og->value = devm_bitmap_zalloc(dev, ngpio, GFP_KERNEL);
	og->pend = devm_bitmap_zalloc(dev, ngpio, GFP_KERNEL);
	og->rd = devm_bitmap_zalloc(dev, ngpio, GFP_KERNEL);
	og->irq_unmasked = devm_bitmap_zalloc(dev, ngpio, GFP_KERNEL);
	og->irq_armed = devm_bitmap_zalloc(dev, ngpio, GFP_KERNEL);
	og->irq_type = devm_kzalloc(dev, ngpio, GFP_KERNEL);
	og->irq_dev = devm_kzalloc(dev, ngpio, GFP_KERNEL);
	names = devm_kcalloc(dev, ngpio, sizeof(*names), GFP_KERNEL);
	if (!og->valid || !og->out || !og->value || !og->pend || !og->rd ||
	    !og->irq_unmasked || !og->irq_armed || !og->irq_type ||
	    !og->irq_dev || !names)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		if (!obmf_gpio_line_ok(&e[i].line))
			continue;
		idx = le16_to_cpu(e[i].index);
		__set_bit(idx, og->valid);
		__assign_bit(idx, og->out,
			     e[i].line.direction == OBMF_GPIO_DIR_OUT);
		__assign_bit(idx, og->value, e[i].line.default_output);
		name = obmf_gpio_name(dev, e[i].line.name,
				      sizeof(e[i].line.name));
		if (IS_ERR(name))
			return PTR_ERR(name);
		names[idx] = name;
	}
	og->gc.ngpio = ngpio;
	og->gc.names = names;
	return 0;
}

static int obmf_gpio_probe(struct auxiliary_device *adev,
			   const struct auxiliary_device_id *id)
{
	struct obmf_fdev *fdev = to_obmf_fdev(adev);
	const struct obmf_channel_info *info = fdev->info;
	struct device *dev = &adev->dev;
	struct gpio_irq_chip *girq;
	struct obmf_gpio *og;
	struct gpio_chip *gc;
	unsigned int n;
	int ret;

	ret = devm_obmf_enable(fdev);
	if (ret)
		return ret;

	if (!info->quirks.gpio_per_get || !info->quirks.gpio_per_set)
		return dev_err_probe(dev, -ENODEV,
				     "channel limits %zu/%zu too small\n",
				     info->max_req, info->max_resp);

	og = devm_kzalloc(dev, sizeof(*og), GFP_KERNEL);
	if (!og)
		return -ENOMEM;
	og->fdev = fdev;
	ret = devm_mutex_init(dev, &og->lock);
	if (ret)
		return ret;
	ret = devm_mutex_init(dev, &og->irq_lock);
	if (ret)
		return ret;

	ret = obmf_gpio_parse(og, dev);
	if (ret)
		return ret;
	gc = &og->gc;

	og->per_get = min(info->quirks.gpio_per_get, gc->ngpio);
	og->per_set = min(info->quirks.gpio_per_set, gc->ngpio);
	n = max(og->per_get, og->per_set);
	og->req = devm_kcalloc(dev, n, sizeof(*og->req), GFP_KERNEL);
	og->resp =
		devm_kcalloc(dev, og->per_get, sizeof(*og->resp), GFP_KERNEL);
	if (!og->req || !og->resp)
		return -ENOMEM;

	obmf_fdev_label(fdev, og->label, sizeof(og->label));
	gc->label = og->label;
	gc->parent = dev;
	gc->owner = THIS_MODULE;
	gc->base = -1;
	gc->can_sleep = true;
	gc->init_valid_mask = obmf_gpio_init_valid_mask;
#ifdef OBMF_COMPAT_GPIO_VALID_MASK_NEEDS_REQUEST
	gc->request = obmf_gpio_request;
#endif
	gc->get_direction = obmf_gpio_get_direction;
	gc->direction_input = obmf_gpio_direction_input;
	gc->direction_output = obmf_gpio_direction_output;
	gc->get = obmf_gpio_get;
	gc->get_multiple = obmf_gpio_get_multiple;
#ifndef OBMF_COMPAT_GPIO_SET_VOID
	gc->set = obmf_gpio_set;
	gc->set_multiple = obmf_gpio_set_multiple;
#else
	gc->set = obmf_gpio_set_void;
	gc->set_multiple = obmf_gpio_set_multiple_void;
#endif

	/* Nested, as the device is configured on a bus that sleeps */
	girq = &gc->irq;
	gpio_irq_chip_set_chip(girq, &obmf_gpio_irq_chip);
	girq->handler = handle_simple_irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->threaded = true;

	auxiliary_set_drvdata(adev, og);
	ret = devm_gpiochip_add_data(dev, gc, og);
	if (ret)
		return ret;
	/* Undone before the chip is removed: no notify reaches a removed chip */
	return devm_obmf_publish(fdev, &obmf_gpio_ops);
}

static const struct auxiliary_device_id obmf_gpio_ids[] = {
	{ .name = "obmf.gpio" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, obmf_gpio_ids);

static struct auxiliary_driver obmf_gpio_driver = {
	.probe = obmf_gpio_probe,
	.id_table = obmf_gpio_ids,
};
module_auxiliary_driver(obmf_gpio_driver);

MODULE_DESCRIPTION("GPIO lines on an OBMF-ICP channel");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("OBMF");
