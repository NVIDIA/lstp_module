# SPDX-License-Identifier: GPL-2.0-only

obj-$(CONFIG_USB_OBMF) += obmf.o
obmf-y := obmf-usb.o obmf-core.o obmf-ctrl.o obmf-bus.o
obmf-$(CONFIG_OBMF_DIALECTS) += obmf-dialect.o
obmf-$(CONFIG_OBMF_LSTP) += obmf-lstp.o
obmf-$(CONFIG_OBMF_KUNIT_TEST) += obmf-test.o obmf-test-nodes.dtbo.o
