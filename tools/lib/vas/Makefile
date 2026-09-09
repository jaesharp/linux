# SPDX-License-Identifier: GPL-2.0-or-later
#
# The VAS/NX userspace library. Warnings are errors; the copy and paste
# instructions it issues make it buildable on powerpc alone.

srctree := $(abspath $(CURDIR)/../../..)

CC ?= gcc
AR ?= ar

# The interface this wraps is this tree's. Its version 2 open attribute and
# its domain ioctls are not in a distribution's installed headers, so the
# in-tree copy is used unless the caller names another.
UAPI_INCLUDE ?= $(srctree)/arch/powerpc/include/uapi

CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Werror -fPIC -MMD -MP
override CFLAGS += -Iinclude -I$(UAPI_INCLUDE)

LIBRARY := libvas.a
OBJECTS := vas.o nx.o strings.o
DEPENDS := $(OBJECTS:.o=.d)

.PHONY: all examples tests check clean

all: $(LIBRARY)

$(LIBRARY): $(OBJECTS)
	$(AR) rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

examples: $(LIBRARY)
	$(MAKE) -C examples

tests: $(LIBRARY)
	$(MAKE) -C tests

# The encoding tests need no accelerator, so they run wherever this builds.
check: tests
	$(MAKE) -C tests run

clean:
	rm -f $(OBJECTS) $(DEPENDS) $(LIBRARY)
	$(MAKE) -C examples clean
	$(MAKE) -C tests clean

-include $(DEPENDS)
