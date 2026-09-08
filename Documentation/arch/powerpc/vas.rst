.. SPDX-License-Identifier: GPL-2.0

============================================
The Virtual Accelerator Switchboard in Linux
============================================

This document describes how Linux drives the Virtual Accelerator
Switchboard (VAS) of POWER9 and later processors: what a window is, how
the kernel gives userspace windows on the nest accelerators, how the
accelerator's memory accesses are translated and confined, what happens
when an access faults, and what an operator can observe. The userspace
interface itself (the device nodes, the ioctl and the status block
protocol) is specified in Documentation/arch/powerpc/vas-api.rst. This
document describes the design behind that interface.

Windows and the switchboard
===========================

VAS is a per-chip unit that routes coprocessor request blocks (CRBs)
between windows. A window is a hardware context of a few hundred bytes,
kept in the chip's memory-mapped window context regions. There are two
kinds:

- A *receive* window owns a FIFO of CRBs in system memory and belongs to
  the consumer of those CRBs: one of the NX engines (GZIP, 842, or the
  symmetric AES/SHA engine), or a thread waiting to be woken.

- A *send* window is bound to one receive window and owns a *paste
  address*: a page of the chip's paste region, one per window id. A
  ``copy`` instruction loads a 128-byte CRB into a per-thread buffer, and
  a ``paste`` to the window's paste address hands the CRB to the
  switchboard, which appends it to the receive window's FIFO and reports
  in CR0 whether the paste was accepted.

Credits bound what is in flight. A send window has as many send credits
as it may have requests outstanding, a receive window has as many receive
credits as its FIFO has room for, and a paste that finds no credit is
refused. The receiving engine returns the credit when the CRB is
consumed.

The NX engines themselves are not part of VAS. They sit behind receive
windows that the kernel opens for them at boot, one per engine per
priority. (The kernel's own 842 driver uses the high priority FIFOs, from
send windows it opens per CPU.) Every request an engine processes names
its buffers by *effective address* in the address space of the sender.
The nest MMU translates those addresses, and the send window carries the
translation context the nest MMU uses.

PowerNV and pseries
===================

On PowerNV the kernel programs windows itself, through each VAS
instance's hypervisor and OS window context registers, and owns one
*fault window* per chip: a receive window into which the NX pastes the
CRB of any request that faulted. On pseries the hypervisor programs the
windows on the partition's behalf, through ``H_ALLOCATE_VAS_WINDOW``,
``H_MODIFY_VAS_WINDOW`` and ``H_DEALLOCATE_VAS_WINDOW``; it hands out
*credits* from two pools (default and quality-of-service) and delivers
faults as interrupts on the window.

The userspace API is one driver for both platforms,
arch/powerpc/platforms/book3s/vas-api.c. The platform installs its window
operations once, at its own initialization, with vas_set_user_win_ops():
how to open a window for a request, the window's paste address, how to
close it, and, on PowerNV, how to drain closes that were deferred. A
driver that has a receive window userspace may attach to registers a
*type*, a struct vas_user_type, and names no platform.

Types and device nodes
======================

A type says what a node is called, where under /dev it lives, which
coprocessor type a send window binds to, and what the type publishes
about itself. The NX driver describes its engines in
drivers/crypto/nx/nx-user-{sym,842,gzip}.c and registers them with
vas_user_type_register(). The user window driver gives each type a device
node (/dev/crypto/nx-gzip and the others), a minor of one character
major indexed by the coprocessor type, a class named after the node
(udev rules match on it), and the type's attributes under the node's
device in sysfs: ``cop_type`` and ``req_max_processed_len``
(Documentation/ABI/testing/sysfs-class-vas).

Opening a node yields a descriptor with no window. One ``VAS_TX_WIN_OPEN``
ioctl per descriptor establishes the window; mmap() of the descriptor
maps the window's paste address into the opener's address space, one
page, and only into that address space; close() releases the window.
Everything after the open (the CRB, the ``copy``, the ``paste`` and the
poll of the status block) happens in userspace, without a system call.

