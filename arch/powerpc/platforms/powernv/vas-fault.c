// SPDX-License-Identifier: GPL-2.0+
/*
 * VAS Fault handling.
 * Copyright 2019, IBM Corporation
 */

#define pr_fmt(fmt) "vas: " fmt

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched/signal.h>
#include <linux/mmu_context.h>
#include <asm/icswx.h>
#include <asm/copro.h>
#include <asm/mmu.h>
#include <asm/book3s/64/mmu-hash.h>

#include "vas.h"

/*
 * Was the address the accelerator faulted on one it was going to write?
 *
 * The engine reads through the source descriptor and writes through the
 * target, so a direct target descriptor covering the address settles it.
 *
 * An indirect descriptor does not, because it names a list of descriptors
 * rather than an extent, and that list is itself in user memory. Falling back
 * to "read" there is wrong rather than merely conservative: the page is
 * brought in without write permission, the hash table entry inserted for it
 * is read-only, and the engine's write then fails the protection check
 * instead of the presence check. The retry the CSB invites repeats it
 * forever.
 *
 * MEASURED with selftests/powerpc/nx-gzip gunz_test, which builds indirect
 * lists for anything past its first buffer: the nest MMU reported
 * MM_FIR1_TW_PG_FAULT_BPCHK_DET alongside the missing-pte bit, and the test
 * gave up with "cannot make progress; too many page fault retries cc= 250".
 *
 * So when the descriptors do not answer the question, ask the mapping. A page
 * in a writable VMA is faulted writable, which is what the process itself
 * would get by touching it and is what the retry needs. A read-only mapping
 * is still faulted read-only, so nothing is granted that the process does not
 * already have.
 */
/*
 * The CSB is 16 bytes and the CPB is contiguous with it, extending at most to
 * the end of a 4096 byte block. "P9 NX Gzip Accelerator" Figure 6-8.
 */
#define VAS_CSB_CPB_SPAN	4096

static bool fault_is_write(struct coprocessor_request_block *crb,
			   struct mm_struct *mm, unsigned long ea)
{
	struct data_descriptor_entry *dde = &crb->target;
	unsigned long base = be64_to_cpu(dde->address);
	unsigned long len = be32_to_cpu(dde->length);
	unsigned long csb = be64_to_cpu(crb->csb_addr) & CRB_CSB_ADDRESS;
	struct vm_area_struct *vma;
	bool write;

	/*
	 * No descriptor covers the CSB or the CPB, and the engine writes
	 * both: the CSB always, and the CPB's output parameters, which follow
	 * its input-only ones in the same span (section 6.8). Resolving that
	 * span read only installs a mapping the engine's store faults on
	 * again, and because the request is retried from the start it never
	 * completes. Named explicitly rather than by asking the VMA, so that
	 * a source buffer sharing a writable VMA is still faulted read and
	 * keeps its copy-on-write.
	 */
	if (csb && ea >= (csb & PAGE_MASK) && ea < csb + VAS_CSB_CPB_SPAN)
		return true;

	if (!dde->count)
		return ea >= base && ea < base + len;

	mmap_read_lock(mm);
	vma = find_vma(mm, ea);
	write = vma && ea >= vma->vm_start && (vma->vm_flags & VM_WRITE);
	mmap_read_unlock(mm);

	return write;
}

/*
 * How far past the faulting address it is worth working.
 *
 * A fault reports one address, but the engine was walking a buffer and will
 * want the rest of it. Resolving a single page means the retry faults on the
 * next one, and a caller with a bounded retry budget never finishes: at 4K
 * pages a 64MB buffer needs 16384 of them, and selftests/powerpc/nx-gzip
 * allows 500 before giving up with "cannot progress; too many faults".
 *
 * A direct descriptor covering the address says how far the buffer runs. An
 * indirect one does not, so take a bounded window and let the caller come
 * back for more; that still turns thousands of retries into a handful.
 */
#define VAS_FAULT_WINDOW	(1UL << 20)

static unsigned long fault_extent_end(struct coprocessor_request_block *crb,
				      struct mm_struct *mm, unsigned long ea)
{
	unsigned long end = ea + VAS_FAULT_WINDOW;
	struct vm_area_struct *vma;
	int i;

