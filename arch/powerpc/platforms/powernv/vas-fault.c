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
#include <linux/pkeys.h>
#include <asm/icswx.h>
#include <asm/copro.h>
#include <asm/mmu.h>
#include <asm/book3s/64/mmu-hash.h>

#include "vas.h"

/* definitions are emitted by vas-window.c */
#include "vas-trace.h"

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
 * So when the descriptors do not answer the question, ask the mapping. A page
 * in a writable VMA is faulted writable, which is what the process itself
 * would get by touching it and is what the retry needs. A read-only mapping
 * is still faulted read-only, so nothing is granted that the process does not
 * already have.
 */
/*
 * Pages one fault CRB may resolve before the fault window moves on.
 *
 * This is a ceiling on how long one request can hold the fault window, not a
 * ration of progress: a caller retries a fault a bounded number of times, so
 * a buffer only ever completes if the pages resolved per fault multiplied by
 * that bound covers it. Set below the extent a run would otherwise cover, it
 * silently stops large requests from ever completing. The default is
 * therefore the whole window, and cond_resched() in the loop is what keeps
 * the wait for other windows short.
 */
/* How far past a faulting address one run may work. */
#define VAS_FAULT_WINDOW	(1UL << 20)

unsigned int vas_fault_page_budget = VAS_FAULT_WINDOW >> PAGE_SHIFT;

/*
 * The CSB is 16 bytes and the CPB is contiguous with it, extending at most to
 * the end of a 4096 byte block. "P9 NX Gzip Accelerator" Figure 6-8. The
 * block is 4096 bytes whatever PAGE_SIZE is.
 */
#define VAS_CSB_CPB_SPAN	4096

/*
 * The page size mapped at this address. A hugetlb mapping is one page to the
 * hash table and to the segment table, so stepping a run by PAGE_SIZE would
 * repeat the same insertion once per base page it happens to contain.
 *
 * Only on hash. get_slice_psize() opens with VM_BUG_ON(radix_enabled()):
 * slices are a hash construct, and radix describes its huge pages in the
 * page tables, which handle_mm_fault() populates whole.
 */
static unsigned long fault_page_size(struct mm_struct *mm, unsigned long ea)
{
	int psize;

	if (radix_enabled())
		return PAGE_SIZE;

	psize = get_slice_psize(mm, ea);

	return 1UL << mmu_psize_defs[psize].shift;
}

/*
 * What one fault asks the kernel to do, decided once.
 *
 * The hardware reports an address; everything else -- whether the engine was
 * reading or writing, how far the buffer runs, what page size the mapping
 * uses -- is derived from the CRB and the mm. Deriving it in one place, under
 * one hold of the mmap lock, keeps the pieces consistent with each other:
 * the extent never leaves the descriptor that decided the direction, so a
 * read run cannot be turned into a write on pages past the buffer; the
 * extent never leaves the VMA, on any path; and the slice map is consulted
 * only for an address the mm can actually have.
 */
struct vas_fault_run {
	unsigned long start;
	unsigned long end;
	unsigned long pgsz;
	bool write;
};

static int vas_fault_describe(struct coprocessor_request_block *crb,
			      struct mm_struct *mm, unsigned long ea,
			      struct vas_fault_run *run)
{
	unsigned long csb = be64_to_cpu(crb->csb_addr) & CRB_CSB_ADDRESS;
	unsigned long csb_blk = csb & ~(VAS_CSB_CPB_SPAN - 1);
	struct vm_area_struct *vma;
	unsigned long pgsz, end;
	bool write = false, covered = false;
	int i;

	if (get_region_id(ea) != USER_REGION_ID)
		return -EFAULT;

	/*
	 * An address the mm cannot have would index past the slice map. The
	 * region check does not exclude it: the user region is larger than
	 * any one mm's limit.
	 */
#ifdef CONFIG_PPC_64S_HASH_MMU
	if (!radix_enabled() && ea >= mm_ctx_slb_addr_limit(&mm->context))
		return -EFAULT;
#endif

