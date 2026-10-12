# SPDX-License-Identifier: GPL-2.0-only
#
# LSTP Kernel Module Makefile
#
# Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.

MODNAME := lstp

UNAME_R := $(shell uname -r)
NPROC   := $(shell nproc 2>/dev/null || echo 1)

KDIR ?= /lib/modules/$(UNAME_R)/build
SRC_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD   := $(SRC_DIR)/build
# Where make load takes the modules from, and make install copies them from.
KO_DIR  ?= $(BUILD)
$(if $(KO_PATH),$(error KO_PATH is replaced by KO_DIR, the directory holding every module))

# Every module Kbuild builds: the core, $(MODNAME), then the subsystem
# drivers, each of which needs the core loaded.
MODULES       := $(patsubst %.o,%,$(shell sed -nE 's/^obj-m[[:space:]]*[:+]?=//p' $(SRC_DIR)/Kbuild))
CHILD_MODULES := $(filter-out $(MODNAME),$(MODULES))
KOFILES       := $(addsuffix .ko,$(MODULES))
KO_PATHS      := $(addprefix $(KO_DIR)/,$(KOFILES))

# Kbuild and the sources it builds, as paths below $(SRC_DIR). The build
# copies them to the same paths below $(BUILD), and removes them afterwards.
SOURCE_DIRS := compat drivers include
SOURCES := Kbuild \
	   $(patsubst $(SRC_DIR)/%,%,$(shell find $(addprefix $(SRC_DIR)/,$(SOURCE_DIRS)) -name '*.[ch]'))
SOURCE_SUBDIRS := $(sort $(dir $(addprefix $(BUILD)/,$(SOURCES))))

KERNEL_SRC := $(HOME)/linux

# updates/ is searched before the kernel's own modules by depmod's default
# and distribution configs, so an installed copy wins over an in-tree driver
# of the same name. Earlier releases installed to extra/.
INSTALL_MOD_DIR ?= updates
MOD_DEST        := /lib/modules/$(UNAME_R)/$(INSTALL_MOD_DIR)
OLD_MOD_DEST    := $(filter-out $(MOD_DEST),/lib/modules/$(UNAME_R)/extra)
UNAME_M := $(shell uname -m)
ifeq ($(UNAME_M),aarch64)
  ARCH ?= arm64
  CROSS_COMPILE ?= aarch64-linux-gnu-
else ifeq ($(UNAME_M),x86_64)
  ARCH ?= x86_64
  CROSS_COMPILE ?=
else ifeq ($(UNAME_M),armv7l)
  ARCH ?= arm
  CROSS_COMPILE ?= arm-linux-gnueabihf-
endif

# Command-line name=value pairs other than these go to the core module as
# parameters; the kernel ignores, with a warning, any it does not know.
_MAKE_VARS := MODNAME UNAME_R NPROC KDIR SRC_DIR BUILD KO_DIR MODULES CHILD_MODULES \
              KOFILES KO_PATHS SOURCE_DIRS SOURCES SOURCE_SUBDIRS \
              KERNEL_SRC INSTALL_MOD_DIR MOD_DEST OLD_MOD_DEST \
              UNAME_M ARCH CROSS_COMPILE DEBUG IPMI_POSTCODES \
              IPMI_MSG_NUM EXTRA_CFLAGS KCFLAGS obj-m MODPARAMS _MAKE_VARS V W O \
              KBUILD_VERBOSE
MODPARAMS ?= $(strip $(foreach v,$(.VARIABLES),\
  $(if $(filter command line,$(origin $(v))),\
    $(if $(filter $(v),$(_MAKE_VARS)),,$(v)=$($(v))))))

.PHONY: default build build_rpi clean clean_rpi reload install uninstall unload load

# DEBUG, IPMI_POSTCODES and IPMI_MSG_NUM take effect in Kbuild, which the
# kernel make reads with this make's command-line variables.
default build:
	@mkdir -p $(SOURCE_SUBDIRS) && for f in $(SOURCES); do cp -p $(SRC_DIR)/$$f $(BUILD)/$$f; done
	@$(MAKE) -j$(NPROC) -C $(KDIR) M=$(BUILD) modules; rc=$$?; for f in $(SOURCES); do rm -f $(BUILD)/$$f; done; exit $$rc

build_rpi:
	@mkdir -p $(SOURCE_SUBDIRS) && for f in $(SOURCES); do cp -p $(SRC_DIR)/$$f $(BUILD)/$$f; done
	@$(MAKE) -j$(NPROC) -C $(KERNEL_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(BUILD) modules; rc=$$?; for f in $(SOURCES); do rm -f $(BUILD)/$$f; done; exit $$rc

clean clean_rpi:
	rm -rf $(BUILD)

reload: unload build load

# Copy every module to $(MOD_DEST), drop the copies an earlier release left
# in extra/, and refresh the index modprobe and hotplug look modules up in.
define install_modules
	@sudo rm -f $(foreach d,$(OLD_MOD_DEST),$(addprefix $(d)/,$(KOFILES)))
	@sudo mkdir -p $(MOD_DEST)/
	@sudo cp $(KO_PATHS) $(MOD_DEST)/
	@sudo depmod -a
endef

# The subsystem drivers load on demand, from their devices' modaliases.
install: build
	$(install_modules)
	@lsmod | grep -q '^$(MODNAME) ' || sudo modprobe $(MODNAME)

uninstall: unload
	@sudo rm -f $(foreach d,$(MOD_DEST) $(OLD_MOD_DEST),$(addprefix $(d)/,$(KOFILES)))
	@sudo depmod -a

# Every module that holds a reference on the core first, whichever tree it
# was built from, then the core.
unload:
	@for h in /sys/module/$(MODNAME)/holders/*; do \
		[ -e "$$h" ] && sudo rmmod "$${h##*/}" || true; \
	done
	@if lsmod | grep -q "^$(MODNAME) "; then sudo rmmod $(MODNAME) || true; fi
	@lsmod | grep $(MODNAME) || true

# Loads the modules in $(KO_DIR) without installing them: the core, with the
# command-line parameters, then every subsystem driver, which needs the core.
# Nothing is loaded on demand from there; install does that.
load:
	@sudo insmod $(KO_DIR)/$(MODNAME).ko $(MODPARAMS)
	@for m in $(CHILD_MODULES); do \
		sudo insmod $(KO_DIR)/$$m.ko || exit 1; \
	done
	@lsmod | grep $(MODNAME) || true
	@sudo dmesg | tail -10
