/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * Copyright 2019 IBM Corp.
 */

#ifndef _UAPI_MISC_VAS_H
#define _UAPI_MISC_VAS_H

#include <linux/types.h>

#include <asm/ioctl.h>

#define VAS_MAGIC	'v'
#define VAS_TX_WIN_OPEN	_IOW(VAS_MAGIC, 0x20, struct vas_tx_win_open_attr)

/*
 * Version 1 ignores the reserved fields and undefined flag bits. Version 2
 * requires both to be zero, and carries any feature added after it.
 */
#define VAS_TX_WIN_OPEN_V1		1
#define VAS_TX_WIN_OPEN_V2		2

/* Flags to VAS TX open window ioctl */
/* To allocate a window with QoS credit, otherwise use default credit */
#define VAS_TX_WIN_FLAG_QOS_CREDIT	0x0000000000000001
/*
 * The window translates under the key mask in amr rather than the opening
 * thread's own. The mask may only withhold rights the thread has; a set bit
 * denies, so every bit set in the thread's mask must be set in amr too.
 * Version 2 only.
 */
#define VAS_TX_WIN_FLAG_AMR		0x0000000000000002

/* Every flag this kernel defines. */
#define VAS_TX_WIN_FLAGS_ALL		(VAS_TX_WIN_FLAG_QOS_CREDIT | \
					 VAS_TX_WIN_FLAG_AMR)
/* Those version 1 carries; the rest are offered under version 2 only. */
#define VAS_TX_WIN_FLAGS_V1		VAS_TX_WIN_FLAG_QOS_CREDIT

struct vas_tx_win_open_attr {
	__u32	version;
	__s16	vas_id;	/* specific instance of vas or -1 for default */
	__u16	reserved1;
	__u64	flags;
	__u64	amr;		/* key mask, with VAS_TX_WIN_FLAG_AMR */
	__u64	reserved2[5];
};

#endif /* _UAPI_MISC_VAS_H */
