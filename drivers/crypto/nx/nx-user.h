/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The NX engines as user window types, one per file: each names its node,
 * the normal priority coprocessor type a user window binds to, and what it
 * publishes about itself. nx-common-powernv.c registers them.
 *
 * GZIP has two nodes. The one named nx-gzip is the interface user space had
 * before this kernel and keeps it; the rest carry this platform's name and
 * are where anything added since is offered. 842 and SYM had no userspace
 * interface to keep, so they have only the platform node.
 */
#ifndef __NX_USER_H__
#define __NX_USER_H__

#include <asm/vas.h>

extern const struct vas_user_type nx_user_sym;
extern const struct vas_user_type nx_user_842;
extern const struct vas_user_type nx_user_gzip_legacy;
extern const struct vas_user_type nx_user_gzip;

#endif /* __NX_USER_H__ */
