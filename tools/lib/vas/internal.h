/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Private to the library: the window's insides, and the instructions that
 * hand a request to an accelerator.
 */

#ifndef _VAS_INTERNAL_H
#define _VAS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vas/field.h>
#include <vas/vas.h>

struct vas_window {
	int fd;
	enum vas_cop cop;
	void *paste_map;
	size_t map_len;
	void *paste_target;
};

/*
 * A destination is a receive window and nothing else: it has no paste mapping,
 * because nothing is pasted to it, and the descriptor is what names it.
 */
struct vas_destination {
	int fd;
	/*
	 * The queue the switchboard writes each paste into, mapped from the
	 * window, or NULL for a destination that is only woken. @cursor is
	 * this reader's own place in the ring: the hardware keeps no head
	 * pointer, and an entry says for itself whether it has arrived.
	 */
	void *queue;
	size_t queue_bytes;
	unsigned int slots;
	unsigned int cursor;
	/*
	 * The queue is another destination's, taken by a thread that joined
	 * its identity, and is unmapped by whoever mapped it.
	 */
	bool borrowed_queue;
};


/*
 * The instructions C has no spelling for, issued as VAS_ISA says (vas.h):
 * by value, or through the builtins of a toolchain that has them. Left
 * unset, the builtins are used wherever the compiler offers all four.
 */
#ifdef __has_builtin
#define VAS_HAS_BUILTIN(name) __has_builtin(name)
#else
#define VAS_HAS_BUILTIN(name) 0
#endif

#if VAS_HAS_BUILTIN(__builtin_ppc_copy) && \
	VAS_HAS_BUILTIN(__builtin_ppc_paste) && \
	VAS_HAS_BUILTIN(__builtin_ppc_copy_paste) && \
	VAS_HAS_BUILTIN(__builtin_ppc_cpabort)
#define VAS_COMPILER_HAS_NEST_BUILTINS 1
#else
#define VAS_COMPILER_HAS_NEST_BUILTINS 0
#endif

#ifndef VAS_ISA
#if VAS_COMPILER_HAS_NEST_BUILTINS
#define VAS_ISA VAS_ISA_BUILTIN
#else
#define VAS_ISA VAS_ISA_DIRECT
#endif
#elif VAS_ISA == VAS_ISA_BUILTIN
#if !VAS_COMPILER_HAS_NEST_BUILTINS
#error "VAS_ISA=builtin needs the enablement toolchain: this compiler has no __builtin_ppc_copy_paste"
#endif
#elif VAS_ISA != VAS_ISA_DIRECT
#error "VAS_ISA must be VAS_ISA_DIRECT or VAS_ISA_BUILTIN"
#endif

/*
 * The encodings, from Power ISA 3.0B book II: copy and paste. (4.4), which
 * load a 128-byte aligned block into the thread's copy buffer and store it
 * to the target, paste. reporting in CR0 whether it was taken; cpabort
 * (4.4), which clears the buffer so the next copy starts from nothing; and
 * wait with WC = 0 (4.2.4), from which the thread resumes on an exception,
 * an event-based branch, or a platform notify.
 *
 * The values are what the library issues under VAS_ISA_DIRECT, for a
 * toolchain that may not assemble the mnemonics, and are checked against an
 * assembler that does in tests/encoding_test.c. The kernel spells copy and
 * paste the same way in
 * tools/testing/selftests/powerpc/nx-gzip/include/copy-paste.h.
 */
#define VAS_PPC_RA(a) (((a) & 0x1f) << 16)
#define VAS_PPC_RB(b) (((b) & 0x1f) << 11)

#define VAS_INST_COPY 0x7c20060c
#define VAS_INST_PASTE 0x7c20070d
#define VAS_INST_CPABORT 0x7c00068c
#define VAS_INST_WAIT 0x7c00003c

#define vas_stringify_1(...) #__VA_ARGS__
#define vas_stringify(...) vas_stringify_1(__VA_ARGS__) " "

#define VAS_COPY(a, b) \
	vas_stringify(.long VAS_INST_COPY | VAS_PPC_RA(a) | VAS_PPC_RB(b))
#define VAS_PASTE(a, b) \
	vas_stringify(.long VAS_INST_PASTE | VAS_PPC_RA(a) | VAS_PPC_RB(b))

/* Order the request's stores ahead of the copy, and the paste behind it. */
static inline void vas_barrier(void)
{
	asm volatile("sync" ::: "memory");
}

#if VAS_ISA == VAS_ISA_DIRECT

/*
 * The condition register field paste. reports in, and the bit within it
 * that says the window took the request. CR0 is four bits, LT GT EQ SO, and
 * paste. sets EQ when the transfer succeeded.
 */
enum { VAS_CR0_BITS = 4, VAS_CR0_SHIFT = 28, VAS_CR0_MASK = 0xf };

static inline struct vas_field vas_cr0_equal(void)
{
	struct vas_field f = { .msb = 2, .width = 1 };

	return f;
}

static inline bool vas_paste_taken(uint32_t cr)
{
	uint32_t cr0 = (cr >> VAS_CR0_SHIFT) & VAS_CR0_MASK;

	return vas_field_get(vas_cr0_equal(), VAS_CR0_BITS, cr0) != 0;
}

static inline void vas_copy_block(const void *block)
{
	asm volatile(VAS_COPY(%0, %1) ";" ::"b"(0), "b"(block) : "memory");
}

static inline bool vas_paste_block(void *paste_target)
{
	uint32_t cr;

	asm volatile(VAS_PASTE(%1, %2) ";"
		     "mfocrf %0, 0x80;"
		     : "=r"(cr)
		     : "b"(0), "b"(paste_target)
		     : "memory", "cr0");

	return vas_paste_taken(cr);
}

/*
 * The pair in one statement, so that nothing the compiler schedules can
 * come between the copy and the paste: every instruction there is another
 * chance to take the interruption that discards the copy buffer.
 */
static inline bool vas_copy_paste_block(const void *block, void *paste_target)
{
	uint32_t cr;

	asm volatile(VAS_COPY(%1, %2) ";"
		     VAS_PASTE(%1, %3) ";"
		     "mfocrf %0, 0x80;"
		     : "=r"(cr)
		     : "b"(0), "b"(block), "b"(paste_target)
		     : "memory", "cr0");

	return vas_paste_taken(cr);
}

static inline void vas_abort_copy(void)
{
	asm volatile(vas_stringify(.long VAS_INST_CPABORT) ::: "memory");
}

static inline void vas_wait_for_notify(void)
{
	asm volatile(vas_stringify(.long VAS_INST_WAIT) ::: "memory");
}

#else /* VAS_ISA_BUILTIN */

/* Each builtin returns 1 when CR0 says the transfer succeeded. */
static inline void vas_copy_block(const void *block)
{
	__builtin_ppc_copy(block);
}

static inline bool vas_paste_block(void *paste_target)
{
	return __builtin_ppc_paste(paste_target) != 0;
}

static inline bool vas_copy_paste_block(const void *block, void *paste_target)
{
	return __builtin_ppc_copy_paste(block, paste_target) != 0;
}

static inline void vas_abort_copy(void)
{
	__builtin_ppc_cpabort();
}

/* No builtin exists for wait; a toolchain with the others assembles it. */
static inline void vas_wait_for_notify(void)
{
	asm volatile("wait 0" ::: "memory");
}

#endif /* VAS_ISA */

#endif /* _VAS_INTERNAL_H */
