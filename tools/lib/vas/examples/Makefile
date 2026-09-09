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

PROGRAMS := discover move move_scatter error_handling wake pingpong null_request multicast suspend wake_tod smt_share wake_scale wake_barriers
SHARED := report.o cycles.o
# Programs needing hand-written loops name their own extra objects.
wake_barriers_EXTRA := barrier_loops.o
OBJECTS := $(SHARED) $(PROGRAMS:=.o) $(wake_barriers_EXTRA)
DEPENDS := $(OBJECTS:.o=.d)

.PHONY: all clean

all: $(PROGRAMS)

$(PROGRAMS): %: %.o $(SHARED) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $< $($@_EXTRA) $(SHARED) $(LIBRARY)

wake_barriers: $(wake_barriers_EXTRA)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIBRARY):
	$(MAKE) -C $(libdir)

clean:
	rm -f $(PROGRAMS) $(OBJECTS) $(DEPENDS)

-include $(DEPENDS)
