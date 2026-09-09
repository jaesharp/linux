# SPDX-License-Identifier: GPL-2.0-or-later
#
# Tests for the VAS/NX userspace library. These check the encoding, so they
# need no accelerator and run wherever the library builds.

srctree := $(abspath $(CURDIR)/../../../..)
libdir := $(abspath $(CURDIR)/..)

CC ?= gcc
# Reads the built library back to check what the compiler made of the
# sequences that are only correct in the machine code.
OBJDUMP ?= objdump

CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Werror -MMD -MP
override CFLAGS += -I$(libdir)/include

LIBRARY := $(libdir)/libvas.a

PROGRAMS := encoding_test
OBJECTS := $(PROGRAMS:=.o)
DEPENDS := $(OBJECTS:.o=.d)

.PHONY: all run disasm clean

all: $(PROGRAMS)

$(PROGRAMS): %: %.o $(LIBRARY)
	$(CC) $(CFLAGS) -o $@ $< $(LIBRARY)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(LIBRARY):
	$(MAKE) -C $(libdir)

# Reads the object rather than running it, so it holds when this is built for
# a machine other than the one building it -- which is the case where a
# reordering would otherwise go unnoticed until the target ran it.
disasm: $(LIBRARY)
	OBJDUMP=$(OBJDUMP) ./compiled-as-expected.sh $(libdir)/vas.o

run: $(PROGRAMS)
	./encoding_test

clean:
	rm -f $(PROGRAMS) $(OBJECTS) $(DEPENDS)

-include $(DEPENDS)
