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
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/pgtable.h>
#include <linux/sched/mm.h>

#include <asm/firmware.h>
#include <asm/mmu.h>
#include <asm/mmu_context.h>
#include <asm/ppc-opcode.h>
#include <asm/cputable.h>
#include <asm/trace.h>

static DEFINE_MUTEX(nmmu_segtab_lock);

/*
 * Segment table geometry, Power ISA 3.0B section 5.7.8.3: the table is a set of
 * 128-byte segment table entry groups of eight 16-byte entries, and its size is
 * 2^q with q = STABSIZE + 12.
 *
 * STABSIZE 0 gives a 4KiB table of 32 STEGs, which is exactly the range of the
 * 1TB STEG selector EA(31-q:23) = EA(19:23) in section 5.7.8.3.2. A larger
 * table would add entries the selector cannot reach.
 */
#define NMMU_STABSIZE		0
#define NMMU_STAB_SHIFT		(NMMU_STABORG_SHIFT + NMMU_STABSIZE)	/* q */
#define NMMU_STAB_SIZE		(1UL << NMMU_STAB_SHIFT)
#define NMMU_STES_PER_STEG	8
#define NMMU_STEG_SHIFT		7			/* 128 bytes */
#define NMMU_STEGS		(NMMU_STAB_SIZE >> NMMU_STEG_SHIFT)

/*
 * The origin is always scaled by 2^12, whatever the table's size: the base is
 * STABORG(0:77-q) || q zeros with the low q-12 bits of STABORG required to be
 * 0, which leaves STABORG itself a virtual address shifted by 12 either way.
 */
#define NMMU_STABORG_SHIFT	12

/*
 * A segment table entry is the SLB entry of Figure 29 without the slot index.
 * The valid bit, segment size selector, VSID and page size selectors sit at the
 * bit positions the SLB_VSID_* constants already name, so the entries are built
 * with the SLB's own helpers rather than a restatement of the same layout.
 */
struct nmmu_ste {
	__be64 esid_data;
	__be64 vsid_data;
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

	return stab + ((sel % NMMU_STEGS) * NMMU_STES_PER_STEG);
}

/*
 * Install a segment table entry for the segment containing ea. The STEG is
 * selected by the low-order ESID bits and holds eight entries; an entry already
 * describing this segment is left alone.
 */
static int nmmu_ste_insert(struct nmmu_ste *stab, struct mm_struct *mm,
			   unsigned long ea)
{
	unsigned long vsid, esid_data, vsid_data;
	struct nmmu_ste *steg;
	int ssize, psize, i, half;

	ssize = user_segment_size(ea);
	vsid = get_user_vsid(&mm->context, ea, ssize);
	if (!vsid)
		return -EFAULT;

	psize = get_slice_psize(mm, ea);

	/*
	 * mk_esid_data() is not used because a segment table entry has no slot
	 * to name; Figure 29 leaves the bits the SLB spends on its index
	 * reserved. The rest is the SLB entry, flags included, so an entry here
	 * describes a segment exactly as the core's own would.
	 */
	esid_data = (ea & slb_esid_mask(ssize)) | SLB_ESID_V;
	vsid_data = __mk_vsid_data(vsid, ssize,
				   SLB_VSID_USER | mmu_psize_defs[psize].sllp);

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
			if (be64_to_cpu(steg[i].esid_data) == esid_data)
				return 0;
	}

	for (half = 0; half < 2; half++) {
		steg = nmmu_steg(stab, ea, ssize, half);

		for (i = 0; i < NMMU_STES_PER_STEG; i++) {
			if (be64_to_cpu(steg[i].esid_data) & SLB_ESID_V)
				continue;

			steg[i].vsid_data = cpu_to_be64(vsid_data);
			asm volatile("ptesync" : : : "memory");
			steg[i].esid_data = cpu_to_be64(esid_data);
			return 0;
		}
	}

	return -ENOSPC;
}

/*
 * Give every segment the mm has mapped an entry. Nothing refills this table on
 * demand yet, so an address mapped after the window is opened will not be
 * translatable until the nest MMU fault path lands.
 *
 * Best effort by design. A group holds eight entries, so an mm whose segments
 * collide in one of them has a segment the accelerator cannot reach; that is a
 * segment to report, not a reason to refuse the window, and it is the same
 * position every address is in before this table exists at all.
 */
static void nmmu_prefault(struct mm_struct *mm, struct nmmu_ste *stab)
{
	VMA_ITERATOR(vmi, mm, 0);
	struct vm_area_struct *vma;
	unsigned long ea;
	int mapped = 0, failed = 0;

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
			if (nmmu_ste_insert(stab, mm, ea))
				failed++;
			else
				mapped++;

			seg = 1UL << nmmu_sid_shift(user_segment_size(ea));
			ea = ALIGN_DOWN(ea, seg) + seg;
		}
	}
	mmap_read_unlock(mm);

	if (failed)
		pr_warn("nest MMU: pid %d, %d of %d segments have no entry\n",
			mm->context.hw_pid, failed, mapped + failed);
}

