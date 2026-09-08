/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The NX engines as user window types, one per file: each names its node,
 * the normal priority coprocessor type a user window binds to, and what it
 * publishes about itself. nx-common-powernv.c registers them.
 */
#ifndef __NX_USER_H__
#define __NX_USER_H__

#include <asm/vas.h>

extern const struct vas_user_type nx_user_sym;
extern const struct vas_user_type nx_user_842;

#endif /* __NX_USER_H__ */