	mmap_read_lock(mm);

	vma = find_vma(mm, ea);
	if (!vma || ea < vma->vm_start) {
		mmap_read_unlock(mm);
		return -EFAULT;
	}

	pgsz = fault_page_size(mm, ea);
	end = ALIGN(ea + max(VAS_FAULT_WINDOW, pgsz), pgsz);

	/*
	 * The engine writes the CSB and the CPB's output words, in the 4096
	 * byte block that holds them; no descriptor covers that block.
	 */
	if (csb && ea >= csb_blk && ea < csb_blk + VAS_CSB_CPB_SPAN) {
		write = true;
		end = min(end, csb_blk + VAS_CSB_CPB_SPAN);
		covered = true;
	}

	/*
	 * A direct descriptor that covers the address says both which way
	 * the engine was going and where the buffer ends. The run stops at
	 * the buffer, so that its direction is not applied to whatever
	 * follows it in the same mapping.
	 */
	for (i = 0; !covered && i < 2; i++) {
		struct data_descriptor_entry *dde = i ? &crb->target
						      : &crb->source;
		unsigned long base, len;

		if (dde->count)
			continue;

		base = be64_to_cpu(dde->address);
		len = be32_to_cpu(dde->length);
		/* Subtract rather than add: the length comes from the CRB. */
		if (ea >= base && ea - base < len) {
			write = i == 1;
			end = min(end, base + len);
			covered = true;
		}
	}

	/*
	 * An indirect descriptor names a list in user memory rather than an
	 * extent, so the mapping is the only thing that can say. A writable
	 * VMA is faulted writable, which is what the process would get by
	 * touching it and is what a retry needs; a read-only one is not
	 * granted anything the process does not have.
	 */
	if (!covered)
		write = !!(vma->vm_flags & VM_WRITE);

	/*
	 * Never past the mapping, on any of the paths above. The pages after
	 * it are not this request's to fault in, and what follows can be the
	 * vDSO data page, where faulting on another task's behalf trips the
	 * WARN in find_timens_vvar_page().
	 */
	end = min(end, vma->vm_end);

	mmap_read_unlock(mm);

	run->start = ALIGN_DOWN(ea, pgsz);
	run->end = end;
	run->pgsz = pgsz;
	run->write = write;
	return 0;
}

/*
 * The stamp says the nest MMU refused a right. Confirm from the mapping, and
 * for a key fault from the window's key snapshot, that it would have: the
 * same snapshot the hardware latched, so the two verdicts cannot diverge by
 * the thread's register having moved since. A stamp value that means
 * something else on another part then degrades to the walk, never to a
 * refusal the mapping does not support.
 */