	for (i = 0; i < 2; i++) {
		struct data_descriptor_entry *dde = i ? &crb->target
						      : &crb->source;
		unsigned long base, len;

		if (dde->count)
			continue;

		base = be64_to_cpu(dde->address);
		len = be32_to_cpu(dde->length);
		/* Subtract rather than add: the length comes from the CRB. */
		if (ea >= base && ea - base < len)
			return min(base + len, end);
	}

	/*
	 * No descriptor covers this address, which is what a fault on the CSB
	 * or the CPB looks like. Stop at the end of the mapping it is in,
	 * rather than walking a megabyte of whatever happens to follow it.
	 *
	 * The addresses in a CRB are written by userspace, so the run has to
	 * be bounded by what the request describes and not by a fixed distance
	 * from an address it chose. The pages past the end of the mapping are
	 * not this request's to fault in, and one of the things that can
	 * follow is the vDSO data page, where faulting on another task's
	 * behalf trips the WARN in find_timens_vvar_page(): the fault thread's
	 * current->mm is never the mm being faulted.
	 */
	mmap_read_lock(mm);
	vma = find_vma(mm, ea);
	if (vma && ea >= vma->vm_start)
		end = min(end, vma->vm_end);
	mmap_read_unlock(mm);

	return end;
}

/*
 * Make the address the accelerator faulted on translatable again, so that the
 * request the caller retries has somewhere to land.
 *
 * Three steps on a hash MMU, and two of them have no equivalent under radix.
 * handle_mm_fault() populates the page tables, which is all a radix nest MMU
 * needs, because it walks the same tree the core does. A hash nest MMU first
 * walks the mm's segment table, which the kernel fills on demand and which
 * has nothing for a segment the core never touched: hash__nmmu_ste_insert()
 * is what gives it one. Then it walks the hash page table, and an entry there
 * is a cache: inserted on demand by a fault from a core, and evicted again.
 * So a page can be present to the process, and to the page tables, with
 * nothing in the hash table for the nest MMU to find. That is the case the
 * hardware reports as MM_FIR1_TW_PG_FAULT_NOPTE_DET, and hash_page_mm() is
 * what clears it.
 *
 * This is what ocxl's xsl_fault_handler_bh() does, for the same reason it
 * gives: update_mmu_cache() will not have loaded the hash, because the trap
 * this arrived through is not one.
 *
 * Where it differs from ocxl is the ending. An OpenCAPI fault is acknowledged
 * with RESTART and the adapter reissues the operation, so the fault never
 * reaches the caller. VAS has no equivalent: the engine terminates a request
 * that faults before the CRB reaches this code, and the CSB says so. Nothing
 * here rescues that request. What it does is give the caller's retry
 * somewhere to land, which is the contract vas_update_csb() already
 * describes.
 */
static void vas_fault_fixup(struct coprocessor_request_block *crb,
			    struct vas_user_win_ref *task_ref)
{
	unsigned long ea = be64_to_cpu(crb->stamp.nx.fault_storage_addr);
	struct mm_struct *mm = task_ref->mm;
	unsigned long access, flags, addr, end;
	bool is_write;
	vm_fault_t flt;
	int rc;

	if (!mm || !ea)
		return;

	vas_stat_inc(VAS_STAT_FIXUP);

	/*
	 * A user window's requests name user addresses. Anything else is not
	 * something to fault in on the window's behalf. Checked before taking
	 * a reference, so that refusing the work cannot leak one.
	 */
	if (get_region_id(ea) != USER_REGION_ID) {
		vas_stat_inc(VAS_STAT_FIXUP_NOT_USER_EA);
		return;
	}

	/*
	 * The window holds this mm with mmgrab(), not mmget(): vas-api.c takes
	 * a reference on mm_count and drops the one on mm_users as soon as the
	 * window is open. So the mm_struct is guaranteed to still exist here
	 * and its address space is not -- exit_mmap() may already have run.
	 * Faulting into that is not a slow path, it is a use-after-free of the
	 * VMAs, so take a real reference and give up if there is none to take.
	 * ocxl's fault handler holds mm_users across its own call for the same
	 * reason. Every path below this point must reach the mmput().
	 */
	if (!mmget_not_zero(mm)) {
		vas_stat_inc(VAS_STAT_FIXUP_MM_GONE);
		return;
	}

