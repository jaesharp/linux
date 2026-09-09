/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The timed loops, written out, because the compiler would not leave them
 * alone.
 *
 * Every attempt to express these in C ended with the accumulators spilled to
 * the stack inside the loop: an array forced them to memory because an element
 * was addressed, and asm volatile forced them to be materialised every
 * iteration. Either way the throughput arm measured stxv rather than the
 * instruction under test, and only the disassembly said so.
 *
 * So the loop is the source. Each function reads the timebase, runs the
 * instruction the stated number of times, reads it again, and returns the
 * difference in ticks. Nothing else is in the loop -- one branch and the
 * operation -- and what runs is what is written here.
 *
 * Two arms for each instruction:
 *
 *   lat_*  one register, so each operation waits for the last: the recurrence
 *          bound that an accumulation cannot beat.
 *   thr_*  eight independent registers, enough to fill the pipeline: the
 *          streaming bound.
 *
 * Registers are the volatile ones of the ELFv2 calling convention -- vs0-vs7
 * and v0-v7 -- so nothing needs saving. r3 carries the iteration count in and
 * the elapsed ticks out.
 *
 * Copyright 2026 J Lynn
 */

	.machine power9
	.abiversion 2
	.text

/* Read the timebase into the named register. SPR 268 is TB. */
#define READ_TB(r)	mfspr	r,268

/*
 * Prologue and epilogue shared by every loop: stamp the timebase, set the
 * count, and on the way out stamp it again and subtract.
 */
/*
 * Zero every register the loops touch. They would otherwise start with
 * whatever the caller left, and a denormal or a NaN reaching a floating-point
 * pipeline is a timing hazard that has nothing to do with the instruction
 * being measured. Zero is a normal value in every format used here, and the
 * cost is twenty instructions outside the loop.
 */
.macro CLEAR_REGS
	.irp n, 0,1,2,3,4,5,6,7,8,9
	xxlxor	\n,\n,\n
	xxlxor	(32+\n),(32+\n),(32+\n)
	.endr
.endm

.macro LOOP_ENTER shift
	CLEAR_REGS
	READ_TB(10)
	.if \shift
	srdi	3,3,\shift
	.endif
	mtctr	3
.endm

.macro LOOP_LEAVE
	READ_TB(3)
	sub	3,3,10
	blr
.endm

.macro FUNC name
	.globl	\name
	.type	\name,@function
	.p2align 4
\name:
.endm

.macro ENDFUNC name
	.size	\name,.-\name
.endm

/* Two-operand form: XT, XB. The latency arm feeds the result back in. */
.macro LOOPS2 name, op, vsx
FUNC lat_\name
	LOOP_ENTER 0
1:	\op	0,0
	bdnz	1b
	LOOP_LEAVE
ENDFUNC lat_\name

FUNC thr_\name
	LOOP_ENTER 3
1:	\op	0,8
	\op	1,8
	\op	2,8
	\op	3,8
	\op	4,8
	\op	5,8
	\op	6,8
	\op	7,8
	bdnz	1b
	LOOP_LEAVE
ENDFUNC thr_\name
.endm

/* Three-operand form: XT, XA, XB. */
.macro LOOPS3 name, op
FUNC lat_\name
	LOOP_ENTER 0
1:	\op	0,0,8
	bdnz	1b
	LOOP_LEAVE
ENDFUNC lat_\name

FUNC thr_\name
	LOOP_ENTER 3
1:	\op	0,8,9
	\op	1,8,9
	\op	2,8,9
	\op	3,8,9
	\op	4,8,9
	\op	5,8,9
	\op	6,8,9
	\op	7,8,9
	bdnz	1b
	LOOP_LEAVE
ENDFUNC thr_\name
.endm

/* Four-operand form: VD, VA, VB, VC, accumulating into VD. */
.macro LOOPS4 name, op
FUNC lat_\name
	LOOP_ENTER 0
1:	\op	0,8,9,0
	bdnz	1b
	LOOP_LEAVE
ENDFUNC lat_\name

FUNC thr_\name
	LOOP_ENTER 3
1:	\op	0,8,9,0
	\op	1,8,9,1
	\op	2,8,9,2
	\op	3,8,9,3
	\op	4,8,9,4
	\op	5,8,9,5
	\op	6,8,9,6
	\op	7,8,9,7
	bdnz	1b
	LOOP_LEAVE
ENDFUNC thr_\name
.endm

/*
 * The multiply-accumulate forms are their own shape: the destination is also
 * an addend, so the latency arm is the natural dependent chain and the
 * throughput arm just needs eight destinations.
 */
FUNC lat_fma
	LOOP_ENTER 0
1:	xvmaddasp 0,8,9
	bdnz	1b
	LOOP_LEAVE
ENDFUNC lat_fma

FUNC thr_fma
	LOOP_ENTER 3
1:	xvmaddasp 0,8,9
	xvmaddasp 1,8,9
	xvmaddasp 2,8,9
	xvmaddasp 3,8,9
	xvmaddasp 4,8,9
	xvmaddasp 5,8,9
	xvmaddasp 6,8,9
	xvmaddasp 7,8,9
	bdnz	1b
	LOOP_LEAVE
ENDFUNC thr_fma

/* Double precision, for the width comparison. */
FUNC lat_fmad
	LOOP_ENTER 0
1:	xvmaddadp 0,8,9
	bdnz	1b
	LOOP_LEAVE
ENDFUNC lat_fmad

FUNC thr_fmad
	LOOP_ENTER 3
1:	xvmaddadp 0,8,9
	xvmaddadp 1,8,9
	xvmaddadp 2,8,9
	xvmaddadp 3,8,9
	xvmaddadp 4,8,9
	xvmaddadp 5,8,9
	xvmaddadp 6,8,9
	xvmaddadp 7,8,9
	bdnz	1b
	LOOP_LEAVE
ENDFUNC thr_fmad

	LOOPS4	imac, vmsumuhm
	LOOPS4	imacd, vmsumudm
	LOOPS3	perm, xxperm
	LOOPS3	pack, vpkudum
	LOOPS2	unpack, vupkhsw
	LOOPS2	cvt_f2i, xvcvspsxws
	LOOPS2	cvt_i2f, xvcvsxwsp
	LOOPS2	cvt_f2h, xvcvsphp
	LOOPS2	cvt_h2f, xvcvhpsp

	.section .note.GNU-stack,"",@progbits
