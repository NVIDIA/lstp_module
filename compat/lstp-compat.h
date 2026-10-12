/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Fallbacks for kernel APIs the LSTP driver uses that older supported
 * kernels lack, and switches for the features only this package carries.
 * Force-included by Kbuild ahead of every source file.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#ifndef __LSTP_COMPAT_H
#define __LSTP_COMPAT_H

/* Included here so that the checks below see the kernel's own definitions. */
#include <linux/cleanup.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/version.h>

/*
 * LSTP_COMPAT_* symbols are defined only here. The undefined branch is the
 * upstream form, so the upstream copy removes them with unifdef -U.
 */

/* gpio_chip .set() and .set_multiple() return int since 6.17, void before. */
#if KERNEL_VERSION(6, 17, 0) > LINUX_VERSION_CODE
#define LSTP_COMPAT_GPIO_SET_VOID 1
#endif

/*
 * gpiolib refuses a request for a line outside the chip's valid_mask since
 * 6.15; before, it checked the mask only for a chip with a .request callback.
 */
#if KERNEL_VERSION(6, 15, 0) > LINUX_VERSION_CODE
#define LSTP_COMPAT_GPIO_VALID_MASK_NEEDS_REQUEST 1
#endif

/* struct i2c_algorithm has .xfer since 6.11, only .master_xfer before. */
#if KERNEL_VERSION(6, 11, 0) > LINUX_VERSION_CODE
#define LSTP_COMPAT_I2C_MASTER_XFER 1
#endif

/*
 * acpi_get_local_u64_address(), added in 6.11. Kernels that report an older
 * version can have it backported, as RHEL 9 does, so Kbuild looks for it in
 * the kernel's <linux/acpi.h> and defines LSTP_HAVE_ACPI_GET_LOCAL_U64_ADDRESS
 * if it is there. The fallback is renamed so that it cannot clash with the
 * kernel's, and <linux/acpi.h> comes first, out of the rename's reach.
 */
#if IS_ENABLED(CONFIG_ACPI) && !defined(LSTP_HAVE_ACPI_GET_LOCAL_U64_ADDRESS)
#include <linux/acpi.h>

static inline int lstp_compat_acpi_get_local_u64_address(acpi_handle handle, u64 *addr)
{
	if (ACPI_FAILURE(acpi_evaluate_integer(handle, METHOD_NAME__ADR, NULL, addr)))
		return -ENODATA;
	return 0;
}

#define acpi_get_local_u64_address lstp_compat_acpi_get_local_u64_address
#endif

/* Deployed endpoints have product IDs the ID table does not list. */
#define LSTP_COMPAT_USB_VENDOR_MATCH 1

/* Deployed firmware nodes name the I2C channel's nvidia,retries "retries". */
#define LSTP_COMPAT_I2C_UNPREFIXED_RETRIES 1

/*
 * The auto_bind_spidev parameter, kept for deployed configurations: the SPI
 * core's new_device attribute does the same from userspace.
 */
#define LSTP_COMPAT_AUTO_BIND_SPIDEV 1

/* The IPMI POST-code node, which IPMI_POSTCODES=1 builds. */
#define LSTP_COMPAT_IPMI_POSTCODES 1

/*
 * Symbol namespaces are string literals since 6.13; before that the macros
 * took a bare token and stringified it, so "LSTP" would arrive quoted twice.
 */
#if KERNEL_VERSION(6, 13, 0) > LINUX_VERSION_CODE
#undef EXPORT_SYMBOL_NS_GPL
#define EXPORT_SYMBOL_NS_GPL(sym, ns) __EXPORT_SYMBOL(sym, "GPL", ns)
#undef MODULE_IMPORT_NS
#define MODULE_IMPORT_NS(ns) MODULE_INFO(import_ns, ns)
#endif

/*
 * Typed allocators with optional GFP flags (GFP_KERNEL if omitted), added in
 * 7.0. 6.18.34 has kzalloc_obj() with mandatory flags and no kzalloc_flex(),
 * so a missing default_gfp() selects these, replacing any the kernel has.
 * Unlike 7.0's, this kzalloc_flex() leaves the __counted_by() counter to the
 * caller. @none is always empty: default_gfp() passes nothing before its
 * first comma.
 */