The translation context of a window
===================================

A send window carries everything the nest MMU needs to translate the
addresses of the requests pasted through it as the sender:

- the *LPID* of the partition and the *PID* of the address space;
- the *MSR* state to translate under (problem state, translation on) and
  the *LPCR* bits that describe the host's page size and translation
  options;
- the *translation mode*: hashed page table, or radix on radix;
- the *AMR*, the key mask every access is checked against.

On radix the nest MMU walks the same tree the cores do, indexed by the PID
the cores use. On the hashed page table the cores do not use the PID at
all (they resolve segments through their own software-loaded SLBs), so
the nest MMU has nothing to walk unless the kernel builds the tables. The
kernel builds three things:

- a *process table*, registered in the partition table, which the nest MMU
  indexes by PID;

- a *segment table* per address space that drives an accelerator (Power
  ISA v3.0B, section 5.7.8.3): one page from the linear map, reachable
  from the address space's process table entry. Its entries are exactly
  the SLB entries the core would load for the same addresses, produced
  by copro_calculate_slb(), so that both translators hash the page table
  with the same VSID and page size. Entries are added when the first
  window opens, for the mappings that exist then, and on demand by the
  fault path for anything mapped later. They are dropped when a slice
  of the address space changes page size, because the page size is part
  of the entry. When both groups a segment hashes to are full, the table
  is discarded and rebuilt from the address space; the fault path can
  always do this, because the table is only a cache of what the address
  space already describes.

- a *hardware PID* per address space, allocated when its first window
  opens, never inherited across fork(), and retired when the address
  space is torn down. On retirement the table is cleared, ``slbiag``
  removes everything the nest MMU cached under the PID, and the process
  table entry's own cached copy is removed with the one process-scoped
  ``tlbie`` form the architecture allows on the hashed page table (Power
  ISA v3.0B, section 5.9.3.3), with the POWER9 errata workarounds that
  every other ``tlbie`` in the tree carries.

The invalidation instructions are the part of this that the core never
exercises. ``slbieg`` invalidates one segment's cached entry for one PID;
``slbiag`` invalidates everything cached for the PID. The kernel uses
``slbiag`` both when a page-size change empties a table and when a PID
is retired: a nest MMU that has cached an entry continues to translate
through it after ``slbieg``, and section 5.9.3.2 of the ISA specifies
``slbiag`` for taking a PID out of service. The core's ``PIDR`` is
maintained across context switches for address spaces that have a
hardware PID, so that the register names the process the nest MMU is
walking for and never selects process table entry 0 by default.

When a request faults
=====================

An engine that cannot translate an address terminates the request. The
request is not resumed. What the process is told is the address the
engine stopped at and a completion code that says whether a retry could
succeed. The fault path is what gets from the engine's termination to
that report.

On PowerNV the NX pastes the terminated CRB into the chip's fault window,
stamped with the faulting address, the access direction and the nest
MMU's own reason. The fault window's threaded interrupt handler takes
CRBs off the FIFO and queues each to the send window it came from: a work
item per window, with a ring of pending CRBs sized by the window's
credits. The FIFO therefore drains at the speed of a copy, resolution
runs as ordinary preemptible kernel work, and windows resolve in
parallel, so one window's slow request delays only that window.

The worker decides what the fault asks for in one place: the direction,
from the descriptor the address falls in or from the status block's span
(the CSB and the CPB after it are the engine's output and no descriptor
names them); the extent, from that descriptor, bounded by the mapping;
and the page size of the mapping. Then:

- If the nest MMU's reason says the access was *refused* (the page's
  protection or the window's key mask) and the mapping agrees, nothing is
  faulted in, because nothing about the page would change, and the request
  completes with ``CSB_CC_PROTECTION`` or ``CSB_CC_WR_PROTECTION``. A stamp
  the mapping does not agree with is not trusted, and the address is walked
  as below.

