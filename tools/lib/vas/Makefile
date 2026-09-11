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

# How the instructions C cannot spell are issued: auto takes the compiler's
# builtins wherever it has them, direct encodes each by value, and builtin
# insists on the enablement toolchain. See include/vas/vas.h.
ISA ?= auto
ifeq ($(ISA),auto)
ISA_FLAGS :=
else ifeq ($(ISA),direct)
ISA_FLAGS := -DVAS_ISA=VAS_ISA_DIRECT
else ifeq ($(ISA),builtin)
ISA_FLAGS := -DVAS_ISA=VAS_ISA_BUILTIN
else
$(error ISA must be auto, direct or builtin, not '$(ISA)')
endif

CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Werror -fPIC -MMD -MP
# The instructions issued here exist from ISA 3.0, which is also what the
# builtins are offered for.
override CFLAGS += -mcpu=power9
override CFLAGS += -Iinclude -DVAS_TRACE=$(TRACE) $(ISA_FLAGS)

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

# Neither half needs an accelerator. The disassembly check reads the object
# and so holds even when this is cross-built; the encoding tests have to run,
# so they need a host of the architecture they were built for.
check: tests
	$(MAKE) -C tests disasm
	$(MAKE) -C tests run

clean:
	rm -f $(OBJECTS) $(DEPENDS) $(LIBRARY)
	$(MAKE) -C examples clean
	$(MAKE) -C tests clean

-include $(DEPENDS)
