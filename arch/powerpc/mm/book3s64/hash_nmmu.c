// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Segment tables for the nest MMU under HPT translation.
 *
 * A core translates a user effective address through its SLB, which the kernel
 * refills from a fault handler whenever it misses. The nest MMU has an SLB of
 * its own but nothing behind it to service a miss: it walks the process table
 * entry its PIDR selects, and for an HPT partition that entry is expected to
 * name a segment table the hardware can search unaided (Power ISA 3.0B section
 * 5.7.8.3). A kernel that never builds one leaves an accelerator with no way to
 * translate a user address at all.
 *
 * The table is addressed virtually, not really: section 5.7.8.3 places its base
 * "in virtual address space" at STABORG(0:77-q) || q zeros, and each of the
 * four hashes in 5.7.8.3.1 through 5.7.8.3.4 locates the STEG at a host VA. A
 * page from the linear map therefore serves directly, because htab_initialize()
 * has already bolted its HPT entry, and reaching the table through the HPT is
 * exactly what the nest MMU has to do.
 */

#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <asm/copro.h>
#include <asm/firmware.h>
#include <asm/mmu.h>
#include <asm/mmu_context.h>
#include <asm/ppc-opcode.h>
#include <asm/cputable.h>
#include <asm/trace.h>
#include <linux/mmdebug.h>

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/sched/task.h>

static DEFINE_MUTEX(nmmu_segtab_lock);

static void nmmu_view_flush(struct nmmu_view *v);

/*
 * What the nest MMU code has done, for debugfs.
 *
 * These answer the questions that otherwise need a kprobe: whether the
 * segment table is being refilled at all, whether a table is full, and
 * whether a page size change reached it. Plain counters rather than a
 * tracepoint because they must be readable after the fact, and the events
 * are rare enough that the cost does not matter.
 */
static struct {
	atomic_t insert;	/* entries added */
	atomic_t present;	/* asked for, already there */
	atomic_t nospc;		/* both hash groups full */
	atomic_t overflow_flush;	/* table discarded to make room */
	atomic_t nospc_fatal;	/* still no room after a flush */
	atomic_t efault;	/* no translation for the address */
	atomic_t flush;		/* tables emptied by a page size change */
	atomic_t pid_alloc;
	atomic_t pid_free;
	atomic_t view_new;	/* views carried by windows */
	atomic_t view_free;
	atomic_t denied;	/* outside a confined view's domains */
	atomic_t domain_add;
	atomic_t domain_drop;
} nmmu_stat;

/*
 * Segment table geometry, Power ISA 3.0B section 5.7.8.3: the table is a set of
 * 128-byte segment table entry groups of eight 16-byte entries, and its size is
 * 2^q with q = STABSIZE + 12.
 *
 * STABSIZE 0 gives a 4KiB table of 32 STEGs, selected by the five low-order
 * ESID bits: EA(43-q:35) for a 256MB segment and EA(31-q:23) for a 1TB one,
 * sections 5.7.8.3.1 and 5.7.8.3.2. The selector widens with q, so a larger
 * table would spread the segments over more groups; sixteen slots for any one
 * value of those five bits has yet to be too few.
 */
#define NMMU_STABSIZE		0
#define NMMU_STAB_SHIFT		(NMMU_STABORG_SHIFT + NMMU_STABSIZE)	/* q */
#define NMMU_STAB_SIZE		(1UL << NMMU_STAB_SHIFT)
#define NMMU_STES_PER_STEG	8
#define NMMU_STEG_SHIFT		7			/* 128 bytes */
#define NMMU_STEGS		(NMMU_STAB_SIZE >> NMMU_STEG_SHIFT)

/*
 * Does a computed value fit the architected field it is about to be shifted
 * into?
 *
 * Every value in this file is assembled from a VSID, an effective address or
 * a page size and then packed into a field the architecture sizes, so a value
 * that does not fit is a kernel bug and not a condition to handle. It is also
 * invisible: the shift simply drops the top of it, the table is written, and
 * the hardware walks to somewhere else. The segment table origin was wrong
 * that way for three boots of this series before a probe caught it, and the
 * kernel never noticed.
 *
 * VM_WARN_ON_ONCE() costs nothing without CONFIG_DEBUG_VM -- it becomes
 * BUILD_BUG_ON_INVALID(), which still type-checks the expression, so these
 * cannot quietly stop compiling -- and says which field went wrong when it is
 * on.
 */
#define NMMU_FIELD_FITS(v, bits)	(!((unsigned long)(v) >> (bits)))

/*
 * The origin is always scaled by 2^12, whatever the table's size: the base is
 * STABORG(0:77-q) || q zeros with the low q-12 bits of STABORG required to be
 * 0, which leaves STABORG itself a virtual address shifted by 12 either way.
 */
#define NMMU_STABORG_SHIFT	12

/*
 * A segment table entry is the SLB entry of Figure 29 without the slot index.
 * The valid bit, segment size selector, VSID and page size selectors sit at the
 * bit positions the SLB_VSID_* constants already name, so an entry is the pair
 * of words copro_calculate_slb() returns, stored as they are.
 */
struct nmmu_ste {
	__be64 esid_data;
	__be64 vsid_data;
};

/*
 * A view of an mm for the nest MMU: one hardware PID, and the segment table
 * its process table entry selects, with the lock every change to the table
 * is made under. An mm has one view of its own, and may have more, each
 * carried by a window that translates through a PID of its own; the mm's
 * own view keeps the list of the others.
 *
 * The lock is a spinlock because one of the callers cannot sleep: a slice
 * changing page size reaches hash__nmmu_segtab_flush() from hash_page_mm(),
 * and the nest MMU fault path and window opening are the others.
 *
 * It is also what keeps an entry from being built against a slice page size
 * that is no longer true, and the argument is narrower than "everyone holds
 * the lock": slice_convert() writes the new sizes under its own lock and
 * calls the flush after releasing it, and nothing here ever takes that lock.
 * What holds is that the inserter reads the sizes inside *this* lock. An
 * inserter that read the old size took the lock before the flusher, so the
 * flush waits for it and removes what it wrote; one that takes the lock
 * after the flush sees the new size through that release/acquire. Hoisting
 * the size read out of the lock, or calling the flush before the sizes are
 * written, breaks this silently.
 */
/* Segments a confined view may translate: [start, end), segment aligned. */
struct nmmu_domain {
	unsigned long start;
	unsigned long end;
	struct list_head node;
};

