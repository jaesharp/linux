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
};


/*
 * copy and paste, from Power ISA 3.0B book II. copy loads a 128-byte aligned
 * block into the thread's copy buffer and paste stores it to the target,
 * reporting in CR0 whether it was taken. Encoded by value because a toolchain
 * old enough to build this may not assemble the mnemonics.
 *
 * The kernel spells these the same way in
 * tools/testing/selftests/powerpc/nx-gzip/include/copy-paste.h.
 */
#define VAS_PPC_RA(a) (((a) & 0x1f) << 16)
#define VAS_PPC_RB(b) (((b) & 0x1f) << 11)

#define VAS_INST_COPY 0x7c20060c
#define VAS_INST_PASTE 0x7c20070d

#define vas_stringify_1(...) #__VA_ARGS__
#define vas_stringify(...) vas_stringify_1(__VA_ARGS__) " "

#define VAS_COPY(a, b) \
	vas_stringify(.long VAS_INST_COPY | VAS_PPC_RA(a) | VAS_PPC_RB(b))
#define VAS_PASTE(a, b) \
	vas_stringify(.long VAS_INST_PASTE | VAS_PPC_RA(a) | VAS_PPC_RB(b))

/*
 * The condition register field paste reports in, and the bit within it that
 * says the accelerator took the request. CR0 is four bits, LT GT EQ SO, and
 * paste sets EQ when the window accepted.
 */
enum { VAS_CR0_BITS = 4, VAS_CR0_SHIFT = 28, VAS_CR0_MASK = 0xf };

static inline struct vas_field vas_cr0_equal(void)
{
	struct vas_field f = { .msb = 2, .width = 1 };

	return f;
}

/* Order the request's stores ahead of the copy, and the paste behind it. */
static inline void vas_barrier(void)
{
	asm volatile("sync" ::: "memory");
}

static inline void vas_copy_block(const void *block)
{
	asm volatile(VAS_COPY(%0, %1) ";" ::"b"(0), "b"(block) : "memory");
}

static inline uint32_t vas_paste_block(void *paste_target)
{
	uint32_t cr;

	asm volatile(VAS_PASTE(%1, %2) ";"
		     "mfocrf %0, 0x80;"
		     : "=r"(cr)
		     : "b"(0), "b"(paste_target)
		     : "memory", "cr0");

	return (cr >> VAS_CR0_SHIFT) & VAS_CR0_MASK;
}

static inline bool vas_paste_accepted(uint32_t cr0)
{
	return vas_field_get(vas_cr0_equal(), VAS_CR0_BITS, cr0) != 0;
}

/*
 * wait, from Power ISA 3.0B book II, with WC = 0: the thread resumes on an
 * exception, an event-based branch, or a platform notify. Encoded by value
 * for the same reason copy and paste are.
 *
 * Resuming on any exception is what makes the caller's loop mandatory rather
 * than defensive: the timer tick alone will return from this.
 */
#define VAS_INST_WAIT 0x7c00003c

static inline void vas_wait_for_notify(void)
{
	asm volatile(vas_stringify(.long VAS_INST_WAIT) ::: "memory");
}

#endif /* _VAS_INTERNAL_H */
