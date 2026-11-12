// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: function device registry
 *
 * One auxiliary device per discovered channel whose type and role have a
 * function driver, named after the type.
 */

#include <linux/acpi.h>
#include <linux/auxiliary_bus.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/idr.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include "obmf-core.h"

struct obmf_fdev_type {
	u8 type;
	u8 role;
	const char *name;
	const char *node;
};

static const struct obmf_fdev_type obmf_fdev_types[] = {
	{ OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, "i2c", "i2c" },
	{ OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, "serial", "serial" },
	{ OBMF_TYPE_SERIAL, OBMF_ROLE_SYMMETRIC, "serial", "serial" },
};

static DEFINE_IDA(obmf_ida);

static const struct obmf_fdev_type *obmf_fdev_type(struct obmf_channel *ch)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(obmf_fdev_types); i++)
		if (obmf_fdev_types[i].type == ch->info.type &&
		    obmf_fdev_types[i].role == ch->info.role)
			return &obmf_fdev_types[i];
	return NULL;
}

static ssize_t name_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct obmf_fdev *fdev = to_obmf_fdev(to_auxiliary_dev(dev));

	return sysfs_emit(buf, "%s\n", fdev->info->name);
}
static DEVICE_ATTR_RO(name);

static ssize_t channel_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct obmf_fdev *fdev = to_obmf_fdev(to_auxiliary_dev(dev));

	return sysfs_emit(buf, "%u\n", fdev->info->number);
}
static DEVICE_ATTR_RO(channel);

static struct attribute *obmf_fdev_attrs[] = { &dev_attr_name.attr,
					       &dev_attr_channel.attr, NULL };
ATTRIBUTE_GROUPS(obmf_fdev);

/* The interface's driver data is the transport */
static ssize_t intf_name_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct obmf_transport *tp = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", tp->core->device_name);
}

static struct device_attribute dev_attr_intf_name =
	__ATTR(name, 0444, intf_name_show, NULL);

static struct attribute *obmf_intf_attrs[] = { &dev_attr_intf_name.attr, NULL };

static const struct attribute_group obmf_intf_group = {
	.name = "obmf",
	.attrs = obmf_intf_attrs,
};

const struct attribute_group *obmf_intf_groups[] = { &obmf_intf_group, NULL };

static void obmf_fdev_release(struct device *dev)
{
	struct obmf_fdev *fdev = to_obmf_fdev(to_auxiliary_dev(dev));

	fwnode_handle_put(dev_fwnode(dev));
	ida_free(&obmf_ida, fdev->adev.id);
	kfree(fdev);
}

#ifdef CONFIG_ACPI
/*
 * An ACPI object carries no class: it matches on its _ADR alone. The
 * channel objects are children of the transport's object, which is the
 * child of the companion whose _ADR is the transport's; without a present
 * one, no channel is described.
 */
static struct fwnode_handle *obmf_find_acpi_node(struct obmf_core *core,
						 struct fwnode_handle *parent,
						 u32 number)
{
	struct acpi_device *adev;

	adev = acpi_find_child_by_adr(to_acpi_device_node(parent),
				      core->tp->acpi_adr);
	if (!adev || !fwnode_device_is_available(acpi_fwnode_handle(adev)))
		return NULL;
	adev = acpi_find_child_by_adr(adev, number);
	return adev ? acpi_fwnode_handle(adev) : NULL;
}
#else
static struct fwnode_handle *obmf_find_acpi_node(struct obmf_core *core,
						 struct fwnode_handle *parent,
						 u32 number)
{
	return NULL;
}
#endif

static bool obmf_node_fits(struct obmf_core *core, struct obmf_channel *ch,
			   const char *class, struct fwnode_handle *node)
{
	u32 reg;

	if (fwnode_property_read_u32(node, "reg", &reg) ||
	    reg != ch->info.number)
		return false;
	if (fwnode_name_eq(node, class))
		return true;
	dev_warn(core->dev, "channel %u: node %pfwP ignored\n", ch->info.number,
		 node);
	return false;
}