struct nmmu_view {
	int hw_pid;
	struct mm_struct *mm;
	spinlock_t lock;	/* every read of the slices and write of ste */
	struct nmmu_ste *ste;
	struct list_head node;		/* on the mm's own view's others */
	struct list_head others;	/* the mm's own view: the rest of them */
	spinlock_t others_lock;		/* outer to lock */
	bool confined;			/* translates only its domains */
	struct list_head domains;	/* under lock */
};

/* Process Table Entry, HPT variant, Power ISA 3.0B Figure 23. */
enum prte_hpt_field {
	PRTE_HPT_B_SHIFT	= 62,	/* dw0 0:1,   segment table segment size */
	PRTE_HPT_STABORGL_SHIFT	= 60,	/* dw1 0:3,   origin, lower */
	PRTE_HPT_STABSIZE_SHIFT	= 4,	/* dw1 56:59, table size */
	PRTE_HPT_STPS_SHIFT	= 1,	/* dw1 60:62, page size holding the table */
	PRTE_HPT_V		= 1,	/* dw1 63,    valid */
	PRTE_HPT_STABORGL_BITS	= 4,
};

static inline int nmmu_sid_shift(int ssize)
{
	return ssize == MMU_SEGSIZE_1T ? SID_SHIFT_1T : SID_SHIFT;
}

/*
 * Point this mm's process table entry at its segment table. The origin is a
 * virtual address scaled by 2^12, split across the two doublewords as
 * STABORGU || STABORGL.
 */
static int nmmu_prte_set(int hw_pid, void *stab)
{
	unsigned long ea = (unsigned long)stab;
	unsigned long vsid, off, staborgu, staborgl, dw0, dw1;
	int s = nmmu_sid_shift(mmu_kernel_ssize);

	vsid = get_kernel_vsid(ea, mmu_kernel_ssize);
	if (!vsid) {
		pr_err("nest MMU: no VSID for the segment table at 0x%lx\n", ea);
		return -EINVAL;
	}

	/*
	 * The origin is assembled from the VSID and the offset within the
	 * segment rather than from the virtual address, because that address
	 * does not fit a register: a 1TB VSID is 38 bits and the segment is
	 * 40, so the virtual address is 78 bits and STABORG itself is 66.
	 * Forming either one whole truncates the top of the VSID silently.
	 *
	 * STABORG is VSID || EA(offset page number), so its upper 62 bits and
	 * lower 4 can each be built without the whole ever existing.
	 */
	off = (ea & ((1UL << s) - 1)) >> NMMU_STABORG_SHIFT;

	staborgu = (vsid << (s - NMMU_STABORG_SHIFT - PRTE_HPT_STABORGL_BITS)) |
		   (off >> PRTE_HPT_STABORGL_BITS);
	staborgl = off & ((1UL << PRTE_HPT_STABORGL_BITS) - 1);

	VM_WARN_ON_ONCE(!NMMU_FIELD_FITS(staborgu, 62));
	VM_WARN_ON_ONCE(!NMMU_FIELD_FITS(staborgl, PRTE_HPT_STABORGL_BITS));

	/*
	 * And the postcondition that matters: read the origin back out of the
	 * two fields and check it still names the segment it was built from.
	 *
	 * A width check alone would not have caught the bug this is here for.
	 * The virtual address was truncated before it was packed, so the value
	 * that reached these fields was smaller than the field and fitted
	 * perfectly; it simply pointed somewhere else. Only reconstructing it
	 * and comparing against the VSID notices that.
	 */
	VM_WARN_ON_ONCE((((staborgu << PRTE_HPT_STABORGL_BITS) | staborgl) >>
			 (s - NMMU_STABORG_SHIFT)) != vsid);
	VM_WARN_ON_ONCE(!NMMU_FIELD_FITS(mmu_kernel_ssize, 2));
	VM_WARN_ON_ONCE(!NMMU_FIELD_FITS(get_sllp_encoding(mmu_linear_psize), 3));
	VM_WARN_ON_ONCE(!NMMU_FIELD_FITS(NMMU_STABSIZE, 4));

	dw0 = ((unsigned long)mmu_kernel_ssize << PRTE_HPT_B_SHIFT) | staborgu;

	dw1 = (staborgl << PRTE_HPT_STABORGL_SHIFT) |
	      ((unsigned long)NMMU_STABSIZE << PRTE_HPT_STABSIZE_SHIFT) |
	      /*
	       * STPS is the page size holding the table, which Figure 23 gives
	       * in "L||LP encoding as in SLBE". The table came from the linear
	       * map, so that is the size the linear map is bolted at.
	       */
	      (get_sllp_encoding(mmu_linear_psize) << PRTE_HPT_STPS_SHIFT);

	/*
	 * The origin has to reach memory before the valid bit that tells the
	 * hardware to follow it. ptesync rather than a smp_* barrier because
	 * the reader is the nest MMU loading the entry out of the L2, which is
	 * the same ordering radix__init_new_context() states its case for.
	 */
	process_tb[hw_pid].prtb0 = cpu_to_be64(dw0);
	asm volatile("ptesync" : : : "memory");
	process_tb[hw_pid].prtb1 = cpu_to_be64(dw1 | PRTE_HPT_V);
	asm volatile("ptesync" : : : "memory");

	return 0;
}

/*
 * The segment table entry group an address hashes to. half 0 is the primary
 * group, half 1 the secondary, which is the ones-complement of the same
 * selector.
 */
static struct nmmu_ste *nmmu_steg(struct nmmu_ste *stab, unsigned long ea,
				  int ssize, int half)
{
	unsigned long sel = ea >> nmmu_sid_shift(ssize);

	if (half)
		sel = ~sel;

	VM_WARN_ON_ONCE(!NMMU_FIELD_FITS(sel % NMMU_STEGS, 5));

	return stab + ((sel % NMMU_STEGS) * NMMU_STES_PER_STEG);
}

/*
 * Install a segment table entry for the segment containing ea. The STEG is
 * selected by the low-order ESID bits and holds eight entries; an entry already
 * describing this segment is left alone.
 *
 * The entry is what copro_calculate_slb() returns: the SLB entry the core would
 * load for this address, with the same VSID, segment size, protection keys and
 * page size selectors. Figure 29 is that entry without the slot index, and the
 * bits the SLB spends on the index are reserved in the table, so the two words
 * are stored as they come. cxl and spufs filled their accelerators' segment
 * caches from the same function, for the same reason: the nest MMU has to
 * translate exactly as the core would, and one builder for both cannot drift.
 */
