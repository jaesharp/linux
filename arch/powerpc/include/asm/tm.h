/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Transactional memory support routines to reclaim and recheckpoint
 * transactional process state.
 *
 * Copyright 2012 Matt Evans & Michael Neuling, IBM Corporation.
 */

#include <uapi/asm/tm.h>

#ifndef __ASSEMBLER__

extern void tm_reclaim(struct thread_struct *thread,
		       uint8_t cause);
extern void tm_reclaim_current(uint8_t cause);
extern void tm_recheckpoint(struct thread_struct *thread);
extern void tm_save_sprs(struct thread_struct *thread);
extern void tm_restore_sprs(struct thread_struct *thread);

extern bool tm_suspend_disabled;

struct pt_regs;

#ifdef CONFIG_PPC_TRANSACTIONAL_MEM
extern bool tm_softpatch_enabled(void);
extern int tm_softpatch_emulate(struct pt_regs *regs);
extern void tm_softpatch_rollback(struct thread_struct *thread);
#else
static inline bool tm_softpatch_enabled(void) { return false; }
static inline int tm_softpatch_emulate(struct pt_regs *regs) { return 0; }
static inline void tm_softpatch_rollback(struct thread_struct *thread) { }
#endif

#endif /* __ASSEMBLER__ */
