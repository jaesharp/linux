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
/*
 * The window delivers to the receive window of the descriptor in target_fd
 * rather than to a coprocessor, so that a paste wakes the thread that opened
 * that window instead of asking an engine for work.
 *
 * The descriptor is the whole of the right to wake that thread: it cannot be
 * forged or guessed, it is passed to a sender over a unix socket like any
 * other descriptor, and the sender may close it once the window is open.
 * Version 2 only.
 */
#define VAS_TX_WIN_FLAG_TARGET		0x0000000000000008
/*
 * The window translates only the segments of the domains added to it with
 * VAS_WIN_DOMAIN_ADD, none until the first is added, and stops translating
 * a domain's segments when VAS_WIN_DOMAIN_DROP withdraws it. Hashed page
 * table kernels only. Version 2 only.
 */
#define VAS_TX_WIN_FLAG_DOMAINS		0x0000000000000004

/* Every flag this kernel defines. */
#define VAS_TX_WIN_FLAGS_ALL		(VAS_TX_WIN_FLAG_QOS_CREDIT | \
					 VAS_TX_WIN_FLAG_AMR | \
					 VAS_TX_WIN_FLAG_DOMAINS | \
					 VAS_TX_WIN_FLAG_TARGET)
/* Those version 1 carries; the rest are offered under version 2 only. */
#define VAS_TX_WIN_FLAGS_V1		VAS_TX_WIN_FLAG_QOS_CREDIT

struct vas_tx_win_open_attr {
	__u32	version;
	__s16	vas_id;	/* specific instance of vas or -1 for default */
	__u16	reserved1;
	__u64	flags;
	__u64	amr;		/* key mask, with VAS_TX_WIN_FLAG_AMR */
	__s32	target_fd;	/* wake destination, with VAS_TX_WIN_FLAG_TARGET */
	__u32	reserved3;
	__u64	reserved2[4];
};

/*
 * A receive window for the calling thread, which a send window may then be
 * pointed at. Opening one does not give the thread anything to paste to: it
 * makes the thread a destination, and the descriptor it was opened on is what
 * names that destination to whoever is to send.
 *
 * Nothing is returned, because the descriptor is the name. A sender is given
 * one over a unix socket and passes it as target_fd; there is no identifier
 * to publish, so no window can be reached by a process that was not handed
 * the right to reach it.
 *
 * The thread that opens the window is the thread that is woken, so a process
 * wanting several destinations opens one window per thread.
 */
struct vas_rx_win_open_attr {
	__u32	version;
	__s16	vas_id;	/* specific instance of vas or -1 for default */
	__u16	reserved1;
	__u64	flags;
	__u64	reserved2[5];
};

/*
 * A range of the address space a confined window may translate, seen at
 * segment granularity: start is rounded down to the segment holding it and
 * the end up to the next segment boundary.
 */
struct vas_win_domain {
	__u64	start;
	__u64	len;
	__u64	reserved[2];
};

#define VAS_WIN_DOMAIN_ADD	_IOW(VAS_MAGIC, 0x21, struct vas_win_domain)
#define VAS_WIN_DOMAIN_DROP	_IOW(VAS_MAGIC, 0x22, struct vas_win_domain)
#define VAS_RX_WIN_OPEN		_IOW(VAS_MAGIC, 0x23, struct vas_rx_win_open_attr)

#endif /* _UAPI_MISC_VAS_H */
