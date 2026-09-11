/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The instructions the library encodes by value, assembled by name into
 * data, so that the values can be held to what an assembler that knows the
 * mnemonics makes of them. Each word is in the host's byte order, which is
 * how encoding_test.c reads it back.
 */

	.section .rodata
	.balign 4

	.globl vas_reference_copy_0_4
vas_reference_copy_0_4:
	copy 0, 4

	.globl vas_reference_paste_0_4
vas_reference_paste_0_4:
	paste. 0, 4

	.globl vas_reference_cpabort
vas_reference_cpabort:
	cpabort

	.globl vas_reference_wait
vas_reference_wait:
	wait 0

	.section .note.GNU-stack,"",@progbits