/*
 * The child node of the interface for the channel: on ACPI the object
 * whose _ADR is the channel number, below the transport's object (see
 * obmf_find_acpi_node()), else the node whose reg is and whose
 * name is the channel's class. The fwnode iterator skips disabled OF
 * nodes and ACPI objects that are not present, so those children are
 * walked directly: such a node means the channel gets no device.
 */
static struct fwnode_handle *obmf_match_node(struct obmf_core *core,
					     struct obmf_channel *ch,
					     const char *class)
{
	struct fwnode_handle *parent = dev_fwnode(core->dev);
	struct fwnode_handle *node = NULL;

	if (!parent || !class)
		return NULL;
	if (is_acpi_node(parent)) {
		node = obmf_find_acpi_node(core, parent, ch->info.number);
	} else if (is_of_node(parent)) {
		for_each_child_of_node_scoped(to_of_node(parent), np) {
			if (obmf_node_fits(core, ch, class,
					   of_fwnode_handle(np))) {
				node = of_fwnode_handle(no_free_ptr(np));
				break;
			}
		}
	} else {
		fwnode_for_each_child_node_scoped(parent, child) {
			if (obmf_node_fits(core, ch, class, child)) {
				node = no_free_ptr(child);
				break;
			}
		}
	}
	if (node && !fwnode_device_is_available(node)) {
		fwnode_handle_put(node);
		return ERR_PTR(-ENODEV);
	}
	return node;
}

/* Bind step 5: firmware nodes, then the channel table goes live */
void obmf_bus_publish(struct obmf_core *core)
{
	const struct obmf_fdev_type *t;
	struct fwnode_handle *node;
	struct obmf_channel *ch;
	int i;

	for (i = 1; i < OBMF_NR_CHANNELS; i++) {
		ch = core->chans[i];
		t = ch ? obmf_fdev_type(ch) : NULL;
		if (!t)
			continue;
		node = obmf_match_node(core, ch, t->node);
		if (IS_ERR(node))
			ch->no_device = true;
		else
			ch->fwnode = node;
	}
	/* Pairs with the receive path: the channels are complete */
	smp_store_release(&core->table, true);
}

static int obmf_fdev_add(struct obmf_core *core, struct obmf_channel *ch,
			 const struct obmf_fdev_type *t)
{
	struct auxiliary_device *adev;
	struct obmf_fdev *fdev;
	int ret;

	fdev = kzalloc_obj(*fdev);
	if (!fdev)
		return -ENOMEM;
	ret = ida_alloc(&obmf_ida, GFP_KERNEL);
	if (ret < 0) {
		kfree(fdev);
		return ret;
	}

	fdev->info = &ch->info;
	fdev->ch = ch;
	adev = &fdev->adev;
	adev->name = t->name;
	adev->id = ret;
	adev->dev.parent = core->dev;
	adev->dev.release = obmf_fdev_release;
	adev->dev.groups = obmf_fdev_groups;
	device_set_node(&adev->dev, fwnode_handle_get(ch->fwnode));

	ret = auxiliary_device_init(adev);
	if (ret) {
		fwnode_handle_put(ch->fwnode);
		ida_free(&obmf_ida, adev->id);
		kfree(fdev);
		return ret;
	}
	ret = auxiliary_device_add(adev);
	if (ret) {
		auxiliary_device_uninit(adev);
		return ret;
	}
	ch->fdev = fdev;
	return 0;
}

static void obmf_debugfs_ch(struct obmf_core *core, struct obmf_channel *ch);

/* Bind step 6; a channel's debugfs directory comes with its device */
void obmf_bus_add(struct obmf_core *core)
{
	const struct obmf_fdev_type *t;
	struct obmf_channel *ch;
	int i, ret;

	for (i = 1; i < OBMF_NR_CHANNELS; i++) {
		ch = core->chans[i];
		if (!ch || ch->no_device)
			continue;
		t = obmf_fdev_type(ch);
		if (!t)
			continue;
		ret = obmf_fdev_add(core, ch, t);
		if (ret)
			dev_warn(core->dev, "channel %d: no device: %d\n", i,
				 ret);
		else
			obmf_debugfs_ch(core, ch);
	}
}

