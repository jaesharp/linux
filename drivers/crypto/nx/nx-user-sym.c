// SPDX-License-Identifier: GPL-2.0
/*
 * The NX symmetric engine, AES and SHA, as a user window type.
 *
 * The kernel has no driver for the engine on this platform, so what user
 * space pastes is the whole of its use. This kernel configures no request
 * length limit on it and reads none, so it publishes none.
 */
#include "nx-user.h"

static const struct vas_user_caps nx_user_sym_caps = {
	.req_max_processed_len = 0,
};

const struct vas_user_type nx_user_sym = {
	.name		= "ibm-power9-nv-nx-sym",
	.dir		= "crypto",
	.cop_type	= VAS_COP_TYPE_SYM,
	.variant	= VAS_NODE_PLATFORM,
	.caps		= &nx_user_sym_caps,
};