#ifndef default_gfp
#define __default_gfp(none, gfp, ...) (none gfp)
#define default_gfp(...) __default_gfp(, ##__VA_ARGS__, GFP_KERNEL)
#undef kzalloc_obj
#undef kmalloc_objs
#undef kzalloc_objs
#undef kzalloc_flex
#define kzalloc_obj(P, ...) kzalloc(sizeof(P), default_gfp(__VA_ARGS__))
#define kmalloc_objs(P, COUNT, ...) kmalloc_array(COUNT, sizeof(P), default_gfp(__VA_ARGS__))
#define kzalloc_objs(P, COUNT, ...) kcalloc(COUNT, sizeof(P), default_gfp(__VA_ARGS__))
#define kzalloc_flex(P, FAM, COUNT, ...) \
	kzalloc(struct_size_t(typeof(P), FAM, COUNT), default_gfp(__VA_ARGS__))
#endif

/*
 * Scoped OF child iterator and its device_node cleanup class, both added in
 * 6.9. The class is private so that a kernel with DEFINE_FREE(device_node)
 * but no iterator still builds.
 */
#ifndef for_each_child_of_node_scoped
DEFINE_FREE(lstp_of_node_put, struct device_node *, if (_T) of_node_put(_T))
#define for_each_child_of_node_scoped(parent, child)                               \
	for (struct device_node *__lstp_of_parent = (parent),                      \
				*child __free(lstp_of_node_put) =                  \
					of_get_next_child(__lstp_of_parent, NULL); \
	     child; child = of_get_next_child(__lstp_of_parent, child))
#endif

/*
 * SPI transfer failure flag, added in 6.8; RHEL 9 lacks it. Testing for it
 * needs <linux/spi/spi.h>, which this header therefore pulls into every
 * source file.
 */
#ifndef SPI_TRANS_FAIL_IO
#define SPI_TRANS_FAIL_IO BIT(1)
#endif

/*
 * For kernels older than 6.5 that backport most of what the driver needs,
 * such as RHEL 9's 5.14: spi_transfer_cs_change_delay_exec() (exported in
 * 6.5) and led_init_default_state_get() (6.2). The functions are renamed, so
 * a kernel that has them keeps its own; their headers come first, out of the
 * renames' reach.
 */
#if KERNEL_VERSION(6, 5, 0) > LINUX_VERSION_CODE
#include <linux/delay.h>
#include <linux/dev_printk.h>
#include <linux/leds.h>
#include <linux/property.h>
#include <linux/string.h>

/* The SPI core's, with its 10 us default for a zero microsecond delay. */
static inline void lstp_compat_spi_transfer_cs_change_delay_exec(struct spi_message *msg,
								 struct spi_transfer *xfer)
{
	u32 unit = xfer->cs_change_delay.unit;

	if (!xfer->cs_change_delay.value) {
		if (unit == SPI_DELAY_UNIT_USECS)
			fsleep(10);
		return;
	}

	if (spi_delay_exec(&xfer->cs_change_delay, xfer)) {
		dev_err_once(&msg->spi->dev,
			     "Use of unsupported delay unit %i, using default of 10us\n", unit);
		fsleep(10);
	}
}

#define spi_transfer_cs_change_delay_exec lstp_compat_spi_transfer_cs_change_delay_exec

static inline enum led_default_state
lstp_compat_led_init_default_state_get(struct fwnode_handle *fwnode)
{
	const char *state = NULL;

	if (!fwnode_property_read_string(fwnode, "default-state", &state)) {
		if (!strcmp(state, "keep"))
			return LEDS_DEFSTATE_KEEP;
		if (!strcmp(state, "on"))
			return LEDS_DEFSTATE_ON;
	}

	return LEDS_DEFSTATE_OFF;
}

#define led_init_default_state_get lstp_compat_led_init_default_state_get
#endif

#endif
