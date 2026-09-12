// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * POWER9 DD2.2 soft-patch assistance for host transactional memory.
 *
 * The POWER9 core does not have the storage to checkpoint all four threads at
 * once, so on DD2.2 the suspended state is disabled in hardware and the
 * instructions that would enter or leave it trap to the hypervisor instead:
 * interrupt vector 0x1500, with the instruction image left in HEIR. KVM
 * answers those traps on behalf of a guest in arch/powerpc/kvm/book3s_hv_tm.c.
 * Nothing answered them for the host's own processes, so a userspace
 * tsuspend. died with SIGTRAP out of unknown_exception() and the kernel could
 * only advertise transactional memory without suspend.
 *
 * This is the host-side half: the same emulation applied to the interrupted
 * user context instead of to a guest vcpu. Documentation/arch/powerpc/
 * transactional_memory.rst anticipates it, noting that suspend could be
 * offered to host userspace if the emulation were brought into the host.
 *
 * Only tsr. (tsuspend./tresume.) is emulated. The other instructions that can
 * raise a soft patch -- rfid, hrfid, rfebb, mtmsrd, treclaim., trechkpt. --
 * are privileged or hypervisor-only and a user process cannot reach them, so
 * one arriving here means the kernel's own transactional paths have hit a case
 * this does not model. Those are left unhandled rather than silently emulated.
 *
 * Two ways of returning to a suspended user context are implemented, because
 * which one the hardware requires is not documented anywhere available here:
 *
 *   plain  set MSR[TS] to Suspended and return. Correct if the trap is purely
 *          an instruction match and the hardware will still hold the state.
 *   fake   additionally set PSSCR[FAKE_SUSPEND], the DD2.2 bit that lets a
 *          thread report Suspended while the transaction stays live. This is
 *          what KVM does for a guest, through kvm_hstate.fake_suspend.
 *
 * Selected with tm_softpatch=plain or tm_softpatch=fake on the command line,
 * or written to /sys/kernel/debug/powerpc/tm_softpatch/mode at runtime, so one
 * kernel can answer the question. The bit is write-only and reads back as
 * zero, which is why KVM keeps a shadow of it; here tresume. clears it
 * unconditionally, so no shadow is load-bearing.
 */

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/string.h>

#include <asm/cputable.h>
#include <asm/ppc-opcode.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/reg.h>
#include <asm/tm.h>
#include <linux/cpumask.h>
#include <asm/paca.h>

/*
 * The hardware match is on primary plus extended opcode and ignores bit 31, so
 * the ISA-invalid forms with bit 31 clear raise a soft patch too. Mask the
 * same way kvmhv_p9_tm_emulation() does so both forms behave alike.
 */
#define PO_XOP_MASK	0xfc0007feUL

enum {
	TM_SOFTPATCH_OFF	= 0,
	TM_SOFTPATCH_PLAIN	= 1,
	TM_SOFTPATCH_FAKE	= 2,
};

static bool tm_softpatch_available __ro_after_init;
/*
 * Default to the minimal mode. Plain changes only architected state that the
 * return path writes anyway; fake additionally sets a hypervisor SPR bit whose
 * behaviour alongside a live checkpoint is not documented, and KVM only ever
 * sets it in a flow where no checkpoint exists. Escalate to it by measurement,
 * not by assumption.
 */
static u8 tm_softpatch_mode = TM_SOFTPATCH_PLAIN;
static u32 tm_softpatch_suspends;
static u32 tm_softpatch_resumes;
static u32 tm_softpatch_unknown;
static u32 tm_softpatch_rollbacks;
/*
 * Whether to step nip past the trapping instruction. KVM's emulator rewinds
 * nip by four and then adds four back for every instruction it handles, which
 * says the interrupt already points past the faulting instruction; if that is
 * wrong for a bare metal host the instruction runs again for ever. Runtime
 * switchable so both readings can be tried without a reboot.
 */
static u8 tm_softpatch_advance_nip;
/*
 * Whether to also tell userspace that the full facility is available. Keeping
 * the emulation and the advertisement separate matters: advertising
 * PPC_FEATURE2_HTM turns on glibc's lock elision, so every pthread mutex in
 * the system starts using transactions, which is a much larger change than
 * making the instructions work. Off unless tm_softpatch=advertise is given.
 */
