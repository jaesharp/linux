# SPDX-License-Identifier: GPL-2.0
# Kernel-driven POWER9 TM state-machine harness. Not part of the automated
# powerpc selftest build (lib.mk): the module must be built against the running
# kernel and loaded as root, so it is driven by hand. See README.md.
obj-m := tm_test.o

KDIR      ?= /lib/modules/$(shell uname -r)/build
HELPER    ?= /var/tmp/tm-test-helper
HELPER_CC ?= clang
HELPER_CFLAGS ?= -O2 -mcpu=power9 -pthread -Wall

all: module $(HELPER)

module:
	$(MAKE) -C $(KDIR) M=$(CURDIR) LLVM=1 ARCH=powerpc modules

$(HELPER): tm-test-helper.c
	$(HELPER_CC) $(HELPER_CFLAGS) $< -o $@

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) ARCH=powerpc clean
	$(RM) $(HELPER)

.PHONY: all module clean
