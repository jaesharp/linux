// SPDX-License-Identifier: GPL-2.0
/*
 * The NX 842 engine as a user window type.
 *
 * The kernel drives the engine through the crypto API on windows of its own
 * on the high priority FIFO; a user window binds to the normal priority one,
 * and its requests carry no coprocessor parameter block. The engine's byte
 * count limit register is left at 0 by firmware and by this kernel, so no
 * request length limit is configured, and none is published.
 */
#include "nx-user.h"

static const struct vas_user_caps nx_user_842_caps = {
	.req_max_processed_len = 0,
};

const struct vas_user_type nx_user_842 = {
	.name		= "nx-842",
	.dir		= "crypto",
	.cop_type	= VAS_COP_TYPE_842,
	.caps		= &nx_user_842_caps,
};
