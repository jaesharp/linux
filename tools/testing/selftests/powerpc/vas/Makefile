# SPDX-License-Identifier: GPL-2.0
TEST_GEN_PROGS := ftw_wake

include ../../lib.mk
include ../flags.mk

# The tree's own uapi header, by symlink, so it cannot drift from the kernel
# being built against. Only this one header is here, so the system's asm/
# headers are still found for everything else.
CFLAGS += -m64 -I./include -I../include

$(TEST_GEN_PROGS): ../utils.c ../harness.c
$(OUTPUT)/ftw_wake: LDLIBS += -lpthread