	is_write = fault_is_write(crb, mm, ea);

	access = _PAGE_PRESENT | _PAGE_READ;
	if (is_write)
		access |= _PAGE_WRITE;

	end = fault_extent_end(crb, mm, ea);

	for (addr = ea & PAGE_MASK; addr < end; addr += PAGE_SIZE) {
		if (copro_handle_mm_fault(mm, addr,
					  is_write ? DSISR_ISSTORE : 0, &flt)) {
			vas_stat_inc(VAS_STAT_FIXUP_PAGE_ERR);
			break;
		}

		vas_stat_inc(VAS_STAT_FIXUP_PAGES);

		if (radix_enabled())
			continue;

		/*
		 * 0x300 is the data storage trap a core would have taken for
		 * this access. Interrupts are disabled across the insertion
		 * for the same reason ocxl's fault handler disables them:
		 * this is the hash fault path entered from somewhere that is
		 * not a hash fault. They are dropped again each time round,
		 * because copro_handle_mm_fault() sleeps.
		 *
		 * A negative return is one page the hash would not take,
		 * and one page is not a reason to abandon the rest of the
		 * run: the accelerator retries the request, and faulting
		 * here again is the same outcome a core would see. Reported
		 * because a silent -1 here cost a day of tracing once.
		 */
		local_irq_save(flags);
		rc = hash_page_mm(mm, addr, access, 0x300, 0);
		local_irq_restore(flags);
		if (rc < 0) {
			vas_stat_inc(VAS_STAT_FIXUP_HASH_ERR);
			pr_warn_ratelimited("VAS: %lx not accepted by the hash table (%d)\n",
					    addr, rc);
		}

		/*
		 * A hash nest MMU walks a segment table before the page
		 * table, and the hardware reports a missing segment through
		 * the same fault as a missing page, so the segment gets its
		 * entry here too -- after the page, not before, although
		 * before reads more naturally. hash_page_mm() can demote
		 * the slice this address sits in (a 4K PFN or a cache
		 * inhibited mapping on a 64K kernel), and a demotion flushes
		 * the whole segment table, taking an entry written a moment
		 * earlier with it and leaving nothing to re-insert it. Made
		 * afterwards, the entry describes the slice as it now is.
		 *
		 * Once per page rather than once per segment for the same
		 * reason: any thread of this mm can convert a slice and
		 * empty the table at any point in this loop, and the next
		 * page's insertion heals what that removed. The common case
		 * -- entry already present and right -- is a scan of
		 * sixteen entries and no write.
		 */
		rc = hash__nmmu_ste_insert(mm, addr);
		if (rc) {
			vas_stat_inc(VAS_STAT_FIXUP_STE_ERR);
			pr_warn_ratelimited("VAS: no segment table entry for %lx (%d)\n",
					    addr, rc);
		}
	}

	mmput(mm);
}

/*
 * The maximum FIFO size for fault window can be 8MB
 * (VAS_RX_FIFO_SIZE_MAX). Using 4MB FIFO since each VAS
 * instance will be having fault window.
 * 8MB FIFO can be used if expects more faults for each VAS
 * instance.
 */
#define VAS_FAULT_WIN_FIFO_SIZE	(4 << 20)

static void dump_fifo(struct vas_instance *vinst, void *entry)
{
	unsigned long *end = vinst->fault_fifo + vinst->fault_fifo_size;
	unsigned long *fifo = entry;
	int i;

	pr_err("Fault fifo size %d, Max crbs %d\n", vinst->fault_fifo_size,
			vinst->fault_fifo_size / CRB_SIZE);

	/* Dump 10 CRB entries or until end of FIFO */
	pr_err("Fault FIFO Dump:\n");
	for (i = 0; i < 10*(CRB_SIZE/8) && fifo < end; i += 4, fifo += 4) {
		pr_err("[%.3d, %p]: 0x%.16lx 0x%.16lx 0x%.16lx 0x%.16lx\n",
			i, fifo, *fifo, *(fifo+1), *(fifo+2), *(fifo+3));
	}
}

