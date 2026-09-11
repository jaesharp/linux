/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The 842 engine: a compressor whose request is a plain request.
 *
 * 842 needs no parameter block of its own, so a request to it is the command
 * word, a source and a target -- which makes it the shortest path from this
 * library to working hardware, and the one the examples take.
 *
 * Function codes are from the NX P8 workbook section 4.9, table 4-28, and the
 * buffer rules from section 4.9.1, both by way of drivers/crypto/nx/nx-842.h.
 */

#ifndef _VAS_NX842_H
#define _VAS_NX842_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vas/field.h>
#include <vas/nx.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nx_842_function {
	NX_842_COMPRESS = 0,
	NX_842_COMPRESS_CRC = 1,
	NX_842_DECOMPRESS = 2,
	NX_842_DECOMPRESS_CRC = 3,
	/* A copy with no compression, which is the engine's DMA path alone. */
	NX_842_MOVE = 4,
};

/*
 * What the engine requires of the memory a request names. The address rule
 * applies to every span; the length rule to every span but the last of a
 * scatter list, whose length need only be a multiple of the smaller figure.
 */
enum {
	NX_842_BUFFER_ALIGN = 128,
	NX_842_LENGTH_MULTIPLE = 32,
	NX_842_LAST_LENGTH_MULTIPLE = 8,
};

/*
 * The command word for a function. A request pasted to a userspace window
 * names only what it wants done: the window has already named the engine.
 */
static inline struct nx_ccw nx_842_ccw(enum nx_842_function function)
{
	struct nx_ccw ccw = { .word = 0 };

	ccw.word = vas_field_put32(nx_ccw_function(), ccw.word, (uint32_t)function);

	return ccw;
}

static inline bool nx_842_address_is_valid(const void *addr)
{
	return ((uintptr_t)addr % NX_842_BUFFER_ALIGN) == 0;
}

/* Whether a span may stand alone, or be the last of a list. */
static inline bool nx_842_last_length_is_valid(size_t len)
{
	return len && (len % NX_842_LAST_LENGTH_MULTIPLE) == 0;
}

/* Whether a span may be followed by another in the same list. */
static inline bool nx_842_length_is_valid(size_t len)
{
	return len && (len % NX_842_LENGTH_MULTIPLE) == 0;
}

static inline bool nx_842_source_is_valid(struct nx_source source)
{
	return nx_842_address_is_valid(source.addr) &&
	       nx_842_last_length_is_valid(source.len);
}

static inline bool nx_842_target_is_valid(struct nx_target target)
{
	return nx_842_address_is_valid(target.addr) &&
	       nx_842_last_length_is_valid(target.len);
}

/* The name of a function code, for a message. */
const char *nx_842_function_name(enum nx_842_function function);

#ifdef __cplusplus
}
#endif

#endif /* _VAS_NX842_H */