static int nmmu_ste_insert(struct nmmu_ste *stab, struct mm_struct *mm,
			   unsigned long ea)
{
	unsigned long esid_data, vsid_data;
	struct copro_slb slb;
	struct nmmu_ste *steg;
	int ssize, i, half;

	if (copro_calculate_slb(mm, ea, &slb))
		return -EFAULT;

	esid_data = slb.esid;
	vsid_data = slb.vsid;
	ssize = (vsid_data & SLB_VSID_B) >> SLB_VSID_SSIZE_SHIFT;

	/*
	 * There are two groups an entry may live in, and the hardware searches
	 * both. Power ISA 3.0B sections 5.7.8.3.1 and 5.7.8.3.2 give the
	 * primary for 256MB and 1TB segments as EA(43-q:35) and EA(31-q:23),
	 * which at q=12 are both the five bits this shift selects; 5.7.8.3.3
	 * and 5.7.8.3.4 give the secondary as the ones-complement of the same
	 * field. So a full primary group is not a reason to give up.
	 *
	 * Both groups are searched for an existing entry before either is
	 * written, because the architecture requires software to ensure no two
	 * entries match one ESID, and an entry already in the secondary would
	 * otherwise be duplicated into the primary.
	 */
	for (half = 0; half < 2; half++) {
		steg = nmmu_steg(stab, ea, ssize, half);

		for (i = 0; i < NMMU_STES_PER_STEG; i++)
			if (be64_to_cpu(steg[i].esid_data) == esid_data) {
				atomic_inc(&nmmu_stat.present);
				return 0;
			}
	}

	for (half = 0; half < 2; half++) {
		steg = nmmu_steg(stab, ea, ssize, half);

		for (i = 0; i < NMMU_STES_PER_STEG; i++) {
			if (be64_to_cpu(steg[i].esid_data) & SLB_ESID_V)
				continue;

			/*
			 * The sequence of section 5.10.1.1, which says it "may
			 * be used to add a new Segment Table Entry": the word
			 * without the valid bit first, eieio to order it
			 * before the word with, and ptesync after both to
			 * order them before the next table search.
			 */
			steg[i].vsid_data = cpu_to_be64(vsid_data);
			asm volatile("eieio" : : : "memory");
			steg[i].esid_data = cpu_to_be64(esid_data);
			asm volatile("ptesync" : : : "memory");
			atomic_inc(&nmmu_stat.insert);
			return 0;
		}
	}

	return -ENOSPC;
}

/*
 * Make the segment holding ea translatable through this view.
 *
 * This is the refill: the nest MMU has no fault handler of its own, so a
 * segment it needs and the table does not describe is reported back through
 * the accelerator and lands here from the VAS fault path. It is also how the
 * table is filled when it is created, and how it is filled again after a
 * flush.
 *
 * -EFAULT if the address has no segment translation, which is the same answer
 * the core would give; -ENOSPC if both groups the segment hashes to are full.
 */
/* Under v->lock. A view that is not confined allows every address. */
static bool nmmu_view_allows(struct nmmu_view *v, unsigned long ea)
{
	struct nmmu_domain *d;

	if (!v->confined)
		return true;
	list_for_each_entry(d, &v->domains, node)
		if (ea >= d->start && ea < d->end)
			return true;
	return false;
}

bool hash__nmmu_view_allows(struct nmmu_view *v, unsigned long ea)
{
	unsigned long flags;
	bool ok;

	spin_lock_irqsave(&v->lock, flags);
	ok = nmmu_view_allows(v, ea);
	spin_unlock_irqrestore(&v->lock, flags);
	return ok;
}

static int nmmu_view_insert(struct nmmu_view *v, unsigned long ea,
			    bool may_flush)
{
	unsigned long flags;
	int rc;

	spin_lock_irqsave(&v->lock, flags);
	if (nmmu_view_allows(v, ea))
		rc = nmmu_ste_insert(v->ste, v->mm, ea);
	else
		rc = -EACCES;
	spin_unlock_irqrestore(&v->lock, flags);

	if (rc == -EACCES) {
		atomic_inc(&nmmu_stat.denied);
		return rc;
	}
	if (rc == -ENOSPC) {
		atomic_inc(&nmmu_stat.nospc);
		if (!may_flush)
			return rc;
		/*
		 * Both groups this segment hashes to are full. The table is a
		 * cache of what the nest MMU has been told, so discarding it
		 * costs re-insertions and nothing else, while leaving the
		 * entry unmade costs the caller a fault that cannot be
		 * resolved and will be retried forever. Every group is empty
		 * afterwards, so the second attempt cannot fail for space.
		 */
		nmmu_view_flush(v);
		atomic_inc(&nmmu_stat.overflow_flush);

		spin_lock_irqsave(&v->lock, flags);
		rc = nmmu_ste_insert(v->ste, v->mm, ea);
		spin_unlock_irqrestore(&v->lock, flags);

		if (rc == -ENOSPC)
			atomic_inc(&nmmu_stat.nospc_fatal);
	}

	if (rc && rc != -ENOSPC)
		atomic_inc(&nmmu_stat.efault);

	return rc;
}

/*
 * Fault path, through the mm's own view: the caller cannot make progress
 * without this entry, so make room for it. -ENODEV if the mm has no view,
 * which means no accelerator has been given its PID and there is nothing to
 * refill.
 */
int hash__nmmu_ste_insert(struct mm_struct *mm, unsigned long ea)
{
	/* Pairs with the release in hash__nmmu_segtab_alloc(). */
	struct nmmu_view *v = smp_load_acquire(&mm->context.nmmu_view);

	if (!v)
		return -ENODEV;
	return nmmu_view_insert(v, ea, true);
}

/* Fault path, through a window's own view. */
int hash__nmmu_view_insert(struct nmmu_view *v, unsigned long ea)
{
	return nmmu_view_insert(v, ea, true);
}

int hash__nmmu_view_pid(const struct nmmu_view *v)
{
	return v->hw_pid;
}

/*
 * Give every segment the mm has mapped an entry, so that the first request
 * through a new window does not have to fault once per segment to get them.
 * Anything mapped later is picked up by hash__nmmu_ste_insert() from the fault
 * path.
 *
 * Best effort by design. A group holds eight entries, so an mm whose segments
 * collide in one of them has a segment the accelerator cannot reach; that is a
 * segment to report, not a reason to refuse the window, and it is the same
 * position every address is in before this table exists at all.
 */
