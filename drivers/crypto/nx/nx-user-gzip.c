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

const struct vas_user_type nx_user_gzip_legacy = {
	.name		= "nx-gzip",
	.dir		= "crypto",
	.cop_type	= VAS_COP_TYPE_GZIP,
	.variant	= VAS_NODE_LEGACY,
	.caps		= &nx_user_gzip_caps,
};

/*
 * The same engine on a node of its own. The node above keeps the name and
 * the interface user space had before this kernel, so a library written
 * against it goes on working unchanged; anything this kernel adds is offered
 * here, where no existing program is relying on what the interface does not
 * do yet.
 */
const struct vas_user_type nx_user_gzip = {
	.name		= "ibm-power9-nv-nx-gzip",
	.dir		= "crypto",
	.cop_type	= VAS_COP_TYPE_GZIP,
	.variant	= VAS_NODE_PLATFORM,
	.caps		= &nx_user_gzip_caps,
};

/*
 * The same engine's high priority receive window. The switchboard serves it
 * ahead of the normal one, and the kernel's own requests use it, so a window
 * here competes with them: this node is not the one to hand out by default.
 */
const struct vas_user_type nx_user_gzip_hipri = {
	.name		= "ibm-power9-nv-nx-gzip-hipri",
	.dir		= "crypto",
	.cop_type	= VAS_COP_TYPE_GZIP_HIPRI,
	.variant	= VAS_NODE_PLATFORM,
	.caps		= &nx_user_gzip_caps,
};
