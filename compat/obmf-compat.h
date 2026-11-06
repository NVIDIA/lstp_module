/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Out-of-tree build only: what the code needs from kernels older than
 * the one it is written for (v7.3). Not part of the upstream code; Kbuild
 * includes it ahead of every source file. Each fallback is the v7.3
 * definition, used only where the target kernel lacks it.
 */

#ifndef __OBMF_COMPAT_H
#define __OBMF_COMPAT_H

#include <linux/cleanup.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

/*
 * Symbol namespaces are string literals from 6.13 on. 6.12 applies
 * __stringify() to the namespace (include/linux/export.h:77 and
 * include/linux/module.h:301 @ rpi-6.12.y), which would quote the
 * literal a second time. These are the v7.3 definitions
 * (include/linux/export.h:92 and include/linux/module.h:302 @ v7.3-rc6),
 * so they change nothing on newer kernels.
 */
#undef EXPORT_SYMBOL_NS_GPL
#define EXPORT_SYMBOL_NS_GPL(sym, ns) __EXPORT_SYMBOL(sym, "GPL", ns)

#undef MODULE_IMPORT_NS
#define MODULE_IMPORT_NS(ns) MODULE_INFO(import_ns, ns)

/* include/linux/gfp.h:17-18 @ v7.3-rc6; not in v6.12 */
#ifndef default_gfp
#define __default_gfp(a, b, ...) b
#define default_gfp(...) __default_gfp(, ##__VA_ARGS__, GFP_KERNEL)
#endif

/*
 * The allocator family is in v7.3 (include/linux/slab.h:1124-1160 @
 * v7.3-rc6) and in later 6.12 stable releases (:938-974 @ rpi-6.12.y),
 * not in v6.12. Neither 6.12 form nor these fallbacks set a
 * __counted_by() counter; the code assigns it.
 */
#ifndef kzalloc_obj
#define kzalloc_obj(P, ...) kzalloc(sizeof(P), default_gfp(__VA_ARGS__))
#endif

#ifndef kmalloc_obj
#define kmalloc_obj(P, ...) kmalloc(sizeof(P), default_gfp(__VA_ARGS__))
#endif

#ifndef kzalloc_objs
#define kzalloc_objs(P, COUNT, ...) \
	kzalloc(size_mul(sizeof(P), COUNT), default_gfp(__VA_ARGS__))
#endif

#ifndef kmalloc_flex
#define kmalloc_flex(P, FAM, COUNT, ...) \
	kmalloc(struct_size_t(typeof(P), FAM, COUNT), default_gfp(__VA_ARGS__))
#endif

#ifndef kzalloc_flex
#define kzalloc_flex(P, FAM, COUNT, ...) \
	kzalloc(struct_size_t(typeof(P), FAM, COUNT), default_gfp(__VA_ARGS__))
#endif

/* include/linux/cleanup.h:250 @ v7.3-rc6 (:237 @ rpi-6.12.y; not in v6.12) */
#ifndef retain_and_null_ptr
#define retain_and_null_ptr(p) ((void)__get_and_null(p, NULL))
#endif

/* include/linux/property.h:179 @ v7.3-rc6; not in rpi-6.12.y */
#ifndef fwnode_for_each_child_node_scoped
#define fwnode_for_each_child_node_scoped(fwnode, child)          \
	for (struct fwnode_handle *child __free(fwnode_handle) =  \
		     fwnode_get_next_child_node(fwnode, NULL);    \
	     child; child = fwnode_get_next_child_node(fwnode, child))
#endif

/*
 * system_dfl_wq is the unbound system workqueue in v7.3
 * (include/linux/workqueue.h:448-470 @ v7.3-rc6) and in later 6.12
 * stable releases (:442-464 @ rpi-6.12.y), not in v6.12. Kbuild passes
 * OBMF_COMPAT_NO_DFL_WQ when the target's workqueue.h lacks it.
 */
#ifdef OBMF_COMPAT_NO_DFL_WQ
#define system_dfl_wq system_unbound_wq
#endif

/*
 * TTY_DRIVER_RESET_SAVED_TERMIOS is an enum member
 * (include/linux/tty_driver.h:92 @ v7.3-rc6), absent from rpi-6.12.y;
 * Kbuild passes OBMF_COMPAT_NO_RESET_SAVED_TERMIOS there. Without it a
 * reused minor keeps the user's saved termios; the line settings are
 * set again at install.
 */
#ifdef OBMF_COMPAT_NO_RESET_SAVED_TERMIOS
#define TTY_DRIVER_RESET_SAVED_TERMIOS 0
#endif

#endif /* __OBMF_COMPAT_H */