static void nmmu_prefault(struct nmmu_view *v)
{
	struct mm_struct *mm = v->mm;
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;
	unsigned long ea;
	int mapped = 0, failed = 0;
	int budget = NMMU_STAB_SIZE / sizeof(struct nmmu_ste);

	mmap_read_lock(mm);
	for_each_vma(vmi, vma) {
		unsigned long seg;

		/*
		 * Step from the start of the segment holding vm_start, not
		 * from vm_start itself. Advancing by a whole segment from an
		 * unaligned address keeps that offset, so the last segment of
		 * the VMA is skipped whenever the tail sits at a lower offset
		 * within its segment than vm_start does within its own -- for
		 * a randomly placed VMA that crosses a boundary, about half
		 * the time. The step is re-derived each iteration because the
		 * segment size changes at 1TB.
		 */
		seg = 1UL << nmmu_sid_shift(user_segment_size(vma->vm_start));
		for (ea = ALIGN_DOWN(vma->vm_start, seg); ea < vma->vm_end; ) {
			if (budget-- <= 0)
				goto done;

			if (nmmu_view_insert(v, ea, false))
				failed++;
			else
				mapped++;

			seg = 1UL << nmmu_sid_shift(user_segment_size(ea));
			ea = ALIGN_DOWN(ea, seg) + seg;

			/*
			 * A mapping that spans the low 1TB is four thousand
			 * segments of 256MB, each taking the table lock with
			 * interrupts off, all inside one ioctl. The lock is
			 * dropped between entries, so let everything else in
			 * too. mmap_read_lock allows sleeping.
			 */
			cond_resched();
		}
	}
done:
	mmap_read_unlock(mm);

	if (failed)
		pr_warn("nest MMU: pid %d, %d of %d segments have no entry\n",
			v->hw_pid, failed, mapped + failed);
}

/*
 * Drop everything the nest MMU has cached for a hardware PID.
 *
 * slbiag is the instruction the architecture names for this. Power ISA 3.0B
 * section 5.9.3.2: "When taking a PID out of service with the intent of
 * reusing it, software should use slbiag to remove stale translations from
 * SLBs and ERATs in the 'nest.'" It invalidates every nest SLB entry for the
 * PID at once, and with them the implementation-specific lookaside information
 * derived from those entries, which is where an accelerator's ERAT lives. The
 * per-entry slbieg reaches only the entries software still knows about; this
 * reaches the ones it has forgotten, or that a walk installed after the table
 * stopped describing them.
 *
 * RS is PID(0:31) || LPID(32:63). LPID is zero: this runs on the host
 * partition, and in hypervisor state the instruction takes the LPID from RS
 * rather than from LPIDR.
 *
 * The ordering is the one section 5.10.1.2 gives for segment table updates,
 * with slbiag standing in for slbieg. The leading ptesync orders the table
 * stores the caller has made before the invalidation; eieio orders the
 * invalidation before slbsync; and slbsync followed by ptesync is what makes
 * the invalidation complete on every other agent before anything after it
 * runs. The ISA is explicit that slbsync alone does not wait: "The slbsync
 * instruction may complete before operations caused by slbieg or slbiag
 * instructions preceding the slbsync instruction have been performed."
 *
 * POWER9 User's Manual section 4.10.11: the core does not invalidate its own
 * SLB on these instructions when UPRT=0, so nothing here reaches the core's
 * translation, and section 4.10 lists slbiag as hypervisor-privileged with
 * GTSE=0, which is what a host runs with.
 */
static void nmmu_slbiag(int hw_pid)
{
	unsigned long rs = (unsigned long)hw_pid << 32;

	VM_WARN_ON_ONCE(!NMMU_FIELD_FITS(hw_pid, 32));

	asm volatile("ptesync" : : : "memory");
	asm volatile(PPC_SLBIAG(%0) : : "r" (rs) : "memory");
	asm volatile("eieio" : : : "memory");
	asm volatile(PPC_SLBSYNC : : : "memory");
	asm volatile("ptesync" : : : "memory");
}

/*
 * tlbie operand encodings, Power ISA 3.0B section 5.9.3.3.
 */
enum tlbie_ric {
	TLBIE_RIC_TLB		= 0,	/* just the TLB */
	TLBIE_RIC_PWC		= 1,	/* just the page walk cache */
	TLBIE_RIC_TABLES	= 2,	/* those, and cached In-Memory Table Entries */
};

enum tlbie_prs {
	TLBIE_PRS_PARTITION	= 0,
	TLBIE_PRS_PROCESS	= 1,
};

enum tlbie_r {
	TLBIE_R_HPT		= 0,
	TLBIE_R_RADIX		= 1,
};

enum tlbie_is {
	TLBIE_IS_VA		= 0,	/* just the target virtual address */
	TLBIE_IS_PID		= 1,	/* everything matching the PID */
	TLBIE_IS_LPID		= 2,
	TLBIE_IS_ALL		= 3,
};

#define TLBIE_IS_SHIFT		10	/* RB bits 52:53 */

/*
 * Drop the nest MMU's cached copy of this mm's process table entry.
 *
 * Required, not defensive. Section 5.9.3.3: "When reassigning an LPID or PID,
 * after updating the Partition and/or Process Table(s) software must use a
 * tlbie instruction to remove lookaside information associated with the old
 * parition or process." Clearing the entry in memory does not reach a copy
 * the hardware is already holding, and freeing the segment table underneath
 * one is worse than leaving it stale: the next walk reads whatever the page
 * has become.
 *
 * Under HPT there is exactly one legal form and the architecture says why.
 * PRS=1 with R=0 is listed invalid for every RIC other than 2, because "The
 * only process-scoped HPT caching is of the Process Table", so RIC=2, PRS=1,
 * R=0, IS=1 names the process table caching for this PID and nothing else.
 *
 * RS is PID(0:31) || LPID(32:63), as for slbieg. RB carries only the
 * invalidation selector. LPID is zero for the same reason it is there.
 */
static void nmmu_tlbie_prte(unsigned long rs, unsigned long rb)
{
	asm volatile(PPC_TLBIE_5(%0, %1, %2, %3, %4)
		     : : "r" (rb), "r" (rs),
			 "i" (TLBIE_RIC_TABLES),
			 "i" (TLBIE_PRS_PROCESS),
			 "i" (TLBIE_R_HPT)
		     : "memory");
}