- Otherwise the run of pages from the faulting address to the end of the
  extent is made resident and translatable, one page at a time: the page
  is faulted in on the address space's behalf and its hash table entry
  inserted, and, on the hashed page table, its segment is given an entry.
  The segment entry is made after the page, and once per page, so that a
  page-size demotion in the middle of the run cannot leave the segment
  table empty. A page budget bounds the run. A run cut short is not an
  error, because the engine reports the next fault where this one
  stopped.

- The status block is then written as the process's own thread would
  write it: with the process's address space borrowed, under the key mask
  the window latched, with ``CSB_CC_FAULT_ADDRESS`` and the address. If
  the address space has since been replaced by execve(), the update is
  dropped, because the address belongs to a program that no longer
  exists. If the status block cannot be written, because the address is
  unmapped or its page carries a key the window's mask denies, the
  process is sent ``SIGSEGV`` with ``SEGV_MAPERR`` or ``SEGV_PKUERR`` and
  the key, because a poller would otherwise wait forever.

Retrying is the application's responsibility. The kernel has made the
pages resident, so a plain resubmission of the same CRB succeeds unless
another page is missing; the loop, not any single retry, is the contract.
A refusal is terminal until the mapping or the window changes.

Every outcome is counted in /sys/kernel/debug/vas/stats
(Documentation/ABI/testing/debugfs-vas), and the tracepoints
``vas_fault_fixup`` and ``vas_fault_done`` record, per run, what a fault
asked for and what was done.

On pseries none of the resolution above happens. The hypervisor raises
the window's fault interrupt, the kernel reads the terminated CRB with
``H_GET_NX_FAULT`` and writes the status block with
``CSB_CC_FAULT_ADDRESS`` and the address, and that is all. The nest MMU
walks the partition's radix tree, so a page the process has touched is
one the engine can reach, and "touch the page and retry" is the literal
contract there rather than a description of what the kernel has already
done.

What a window may touch
=======================

The invariant: an engine acting on a window's request can read and write
exactly what the opening address space could, under the key mask the
window latched. The kernel validates nothing in a CRB. The addresses in
it are effective addresses that the nest MMU translates as the sender,
and an address the sender could not use is a fault, not a breach.

The mask is the opening thread's AMR at the moment of the open, or, with
``VAS_TX_WIN_FLAG_AMR``, a mask the opener names that withholds at least
what the thread's own mask does. It is a snapshot: rights the thread
withdraws afterwards are not enforced on the engine, and rights it gains
are not granted to the engine. The nest MMU checks the mask for loads and
for stores separately, on every page the engine touches, including the
status block and the parameter block; the kernel's own write of the
status block obeys the same mask.

Protection keys are therefore the page-granular categories of this
model, and a window's mask is the subject's rights over them. What keys
cannot give is revocation (the mask is frozen at the open) or a scope
narrower than the address space (the mask applies to every window's
pages alike, and only the category, not the window, distinguishes
pages). On the hashed page table there is a second lever the cores never
use, and it gives both: the nest MMU translates only the segments that
have entries in the table the kernel built for the window's PID.

Domains
-------

A window opened with ``VAS_TX_WIN_FLAG_DOMAINS`` translates through a
view of its own: a hardware PID of its own, a segment table of its own,
and a set of domains. A domain is a range of the address space, taken at
segment granularity, that the window may reach. The table carries the
same VSIDs as the process's own, so a page is the same page to the
engine as to the process and no page table entry is duplicated; what
differs is which segments have an entry at all.

The window translates nothing until a domain is added. Adding one gives
its mapped segments their entries at once and the rest on demand.
Withdrawing one empties the view's table and invalidates everything the
nest MMU holds for its PID, so the engine's next access to any segment
of that range faults and is refused, and the segments of the domains
that remain are given their entries again. That is the revocation keys
cannot express: it takes effect while the window is open, and it is
scoped to the window rather than to the address space.

The fault path tells the two refusals apart. A segment absent because no
domain covers it is refused by policy, counted as fixup_refused_domain,
and no page is walked; a segment absent because nothing has inserted it
yet is a translation fault and is resolved. Radix has no per-window
table to express this in, so the flag is refused there.

