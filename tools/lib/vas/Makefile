# SPDX-License-Identifier: GPL-2.0-or-later
#
# The VAS/NX userspace library. Warnings are errors; the copy and paste
# instructions it issues make it buildable on powerpc alone.

srctree := $(abspath $(CURDIR)/../../..)

CC ?= gcc
AR ?= ar

# The interface this wraps is this tree's. Its version 2 open attribute and
# its domain ioctls are not in a distribution's installed headers, so
# include/asm/vas-api.h links to the in-tree copy.
#
# The link, rather than the directory on the include path: that directory also
# holds asm/socket.h and asm/sigcontext.h, which would shadow the C library's
# and break any translation unit that includes <sys/socket.h>. One header is
# wanted from it, so one header is what is reachable.

# Static trace points, off by default because they are for looking at a
# problem rather than for running with. TRACE=1 builds them: USDT notes
# where <sys/sdt.h> is available, and uprobe anchors otherwise.
TRACE ?= 0

CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Werror -fPIC -MMD -MP
override CFLAGS += -Iinclude -DVAS_TRACE=$(TRACE)

LIBRARY := libvas.a
OBJECTS := vas.o nx.o strings.o trace.o
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
