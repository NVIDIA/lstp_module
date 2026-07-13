# SPDX-License-Identifier: GPL-2.0-only
#
# LSTP Kernel Module Makefile
#
# Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
#
# This program is distributed in the hope it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
# more details.

MODNAME := lstp
KOFILE  := $(MODNAME).ko

UNAME_R := $(shell uname -r)
NPROC   := $(shell nproc 2>/dev/null || echo 1)

KDIR ?= /lib/modules/$(UNAME_R)/build
SRC_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
BUILD   := $(SRC_DIR)/build
KO_PATH := $(BUILD)/$(KOFILE)

SOURCES := Kbuild $(filter-out %.mod.c,$(notdir $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*.h)))

KERNEL_SRC := $(HOME)/linux
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
ifdef DEBUG
  EXTRA_CFLAGS += -DDEBUG
endif

obj-m += $(MODNAME).o

# Pass command-line name=value pairs to insmod; unknown names fail at insmod time.
_MAKE_VARS := MODNAME KOFILE UNAME_R NPROC KDIR SRC_DIR BUILD KO_PATH SOURCES \
              KERNEL_SRC UNAME_M ARCH CROSS_COMPILE DEBUG EXTRA_CFLAGS obj-m \
              MODPARAMS _MAKE_VARS V W O KBUILD_VERBOSE
MODPARAMS ?= $(strip $(foreach v,$(.VARIABLES),\
  $(if $(filter command line,$(origin $(v))),\
    $(if $(filter $(v),$(_MAKE_VARS)),,$(v)=$($(v))))))

.PHONY: default build build_rpi clean clean_rpi reload install uninstall unload load

default build:
	@mkdir -p $(BUILD) && for f in $(SOURCES); do cp -p $(SRC_DIR)/$$f $(BUILD)/$$f; done
	@$(MAKE) -j$(NPROC) -C $(KDIR) M=$(BUILD) modules EXTRA_CFLAGS="$(EXTRA_CFLAGS)"; rc=$$?; for f in $(SOURCES); do rm -f $(BUILD)/$$f; done; exit $$rc

build_rpi:
	@mkdir -p $(BUILD) && for f in $(SOURCES); do cp -p $(SRC_DIR)/$$f $(BUILD)/$$f; done
	@$(MAKE) -j$(NPROC) -C $(KERNEL_SRC) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(BUILD) modules EXTRA_CFLAGS="$(EXTRA_CFLAGS)"; rc=$$?; for f in $(SOURCES); do rm -f $(BUILD)/$$f; done; exit $$rc

clean clean_rpi:
	rm -rf $(BUILD)

reload: unload build load

install: build
	@sudo mkdir -p /lib/modules/$(UNAME_R)/extra/
	@sudo cp $(KO_PATH) /lib/modules/$(UNAME_R)/extra/
	@sudo depmod -a
	@lsmod | grep -q '^$(MODNAME) ' || sudo modprobe $(MODNAME)

uninstall: unload
	@sudo rm -f /lib/modules/$(UNAME_R)/extra/$(KOFILE)
	@sudo depmod -a

unload:
	@sudo rmmod $(MODNAME) 2>/dev/null || true
	@lsmod | grep $(MODNAME) || true

load:
	@sudo insmod $(KO_PATH) $(MODPARAMS)
	@lsmod | grep $(MODNAME) || true
	@sudo dmesg | tail -10