/* Unbind step 4, in reverse order of creation */
void obmf_bus_del(struct obmf_core *core)
{
	struct obmf_channel *ch;
	int i;

	for (i = OBMF_NR_CHANNELS - 1; i > 0; i--) {
		ch = core->chans[i];
		if (!ch || !ch->fdev)
			continue;
		auxiliary_device_delete(&ch->fdev->adev);
		auxiliary_device_uninit(&ch->fdev->adev);
		ch->fdev = NULL;
	}
}

/*
 * Remove steps 3 and 4. The flush comes first: a delivery still running
 * can keep its record after unpublish has returned.
 */
static void obmf_fdev_disable(void *data)
{
	struct obmf_fdev *fdev = data;
	struct obmf_channel *ch = fdev->ch;

	flush_work(&ch->work);
	obmf_ch_answer_leftovers(ch);
	ch->bound = false;
}

static int obmf_fdev_usable(struct obmf_fdev *fdev)
{
	int state = obmf_core_state(fdev->ch->core);

	if (WARN_ON_ONCE(!fdev->adev.dev.driver))
		return -EINVAL;
	if (state == OBMF_FAILED || state == OBMF_STOPPING ||
	    state == OBMF_STOPPED)
		return -ENODEV;
	return 0;
}

/**
 * devm_obmf_enable - first call of a function driver's probe
 * @fdev: the function device
 *
 * Books the steps that end the binding: leftover device requests get the
 * default answer. The device enables its channels; the core never
 * switches one on or off.
 *
 * Return: 0 or a negative errno, -ENODEV once the device has stopped or
 * failed.
 */
int devm_obmf_enable(struct obmf_fdev *fdev)
{
	struct obmf_channel *ch = fdev->ch;
	int ret;

	ret = obmf_fdev_usable(fdev);
	if (ret)
		return ret;

	ch->bound = true;
	return devm_add_action_or_reset(&fdev->adev.dev, obmf_fdev_disable,
					fdev);
}
EXPORT_SYMBOL_NS_GPL(devm_obmf_enable, "OBMF");

static void obmf_fdev_unpublish(void *data)
{
	struct obmf_fdev *fdev = data;
	struct obmf_channel *ch = fdev->ch;

	guard(mutex)(&ch->dispatch_lock);
	ch->ops = NULL;
	ch->publisher = NULL;
}

/**
 * devm_obmf_publish - last call of a function driver's probe
 * @fdev: the function device
 * @ops: device request operations, or NULL
 *
 * From here on device requests go to @ops; before, they get the default
 * answer. Needs devm_obmf_enable() first.
 *
 * Return: 0 or a negative errno, -ENODEV once the device has stopped or
 * failed, -EINVAL before devm_obmf_enable().
 */
int devm_obmf_publish(struct obmf_fdev *fdev, const struct obmf_fdev_ops *ops)
{
	struct obmf_channel *ch = fdev->ch;
	int ret;

	ret = obmf_fdev_usable(fdev);
	if (ret)
		return ret;
	if (WARN_ON_ONCE(!ch->bound))
		return -EINVAL;

	scoped_guard(mutex, &ch->dispatch_lock) {
		ch->ops = ops;
		ch->publisher = fdev;
	}
	return devm_add_action_or_reset(&fdev->adev.dev, obmf_fdev_unpublish,
					fdev);
}
EXPORT_SYMBOL_NS_GPL(devm_obmf_publish, "OBMF");

/**
 * obmf_fdev_label - a name for what the function driver registers
 * @fdev: the function device
 * @buf: the label
 * @size: size of @buf
 *
 * "<device name>_<channel name>", the device name cut to fit; the channel
 * name alone, or the device name of @fdev, when the device gives fewer
 * names.
 */
void obmf_fdev_label(struct obmf_fdev *fdev, char *buf, size_t size)
{
	struct obmf_core *core = fdev->ch->core;
	const char *dname = core->device_name;
	const char *cname = fdev->info->name;
	size_t dlen, room;

	if (*dname && *cname && size > strlen(cname) + 2) {
		room = size - strlen(cname) - 2;
		dlen = strlen(dname);
		while (dlen > room || (dlen && (dname[dlen] & 0xc0) == 0x80))
			dlen--;
		snprintf(buf, size, "%.*s_%s", (int)dlen, dname, cname);
		return;
	}
	if (*cname) {
		dev_warn(&fdev->adev.dev, "no device name, label '%s'\n",
			 cname);
		strscpy(buf, cname, size);
		return;
	}
	dev_warn(&fdev->adev.dev, "no channel name, label '%s'\n",
		 dev_name(&fdev->adev.dev));
	strscpy(buf, dev_name(&fdev->adev.dev), size);
}
EXPORT_SYMBOL_NS_GPL(obmf_fdev_label, "OBMF");

