/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The paste sequence, taken apart.
 *
 * Almost all of a same-chip wake is the sender's paste rather than delivery,
 * so what the sequence is made of decides what the mechanism costs. It is four
 * things -- a barrier, a copy, a paste and another barrier -- and only
 * measurement says how they divide. The answer is in cycles rather than
 * nanoseconds because this machine moves its core clock by a factor of 1.78
 * between runs without being asked.
 *
 * The leading barrier is required. Power ISA 3.0B section 1.7.2: between a
 * storage access and a data transfer "the sequential execution model and
 * coherence-required ordering relationships do not apply", so a store the
 * woken thread must see is not ordered against the paste by anything weaker,
 * and section 4.6.3 has L=1 order only Load, Store and dcbz -- not transfers.
 * A release store cannot do this job.
 *
 * The trailing barrier is the open question. It orders the transfer against
 * what the sender does next, which a sender that does nothing next does not
 * need.
 *
 * Each function takes the paste target in r3, a 128-byte aligned block in r4
 * and an iteration count in r5, and returns elapsed timebase ticks.
 *
 * Copyright 2026 J Lynn
 */

	.machine power9
	.abiversion 2
	.text

#define READ_TB(r)	mfspr	r,268

.macro FUNC name
	.globl	\name
	.type	\name,@function
	.p2align 4
\name:
	mr	6,3			/* target, out of the return register */
	li	10,0			/* copy and paste take RA + RB */
	READ_TB(11)
	mtctr	5
.endm

.macro ENDFUNC name
	READ_TB(3)
	sub	3,3,11
	blr
	.size	\name,.-\name
.endm

/* As the library sends it today. */
FUNC paste_full
1:	hwsync
	copy	10,4
	paste.	10,6
	hwsync
	bdnz	1b
ENDFUNC paste_full

/* Without the trailing barrier, which orders only what the sender does next. */
FUNC paste_no_trailing
1:	hwsync
	copy	10,4
	paste.	10,6
	bdnz	1b
ENDFUNC paste_no_trailing

/* Neither barrier: the transfer alone, and not a correct sequence to send. */
FUNC paste_bare
1:	copy	10,4
	paste.	10,6
	bdnz	1b
ENDFUNC paste_bare

/*
 * lwsync in place of hwsync. Included to price the difference, not to offer
 * it: section 4.6.3 gives L=1 no ordering over data transfers at all, so this
 * does not order the store it would need to.
 */
FUNC paste_lwsync
1:	lwsync
	copy	10,4
	paste.	10,6
	bdnz	1b
ENDFUNC paste_lwsync

/*
 * The same sequence, counting how many pastes the switchboard accepted. A
 * rejected paste is not the operation being priced, and paste. reports which
 * it was in CR0 -- so a loop that ignores CR0 cannot tell a measurement of the
 * mechanism from a measurement of it refusing. Returns the count, not ticks.
 */
	.globl	paste_accepted
	.type	paste_accepted,@function
	.p2align 4
paste_accepted:
	mr	6,3
	li	10,0
	li	7,0			/* accepted so far */
	mtctr	5
1:	hwsync
	copy	10,4
	paste.	10,6
	hwsync
	bne	2f			/* CR0 EQ set means accepted */
	addi	7,7,1
2:	bdnz	1b
	mr	3,7
	blr
	.size	paste_accepted,.-paste_accepted

/* Two barriers and no transfer, so the barriers can be priced on their own. */
FUNC barriers_only
1:	hwsync
	hwsync
	bdnz	1b
ENDFUNC barriers_only

	.section .note.GNU-stack,"",@progbits