/*
 * Process valid CRBs in fault FIFO.
 * NX process user space requests, return credit and update the status
 * in CRB. If it encounters transalation error when accessing CRB or
 * request buffers, raises interrupt on the CPU to handle the fault.
 * It takes credit on fault window, updates nx_fault_stamp in CRB with
 * the following information and pastes CRB in fault FIFO.
 *
 * pswid - window ID of the window on which the request is sent.
 * fault_storage_addr - fault address
 *
 * It can raise a single interrupt for multiple faults. Expects OS to
 * process all valid faults and return credit for each fault on user
 * space and fault windows. This fault FIFO control will be done with
 * credit mechanism. NX can continuously paste CRBs until credits are not
 * available on fault window. Otherwise, returns with RMA_reject.
 *
 * Total credits available on fault window: FIFO_SIZE(4MB)/CRBS_SIZE(128)
 *
 */
irqreturn_t vas_fault_thread_fn(int irq, void *data)
{
	struct vas_instance *vinst = data;
	struct coprocessor_request_block *crb, *entry;
	struct coprocessor_request_block buf;
	struct pnv_vas_window *window;
	unsigned long flags;
	void *fifo;

	crb = &buf;

	/*
	 * VAS can interrupt with multiple page faults. So process all
	 * valid CRBs within fault FIFO until reaches invalid CRB.
	 * We use CCW[0] and pswid to validate CRBs:
	 *
	 * CCW[0]	Reserved bit. When NX pastes CRB, CCW[0]=0
	 *		OS sets this bit to 1 after reading CRB.
	 * pswid	NX assigns window ID. Set pswid to -1 after
	 *		reading CRB from fault FIFO.
	 *
	 * We exit this function if no valid CRBs are available to process.
	 * So acquire fault_lock and reset fifo_in_progress to 0 before
	 * exit.
	 * In case kernel receives another interrupt with different page
	 * fault, interrupt handler returns with IRQ_HANDLED if
	 * fifo_in_progress is set. Means these new faults will be
	 * handled by the current thread. Otherwise set fifo_in_progress
	 * and return IRQ_WAKE_THREAD to wake up thread.
	 */
	while (true) {
		spin_lock_irqsave(&vinst->fault_lock, flags);
		/*
		 * Advance the fault fifo pointer to next CRB.
		 * Use CRB_SIZE rather than sizeof(*crb) since the latter is
		 * aligned to CRB_ALIGN (256) but the CRB written to by VAS is
		 * only CRB_SIZE in len.
		 */
		fifo = vinst->fault_fifo + (vinst->fault_crbs * CRB_SIZE);
		entry = fifo;

		if ((entry->stamp.nx.pswid == cpu_to_be32(FIFO_INVALID_ENTRY))
			|| (entry->ccw & cpu_to_be32(CCW0_INVALID))) {
			vinst->fifo_in_progress = 0;
			spin_unlock_irqrestore(&vinst->fault_lock, flags);
			return IRQ_HANDLED;
		}

		spin_unlock_irqrestore(&vinst->fault_lock, flags);
		vinst->fault_crbs++;
		if (vinst->fault_crbs == (vinst->fault_fifo_size / CRB_SIZE))
			vinst->fault_crbs = 0;

		memcpy(crb, fifo, CRB_SIZE);
		entry->stamp.nx.pswid = cpu_to_be32(FIFO_INVALID_ENTRY);
		entry->ccw |= cpu_to_be32(CCW0_INVALID);
		/*
		 * Return credit for the fault window.
		 */
		vas_return_credit(vinst->fault_win, false);

		pr_devel("VAS[%d] fault_fifo %p, fifo %p, fault_crbs %d\n",
				vinst->vas_id, vinst->fault_fifo, fifo,
				vinst->fault_crbs);

		vas_stat_inc(VAS_STAT_FAULT_CRBS);

		vas_dump_crb(crb);
		window = vas_pswid_to_window(vinst,
				be32_to_cpu(crb->stamp.nx.pswid));

		if (IS_ERR(window)) {
			vas_stat_inc(VAS_STAT_FAULT_BAD_PSWID);
			/*
			 * We got an interrupt about a specific send
			 * window but we can't find that window and we can't
			 * even clean it up (return credit on user space
			 * window).
			 * But we should not get here.
			 * TODO: Disable IRQ.
			 */
			dump_fifo(vinst, (void *)entry);
			pr_err("VAS[%d] fault_fifo %p, fifo %p, pswid 0x%x, fault_crbs %d bad CRB?\n",
				vinst->vas_id, vinst->fault_fifo, fifo,
				be32_to_cpu(crb->stamp.nx.pswid),
				vinst->fault_crbs);

			WARN_ON_ONCE(1);
		} else {
			/*
			 * NX sees faults only with user space windows.
			 */
			if (window->user_win) {
				vas_fault_fixup(crb,
						&window->vas_win.task_ref);
				vas_update_csb(crb,
					       &window->vas_win.task_ref);
			} else {
				WARN_ON_ONCE(!window->user_win);
			}

			/*
			 * Return credit for send window after processing
			 * fault CRB.
			 */
			vas_return_credit(window, true);
		}
	}
}