/*
 * The POWER9 tlbie errata, worked around the way fixup_tlbie_vpn() does in
 * hash_native.c. Every other tlbie in the tree carries this; one without it
 * may simply not take effect, and an invalidation that does not take effect
 * is worse here than elsewhere. The process table entry would stay cached
 * against a hardware PID that has been returned to the allocator, so the next
 * mm to be given that PID has its accelerator requests translated through a
 * segment table page that has been freed and reused.
 */
static void nmmu_tlbie_fixup(unsigned long rs, unsigned long rb)
{
	if (cpu_has_feature(CPU_FTR_P9_TLBIE_ERAT_BUG)) {
		/*
		 * A radix-format flush for a hash guest, as hash_native.c
		 * issues. The extra ptesync is what keeps it from being
		 * reordered ahead of the invalidation it is fixing up.
		 */
		unsigned long frb = PPC_BIT(52);	/* IS = 2, by LPID */

		asm volatile("ptesync" : : : "memory");
		asm volatile(PPC_TLBIE_5(%0, %4, %3, %2, %1)
			     : : "r" (frb), "i" (1), "i" (0), "i" (0),
				 "r" (0UL)
			     : "memory");
	}

	if (cpu_has_feature(CPU_FTR_P9_TLBIE_STQ_BUG)) {
		asm volatile("ptesync" : : : "memory");
		nmmu_tlbie_prte(rs, rb);
	}
}

static void nmmu_prte_invalidate(int hw_pid)
{
	unsigned long rs = (unsigned long)hw_pid << 32;
	unsigned long rb = (unsigned long)TLBIE_IS_PID << TLBIE_IS_SHIFT;

	/* RS is PID(0:31) || LPID(32:63); a wider PID would land in the LPID. */
	VM_WARN_ON_ONCE(!NMMU_FIELD_FITS(hw_pid, 32));

	asm volatile("ptesync" : : : "memory");
	nmmu_tlbie_prte(rs, rb);
	nmmu_tlbie_fixup(rs, rb);
	asm volatile("eieio; tlbsync; ptesync" : : : "memory");

	trace_tlbie(0, 0, rb, rs, TLBIE_RIC_TABLES, TLBIE_PRS_PROCESS,
		    TLBIE_R_HPT);
}

/* Drop every entry of one view and everything the nest MMU cached for it. */
static void nmmu_view_flush(struct nmmu_view *v)
{
	unsigned long flags;
	int i;

	spin_lock_irqsave(&v->lock, flags);
	for (i = 0; i < NMMU_STAB_SIZE / sizeof(*v->ste); i++)
		v->ste[i].esid_data = 0;
	nmmu_slbiag(v->hw_pid);
	spin_unlock_irqrestore(&v->lock, flags);

	atomic_inc(&nmmu_stat.flush);
}

/*
 * Drop every entry in every view of this mm, because the segments no longer
 * mean what the entries say.
 *
 * Called when a slice changes page size. The entry for a segment carries the
 * slice's page size in its L and LP fields, and the nest MMU hashes the page
 * table with it, so an entry written for the old size finds the old size's
 * groups and nothing in them. The core handles the same change by flushing
 * its SLB and refilling on the next miss; this is the same for the tables,
 * with the fault path as the refill.
 *
 * Power ISA 3.0B section 5.9.3.2: "After updating a Segment Table Entry,
 * software must use an slbie or slbieg instruction to remove lookaside
 * information associated with the old contents of the entry." The entries are
 * all invalidated rather than the ones the change touched, so the one
 * instruction that removes everything cached for the PID does, and section
 * 5.10.1.2's deletion sequence is what orders the stores before it.
 *
 * May not sleep: this is reachable from hash_page_mm() through
 * demote_segment_4k(), with interrupts off.
 */
void hash__nmmu_segtab_flush(struct mm_struct *mm)
{
	struct nmmu_view *v, *o;
	unsigned long flags;

	/* Pairs with the release in hash__nmmu_segtab_alloc(). */
	v = smp_load_acquire(&mm->context.nmmu_view);
	if (!v)
		return;

	nmmu_view_flush(v);
	spin_lock_irqsave(&v->others_lock, flags);
	list_for_each_entry(o, &v->others, node)
		nmmu_view_flush(o);
	spin_unlock_irqrestore(&v->others_lock, flags);
}

/*
 * A view of mm translating under hw_pid, with an empty table the process
 * table entry already points at. An empty table is a valid one: a walk that
 * finds no entry reports a segment fault, which is the condition the refill
 * exists to answer, and the entries go in afterwards through the same add
 * sequence the fault path uses on a live table.
 */
static struct nmmu_view *nmmu_view_alloc(struct mm_struct *mm, int hw_pid)
{
	struct nmmu_view *v;
	int rc;

	v = kzalloc(sizeof(*v), GFP_KERNEL);
	if (!v)
		return ERR_PTR(-ENOMEM);
	v->hw_pid = hw_pid;
	v->mm = mm;
	spin_lock_init(&v->lock);
	spin_lock_init(&v->others_lock);
	INIT_LIST_HEAD(&v->node);
	INIT_LIST_HEAD(&v->others);
	INIT_LIST_HEAD(&v->domains);

	v->ste = (struct nmmu_ste *)get_zeroed_page(GFP_KERNEL);
	if (!v->ste) {
		kfree(v);
		return ERR_PTR(-ENOMEM);
	}

	rc = nmmu_prte_set(hw_pid, v->ste);
	if (rc) {
		free_page((unsigned long)v->ste);
		kfree(v);
		return ERR_PTR(rc);
	}
	return v;
}

/*
 * Build the mm's own view for an mm that is about to drive an accelerator.
 * Called once, from the same place its hardware PID is allocated.
 */