static bool vas_fault_refused(struct mm_struct *mm, unsigned long ea,
			      u64 amr, bool write, u8 fs)
{
	struct vm_area_struct *vma;
	bool refused = false;

	mmap_read_lock(mm);
	vma = find_vma(mm, ea);
	if (vma && ea >= vma->vm_start) {
		if (fs == NX_FS_PROTECTION)
			refused = !(vma->vm_flags & (write ? VM_WRITE : VM_READ));
#ifdef CONFIG_PPC_MEM_KEYS
		else if (fs == NX_FS_KEY)
			refused = !pkey_amr_access_permitted(amr, vma_pkey(vma),
							     write);
#endif
	}
	mmap_read_unlock(mm);

	return refused;
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
static u8 vas_fault_fixup(struct coprocessor_request_block *crb,
			  struct vas_user_win_ref *task_ref)
{
	unsigned long ea = be64_to_cpu(crb->stamp.nx.fault_storage_addr);
	struct mm_struct *mm = task_ref->mm;
	unsigned long access, flags, addr, end;
	/* clamp before narrowing: a u32 above INT_MAX would go negative */
	int budget = clamp_t(unsigned int, READ_ONCE(vas_fault_page_budget),
			     1, INT_MAX);
	int pages = 0;
	bool is_write, stamp_write;
	u8 fs = crb->stamp.nx.fault_status;
	u8 cc = CSB_CC_FAULT_ADDRESS;
	vm_fault_t flt;
	struct vas_fault_run run;
	int hash_rc = 0, ste_rc = 0;
	bool cut_short = false;

	if (!mm || !ea)
		return cc;

	vas_stat_inc(VAS_STAT_FIXUP);

	/*
	 * A user window's requests name user addresses. Anything else is not
	 * something to fault in on the window's behalf. Checked before taking
	 * a reference, so that refusing the work cannot leak one.
	 */
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
		return cc;
	}

	if (vas_fault_describe(crb, mm, ea, &run)) {
		vas_stat_inc(VAS_STAT_FIXUP_NOT_USER_EA);
		mmput(mm);
		return cc;
	}
	is_write = run.write;
	stamp_write = !!(crb->stamp.nx.flags & NX_FAULT_FLAG_WRITE);
	if (stamp_write != is_write)
		vas_stat_inc(VAS_STAT_STAMP_DIR_DISAGREE);

	end = run.end;
	trace_vas_fault_fixup(pid_vnr(task_ref->pid), ea, end, run.pgsz,
			      is_write, fs, crb->stamp.nx.flags);

	/*
	 * A confined window translates nothing outside its domains: the
	 * segment is absent by policy, not merely not yet inserted, so no
	 * page is walked and the request ends refused, in the direction the
	 * hardware reports.
	 */
	if (task_ref->nmmu_view &&
	    !hash__nmmu_view_allows(task_ref->nmmu_view, ea)) {
		vas_stat_inc(VAS_STAT_FIXUP_REFUSED_DOMAIN);
		cc = stamp_write ? CSB_CC_WR_PROTECTION : CSB_CC_PROTECTION;
		trace_vas_fault_done(pid_vnr(task_ref->pid), ea, 0, budget, 0, 0,
				     cc);
		mmput(mm);
		return cc;
	}

	/*
	 * A refused right is not a missing translation. Faulting the pages
	 * in cannot grant it, the retry meets the same refusal, and 250
	 * tells the process to retry: that is a livelock, and each round of
	 * it costs the kernel a walk of the whole run. So a protection stamp
	 * ends here, with the architected code for the direction the
	 * hardware reports, and the address as the translation case already
	 * gives it. The mapping is asked to agree first; if it does not, the
	 * stamp is not trusted and the walk proceeds as it always has.
	 */
	switch (fs) {
	case NX_FS_PROTECTION:
	case NX_FS_KEY:
		if (vas_fault_refused(mm, ea, task_ref->amr, stamp_write, fs)) {
			vas_stat_inc(stamp_write ? VAS_STAT_FIXUP_REFUSED_STORE
						 : VAS_STAT_FIXUP_REFUSED_LOAD);
			cc = stamp_write ? CSB_CC_WR_PROTECTION
					 : CSB_CC_PROTECTION;
			trace_vas_fault_done(pid_vnr(task_ref->pid), ea, 0,
					     budget, 0, 0, cc);
			mmput(mm);
			return cc;
		}
		vas_stat_inc(VAS_STAT_STAMP_PROT_DISAGREE);
		break;
	case NX_FS_SEGMENT:
	case NX_FS_NO_PTE:
		break;
	default:
		vas_stat_inc(VAS_STAT_STAMP_UNKNOWN);
		break;
	}

	access = _PAGE_PRESENT | _PAGE_READ;
	if (is_write)
		access |= _PAGE_WRITE;

	vas_stat_inc(VAS_STAT_FIXUP_WALKED);

	for (addr = run.start; addr < end; addr += run.pgsz) {
		/*
		 * One fault window serves every window on the chip, so the
		 * work one request may buy has to be bounded independently
		 * of how large its buffer is. Resolving part of the run is
		 * not a failure: the accelerator reissues the request, and
		 * the next fault resumes where this stopped.
		 */
		if (budget-- <= 0) {
			vas_stat_inc(VAS_STAT_WALK_BUDGET);
			cut_short = true;
			break;
		}

		cond_resched();

		if (copro_handle_mm_fault(mm, addr,
					  is_write ? DSISR_ISSTORE : 0, &flt)) {
			vas_stat_inc(VAS_STAT_WALK_PAGE_ERR);
			cut_short = true;
			break;
		}

		vas_stat_inc(VAS_STAT_PAGES_FAULTED);
		pages++;

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
		 * Neither a negative nor a positive return inserted an entry.
		 * Negative is the hash refusing a page it should have taken.
		 * Positive means the walk found no present PTE, or one that
		 * does not permit this access -- the ordinary outcome while
		 * another thread migrates the page or a hinting scan holds it
		 * PROT_NONE. Either way the page stays untranslatable by the
		 * nest MMU and the request comes back for it, so one page is
		 * not a reason to abandon the rest of the run.
		 *
		 * The positive case is counted and not logged: it is expected,
		 * user space sets its rate, and a ratelimited print here would
		 * evict the warnings that do mean something.
		 */
		local_irq_save(flags);
		hash_rc = hash_page_mm(mm, addr, access, 0x300, 0);
		local_irq_restore(flags);
		if (hash_rc < 0) {
			vas_stat_inc(VAS_STAT_PAGES_HASH_ERR);
			pr_warn_ratelimited("VAS: %lx not accepted by the hash table (%d)\n",
					    addr, hash_rc);
		} else if (hash_rc) {
			vas_stat_inc(VAS_STAT_PAGES_HASH_NOINSERT);
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
		ste_rc = task_ref->nmmu_view ?
			 hash__nmmu_view_insert(task_ref->nmmu_view, addr) :
			 hash__nmmu_ste_insert(mm, addr);
		if (ste_rc) {
			vas_stat_inc(VAS_STAT_PAGES_STE_ERR);
			pr_warn_ratelimited("VAS: no segment table entry for %lx (%d)\n",
					    addr, ste_rc);
		}
	}

	if (!cut_short)
		vas_stat_inc(VAS_STAT_WALK_COMPLETED);

	trace_vas_fault_done(pid_vnr(task_ref->pid), ea, pages, budget,
			     hash_rc, ste_rc, cc);
	mmput(mm);
	return cc;
}

/*
 * Faults are resolved on a workqueue, not on the IRQ thread that drains the
 * FIFO. The IRQ core runs that thread SCHED_FIFO, so it does not yield to
 * ordinary tasks, and it is one thread per chip serving every window on it,
 * so a request that takes a long time to resolve holds up the faults of
 * every other window behind it. Taking the CRB off the FIFO and handing it
 * to per-window work makes the FIFO drain quickly, lets the resolution be
 * preempted like any other kernel work, and lets windows proceed in
 * parallel with each other.
 *
 * Nothing about lifetime changes. The send credit for a faulted request is
 * returned by the work that resolved it, as its last touch of the window,
 * and vas_win_close() waits for every credit before it frees anything, so a
 * window with work queued or running cannot go away under it.
 */
struct workqueue_struct *vas_fault_wq;

int vas_fault_ring_alloc(struct pnv_vas_window *window)
{
	window->fault_ring_size = window->vas_win.wcreds_max;
	window->fault_ring = kcalloc(window->fault_ring_size,
				     sizeof(*window->fault_ring), GFP_KERNEL);
	if (!window->fault_ring)
		return -ENOMEM;

	window->fault_head = 0;
	window->fault_tail = 0;
	spin_lock_init(&window->fault_ring_lock);
	INIT_WORK(&window->fault_work, vas_fault_work_fn);
	return 0;
}

void vas_fault_ring_free(struct pnv_vas_window *window)
{
	if (!window->fault_ring)
		return;

	/* the credit wait in close has already drained it; this is the fence */
	cancel_work_sync(&window->fault_work);
	kfree(window->fault_ring);
	window->fault_ring = NULL;
}

static void vas_fault_resolve(struct pnv_vas_window *window,
			      struct coprocessor_request_block *crb)
{
	u8 cc = vas_fault_fixup(crb, &window->vas_win.task_ref);

	vas_update_csb(crb, &window->vas_win.task_ref, cc);
	/*
	 * Last touch of the window: close waits on this credit, and may free
	 * the window as soon as the final one comes back.
	 */
	vas_return_credit(window, true);
}

void vas_fault_work_fn(struct work_struct *work)
{
	struct pnv_vas_window *window = container_of(work, struct pnv_vas_window,
						     fault_work);
	struct coprocessor_request_block crb;
	unsigned long flags;

	for (;;) {
		spin_lock_irqsave(&window->fault_ring_lock, flags);
		if (window->fault_head == window->fault_tail) {
			spin_unlock_irqrestore(&window->fault_ring_lock, flags);
			return;
		}
		crb = window->fault_ring[window->fault_head % window->fault_ring_size];
		window->fault_head++;
		spin_unlock_irqrestore(&window->fault_ring_lock, flags);

		vas_fault_resolve(window, &crb);
	}
}

/*
 * Hand a CRB to the window's work. Returns false if it could not be queued,
 * in which case the caller resolves it inline as the IRQ thread always did.
 */
static bool vas_fault_queue(struct pnv_vas_window *window,
			    struct coprocessor_request_block *crb)
{
	unsigned long flags;
	bool queued = false;

	if (!vas_fault_wq || !window->fault_ring)
		return false;

	spin_lock_irqsave(&window->fault_ring_lock, flags);
	if (window->fault_tail - window->fault_head < window->fault_ring_size) {
		window->fault_ring[window->fault_tail % window->fault_ring_size] = *crb;
		window->fault_tail++;
		queued = true;
	}
	spin_unlock_irqrestore(&window->fault_ring_lock, flags);

	if (queued)
		queue_work(vas_fault_wq, &window->fault_work);
	return queued;
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

		if (IS_ERR_OR_NULL(window)) {
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
				if (!vas_fault_queue(window, crb))
					vas_fault_resolve(window, crb);
			} else {
				WARN_ON_ONCE(!window->user_win);
				vas_return_credit(window, true);
			}
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

	if (!vas_fault_wq) {
		vas_fault_wq = alloc_workqueue("vas-fault", WQ_UNBOUND, 0);
		/*
		 * Separate from the fault queue: a deferred close waits on
		 * hardware, and must not sit in front of the fault work that
		 * is often what lets that hardware finish.
		 */
		vas_close_wq = alloc_workqueue("vas-close", WQ_UNBOUND, 0);
		if (!vas_fault_wq)
			pr_warn("VAS: no fault workqueue; resolving on the IRQ thread\n");
	}

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
	/*
	 * The fault window is notified by interrupt, not by thread wakeup:
	 * vas_init_rx_win_attr() sets notify_disable for VAS_COP_TYPE_FAULT,
	 * which becomes VAS_NOTIFY_DISABLE in LNOTIFY_CTL, and the interrupt
	 * is programmed into HV_INTR_SRC_RA. So LNOTIFY_LPID, LNOTIFY_PID and
	 * LNOTIFY_TID are never consulted for this window.
	 *
	 * Leave them zero rather than filling two of them from SPRN_PID. That
	 * register holds a thread's hardware PID under radix, where it means
	 * nothing here because this is a receive window belonging to no
	 * process, and holds whatever firmware left behind under HPT, where
	 * the core does not maintain it at all.
	 */
	attr.lnotify_lpid = 0;
	attr.lnotify_pid = 0;
	attr.lnotify_tid = 0;

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
