.. SPDX-License-Identifier: GPL-2.0-or-later

=====================================
Userspace library for VAS and the NX
=====================================

A C library for opening a Virtual Accelerator Switchboard send window and
running requests on the Nest Accelerator through it.

``Documentation/arch/powerpc/vas.rst`` describes the model and
``Documentation/arch/powerpc/vas-api.rst`` the kernel interface this wraps.
Read those first; this covers only what the library adds.

Building
========

::

    make            # libvas.a
    make examples
    make check      # encoding tests, which need no accelerator

powerpc only: submitting a request is the ``copy`` and ``paste``
instruction pair. Both endiannesses build, and nothing assumes a page size.
The headers come from this tree rather than the distribution's, because the
version 2 open attribute and the domain ioctls are not upstream; point
``UAPI_INCLUDE`` elsewhere to override that.

The three headers
=================

``vas/vas.h``
    Finding an engine, opening a window, and bounding what it may translate.

``vas/nx.h``
    A request: what to do, where to read, where to write, and what came back.

``vas/nx842.h``
    The 842 engine's function codes and buffer rules. Its move function is a
    copy with no compression, which is the shortest path to a working
    request and what the examples use.

Priority
========

Each engine has two receive queues, and the switchboard serves the high
priority one before the normal one. The kernel's own requests use the high
priority queue, so a window opened on a ``-hipri`` node competes with them.
``vas_cop_hipri()`` names the high priority type of an engine and
``vas_cop_is_hipri()`` recognises one.

That is a permission rather than a flag: the nodes are separate, so a system
can grant one and withhold the other, and the sample rules give high
priority groups of its own that the general ones do not reach.

Which node
==========

An engine may offer more than one node, and ``vas_window_attr.node``
chooses. The default is the platform's node, which carries everything the
running kernel offers. ``VAS_NODE_LEGACY`` opens the node userspace had
before this kernel, which refuses anything later so that a program written
against it cannot have the interface change underneath; only GZIP has one,
because only GZIP had userspace to keep.

A whole request
===============

::

    struct vas_window_attr attr;
    struct vas_window *window;
    struct nx_request *request;
    struct nx_completion done;

    vas_window_attr_init(&attr, VAS_COP_842);
    vas_window_open(&attr, &window);

    nx_request_create(&request);
    nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
    nx_request_set_source(request, nx_source(in, len));
    nx_request_set_target(request, nx_target(out, len));

    nx_execute(window, request, NULL);
    nx_request_completion(request, &done);

    if (done.cc != NX_CC_SUCCESS)
        fprintf(stderr, "%s\n", nx_cc_describe(done.cc));

``nx_execute()`` submits, waits, and resolves what can be resolved: a paste
the window refuses because its queue is full, and an address the accelerator
could not translate. Pass a ``struct nx_retry_policy`` to change or withhold
either allowance. ``nx_submit()`` and ``nx_wait()`` are the same steps
separately, for a caller running several requests at once.

Types that cannot be transposed
===============================

A source and a target are different types, and each carries its own length,
so neither the two directions nor an address and a length can be swapped by
mistake. The same holds for the scatter lists. Bit fields are described by
position and width rather than written as masks, and are applied to
host-order values, which is why nothing here depends on the endianness of
the host.

Scattered memory
================

A request through a userspace window carries effective addresses, so a
buffer needs no physical contiguity and no splitting at page boundaries: one
virtually contiguous region is one span however many pages it covers. Lists
are for data that really is in pieces::

    struct nx_source_list *gathered;

    nx_source_list_create(count, &gathered);
    for (i = 0; i < count; i++)
        nx_source_list_add(gathered, nx_source(piece[i], each));
    nx_request_set_source_list(request, gathered);

The two sides are independent: a request may gather from many spans and
write to one, or the reverse, or both, so long as the totals agree. The list
must outlive the request that names it, because the accelerator reads it.

Every span but the last must satisfy the engine's larger length rule, and
every span its address alignment; ``vas/nx842.h`` states both and offers
``nx_842_source_is_valid()`` and ``nx_842_target_is_valid()`` to check
before submitting, which turns a completion code into an ordinary error.

When something goes wrong
=========================

A call that fails before the accelerator sees anything returns a negative
errno. A request that reached the engine returns a completion code, which
``nx_request_completion()`` reads along with the byte count and, when the
code is ``NX_CC_FAULT_ADDRESS``, the address that could not be translated,
whether the access was a load or a store, and what the nest MMU objected to.
``nx_cc_name()`` and ``nx_cc_describe()`` put either into a message.

Only ``NX_CC_FAULT_ADDRESS`` is worth retrying, which is what
``nx_cc_is_retryable()`` answers; every other non-zero code describes the
request itself, and repeating it repeats the answer. ``examples/error_handling.c``
provokes each class on purpose.

Permissions
===========

``udev/`` holds a sample. Two tiers of group, because using an engine and
administering one are different rights: a ``-users`` group may open a
window on one engine, on any NX engine, or on any window at all, and the
group without that suffix may also administer it. ``nx-accelerator-groups``
creates them and ``nx-accelerator-access`` grants the wider ones by access
control entry, since a node has room for one owning group and there are
more groups than that.

Opening a node is all an unprivileged process needs, and one user's window
cannot reach another user's memory, but windows are a finite hardware
resource: the bound on how many a user may hold is the misc cgroup
controller's ``vas_windows`` and ``vas_qos_windows`` keys, not the file
permissions.

What is not here
================

Window to window, with no coprocessor in between. VAS can do it: a
copy-paste pair to a send window aimed at a receive window sends an
ASB_Notify that wakes a core out of ``wait`` without going through memory,
described in POWER9 User's Manual section 12.3 as core-core wakeup. The
kernel has the coprocessor type for it, ``VAS_COP_TYPE_FTW``, but registers
no device node for it, and setting up the receiving side means writing Local
Notify LPID, PID and TID registers and clearing the window control register's
credit bits, all of which are privileged. The manual also says the feature
"is only supported on DD2.0 hardware", and its availability on later
revisions is unestablished.
