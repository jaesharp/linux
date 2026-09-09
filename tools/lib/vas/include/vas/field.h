/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Bit fields of the accelerator's structures, described rather than masked.
 *
 * POWER numbers bits from the most significant, so a field is its leftmost bit
 * and a width, exactly as the workbooks and the firmware headers write it. A
 * field value is inserted into and extracted from a host-order integer; the
 * conversion to the big-endian form the hardware reads happens once, at the
 * structure boundary, so nothing here depends on the endianness of the host.
 */

#ifndef _VAS_FIELD_H
#define _VAS_FIELD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * A field of a register or structure word. @msb is the position of its most
 * significant bit counted from the left of the containing word, so for a
 * 32-bit word bit 0 is 0x80000000. @width is in bits and is at least one.
 */
struct vas_field {
	uint8_t msb;
	uint8_t width;
};

/* The number of bits in the word a field of @bits total is carried in. */
static inline unsigned int vas_field_shift(struct vas_field f, unsigned int bits)
{
	return bits - f.msb - f.width;
}

static inline uint64_t vas_field_mask(struct vas_field f, unsigned int bits)
{
	uint64_t span = (f.width == 64) ? UINT64_MAX : ((UINT64_C(1) << f.width) - 1);

	return span << vas_field_shift(f, bits);
}

static inline uint64_t vas_field_get(struct vas_field f, unsigned int bits, uint64_t word)
{
	return (word & vas_field_mask(f, bits)) >> vas_field_shift(f, bits);
}

static inline uint64_t vas_field_put(struct vas_field f, unsigned int bits,
				     uint64_t word, uint64_t value)
{
	uint64_t shifted = value << vas_field_shift(f, bits);

	return (word & ~vas_field_mask(f, bits)) | (shifted & vas_field_mask(f, bits));
}

/*
 * Width-fixed spellings. The width of the containing word is part of what a
 * field means -- bit 8 of a byte does not exist, and bit 8 of a 32-bit word is
 * not bit 8 of a 64-bit one -- so it is named at every use rather than
 * inferred.
 */
static inline uint8_t vas_field_get8(struct vas_field f, uint8_t word)
{
	return (uint8_t)vas_field_get(f, 8, word);
}

static inline uint8_t vas_field_put8(struct vas_field f, uint8_t word, uint8_t value)
{
	return (uint8_t)vas_field_put(f, 8, word, value);
}

static inline uint16_t vas_field_get16(struct vas_field f, uint16_t word)
{
	return (uint16_t)vas_field_get(f, 16, word);
}

static inline uint16_t vas_field_put16(struct vas_field f, uint16_t word, uint16_t value)
{
	return (uint16_t)vas_field_put(f, 16, word, value);
}

static inline uint32_t vas_field_get32(struct vas_field f, uint32_t word)
{
	return (uint32_t)vas_field_get(f, 32, word);
}

static inline uint32_t vas_field_put32(struct vas_field f, uint32_t word, uint32_t value)
{
	return (uint32_t)vas_field_put(f, 32, word, value);
}

static inline uint64_t vas_field_get64(struct vas_field f, uint64_t word)
{
	return vas_field_get(f, 64, word);
}

static inline uint64_t vas_field_put64(struct vas_field f, uint64_t word, uint64_t value)
{
	return vas_field_put(f, 64, word, value);
}

/* Whether a single-bit field is set. */
static inline bool vas_field_is_set8(struct vas_field f, uint8_t word)
{
	return vas_field_get8(f, word) != 0;
}

static inline bool vas_field_is_set64(struct vas_field f, uint64_t word)
{
	return vas_field_get64(f, word) != 0;
}

/* Whether @value fits the field that is to carry it. */
static inline bool vas_field_fits(struct vas_field f, uint64_t value)
{
	if (f.width >= 64)
		return true;

	return value < (UINT64_C(1) << f.width);
}

#endif /* _VAS_FIELD_H */