int hash__nmmu_segtab_alloc(struct mm_struct *mm, int hw_pid)
{
	struct nmmu_view *v;

	/*
	 * Only where this kernel owns the process table. Under a hypervisor it
	 * belongs to the hypervisor -- pseries registers one with
	 * H_REGISTER_PROC_TBL, or none at all for an HPT guest -- so a guest
	 * has no entries of its own to write, and hash_init_process_table()
	 * does not run there for the same reason. The PID the caller allocated
	 * is still meaningful; it is what the window carries.
	 */
	if (firmware_has_feature(FW_FEATURE_LPAR))
		return 0;

	if (!process_tb)
		return -ENODEV;

	BUILD_BUG_ON(NMMU_STAB_SIZE > PAGE_SIZE);
	BUILD_BUG_ON(sizeof(struct nmmu_ste) * NMMU_STES_PER_STEG !=
		     (1UL << NMMU_STEG_SHIFT));

	/*
	 * Window opening is rare and already slow, so one lock for all of it is
	 * cheaper than a per-mm one and makes the loser of a hardware PID race
	 * wait for the winner's view rather than return before it exists.
	 */
	mutex_lock(&nmmu_segtab_lock);
	if (mm->context.nmmu_view) {
		mutex_unlock(&nmmu_segtab_lock);
		return 0;
	}

	v = nmmu_view_alloc(mm, hw_pid);
	if (IS_ERR(v)) {
		mutex_unlock(&nmmu_segtab_lock);
		return PTR_ERR(v);
	}

	/*
	 * Publish with release ordering against the lock and pointer above:
	 * the flush path can find the view from any context and takes the
	 * lock it finds.
	 */
	smp_store_release(&mm->context.nmmu_view, v);
	atomic_inc(&nmmu_stat.pid_alloc);
	mutex_unlock(&nmmu_segtab_lock);

	nmmu_prefault(v);

	return 0;
}

/*
 * Take this mm's hardware PID out of service.
 *
 * The order is set by what the hardware may still be holding. Power ISA 3.0B
 * section 5.9.3.3: "When reassigning an LPID or PID, after updating the
 * Partition and/or Process Table(s) software must use a tlbie instruction to
 * remove lookaside information associated with the old partition or process."
 * Clearing the entries in memory is the update; the tlbie is what reaches a
 * process table entry the nest MMU has already cached, and until it has run
 * a walk for this PID can still find the old table. Only then does slbiag
 * drop the segment entries such a walk had installed, so that nothing can
 * re-install one behind it.
 *
 * The table is cleared, not just unlinked. An entry left valid in a page that
 * is about to be freed is a description of this mm's segments that outlives
 * the mm: if any cached copy of the process table entry survived the tlbie,
 * the walk lands here and finds VSIDs that hash to the previous owner's page
 * table groups. Section 5.7.6.2 gives the valid bit precisely so that a table
 * can be taken out of use while it is changed, and the deletion sequence in
 * section 5.10.1.2 begins with it: "STE_V <- 0", ptesync, then the
 * invalidation.
 */
static void nmmu_view_free(struct nmmu_view *v)
{
	struct nmmu_ste *stab = v->ste;
	int hw_pid = v->hw_pid;
	int i;

	for (i = 0; i < NMMU_STAB_SIZE / sizeof(*stab); i++)
		stab[i].esid_data = 0;

	if (process_tb && hw_pid != MMU_HW_PID_NONE) {
		process_tb[hw_pid].prtb1 = 0;
		asm volatile("ptesync" : : : "memory");
		process_tb[hw_pid].prtb0 = 0;

		nmmu_prte_invalidate(hw_pid);
		nmmu_slbiag(hw_pid);
	}

	free_page((unsigned long)stab);
	kfree(v);
}

void hash__nmmu_segtab_free(struct mm_struct *mm)
{
	struct nmmu_view *v = mm->context.nmmu_view;

	if (!v)
		return;

	/* Every other view is carried by a window, and none is left by now. */
	VM_WARN_ON_ONCE(!list_empty(&v->others));

	mm->context.nmmu_view = NULL;
	nmmu_view_free(v);
	atomic_inc(&nmmu_stat.pid_free);
}

/*
 * A view of mm for one window: a PID of its own, an empty table, and no
 * domains, so it translates nothing until one is added. The mm's own view
 * must exist, because it is what the core's PIDR names and what keeps the
 * list. -EOPNOTSUPP where this kernel does not own the process table.
 */
struct nmmu_view *hash__nmmu_view_new(struct mm_struct *mm)
{
	struct nmmu_view *own, *v;
	unsigned long flags;
	int pid;

	if (firmware_has_feature(FW_FEATURE_LPAR) || !process_tb)
		return ERR_PTR(-EOPNOTSUPP);
	own = smp_load_acquire(&mm->context.nmmu_view);
	if (!own)
		return ERR_PTR(-EINVAL);

	pid = hash__hw_pid_get();
	if (pid < 0)
		return ERR_PTR(pid);
	v = nmmu_view_alloc(mm, pid);
	if (IS_ERR(v)) {
		hash__hw_pid_put(pid);
		return v;
	}
	v->confined = true;

	spin_lock_irqsave(&own->others_lock, flags);
	list_add(&v->node, &own->others);
	spin_unlock_irqrestore(&own->others_lock, flags);
	atomic_inc(&nmmu_stat.view_new);
	return v;
}

/*
 * Retire a window's view. The caller has waited for the hardware to be done
 * with the window, so nothing translates through the PID any more; the mm
 * is still allocated, because the window holds it.
 */
void hash__nmmu_view_free(struct nmmu_view *v)
{
	struct nmmu_view *own = v->mm->context.nmmu_view;
	struct nmmu_domain *d, *tmp;
	unsigned long flags;
	int pid = v->hw_pid;

	spin_lock_irqsave(&own->others_lock, flags);
	list_del(&v->node);
	spin_unlock_irqrestore(&own->others_lock, flags);

	list_for_each_entry_safe(d, tmp, &v->domains, node) {
		list_del(&d->node);
		kfree(d);
	}
	nmmu_view_free(v);
	hash__hw_pid_put(pid);
	atomic_inc(&nmmu_stat.view_free);
}

/*
 * A domain's bounds at segment granularity: the start rounded down to the
 * segment holding it, the end up to the next boundary, each at the segment
 * size of its own address, since the size changes at 1TB.
 */
static int nmmu_domain_bounds(struct mm_struct *mm, unsigned long start,
			      unsigned long len, unsigned long *s,
			      unsigned long *e)
{
	unsigned long end, seg;

	if (!len || check_add_overflow(start, len, &end))
		return -EINVAL;
	/* The slice map, which the entries are built from, ends here. */
	if (end > mm_ctx_slb_addr_limit(&mm->context))
		return -EINVAL;

	seg = 1UL << nmmu_sid_shift(user_segment_size(start));
	*s = ALIGN_DOWN(start, seg);
	seg = 1UL << nmmu_sid_shift(user_segment_size(end - 1));
	*e = ALIGN(end, seg);
	return 0;
}

