# SPDX-License-Identifier: GPL-2.0-only
#
# Out-of-tree build of the OBMF-ICP drivers. The sources keep their
# kernel tree places; compat/obmf-compat.h covers older kernels.

ccflags-y += -I$(src)/include -include $(src)/compat/obmf-compat.h

# OBMF_DEBUG=y: the dev_dbg() messages on a kernel without dynamic debug
ccflags-$(OBMF_DEBUG) += -DDEBUG

# A variable, so the compat header cannot test for it with #ifdef
ifeq ($(shell grep -qw system_dfl_wq $(srctree)/include/linux/workqueue.h && echo y),)
ccflags-y += -DOBMF_COMPAT_NO_DFL_WQ
endif
ifeq ($(shell grep -qw TTY_DRIVER_RESET_SAVED_TERMIOS $(srctree)/include/linux/tty_driver.h && echo y),)
ccflags-y += -DOBMF_COMPAT_NO_RESET_SAVED_TERMIOS
endif

# Where __acquires() is still a sparse context, sparse cannot follow
# guard(spinlock*); newer kernels leave lock contexts to clang.
ifneq ($(shell grep -qs 'context(x,0,1)' $(srctree)/include/linux/compiler_types.h $(srctree)/include/linux/compiler-context-analysis.h && echo y),)
CHECKFLAGS += -Wno-context
endif

# OBMF_CHECK_NAMES=y: stop unless only the core translates: a function
# driver, the public header and the native tests never name the dialect,
# and only the LSTP files and the USB ID table name LSTP. OBMF_TOOLS is
# the directory of check-names.sh, by default ../tools
ifeq ($(OBMF_CHECK_NAMES),y)
OBMF_TOOLS ?= $(src)/../tools
obmf-names := $(shell sh $(OBMF_TOOLS)/check-names.sh $(src) 2>&1 || echo failed)
ifneq ($(obmf-names),)
$(error $(obmf-names))
endif
endif

obj-m += obmf.o
obmf-y := drivers/usb/misc/obmf/obmf-usb.o \
	  drivers/usb/misc/obmf/obmf-core.o \
	  drivers/usb/misc/obmf/obmf-ctrl.o \
	  drivers/usb/misc/obmf/obmf-bus.o

# OBMF_DIALECTS=y: devices that speak another wire protocol, translated
# to OBMF-ICP. OBMF_LSTP=y: also bind LSTP devices; implies OBMF_DIALECTS,
# as the Kconfig dependency does
ifeq ($(OBMF_LSTP),y)
override OBMF_DIALECTS := y
ccflags-y += -DCONFIG_OBMF_LSTP=1
obmf-y += drivers/usb/misc/obmf/obmf-lstp.o
endif
ifeq ($(OBMF_DIALECTS),y)
ccflags-y += -DCONFIG_OBMF_DIALECTS=1
obmf-y += drivers/usb/misc/obmf/obmf-dialect.o
endif

# OBMF_KUNIT_TEST=y: the KUnit cases in obmf.ko, on a kernel with KUnit
ifeq ($(OBMF_KUNIT_TEST),y)
ifeq ($(CONFIG_KUNIT),)
$(error OBMF_KUNIT_TEST=y needs a kernel with CONFIG_KUNIT)
endif
ifeq ($(CONFIG_DTC),)
$(error OBMF_KUNIT_TEST=y needs a kernel with CONFIG_DTC, for the cases' overlay)
endif
ccflags-y += -DCONFIG_OBMF_KUNIT_TEST=1
obmf-y += drivers/usb/misc/obmf/obmf-test.o \
	  drivers/usb/misc/obmf/obmf-test-nodes.dtbo.o
endif

# OBMF_KUNIT_TEST=y with OBMF_LSTP=y: the LSTP cases too
ifeq ($(OBMF_KUNIT_TEST)$(OBMF_LSTP),yy)
ccflags-y += -DCONFIG_OBMF_LSTP_KUNIT_TEST=1
endif

obj-m += i2c-obmf.o
i2c-obmf-y := drivers/i2c/busses/i2c-obmf.o

obj-m += obmf_tty.o
obmf_tty-y := drivers/tty/obmf_tty.o
