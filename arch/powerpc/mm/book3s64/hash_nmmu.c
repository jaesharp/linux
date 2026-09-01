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
 * Install a segment table entry for the segment containing ea. The STEG is
 * selected by the low-order ESID bits and holds eight entries; an entry already
 * describing this segment is left alone.
 */
static int nmmu_ste_insert(struct nmmu_ste *stab, struct mm_struct *mm,
			   unsigned long ea)
{
	unsigned long vsid, esid_data, vsid_data;
	struct nmmu_ste *steg;
	int ssize, psize, i;

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

	steg = stab + (((ea >> nmmu_sid_shift(ssize)) % NMMU_STEGS) *
		       NMMU_STES_PER_STEG);

	for (i = 0; i < NMMU_STES_PER_STEG; i++) {
		unsigned long cur = be64_to_cpu(steg[i].esid_data);

		if (cur == esid_data)
			return 0;
		if (cur & SLB_ESID_V)
			continue;

		steg[i].vsid_data = cpu_to_be64(vsid_data);
		asm volatile("ptesync" : : : "memory");
		steg[i].esid_data = cpu_to_be64(esid_data);
		return 0;
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
		for (ea = vma->vm_start; ea < vma->vm_end;
		     ea += 1UL << nmmu_sid_shift(user_segment_size(ea))) {
			if (nmmu_ste_insert(stab, mm, ea))
				failed++;
			else
				mapped++;
		}
	}
	mmap_read_unlock(mm);

	if (failed)
		pr_warn("nest MMU: pid %d, %d of %d segments have no entry\n",
			mm->context.hw_pid, failed, mapped + failed);
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
	 * Clearing the entry does not evict a copy the nest MMU may already
	 * hold; invalidating that is the segment-invalidation work, not this.
	 * Until then the PID is still not reused while an entry is live,
	 * because the caller frees it only after this returns.
	 */
	if (process_tb && hw_pid != MMU_HW_PID_NONE) {
		process_tb[hw_pid].prtb1 = 0;
		asm volatile("ptesync" : : : "memory");
		process_tb[hw_pid].prtb0 = 0;
	}

	mm->context.nmmu_segtab = NULL;
	free_page((unsigned long)stab);
}