/* Insert the mapped segments of [s, e), best effort, outside the lock. */
static void nmmu_view_fill(struct nmmu_view *v, unsigned long s,
			   unsigned long e)
{
	unsigned long ea, seg;

	for (ea = s; ea < e; ea = ALIGN_DOWN(ea, seg) + seg) {
		seg = 1UL << nmmu_sid_shift(user_segment_size(ea));
		nmmu_view_insert(v, ea, false);
		cond_resched();
	}
}

/*
 * Let the view translate a range. Its segments are given entries at once
 * where the mm has mappings for them; the rest are inserted on demand.
 */
int hash__nmmu_view_allow(struct nmmu_view *v, unsigned long start,
			  unsigned long len)
{
	struct nmmu_domain *d;
	unsigned long s, e, flags;
	int rc;

	rc = nmmu_domain_bounds(v->mm, start, len, &s, &e);
	if (rc)
		return rc;
	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->start = s;
	d->end = e;

	spin_lock_irqsave(&v->lock, flags);
	list_add(&d->node, &v->domains);
	spin_unlock_irqrestore(&v->lock, flags);
	atomic_inc(&nmmu_stat.domain_add);

	nmmu_view_fill(v, s, e);
	return 0;
}

/*
 * Withdraw a range added exactly as given. The view's table is emptied and
 * everything the nest MMU cached for its PID invalidated, so the next access
 * to any of the range's segments faults, and is refused. The remaining
 * domains' mapped segments are then given their entries again, so a drop
 * costs the engine nothing on the domains that stay.
 */
int hash__nmmu_view_deny(struct nmmu_view *v, unsigned long start,
			 unsigned long len)
{
	struct nmmu_domain *d, *found = NULL;
	unsigned long s, e, flags;
	unsigned long (*keep)[2];
	int rc, n = 0, i;

	rc = nmmu_domain_bounds(v->mm, start, len, &s, &e);
	if (rc)
		return rc;

	spin_lock_irqsave(&v->lock, flags);
	list_for_each_entry(d, &v->domains, node) {
		if (d->start == s && d->end == e) {
			found = d;
			list_del(&d->node);
			break;
		}
	}
	list_for_each_entry(d, &v->domains, node)
		n++;
	spin_unlock_irqrestore(&v->lock, flags);
	if (!found)
		return -ENOENT;
	kfree(found);

	/* The list can change only under the descriptor's own serialisation. */
	keep = kcalloc(n, sizeof(*keep), GFP_KERNEL);
	spin_lock_irqsave(&v->lock, flags);
	i = 0;
	list_for_each_entry(d, &v->domains, node) {
		if (!keep || i >= n)
			break;
		keep[i][0] = d->start;
		keep[i][1] = d->end;
		i++;
	}
	spin_unlock_irqrestore(&v->lock, flags);

	nmmu_view_flush(v);
	atomic_inc(&nmmu_stat.domain_drop);

	for (i = 0; keep && i < n; i++)
		nmmu_view_fill(v, keep[i][0], keep[i][1]);
	kfree(keep);
	return 0;
}

#ifdef CONFIG_DEBUG_FS
/*
 * Dump one mm's nest MMU state to the log, checking each entry against what
 * the core would use for the same segment. The hardware reports a translation
 * failure as a completion code and a fault address and says nothing about
 * which of the process table entry, the segment table or the page table it
 * could not follow, so the three have to be read back and compared by hand.
 *
 *	echo <pid> > /sys/kernel/debug/powerpc/nmmu_segtab
 */
static void nmmu_view_dump(struct nmmu_view *v, u64 val, struct nmmu_ste *stab)
{
	struct mm_struct *mm = v->mm;
	unsigned long flags;
	int i, valid = 0, bad = 0;

	/*
	 * Snapshot the table instead of walking it live. The inserter writes
	 * the VSID word before the ESID word, so a live walk can pair a valid
	 * ESID with a stale VSID and print a MISMATCH that never existed --
	 * misleading exactly the debugging session this exists for. The
	 * validation stays outside the lock; a slice changing size while the
	 * snapshot is checked can still show as a mismatch, but a real one,
	 * of a moment that existed.
	 */
	spin_lock_irqsave(&v->lock, flags);
	memcpy(stab, v->ste, NMMU_STAB_SIZE);
	spin_unlock_irqrestore(&v->lock, flags);

	pr_info("nest MMU: pid %llu hw_pid %d segtab %p%s\n", val, v->hw_pid,
		v->ste, v == mm->context.nmmu_view ? "" : " (window view)");

	if (process_tb && v->hw_pid != MMU_HW_PID_NONE)
		pr_info("nest MMU:   prtb0 %016llx prtb1 %016llx\n",
			be64_to_cpu(process_tb[v->hw_pid].prtb0),
			be64_to_cpu(process_tb[v->hw_pid].prtb1));

	for (i = 0; i < NMMU_STAB_SIZE / sizeof(*stab); i++) {
		unsigned long e0 = be64_to_cpu(stab[i].esid_data);
		unsigned long e1 = be64_to_cpu(stab[i].vsid_data);
		struct copro_slb want = {};
		bool ok;
		int ssize;

		if (!(e0 & SLB_ESID_V))
			continue;
		valid++;

		/*
		 * The whole second word is compared, not the VSID out of it:
		 * a page size selector that disagrees with the slice sends
		 * the nest MMU to the wrong hash group just as surely as a
		 * wrong VSID does, and reports the same NOPTE.
		 */
		ssize = (e1 & SLB_VSID_B) >> SLB_VSID_SSIZE_SHIFT;
		ok = !copro_calculate_slb(mm, e0 & slb_esid_mask(ssize), &want) &&
		     e0 == want.esid && e1 == want.vsid;
		if (!ok)
			bad++;
		pr_info("nest MMU:   [%3d] esid %016lx vsid %016lx want %016llx %016llx %s\n",
			i, e0, e1, want.esid, want.vsid,
			ok ? "ok" : "MISMATCH");
	}

	pr_info("nest MMU: %d valid entries, %d mismatched\n", valid, bad);
}