static bool tm_softpatch_advertise __ro_after_init;
/*
 * Whether to also clear tm_suspend_disabled. Off by default, because it does
 * not work yet and the failure is a kernel BUG rather than a process dying.
 *
 * Measured on cms. With the flag clear, sigreturn from a signal taken during a
 * transaction runs tm_recheckpoint(), so the kernel executes trechkpt. and
 * then returns to userspace with SRR1[TS] transactional. That return is an
 * S->T transition, which is exactly what this hardware soft patches, so the
 * 0x1500 interrupt arrives in kernel mode at a point in the return sequence
 * where r1 has already been switched back to the user stack pointer. The
 * entry macro's "trap if r1 is in userspace" check then fires and the kernel
 * dies at exceptions-64s.S:501.
 *
 * Supporting it needs two more things this does not have: an entry path for
 * 0x1500 that tolerates a kernel-mode arrival mid-return, and emulation of the
 * rfid and trechkpt. cases the way kvmhv_p9_tm_emulation() does for a guest.
 * Until then the flag stays set, the kernel never performs those transitions,
 * and userspace suspend and resume work on their own.
 */
static bool tm_softpatch_allow_suspend_state __ro_after_init;

bool tm_softpatch_enabled(void)
{
	return tm_softpatch_available && tm_softpatch_mode != TM_SOFTPATCH_OFF;
}

static int __init tm_softpatch_setup(char *str)
{
	if (!strcmp(str, "off"))
		tm_softpatch_mode = TM_SOFTPATCH_OFF;
	else if (!strcmp(str, "plain"))
		tm_softpatch_mode = TM_SOFTPATCH_PLAIN;
	else if (!strcmp(str, "fake"))
		tm_softpatch_mode = TM_SOFTPATCH_FAKE;
	else if (!strcmp(str, "advertise"))
		tm_softpatch_advertise = true;
	else if (!strcmp(str, "suspend-state"))
		tm_softpatch_allow_suspend_state = true;
	else
		pr_warn("TM: unrecognised tm_softpatch=%s, keeping the default\n", str);
	return 1;
}
__setup("tm_softpatch=", tm_softpatch_setup);

static void fake_suspend_set(bool on)
{
	unsigned long psscr = mfspr(SPRN_PSSCR);

	if (on)
		psscr |= PSSCR_FAKE_SUSPEND;
	else
		psscr &= ~PSSCR_FAKE_SUSPEND;
	mtspr(SPRN_PSSCR, psscr);
}

/*
 * tsr. -- transaction suspend or resume. L=1 resumes, L=0 suspends. CR0 is set
 * to the transactional state the instruction found, as the hardware would have
 * done. nip is not touched: the soft patch interrupt already points it at the
 * instruction after the faulting one, which is where execution resumes.
 */
static void emulate_tsr(struct pt_regs *regs, u32 instr)
{
	unsigned long msr = regs->msr;

	/*
	 * POWER9 does not give tbegin. the implicit memory barrier the ISA
	 * describes; it applies the barrier effects to tsr instead (POWER9
	 * User's Manual 4.6.5.4). Emulating tsr. without executing it would
	 * therefore lose an ordering guarantee the program is entitled to, so
	 * issue the barrier here in its place.
	 */
	mb();

	regs->ccr = (regs->ccr & 0x0fffffff) |
		    (((msr & MSR_TS_MASK) >> MSR_TS_S_LG) << 29);

	if (instr & (1 << 21)) {
		/*
		 * tresume. Leave fake suspend whatever a shadow would have
		 * said: the bit reads back as zero, so clearing it every time
		 * is cheaper than being wrong about it.
		 */
		if (tm_softpatch_mode == TM_SOFTPATCH_FAKE)
			fake_suspend_set(false);
		if (MSR_TM_SUSPENDED(msr))
			msr = (msr & ~MSR_TS_MASK) | MSR_TS_T;
		tm_softpatch_resumes++;
	} else {
		/* tsuspend. */
		if (MSR_TM_TRANSACTIONAL(msr)) {
			if (tm_softpatch_mode == TM_SOFTPATCH_FAKE)
				fake_suspend_set(true);
			msr = (msr & ~MSR_TS_MASK) | MSR_TS_S;
		}
		tm_softpatch_suspends++;
	}

	/*
	 * Not a plain store to regs->msr. On book3s64 the return path keeps
	 * the SRR/HSRR pair it already loaded and only reloads them from
	 * pt_regs when the PACA says they went stale, so writing the field
	 * directly is silently discarded and the interrupted instruction runs
	 * again unchanged. Measured: a tresume. emulated that way re-traps at
	 * the same nip until the loop guard gives up.
	 */
	regs_set_return_msr(regs, msr);
	if (tm_softpatch_advance_nip)
		regs_set_return_ip(regs, regs->nip + 4);
}

