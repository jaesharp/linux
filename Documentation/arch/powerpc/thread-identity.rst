.. SPDX-License-Identifier: GPL-2.0

=========================================
What a thread is called outside its core
=========================================

Hardware that acts on a thread's behalf while the thread is elsewhere has
to be able to name that thread. On POWER9 the name is three registers
taken together: the logical partition identity, the process identity, and
the thread identity. Power ISA 3.0B book II states it plainly, in the
note beside the ``wait`` instruction: the values in LPIDR, PIDR and TIDR
uniquely identify a thread that has initiated processing on an
accelerator, platforms may use them to track where threads are, and the
service this enables is an accelerator causing its initiating thread to
resume when results are available.

So the triple is a namespace, it is architected, and the kernel is what
populates it. This describes what is in it, who reads it, and where it is
thinner than it looks.

The three registers
===================

============ ====== ============ =========================================
Register     Bits   Values       What the kernel puts there
============ ====== ============ =========================================
LPIDR        12     4096         Zero on a bare-metal host. A partition
                                 identity under a hypervisor.
PIDR         20     1048576      The address space's context id. Hash
                                 allocates those from 19 bits, so at most
                                 524286 of the field is ever used.
TIDR         16     65536        Set on demand, from the low sixteen bits
                                 of the task's global pid.
============ ====== ============ =========================================

The widths are the hardware's, and the Virtual Accelerator Switchboard
compares its notify-match registers against them field for field. Nothing
software does can repartition them.

Who reads the triple
====================

The switchboard, to decide which thread or engine a notification is for.
A receive window carries a triple, and a notification is delivered where
the running thread's registers match it.

An accelerator has no identity of its own, so firmware invents one.
skiboot programs each accelerator's receive queue with a partition
identity of all ones, a process identity of the coprocessor type, and a
thread identity counting within that type, and exports the same three
values on the device tree node for the kernel to program into the
matching window. The all-ones partition value is deliberate: it is a
reservation, so an accelerator's name can never collide with a real
partition's.

The kernel maintains the thread half. TIDR is set on demand rather than
for every thread, because a thread that no hardware will ever name does
not need one, and it is restored on every context switch so that a
notification finds the thread wherever it is running. Addressing by
identity rather than by location is what makes migration harmless: only
being off-CPU loses a notification, not moving between CPUs.

Where the space runs out
========================

The thread field is the only one of the three that software defines
freely, and it is the one that is used carelessly.

Sixteen bits is ample. A thread identity only has to be unique within one
address space, because the partition and process fields have already
pinned the address space before the thread field is consulted, and a
process cannot hold anywhere near 65536 threads in practice.

The kernel does not allocate from those bits. It truncates the task's
global pid into them. At the default maximum pid of 32768 that is
lossless and the low sixteen bits are the whole pid, so nothing can
collide. The maximum pid is a writable sysctl bounded at four million,
and above 65536 two threads of one process whose global pids differ by an
exact multiple of 65536 receive the same identity. Nothing checks.

The failure is quiet rather than dangerous. Two different processes
cannot collide however their pids fall, because the process field
separates them. Within one process, a notification meant for one thread
wakes its twin, and both are looping on their own condition, so both stay
correct and simply fall back to whatever else resumes them. What is lost
is the latency the mechanism existed to save.

The fix is an allocator rather than a rearrangement: a sixteen-bit
identity per address space, taken when a thread first needs one.

What looks free and is not
==========================

The partition field has 4094 unused values on a bare-metal host and the
process field has a spare bit above the context space, so a reader
looking for room will find plenty. Both are read by the translation
machinery for their real purpose. They are unoccupied rather than
available, and nothing may be encoded in them.

Lifetime
========

A thread identity, once set, is never given back. There is no function to
clear one; there was, and it was removed. So a thread that acquires one
carries an extra special-register restore on every context switch for the
rest of its life, and the identity cannot be reused while the thread
lives.

That is a consequence of deriving the value rather than allocating it: a
derived value has no owner and nothing to return it to. An allocator
would give the identity back at thread exit and make clearing meaningful
again.
