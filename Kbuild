# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.

# This file's own directory, resolved while it is being read: $(src) does
# not name the source directory in every build mode, and -include, unlike
# -I, gets no $(srctree) prefix.
lstp-src := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
ccflags-y += -include $(lstp-src)/compat/lstp-compat.h

# Kernel functions the compat header falls back on where they are missing.
# The preprocessor cannot test for a function, and a version test misses
# backports, so look for the declaration in the kernel's headers instead.
ifneq ($(shell grep -lsw acpi_get_local_u64_address $(srctree)/include/linux/acpi.h),)
ccflags-y += -DLSTP_HAVE_ACPI_GET_LOCAL_U64_ADDRESS
endif

# The sources sit where they would in a kernel tree, and include the shared
# header as <linux/usb/lstp.h>. NOSTDINC_FLAGS, unlike ccflags-y, comes
# before the kernel's own include paths, so this copy wins over any header
# of that name the kernel ships.
NOSTDINC_FLAGS += -I$(lstp-src)/include

# An out-of-tree build never reads Kconfig, so the options the driver honours
# are set here from make variables, where a direct "make -C <kdir> M=<dir>"
# build sees them too. They go through ccflags-y: kbuild stopped reading
# EXTRA_CFLAGS in 6.15.
ifneq ($(KBUILD_EXTMOD),)
ifneq ($(IPMI_MSG_NUM),0)
ccflags-y += -DCONFIG_USB_LSTP_IPMI_MSG_NUM
endif
ifdef IPMI_POSTCODES
ccflags-y += -DCONFIG_SEPARATE_LSTP_IPMI_POSTCODES
endif
ifdef DEBUG
ccflags-y += -DDEBUG
endif
endif

# The Makefile and the compatibility harness read the module names from the
# obj-m lines; keep each on one line, without a trailing comment.
obj-m := lstp.o
lstp-y := drivers/usb/misc/lstp.o

# The subsystem drivers, each a module of its own on the auxiliary bus.
obj-m += gpio-lstp.o
gpio-lstp-y := drivers/gpio/gpio-lstp.o
obj-m += leds-lstp.o
leds-lstp-y := drivers/leds/leds-lstp.o
obj-m += i2c-lstp.o
i2c-lstp-y := drivers/i2c/busses/i2c-lstp.o
obj-m += spi-lstp.o
spi-lstp-y := drivers/spi/spi-lstp.o
obj-m += lstp_tty.o
lstp_tty-y := drivers/tty/lstp_tty.o
obj-m += ipmi-lstp.o
ipmi-lstp-y := drivers/char/ipmi/ipmi-lstp.o
obj-m += lstp-mmio.o
lstp-mmio-y := drivers/misc/lstp-mmio.o
