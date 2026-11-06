/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OBMF-ICP over USB: the USB match entries of LSTP devices
 */

#ifndef _OBMF_USB_LSTP_H
#define _OBMF_USB_LSTP_H

#include <linux/usb.h>

struct obmf_usb_proto;

extern const struct obmf_usb_proto obmf_lstp_usb_proto;

/* LSTP 1.0, 2: class 255, subclass 63, protocol 1 */
#define OBMF_LSTP_USB_SUBCLASS 0x3f

#define OBMF_LSTP_DEV(vid, pid)                                          \
	{ USB_DEVICE_AND_INTERFACE_INFO(vid, pid, USB_CLASS_VENDOR_SPEC, \
					OBMF_LSTP_USB_SUBCLASS, 1),      \
	  .driver_info = (kernel_ulong_t)&obmf_lstp_usb_proto }

#define OBMF_LSTP_VENDOR(vid)                                       \
	{ USB_VENDOR_AND_INTERFACE_INFO(vid, USB_CLASS_VENDOR_SPEC, \
					OBMF_LSTP_USB_SUBCLASS, 1), \
	  .driver_info = (kernel_ulong_t)&obmf_lstp_usb_proto }

/* Product entries first, then one entry per vendor */
#define OBMF_LSTP_USB_IDS                                                \
	OBMF_LSTP_DEV(0x0955, 0xcf11), OBMF_LSTP_DEV(0x0424, 0x4940),    \
		OBMF_LSTP_DEV(0x174c, 0x2214), OBMF_LSTP_VENDOR(0x0955), \
		OBMF_LSTP_VENDOR(0x0424), OBMF_LSTP_VENDOR(0x174c),

#endif /* _OBMF_USB_LSTP_H */
