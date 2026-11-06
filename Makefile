# SPDX-License-Identifier: GPL-2.0-only
#
# Build the OBMF-ICP modules against the running kernel's headers, or
# against KDIR.

KDIR ?= /lib/modules/$(shell uname -r)/build

all modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all modules clean