irqreturn_t vas_fault_handler(int irq, void *dev_id)
{
	struct vas_instance *vinst = dev_id;
	irqreturn_t ret = IRQ_WAKE_THREAD;
	unsigned long flags;

	/*
	 * NX can generate an interrupt for multiple faults. So the
	 * fault handler thread process all CRBs until finds invalid
	 * entry. In case if NX sees continuous faults, it is possible
	 * that the thread function entered with the first interrupt
	 * can execute and process all valid CRBs.
	 * So wake up thread only if the fault thread is not in progress.
	 */
	spin_lock_irqsave(&vinst->fault_lock, flags);

	if (vinst->fifo_in_progress)
		ret = IRQ_HANDLED;
	else
		vinst->fifo_in_progress = 1;

	spin_unlock_irqrestore(&vinst->fault_lock, flags);

	return ret;
}

/*
 * Fault window is opened per VAS instance. NX pastes fault CRB in fault
 * FIFO upon page faults.
 */
int vas_setup_fault_window(struct vas_instance *vinst)
{
	struct vas_rx_win_attr attr;
	struct vas_window *win;

	vinst->fault_fifo_size = VAS_FAULT_WIN_FIFO_SIZE;
	vinst->fault_fifo = kzalloc(vinst->fault_fifo_size, GFP_KERNEL);
	if (!vinst->fault_fifo) {
		pr_err("Unable to alloc %d bytes for fault_fifo\n",
				vinst->fault_fifo_size);
		return -ENOMEM;
	}

	/*
	 * Invalidate all CRB entries. NX pastes valid entry for each fault.
	 */
	memset(vinst->fault_fifo, FIFO_INVALID_ENTRY, vinst->fault_fifo_size);
	vas_init_rx_win_attr(&attr, VAS_COP_TYPE_FAULT);

	attr.rx_fifo_size = vinst->fault_fifo_size;
	attr.rx_fifo = __pa(vinst->fault_fifo);

	/*
	 * Max creds is based on number of CRBs can fit in the FIFO.
	 * (fault_fifo_size/CRB_SIZE). If 8MB FIFO is used, max creds
	 * will be 0xffff since the receive creds field is 16bits wide.
	 */
	attr.wcreds_max = vinst->fault_fifo_size / CRB_SIZE;
	attr.lnotify_lpid = 0;
	attr.lnotify_pid = mfspr(SPRN_PID);
	attr.lnotify_tid = mfspr(SPRN_PID);

	win = vas_rx_win_open(vinst->vas_id, VAS_COP_TYPE_FAULT, &attr);
	if (IS_ERR(win)) {
		pr_err("VAS: Error %ld opening FaultWin\n", PTR_ERR(win));
		kfree(vinst->fault_fifo);
		return PTR_ERR(win);
	}

	vinst->fault_win = container_of(win, struct pnv_vas_window, vas_win);

	pr_devel("VAS: Created FaultWin %d, LPID/PID/TID [%d/%d/%d]\n",
			vinst->fault_win->vas_win.winid, attr.lnotify_lpid,
			attr.lnotify_pid, attr.lnotify_tid);

	return 0;
}