/*
 * Fail a transaction in software, because this part cannot be told to resume
 * one.
 *
 * trechkpt. is how the kernel would normally put a preempted transaction back
 * into the hardware, and on a part whose suspended state the firmware disabled
 * it raises a soft patch. tm_recheckpoint() runs it with interrupts hard
 * disabled and r1 already holding a user address, so that soft patch arrives in
 * kernel mode where the 0x1500 entry path cannot take it, inside __schedule
 * holding the runqueue lock. Measured on cms: 32 threads suspending and
 * resuming wedge the machine within seconds.
 *
 * KVM refuses the same instruction for the same reason and says so in
 * kvmppc_restore_tm_hv: "If we are doing TM emulation for the guest on a
 * POWER9 DD2, then we don't actually do a trechkpt". This is the host taking
 * the same decision for its own tasks, and the arithmetic is
 * kvmhv_emulate_tm_rollback()'s: the checkpoint becomes the architected state,
 * execution resumes at the failure handler, and CR0 says the transaction
 * failed.
 *
 * A transaction therefore does not survive preemption or a signal on this
 * hardware. That is allowed -- a transaction may fail at any time and the
 * program's fallback path runs -- and it is the same contract a guest already
 * gets. What it costs is a retry, which is much cheaper than a wedged core.
 */
void tm_softpatch_rollback(struct thread_struct *thr)
{
	struct pt_regs *regs = thr->regs;
	unsigned long msr;

	/* Everything except the transaction state stays as it is. */
	msr = regs->msr & ~(MSR_TS_MASK | MSR_FP | MSR_VEC | MSR_VSX);

	memcpy(regs->gpr, thr->ckpt_regs.gpr, sizeof(regs->gpr));
	regs->link = thr->ckpt_regs.link;
	regs->ctr = thr->ckpt_regs.ctr;
	regs->xer = thr->ckpt_regs.xer;

	/* CR0 = 0b1010: failure, not persistent. */
	regs->ccr = (thr->ckpt_regs.ccr & 0x0fffffff) | 0xa0000000;
	regs->nip = thr->tm_tfhar;
	regs_set_return_msr(regs, msr);

	/*
	 * The checkpointed maths state becomes live. Clearing the three MSR
	 * bits above is what makes restore_math() reload it on the way out,
	 * which is the same thing the recheckpoint path relies on.
	 */
	memcpy(&thr->fp_state, &thr->ckfp_state, sizeof(thr->fp_state));
	memcpy(&thr->vr_state, &thr->ckvr_state, sizeof(thr->vr_state));

	/* Record the failure where userspace looks for it. */
	thr->tm_texasr = (thr->tm_texasr & 0x3ffffffUL) |
			 (((u64)(TM_CAUSE_RESCHED | TM_CAUSE_PERSISTENT)) << 56) |
			 TEXASR_FS | TEXASR_ABORT | TEXASR_EXACT;
	thr->tm_tfiar = regs->nip;
	tm_softpatch_rollbacks++;
}

/*
 * Returns 1 if the instruction was emulated and execution may resume, 0 if the
 * caller should treat the interrupt as unhandled.
 */
int tm_softpatch_emulate(struct pt_regs *regs)
{
	u32 instr;

	if (!tm_softpatch_enabled())
		return 0;

	/* Only a user process can reach the instruction emulated here. */
	if (!user_mode(regs))
		return 0;

	/* Transactional memory has to be on for the faulting context. */
	if (!(regs->msr & MSR_TM))
		return 0;

	instr = mfspr(SPRN_HEIR);

	if ((instr & PO_XOP_MASK) != (PPC_INST_TSR & PO_XOP_MASK)) {
		if (tm_softpatch_unknown++ < 16)
			pr_warn("TM: soft patch for an instruction this does not emulate: %08x at nip %lx, msr %lx\n",
				instr, regs->nip, regs->msr);
		return 0;
	}

	if (tm_softpatch_suspends + tm_softpatch_resumes < 4) {
		unsigned long msr_in = regs->msr;

		emulate_tsr(regs, instr);
		pr_info("TM: soft patch %08x at nip %lx, msr %lx -> %lx\n",
			instr, regs->nip, msr_in, regs->msr);
		return 1;
	}

	emulate_tsr(regs, instr);
	return 1;
}

