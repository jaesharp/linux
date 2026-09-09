# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for the VAS/NX userspace library. These check the encoding, so they
# need no accelerator and run wherever the library builds.

srctree := $(abspath $(CURDIR)/../../../..)
libdir := $(abspath $(CURDIR)/..)

CC ?= gcc


CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Werror -MMD -MP
override CFLAGS += -I$(libdir)/include

LIBRARY := $(libdir)/libvas.a

PROGRAMS := encoding_test
OBJECTS := $(PROGRAMS:=.o)
DEPENDS := $(OBJECTS:.o=.d)

.PHONY: all run clean

all: $(PROGRAMS)

$(PROGRAMS): %: %.o $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $< $(LIBRARY)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIBRARY):
	$(MAKE) -C $(libdir)

run: $(PROGRAMS)
	./encoding_test

clean:
	rm -f $(PROGRAMS) $(OBJECTS) $(DEPENDS)

-include $(DEPENDS)
