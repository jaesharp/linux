.. SPDX-License-Identifier: GPL-2.0

==========================================
Running accelerator requests in the kernel
==========================================

A request submitted to a Virtual Accelerator Switchboard window never
passes through the kernel. The process composes a request block, copies
it into the thread's copy buffer and pastes the buffer at the window's
paste address, all in user mode; the accelerator reads the block, does
the work, and stores its status block straight back into the process's
memory. The kernel is drawn in only when a translation faults or when it
has to write a completion on the engine's behalf.

That is the point of the interface, and it has three consequences.
Nothing but the process can observe an ordinary request, so tracing
shows only failures. Nothing can mediate one, so a security policy has
nothing to attach to. And a machine without the hardware cannot run the
software at all, so anything written against the interface can only be
tested where an accelerator exists.

This describes a mode in which the kernel runs the request instead: it
receives the block, may execute it in software, and writes the status
block back. It is much slower than the hardware path and is not meant to
replace it. What it buys is that the request becomes visible, mediable,
and available where no accelerator is.

What the architecture allows
============================

The shape of this is decided by Power ISA 3.0B section 4.4, which
constrains it more than it first appears.

**The copy buffer cannot be borrowed.** The buffer that holds the block
between the ``copy`` and the ``paste.`` is hidden state: the
architecture says it cannot be saved or restored, that any disruption of
program execution may prevent the transfer completing, and that the
handler of such a disruption is responsible for issuing ``cpabort`` to
discard it. So a kernel that traps a paste cannot take what was pasted
and paste it somewhere else. It is obliged to throw it away.

**The pair cannot be found by looking.** The same section says the two
instructions of a pair need not be adjacent in the instruction stream.
The ``sync``, ``copy``, ``paste.``, ``sync`` sequence that in-tree code
uses is a convention. Walking back from a faulting ``paste.`` to find
the ``copy`` and reading its operand registers is therefore a guess
against a pattern nothing guarantees.

**A paste to ordinary memory traps, and says so.** Pasting to storage
that is not an accelerator invokes the data storage error handler, and
the interrupt carries ``DSISR_BAD_COPYPASTE``, a bit of its own. This is
the only part of the mechanism that comes for free. The kernel already
treats that bit as non-fatal in one case, under ``MMU_FTR_NX_DSI``,
where PAPR defines a paste to a suspended window that must not kill the
program.

**Accelerator storage may not be touched any other way.** Any storage
access to an accelerator that is not an operand of ``copy`` or
``paste.`` raises a machine check. A decoy paste target must therefore
be ordinary memory rather than a remapping within the accelerator's own
range, which is also what makes it raise ``DSISR_BAD_COPYPASTE`` rather
than something fatal.

Taken together: a trapped paste tells the kernel that a request was
submitted and, if each window's paste page is distinct, which window
submitted it. It does not tell the kernel what the request was, and the
architecture closes both routes to recovering it after the fact.

Submitting through the kernel
=============================

So the request is handed over rather than intercepted. A window opened
in this mode has no paste mapping, and requests are submitted with an
ioctl carrying the block. The kernel then has the block itself, with no
inference and no dependence on instruction encoding or on hidden state.

The cost is a system call for each request, which is precisely what the
paste interface exists to avoid. That is why this is a mode and not the
default.

What the kernel does with the block is a separate question from how it
arrives. It may run the request in software, or paste it to real
hardware on the process's behalf. Only the first works where there is no
accelerator; only the second produces the engine's own output.

Emulating an engine
-------------------

The algorithms are already in the tree. ``lib/842`` compresses and
decompresses, and the 842 crypto driver already falls back to it when
the hardware declines, which is what makes the two interchangeable in
practice: streams the software compressor produces are ones the hardware
decompressor accepts.

Decompression is exact, so an emulated decompress returns the bytes the
hardware would have returned. Compression is not: a compressor need only
produce a valid stream, and the software one does not produce the
hardware's. Anything comparing compressed output against a recorded
result will see a difference; anything decompressing it will not.

The status block is written back exactly as the fault path already
writes one, under the opener's key mask and into the opener's address
space, so a process polling for its completion cannot tell the
difference.

Intercepting a paste
====================

Handing the request over needs the submitting program to cooperate, even
if only through a library. Intercepting one does not, and is what would
let an unmodified binary run on a machine with no accelerator.

The kernel can map ordinary memory at the window's paste address, so the
paste traps. Distinct pages per window make the faulting address name
the window. What remains is the block, and the architecture leaves only
two ways to it.

The submitting program can say where its requests live, at window open,
and undertake to paste only from there. One request buffer per window
makes the faulting paste name the block unambiguously. This is a
contract rather than an inference, and a library can honour it without
its callers knowing, but a program that pastes for itself must be
changed to take part.

Or the thread can be single stepped, so the ``copy`` is observed as it
executes and its operands read from the register file. That needs
nothing of the program and no assumption about instruction layout, and
it is slow in a way that makes the rest of this look fast.

Neither is implemented. They are recorded here because the reasoning
that rules out the cheaper alternatives is not obvious, and the next
person to want this should not have to rediscover that the copy buffer
is unreachable and the pair is not required to be adjacent.

Choosing a backend
==================

Most of the indirection this needs is already there. ``struct
vas_user_win_ops`` is the table a node's windows are opened, closed,
mapped and bounded through, and every call site already reaches it as a
property of the node the descriptor was opened from. What is global is
only where the table comes from: the running platform installs one with
``vas_set_user_win_ops()`` and every node is given the same one, so
today all windows on a machine share a backend whether or not they need
to.

Making the table a property of the node rather than of the machine is
therefore a small change, and it is the whole of what a runtime choice
of backend requires. A node registered against a software backend gets a
table whose ``open_win`` allocates no hardware window, whose
``paste_addr`` returns nothing, and which carries one entry the hardware
table does not need: somewhere to hand a submitted block. A node against
the hardware backend keeps the table it has.

That also decides what an emulated node looks like from user space. It
is a node like any other, with its own name and its own minor, and the
only difference a program can see is that mapping it for a paste address
fails and submission is by ioctl. Nothing has to guess: a node either
offers a paste address or it does not.

What the driver says at start
=============================

A machine can now be in several states that behave alike and perform
nothing alike, so the driver has to say which one it is in. Silence
would leave a software path a hundred times slower than the hardware one
indistinguishable from the hardware path.

At initialisation it should report, once, whether a switchboard was
found at all; for each coprocessor type, whether an engine was found,
and which backend its nodes were given; and for each node created, its
name, its minor and its backend. A type that exists only in software and
a type that is absent are different answers and must read differently.

The same applies to what is not there. A machine with no switchboard is
not a failure to be silent about if the software backend is available;
it is a machine running requests in the kernel, and an operator who
does not know that will misread every measurement taken on it.

Where the switch is
===================

Three separate decisions, and they are not three spellings of one.

Whether the code exists at all is a build option, because it puts a
system call in the path of a data-plane interface and has no place in a
kernel that will not use it.

Whether it is forced for every window is a boot or module parameter, for
bringing a machine up without an accelerator or tracing everything on
it.

Whether one window uses it is a flag at open, which is the ordinary
case: observing one process should not slow the machine for the rest.