/*
 * Drop the nest MMU's cached copy of one segment table entry.
 *
 * slbieg names the entry by hardware PID and segment rather than by table, so
 * this reaches what the nest MMU has cached without touching the table itself.
 * Power ISA 3.0B gives RS as PID(0:31) || LPID(32:63) and RB as the ESID with
 * the segment size in bits 37:38, which is where SLB_VSID_SSIZE_SHIFT already
 * puts it for slbie; bit 36 must be zero, so the entry's valid bit is masked
 * off rather than carried across.
 *
 * LPID is zero. This runs on the host partition, and in hypervisor state the
 * instruction takes the LPID from RS rather than from LPIDR.
 */
static void nmmu_slbieg(int hw_pid, unsigned long esid_data, int ssize)
{
	unsigned long rs = (unsigned long)hw_pid << 32;
	unsigned long rb = (esid_data & slb_esid_mask(ssize)) |
			   ((unsigned long)ssize << SLBIE_SSIZE_SHIFT);

	asm volatile(PPC_SLBIEG(%0, %1) : : "r" (rs), "r" (rb) : "memory");
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

	asm volatile("ptesync" : : : "memory");
	nmmu_tlbie_prte(rs, rb);
	nmmu_tlbie_fixup(rs, rb);
	asm volatile("eieio; tlbsync; ptesync" : : : "memory");

	trace_tlbie(0, 0, rb, rs, TLBIE_RIC_TABLES, TLBIE_PRS_PROCESS,
		    TLBIE_R_HPT);
}

/*
 * Invalidate every segment this table described.
 *
 * Required before the hardware PID goes back to the allocator. The nest MMU
 * caches segment table entries and nothing else evicts them, so without this
 * the next mm to be given the PID is translated with the previous mm's VSIDs.
 * POWER9 User's Manual section 4.10.11 is explicit that this is what the
 * instruction is for here: slbieg "does not invalidate SLB entries in the
 * processor core when UPRT = 0" but "is used to manage STEs cached by the
 * NMMU".
 *
 * The ordering is the one section 5.9.3 states for slbsync: eieio separates
 * the invalidations from it, and the ptesync after it is the barrier that
 * makes them complete.
 */
static void nmmu_segtab_invalidate(struct nmmu_ste *stab, int hw_pid)
{
	int i, invalidated = 0;

	asm volatile("ptesync" : : : "memory");

	for (i = 0; i < NMMU_STAB_SIZE / sizeof(struct nmmu_ste); i++) {
		unsigned long e0 = be64_to_cpu(stab[i].esid_data);
		unsigned long e1 = be64_to_cpu(stab[i].vsid_data);

		if (!(e0 & SLB_ESID_V))
			continue;

		nmmu_slbieg(hw_pid, e0,
			    (e1 >> SLB_VSID_SSIZE_SHIFT) & 0x3);
		invalidated++;
	}

	if (invalidated)
		asm volatile("eieio" : : : "memory");
	asm volatile(PPC_SLBSYNC : : : "memory");
	asm volatile("ptesync" : : : "memory");
}

/*
 * Build the segment table for an mm that is about to drive an accelerator.
 * Called once, from the same place its hardware PID is allocated.
 */
int hash__nmmu_segtab_alloc(struct mm_struct *mm, int hw_pid)
{
	struct nmmu_ste *stab;
	int rc;

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
	 * wait for the winner's table rather than return before it exists.
	 */
	mutex_lock(&nmmu_segtab_lock);
	if (mm->context.nmmu_segtab) {
		mutex_unlock(&nmmu_segtab_lock);
		return 0;
	}

	stab = (struct nmmu_ste *)get_zeroed_page(GFP_KERNEL);
	if (!stab) {
		mutex_unlock(&nmmu_segtab_lock);
		return -ENOMEM;
	}

	/*
	 * Fill the table before the process table entry points at it. Section
	 * 5.7.6.2 gives the entry a valid bit precisely so it can be off "while
	 * changes are made to the entry and Segment Table", and building in
	 * this order means the hardware never sees a half-built one.
	 */
	nmmu_prefault(mm, stab);

	rc = nmmu_prte_set(hw_pid, stab);
	if (rc) {
		free_page((unsigned long)stab);
		mutex_unlock(&nmmu_segtab_lock);
		return rc;
	}

	mm->context.nmmu_segtab = stab;
	mutex_unlock(&nmmu_segtab_lock);

	return 0;
}

void hash__nmmu_segtab_free(struct mm_struct *mm)
{
	void *stab = mm->context.nmmu_segtab;
	int hw_pid = mm->context.hw_pid;

	if (!stab)
		return;

	/*
	 * Stop the walks first, then drop what has already been cached from
	 * them. Clearing the entry on its own leaves the nest MMU holding
	 * segments it can still translate with.
	 */
	if (process_tb && hw_pid != MMU_HW_PID_NONE) {
		process_tb[hw_pid].prtb1 = 0;
		asm volatile("ptesync" : : : "memory");
		process_tb[hw_pid].prtb0 = 0;

		/*
		 * The entry first, then the segments it reached. After both,
		 * nothing the hardware holds can name this table, which is
		 * what makes the page safe to give back.
		 */
		nmmu_prte_invalidate(hw_pid);
		nmmu_segtab_invalidate(stab, hw_pid);
	}

	mm->context.nmmu_segtab = NULL;
	free_page((unsigned long)stab);
}