static int nmmu_segtab_dump(void *data, u64 val)
{
	struct task_struct *tsk;
	struct nmmu_view *v, *o;
	struct mm_struct *mm;
	struct nmmu_ste *stab;
	unsigned long flags;

	rcu_read_lock();
	tsk = find_task_by_vpid((pid_t)val);
	if (tsk)
		get_task_struct(tsk);
	rcu_read_unlock();
	if (!tsk)
		return -ESRCH;

	mm = get_task_mm(tsk);
	put_task_struct(tsk);
	if (!mm)
		return -ESRCH;

	v = smp_load_acquire(&mm->context.nmmu_view);
	if (!v) {
		pr_info("nest MMU: pid %llu hw_pid %d, no view\n", val,
			mm->context.hw_pid);
		mmput(mm);
		return 0;
	}

	stab = (struct nmmu_ste *)__get_free_page(GFP_KERNEL);
	if (!stab) {
		mmput(mm);
		return -ENOMEM;
	}

	nmmu_view_dump(v, val, stab);
	/* The lock keeps a window from taking its view away mid-dump. */
	spin_lock_irqsave(&v->others_lock, flags);
	list_for_each_entry(o, &v->others, node)
		nmmu_view_dump(o, val, stab);
	spin_unlock_irqrestore(&v->others_lock, flags);

	free_page((unsigned long)stab);
	mmput(mm);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(nmmu_segtab_fops, NULL, nmmu_segtab_dump, "%llu\n");

/*
 * The slice map of one mm, as page sizes per address range.
 *
 * A slice's page size is what a segment table entry for that range has to
 * carry, so this is the other half of the segment table dump: an entry that
 * looks wrong is usually a slice that changed. It is also the only view of
 * the slice map there is -- nothing in /proc reports it -- and the map
 * explains behaviour that is otherwise mystifying, such as a hugetlb mapping
 * converting nothing because the slice it landed in was converted already,
 * page sizes being sticky across unmap, and a slice above 1TB being 1TB wide.
 *
 *	echo <pid> > /sys/kernel/debug/powerpc/nmmu_slices
 */
static const char *nmmu_psize_name(unsigned int psize)
{
	switch (psize) {
	case MMU_PAGE_4K:	return "4K";
	case MMU_PAGE_64K:	return "64K";
	case MMU_PAGE_16M:	return "16M";
	case MMU_PAGE_16G:	return "16G";
	default:		return "?";
	}
}

static int nmmu_slices_dump(void *data, u64 val)
{
	unsigned long addr, limit, start = 0;
	unsigned int psize, prev;
	struct task_struct *tsk;
	struct mm_struct *mm;
	int runs = 0;

	rcu_read_lock();
	tsk = find_task_by_vpid((pid_t)val);
	if (tsk)
		get_task_struct(tsk);
	rcu_read_unlock();
	if (!tsk)
		return -ESRCH;
	mm = get_task_mm(tsk);
	put_task_struct(tsk);
	if (!mm)
		return -ESRCH;
	if (radix_enabled()) {
		mmput(mm);
		return -ENODEV;
	}

	limit = mm_ctx_slb_addr_limit(&mm->context);
	pr_info("slices: pid %llu, address limit 0x%lx, default %s\n", val,
		limit, nmmu_psize_name(mm_ctx_user_psize(&mm->context)));

	/*
	 * Printed as runs rather than one line per slice: there are 16 low
	 * slices of 256MB and up to 512 high slices of 1TB, nearly all of
	 * them the same, and the interesting thing is where that changes.
	 */
	prev = get_slice_psize(mm, 0);
	for (addr = 1UL << SLICE_LOW_SHIFT; addr < limit; ) {
		psize = get_slice_psize(mm, addr);
		if (psize != prev) {
			pr_info("slices:   0x%016lx-0x%016lx %s\n", start,
				addr - 1, nmmu_psize_name(prev));
			runs++;
			start = addr;
			prev = psize;
		}
		addr += addr < SLICE_LOW_TOP ? (1UL << SLICE_LOW_SHIFT)
					     : (1UL << SLICE_HIGH_SHIFT);
	}
	pr_info("slices:   0x%016lx-0x%016lx %s\n", start, limit - 1,
		nmmu_psize_name(prev));
	pr_info("slices: %d change(s) of page size\n", runs);

	mmput(mm);
	return 0;
}
DEFINE_DEBUGFS_ATTRIBUTE(nmmu_slices_fops, NULL, nmmu_slices_dump, "%llu\n");

static int nmmu_stats_show(struct seq_file *m, void *v)
{
	seq_printf(m, "ste_insert      %d\n", atomic_read(&nmmu_stat.insert));
	seq_printf(m, "ste_present     %d\n", atomic_read(&nmmu_stat.present));
	seq_printf(m, "ste_nospc       %d\n", atomic_read(&nmmu_stat.nospc));
	seq_printf(m, "ste_ovf_flush   %d\n",
		   atomic_read(&nmmu_stat.overflow_flush));
	seq_printf(m, "ste_nospc_fatal %d\n",
		   atomic_read(&nmmu_stat.nospc_fatal));
	seq_printf(m, "ste_efault      %d\n", atomic_read(&nmmu_stat.efault));
	seq_printf(m, "segtab_flush    %d\n", atomic_read(&nmmu_stat.flush));
	seq_printf(m, "hw_pid_alloc    %d\n", atomic_read(&nmmu_stat.pid_alloc));
	seq_printf(m, "hw_pid_free     %d\n", atomic_read(&nmmu_stat.pid_free));
	seq_printf(m, "view_new        %d\n", atomic_read(&nmmu_stat.view_new));
	seq_printf(m, "view_free       %d\n", atomic_read(&nmmu_stat.view_free));
	seq_printf(m, "denied          %d\n", atomic_read(&nmmu_stat.denied));
	seq_printf(m, "domain_add      %d\n", atomic_read(&nmmu_stat.domain_add));
	seq_printf(m, "domain_drop     %d\n", atomic_read(&nmmu_stat.domain_drop));
	seq_printf(m, "segtab_entries  %lu\n",
		   NMMU_STAB_SIZE / sizeof(struct nmmu_ste));
	seq_printf(m, "segtab_groups   %lu\n", (unsigned long)NMMU_STEGS);
	seq_printf(m, "process_table   %s\n", process_tb ? "present" : "absent");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(nmmu_stats);

static int __init nmmu_segtab_debugfs_init(void)
{
	debugfs_create_file_unsafe("nmmu_segtab", 0200, arch_debugfs_dir,
				   NULL, &nmmu_segtab_fops);
	debugfs_create_file_unsafe("nmmu_slices", 0200, arch_debugfs_dir,
				   NULL, &nmmu_slices_fops);
	debugfs_create_file("nmmu_stats", 0400, arch_debugfs_dir, NULL,
			    &nmmu_stats_fops);
	return 0;
}
device_initcall(nmmu_segtab_debugfs_init);
#endif /* CONFIG_DEBUG_FS */