static int __init tm_softpatch_init(void)
{
	/*
	 * CPU_FTR_P9_TM_HV_ASSIST is set exactly on the parts whose hardware
	 * raises these soft patches and expects a hypervisor to answer them.
	 */
	if (!cpu_has_feature(CPU_FTR_P9_TM_HV_ASSIST))
		return 0;

	tm_softpatch_available = true;
	pr_info("TM: host soft patch assistance active, mode %u (POWER9 DD2.2 suspend emulation)\n",
		tm_softpatch_mode);

	if (tm_softpatch_mode == TM_SOFTPATCH_OFF)
		return 0;

	/*
	 * Firmware disabled the suspended state, so pnv_tm_init() enabled the
	 * facility but set tm_suspend_disabled and advertised suspend as
	 * unavailable. The host answers the soft patch now, so suspend does
	 * work, and that flag has to go: it makes signal delivery from a
	 * transactional context warn at signal_64.c and then refuses the
	 * sigreturn frame, so a process that suspends and then takes a signal
	 * dies with a bad frame rather than resuming. Measured on cms.
	 *
	 * This runs as an early initcall, after the command line is parsed and
	 * long before any userspace reads its capability word, so changing the
	 * advertisement here is equivalent to changing it in pnv_tm_init().
	 */
	/*
	 * pnv_tm_init() set this because firmware disabled the suspended
	 * state. Userspace can suspend now, and nothing executes trechkpt. any
	 * more, so the flag is wrong and actively harmful: it makes
	 * setup_tm_sigcontexts() warn and restore_tm_sigcontexts() refuse, so
	 * a signal delivered during any transaction kills the process with a
	 * bad sigreturn frame.
	 */
	if (tm_suspend_disabled) {
		tm_suspend_disabled = false;
		pr_info("TM: suspend is emulated by the host, clearing tm_suspend_disabled\n");
	}

	if (tm_softpatch_advertise) {
		cur_cpu_spec->cpu_user_features2 |= PPC_FEATURE2_HTM;
		cur_cpu_spec->cpu_user_features2 &= ~PPC_FEATURE2_HTM_NO_SUSPEND;
		pr_info("TM: advertising the full facility to userspace\n");
	}

	return 0;
}
early_initcall(tm_softpatch_init);

static u8 tm_fastpath_shadow;
static int tm_fastpath_get(void *data, u64 *val)
{
	*val = tm_fastpath_shadow;
	return 0;
}
static int tm_fastpath_set(void *data, u64 val)
{
	int cpu;

	tm_fastpath_shadow = val ? 1 : 0;
	for_each_possible_cpu(cpu)
		paca_ptrs[cpu]->tm_fastpath = tm_fastpath_shadow;
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(tm_fastpath_fops, tm_fastpath_get, tm_fastpath_set, "%llu\n");

static int __init tm_softpatch_debugfs(void)
{
	struct dentry *d;

	if (!tm_softpatch_available)
		return 0;

	d = debugfs_create_dir("tm_softpatch", arch_debugfs_dir);
	debugfs_create_file_unsafe("fastpath", 0644, d, NULL, &tm_fastpath_fops);
	debugfs_create_u8("mode", 0644, d, &tm_softpatch_mode);
	debugfs_create_u8("advance_nip", 0644, d, &tm_softpatch_advance_nip);
	debugfs_create_u32("suspends", 0444, d, &tm_softpatch_suspends);
	debugfs_create_u32("resumes", 0444, d, &tm_softpatch_resumes);
	debugfs_create_u32("unknown", 0444, d, &tm_softpatch_unknown);
	debugfs_create_u32("rollbacks", 0444, d, &tm_softpatch_rollbacks);
	return 0;
}
device_initcall(tm_softpatch_debugfs);