Resources, limits and lifecycle
===============================

A window is charged to the opener's cgroup on the misc controller
(Documentation/admin-guide/cgroup-v2.rst), as ``vas_windows`` or
``vas_qos_windows`` depending on the pool it was opened from, one unit
per window. The charge stays with the cgroup that opened the window until
the window is really gone, whichever process closes it. The capacity is
what the platform has: the windows the chips provide on PowerNV, and the
partition's credits on pseries, where the hypervisor can change them at
run time and the kernel closes or reopens windows to follow.

Opening is serialized per descriptor, so two threads racing on one
descriptor cannot both install a window and leave one unreferenced. The
open takes the references a window needs (the opener's pid and thread
group, its address space, its cgroup and its key mask) before the window
is published anywhere.

Closing a window has to wait for the hardware: the window is unpinned,
the switchboard is asked to stop accepting pastes, and the close waits
for the window to go idle and for its credits to return. The wait is
bounded and killable. When it gives up, on a fatal signal or at the
bound, the close is not abandoned: a worker keeps trying at intervals,
for up to twenty minutes, and completes exactly the steps the synchronous
path would have. Only then is the window *retained*: kept with its id,
credits, charge, address space and hardware PID, because an engine may
still write through its translation, and counted where an operator can
see it.

Two interactions with the process's own lifecycle are deliberate. A
process that calls execve() with requests in flight keeps the window,
because the descriptor survives, but the results of those requests are
dropped rather than written into an address space they were never meant
for. A process that exits with faults pending is not a warning: the fault
path finds the address space gone and drops the work, and the references
the window holds are released with the window.

What an operator can see
========================

The debugfs files are described in Documentation/ABI/testing/debugfs-vas
and Documentation/ABI/testing/debugfs-powerpc-nmmu, and the sysfs
attributes in Documentation/ABI/testing/sysfs-class-vas. In summary:

/sys/kernel/debug/vas/stats
	One counter per fault-path outcome, kept unconditionally: CRBs
	taken from the fault FIFO, runs entered, pages resolved, refusals
	by kind (address space gone or replaced, the hardware's protection
	and key refusals, addresses outside a window's domains, unreadable
	stamps), status block updates and the signals sent for the ones
	that could not be written, and windows retained.

/sys/kernel/debug/vas/fault_page_budget
	The most pages one fault resolves before the run is cut short.

/sys/kernel/debug/vas/v<N>/
	Per instance: the retained count in ``retained``, and per window,
	in ``w<id>/info`` and ``w<id>/hvwc``, the kernel's view of the
	window and the hardware's window context, including the latched
	mask.

/sys/kernel/debug/powerpc/nmmu_segtab, nmmu_stats and nmmu_slices
	The hashed-page-table side: a process's process table entry and
	segment table with every entry recomputed and checked against what
	the core would load; counters for entries inserted, tables emptied,
	hardware PIDs taken and returned, and views and domains taken and
	withdrawn; and a process's slice map, the page sizes the entries
	must carry.

/sys/class/<node>/<node>/cop_type and req_max_processed_len
	What each type publishes.

Tracepoints: ``vas_rx_win_open``, ``vas_tx_win_open``, ``vas_paste_crb``,
``vas_fault_fixup`` and ``vas_fault_done``.

Tests
=====

The tests are in tools/testing/selftests/powerpc/nx-gzip. The stress
driver, nx_gzip_stress, has modes that each establish one property of
this document on the machine they run on: that a window's descriptor can
be passed and its address space replaced under it, how many windows one
process can hold, that a fault is resolved rather than merely reported
and what one fault buys, what a window on the far chip costs, that the
key mask gates loads and stores and is a snapshot, that a refusal is
terminal and a named mask is honored, what each fault class the nest MMU
distinguishes looks like, that every registered type publishes itself and
opens a window, and that a window confined to domains translates only
them and loses one the moment it is withdrawn.