/**
 * obmf_fdev_protocol_error - count a malformed message a driver found
 * @fdev: the function device
 */
void obmf_fdev_protocol_error(struct obmf_fdev *fdev)
{
	obmf_ch_count(fdev->ch, OBMF_CH_PROTOCOL_ERRORS);
}
EXPORT_SYMBOL_NS_GPL(obmf_fdev_protocol_error, "OBMF");

static struct dentry *obmf_debugfs_root;

static const char *const obmf_if_names[OBMF_IF_NR] = {
	[OBMF_IF_RX_MESSAGES] = "rx_messages",
	[OBMF_IF_TX_MESSAGES] = "tx_messages",
	[OBMF_IF_RX_MALFORMED] = "rx_malformed",
	[OBMF_IF_RX_NO_CHANNEL] = "rx_no_channel",
	[OBMF_IF_RX_URB_ERRORS] = "rx_urb_errors",
	[OBMF_IF_TX_URB_ERRORS] = "tx_urb_errors",
	[OBMF_IF_STALLS] = "stalls",
	[OBMF_IF_FATAL_ERRORS] = "fatal_errors",
};

static const char *const obmf_ch_names[OBMF_CH_NR] = {
	[OBMF_CH_TX_REQUESTS] = "tx_requests",
	[OBMF_CH_RX_RESPONSES] = "rx_responses",
	[OBMF_CH_RX_LATE] = "rx_late",
	[OBMF_CH_TIMEOUTS] = "timeouts",
	[OBMF_CH_STATUS_ERRORS] = "status_errors",
	[OBMF_CH_PROTOCOL_ERRORS] = "protocol_errors",
	[OBMF_CH_TX_ERRORS] = "tx_errors",
	[OBMF_CH_RX_DEVICE_REQUESTS] = "rx_device_requests",
	[OBMF_CH_TX_ANSWERS] = "tx_answers",
	[OBMF_CH_DEFAULT_ANSWERS] = "default_answers",
	[OBMF_CH_RX_DROPPED] = "rx_dropped",
	[OBMF_CH_RX_REPLACED] = "rx_replaced",
};

static int obmf_counter_get(void *data, u64 *val)
{
	*val = atomic_long_read(data);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(obmf_counter_fops, obmf_counter_get, NULL, "%llu\n");

static void obmf_debugfs_ch(struct obmf_core *core, struct obmf_channel *ch)
{
	char name[8];
	int i;

	if (!core->dbg)
		return;
	snprintf(name, sizeof(name), "ch%u", ch->info.number);
	ch->dbg = debugfs_create_dir(name, core->dbg);
	for (i = 0; i < OBMF_CH_NR; i++)
		debugfs_create_file_unsafe(obmf_ch_names[i], 0444, ch->dbg,
					   &ch->cnt[i], &obmf_counter_fops);
}

void obmf_debugfs_add(struct obmf_core *core)
{
	int i;

	if (!obmf_debugfs_root)
		return;
	core->dbg = debugfs_create_dir(dev_name(core->dev), obmf_debugfs_root);
	for (i = 0; i < OBMF_IF_NR; i++)
		debugfs_create_file_unsafe(obmf_if_names[i], 0444, core->dbg,
					   &core->cnt[i], &obmf_counter_fops);
	obmf_debugfs_ch(core, core->ch0);
}

void obmf_debugfs_del(struct obmf_core *core)
{
	debugfs_remove(core->dbg);
	core->dbg = NULL;
}

void obmf_debugfs_init(struct dentry *parent)
{
	obmf_debugfs_root = debugfs_create_dir("obmf", parent);
}

void obmf_debugfs_exit(void)
{
	debugfs_remove(obmf_debugfs_root);
	obmf_debugfs_root = NULL;
}
