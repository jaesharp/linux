# SPDX-License-Identifier: GPL-2.0-or-later
#
# Examples for the VAS/NX userspace library. Warnings are errors.

srctree := $(abspath $(CURDIR)/../../../..)
libdir := $(abspath $(CURDIR)/..)

CC ?= gcc


CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Werror -MMD -MP
override CFLAGS += -I$(libdir)/include

LIBRARY := $(libdir)/libvas.a

PROGRAMS := discover move move_scatter error_handling wake pingpong null_request multicast suspend wake_tod smt_share amo_reduce vsx_rates wake_scale wake_barriers systolic dma_overlap broadcast share_read irregular
SHARED := report.o cycles.o
# Programs needing hand-written loops name their own extra objects.
vsx_rates_EXTRA := vsx_loops.o
wake_barriers_EXTRA := barrier_loops.o
dma_overlap_EXTRA := vsx_loops.o
OBJECTS := $(SHARED) $(PROGRAMS:=.o) $(vsx_rates_EXTRA) $(wake_barriers_EXTRA) $(dma_overlap_EXTRA)
DEPENDS := $(OBJECTS:.o=.d)

.PHONY: all clean

all: $(PROGRAMS)

$(PROGRAMS): %: %.o $(SHARED) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $< $($@_EXTRA) $(SHARED) $(LIBRARY)

vsx_rates: $(vsx_rates_EXTRA)
wake_barriers: $(wake_barriers_EXTRA)
dma_overlap: $(dma_overlap_EXTRA)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIBRARY):
	$(MAKE) -C $(libdir)

clean:
	rm -f $(PROGRAMS) $(OBJECTS) $(DEPENDS)

-include $(DEPENDS)
