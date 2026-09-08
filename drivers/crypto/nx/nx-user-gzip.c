// SPDX-License-Identifier: GPL-2.0
/*
 * The NX GZIP engine as a user window type.
 *
 * DEFLATE with fixed and dynamic Huffman tables, ZLIB and GZIP framing,
 * CRC32 and Adler32. The kernel has no driver for it; user space is its only
 * user. The engine's byte count limit register is left at 0 by firmware and
 * by this kernel, so no request length limit is configured and none is
 * published: the engine takes any length its request format can express,
 * and a request in progress is never suspended for a higher priority one.
 */
#include "nx-user.h"

static const struct vas_user_caps nx_user_gzip_caps = {
	.req_max_processed_len = 0,
};

const struct vas_user_type nx_user_gzip = {
	.name		= "nx-gzip",
	.dir		= "crypto",
	.cop_type	= VAS_COP_TYPE_GZIP,
	.caps		= &nx_user_gzip_caps,
};
