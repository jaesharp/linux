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

PROGRAMS := discover move move_scatter error_handling wake pingpong null_request
SHARED := report.o
OBJECTS := $(SHARED) $(PROGRAMS:=.o)
DEPENDS := $(OBJECTS:.o=.d)

.PHONY: all clean

all: $(PROGRAMS)

$(PROGRAMS): %: %.o $(SHARED) $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $< $(SHARED) $(LIBRARY)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIBRARY):
	$(MAKE) -C $(libdir)

clean:
	rm -f $(PROGRAMS) $(OBJECTS) $(DEPENDS)

-include $(DEPENDS)
