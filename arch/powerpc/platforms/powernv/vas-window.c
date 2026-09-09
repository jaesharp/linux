// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright 2016-17 IBM Corp.
 */

#define pr_fmt(fmt) "vas: " fmt

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/rcupdate.h>
#include <linux/cred.h>
#include <linux/sched/mm.h>
#include <linux/mmu_context.h>
#include <asm/switch_to.h>
#include <asm/kup.h>
#include <asm/ppc-opcode.h>
#include <asm/vas.h>
#include "vas.h"
#include "copy-paste.h"

#define CREATE_TRACE_POINTS
#include "vas-trace.h"

/*
 * Compute the paste address region for the window @window using the
 * ->paste_base_addr and ->paste_win_id_shift we got from device tree.
 */
void vas_win_paste_addr(struct pnv_vas_window *window, u64 *addr, int *len)
{
	int winid;
	u64 base, shift;

	base = window->vinst->paste_base_addr;
	shift = window->vinst->paste_win_id_shift;
	winid = window->vas_win.winid;

	*addr  = base + (winid << shift);
	if (len)
		*len = PAGE_SIZE;

	pr_debug("Txwin #%d: Paste addr 0x%llx\n", winid, *addr);
}

static inline void get_hvwc_mmio_bar(struct pnv_vas_window *window,
			u64 *start, int *len)
{
	u64 pbaddr;

	pbaddr = window->vinst->hvwc_bar_start;
	*start = pbaddr + window->vas_win.winid * VAS_HVWC_SIZE;
	*len = VAS_HVWC_SIZE;
}

static inline void get_uwc_mmio_bar(struct pnv_vas_window *window,
			u64 *start, int *len)
{
	u64 pbaddr;

	pbaddr = window->vinst->uwc_bar_start;
	*start = pbaddr + window->vas_win.winid * window->vinst->uwc_win_size;
	*len = window->vinst->uwc_win_size;
}

/*
 * Map the paste bus address of the given send window into kernel address
 * space. Unlike MMIO regions (map_mmio_region() below), paste region must
 * be mapped cache-able and is only applicable to send windows.
 */
static void *map_paste_region(struct pnv_vas_window *txwin)
{
	int len;
	void *map;
	char *name;
	u64 start;

	name = kasprintf(GFP_KERNEL, "window-v%d-w%d", txwin->vinst->vas_id,
				txwin->vas_win.winid);
	if (!name)
		return ERR_PTR(-ENOMEM);

	vas_win_paste_addr(txwin, &start, &len);

	if (!request_mem_region(start, len, name)) {
		pr_devel("%s(): request_mem_region(0x%llx, %d) failed\n",
				__func__, start, len);
		goto free_name;
	}

	map = ioremap_cache(start, len);
	if (!map) {
		pr_devel("%s(): ioremap_cache(0x%llx, %d) failed\n", __func__,
				start, len);
		goto free_region;
	}

	/*
	 * Recorded only once the mapping exists: the name doubles as the
	 * marker unmap_paste_region() goes by, and a failure path that
	 * leaves it set hands the unmapper a region that was never mapped
	 * and a name that was already freed.
	 */
	txwin->paste_addr_name = name;
	pr_devel("Mapped paste addr 0x%llx to kaddr 0x%p\n", start, map);
	return map;

free_region:
	release_mem_region((phys_addr_t)start, len);
free_name:
	kfree(name);
	return ERR_PTR(-ENOMEM);
}

static void *map_mmio_region(char *name, u64 start, int len)
{
	void *map;

	if (!request_mem_region(start, len, name)) {
		pr_devel("%s(): request_mem_region(0x%llx, %d) failed\n",
				__func__, start, len);
		return NULL;
	}

	map = ioremap(start, len);
	if (!map) {
		pr_devel("%s(): ioremap(0x%llx, %d) failed\n", __func__, start,
				len);
		release_mem_region((phys_addr_t)start, len);
		return NULL;
	}

	return map;
}

static void unmap_region(void *addr, u64 start, int len)
{
	iounmap(addr);
	release_mem_region((phys_addr_t)start, len);
}

/*
 * Unmap the paste address region for a window.
 */
static void unmap_paste_region(struct pnv_vas_window *window)
{
	int len;
	u64 busaddr_start;

	if (window->paste_kaddr) {
		vas_win_paste_addr(window, &busaddr_start, &len);
		unmap_region(window->paste_kaddr, busaddr_start, len);
		window->paste_kaddr = NULL;
		kfree(window->paste_addr_name);
		window->paste_addr_name = NULL;
	}
}

/*
 * Unmap the MMIO regions for a window. Hold the vas_mutex so we don't
 * unmap when the window's debugfs dir is in use. This serializes close
 * of a window even on another VAS instance but since its not a critical
 * path, just minimize the time we hold the mutex for now. We can add
 * a per-instance mutex later if necessary.
 */
static void unmap_winctx_mmio_bars(struct pnv_vas_window *window)
{
	int len;
	void *uwc_map;
	void *hvwc_map;
	u64 busaddr_start;

	mutex_lock(&vas_mutex);

	hvwc_map = window->hvwc_map;
	window->hvwc_map = NULL;

	uwc_map = window->uwc_map;
	window->uwc_map = NULL;

	mutex_unlock(&vas_mutex);

	if (hvwc_map) {
		get_hvwc_mmio_bar(window, &busaddr_start, &len);
		unmap_region(hvwc_map, busaddr_start, len);
	}

	if (uwc_map) {
		get_uwc_mmio_bar(window, &busaddr_start, &len);
		unmap_region(uwc_map, busaddr_start, len);
	}
}

/*
 * Find the Hypervisor Window Context (HVWC) MMIO Base Address Region and the
 * OS/User Window Context (UWC) MMIO Base Address Region for the given window.
 * Map these bus addresses and save the mapped kernel addresses in @window.
 */
static int map_winctx_mmio_bars(struct pnv_vas_window *window)
{
	int len;
	u64 start;

	get_hvwc_mmio_bar(window, &start, &len);
	window->hvwc_map = map_mmio_region("HVWCM_Window", start, len);

	get_uwc_mmio_bar(window, &start, &len);
	window->uwc_map = map_mmio_region("UWCM_Window", start, len);

	if (!window->hvwc_map || !window->uwc_map) {
		unmap_winctx_mmio_bars(window);
		return -1;
	}

	return 0;
}

/*
 * Reset all valid registers in the HV and OS/User Window Contexts for
 * the window identified by @window.
 *
 * NOTE: We cannot really use a for loop to reset window context. Not all
 *	 offsets in a window context are valid registers and the valid
 *	 registers are not sequential. And, we can only write to offsets
 *	 with valid registers.
 */
static void reset_window_regs(struct pnv_vas_window *window)
{
	write_hvwc_reg(window, VREG(LPID), 0ULL);
	write_hvwc_reg(window, VREG(PID), 0ULL);
	write_hvwc_reg(window, VREG(XLATE_MSR), 0ULL);
	write_hvwc_reg(window, VREG(XLATE_LPCR), 0ULL);
	write_hvwc_reg(window, VREG(XLATE_CTL), 0ULL);
	write_hvwc_reg(window, VREG(AMR), 0ULL);
	write_hvwc_reg(window, VREG(SEIDR), 0ULL);
	write_hvwc_reg(window, VREG(FAULT_TX_WIN), 0ULL);
	write_hvwc_reg(window, VREG(OSU_INTR_SRC_RA), 0ULL);
	write_hvwc_reg(window, VREG(HV_INTR_SRC_RA), 0ULL);
	write_hvwc_reg(window, VREG(PSWID), 0ULL);
	write_hvwc_reg(window, VREG(LFIFO_BAR), 0ULL);
	write_hvwc_reg(window, VREG(LDATA_STAMP_CTL), 0ULL);
	write_hvwc_reg(window, VREG(LDMA_CACHE_CTL), 0ULL);
	write_hvwc_reg(window, VREG(LRFIFO_PUSH), 0ULL);
	write_hvwc_reg(window, VREG(CURR_MSG_COUNT), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_AFTER_COUNT), 0ULL);
	write_hvwc_reg(window, VREG(LRX_WCRED), 0ULL);
	write_hvwc_reg(window, VREG(LRX_WCRED_ADDER), 0ULL);
	write_hvwc_reg(window, VREG(TX_WCRED), 0ULL);
	write_hvwc_reg(window, VREG(TX_WCRED_ADDER), 0ULL);
	write_hvwc_reg(window, VREG(LFIFO_SIZE), 0ULL);
	write_hvwc_reg(window, VREG(WINCTL), 0ULL);
	write_hvwc_reg(window, VREG(WIN_STATUS), 0ULL);
	write_hvwc_reg(window, VREG(WIN_CTX_CACHING_CTL), 0ULL);
	write_hvwc_reg(window, VREG(TX_RSVD_BUF_COUNT), 0ULL);
	write_hvwc_reg(window, VREG(LRFIFO_WIN_PTR), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_CTL), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_PID), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_LPID), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_TID), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_SCOPE), 0ULL);
	write_hvwc_reg(window, VREG(NX_UTIL_ADDER), 0ULL);

	/* Skip read-only registers: NX_UTIL and NX_UTIL_SE */

	/*
	 * The send and receive window credit adder registers are also
	 * accessible from HVWC and have been initialized above. We don't
	 * need to initialize from the OS/User Window Context, so skip
	 * following calls:
	 *
	 *	write_uwc_reg(window, VREG(TX_WCRED_ADDER), 0ULL);
	 *	write_uwc_reg(window, VREG(LRX_WCRED_ADDER), 0ULL);
	 */
}

/*
 * Initialize window context registers related to Address Translation.
 * These registers are common to send/receive windows although they
 * differ for user/kernel windows. As we resolve the TODOs we may
 * want to add fields to vas_winctx and move the initialization to
 * init_vas_winctx_regs().
 */
/*
 * The AMR a window's requests are translated under. A user window gets the
 * mask the user window driver settled on. Reading the register here would
 * hand it the kernel's mask, which under KUAP denies key 0, the key every
 * ordinary user page carries. A kernel window keeps the register: its
 * requests name kernel addresses and run under the protection the kernel
 * is running under.
 */
static u64 xlate_amr(const struct vas_winctx *winctx)
{
	if (winctx->user_win)
		return winctx->amr;
	return mfspr(SPRN_AMR);
}

static void init_xlate_regs(struct pnv_vas_window *window,
			    const struct vas_winctx *winctx)
{
	u64 lpcr, val;

	/*
	 * MSR_TA, MSR_US are false for both kernel and user.
	 * MSR_DR and MSR_PR are false for kernel.
	 */
	val = 0ULL;
	val = SET_FIELD(VAS_XLATE_MSR_HV, val, 1);
	val = SET_FIELD(VAS_XLATE_MSR_SF, val, 1);
	if (winctx->user_win) {
		val = SET_FIELD(VAS_XLATE_MSR_DR, val, 1);
		val = SET_FIELD(VAS_XLATE_MSR_PR, val, 1);
	}
	write_hvwc_reg(window, VREG(XLATE_MSR), val);

	lpcr = mfspr(SPRN_LPCR);
	val = 0ULL;
	/*
	 * NOTE: From Section 5.7.8.1 Segment Lookaside Buffer of the
	 *	 Power ISA, v3.0B, Page size encoding is 0 = 4KB, 5 = 64KB.
	 *
	 * NOTE: From Section 1.3.1, Address Translation Context of the
	 *	 Nest MMU Workbook, LPCR_SC should be 0 for Power9.
	 */
	BUILD_BUG_ON(PAGE_SHIFT != 12 && PAGE_SHIFT != 16);
	val = SET_FIELD(VAS_XLATE_LPCR_PAGE_SIZE, val,
			(PAGE_SHIFT == 16) ? 5 : 0);
	val = SET_FIELD(VAS_XLATE_LPCR_ISL, val, lpcr & LPCR_ISL);
	val = SET_FIELD(VAS_XLATE_LPCR_TC, val, lpcr & LPCR_TC);
	val = SET_FIELD(VAS_XLATE_LPCR_SC, val, 0);
	write_hvwc_reg(window, VREG(XLATE_LPCR), val);

	/*
	 * Section 1.3.1 (Address translation Context) of NMMU workbook.
	 *	0b00	Hashed Page Table mode
	 *	0b01	Reserved
	 *	0b10	Radix on HPT
	 *	0b11	Radix on Radix
	 */
	val = 0ULL;
	val = SET_FIELD(VAS_XLATE_MODE, val,
			radix_enabled() ? VAS_XLATE_MODE_RADIX_ON_RADIX
					: VAS_XLATE_MODE_HPT);
	write_hvwc_reg(window, VREG(XLATE_CTL), val);

	val = 0ULL;
	val = SET_FIELD(VAS_AMR, val, xlate_amr(winctx));
	write_hvwc_reg(window, VREG(AMR), val);

	val = 0ULL;
	val = SET_FIELD(VAS_SEIDR, val, 0);
	write_hvwc_reg(window, VREG(SEIDR), val);
}

/*
 * Initialize Reserved Send Buffer Count for the send window. It involves
 * writing to the register, reading it back to confirm that the hardware
 * has enough buffers to reserve. See section 1.3.1.2.1 of VAS workbook.
 *
 * Since we can only make a best-effort attempt to fulfill the request,
 * we don't return any errors if we cannot.
 *
 * TODO: Reserved (aka dedicated) send buffers are not supported yet.
 */
static void init_rsvd_tx_buf_count(struct pnv_vas_window *txwin,
				struct vas_winctx *winctx)
{
	write_hvwc_reg(txwin, VREG(TX_RSVD_BUF_COUNT), 0ULL);
}

/*
 * init_winctx_regs()
 *	Initialize window context registers for a receive window.
 *	Except for caching control and marking window open, the registers
 *	are initialized in the order listed in Section 3.1.4 (Window Context
 *	Cache Register Details) of the VAS workbook although they don't need
 *	to be.
 *
 * Design note: For NX receive windows, NX allocates the FIFO buffer in OPAL
 *	(so that it can get a large contiguous area) and passes that buffer
 *	to kernel via device tree. We now write that buffer address to the
 *	FIFO BAR. Would it make sense to do this all in OPAL? i.e have OPAL
 *	write the per-chip RX FIFO addresses to the windows during boot-up
 *	as a one-time task? That could work for NX but what about other
 *	receivers?  Let the receivers tell us the rx-fifo buffers for now.
 */
static void init_winctx_regs(struct pnv_vas_window *window,
			     struct vas_winctx *winctx)
{
	u64 val;
	int fifo_size;

	reset_window_regs(window);

	val = 0ULL;
	val = SET_FIELD(VAS_LPID, val, winctx->lpid);
	write_hvwc_reg(window, VREG(LPID), val);

	val = 0ULL;
	val = SET_FIELD(VAS_PID_ID, val, winctx->pidr);
	write_hvwc_reg(window, VREG(PID), val);

	init_xlate_regs(window, winctx);

	val = 0ULL;
	val = SET_FIELD(VAS_FAULT_TX_WIN, val, winctx->fault_win_id);
	write_hvwc_reg(window, VREG(FAULT_TX_WIN), val);

	/* In PowerNV, interrupts go to HV. */
	write_hvwc_reg(window, VREG(OSU_INTR_SRC_RA), 0ULL);

	val = 0ULL;
	val = SET_FIELD(VAS_HV_INTR_SRC_RA, val, winctx->irq_port);
	write_hvwc_reg(window, VREG(HV_INTR_SRC_RA), val);

	val = 0ULL;
	val = SET_FIELD(VAS_PSWID_EA_HANDLE, val, winctx->pswid);
	write_hvwc_reg(window, VREG(PSWID), val);

	write_hvwc_reg(window, VREG(SPARE1), 0ULL);
	write_hvwc_reg(window, VREG(SPARE2), 0ULL);
	write_hvwc_reg(window, VREG(SPARE3), 0ULL);

	/*
	 * NOTE: VAS expects the FIFO address to be copied into the LFIFO_BAR
	 *	 register as is - do NOT shift the address into VAS_LFIFO_BAR
	 *	 bit fields! Ok to set the page migration select fields -
	 *	 VAS ignores the lower 10+ bits in the address anyway, because
	 *	 the minimum FIFO size is 1K?
	 *
	 * See also: Design note in function header.
	 */
	val = winctx->rx_fifo;
	val = SET_FIELD(VAS_PAGE_MIGRATION_SELECT, val, 0);
	write_hvwc_reg(window, VREG(LFIFO_BAR), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LDATA_STAMP, val, winctx->data_stamp);
	write_hvwc_reg(window, VREG(LDATA_STAMP_CTL), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LDMA_TYPE, val, winctx->dma_type);
	val = SET_FIELD(VAS_LDMA_FIFO_DISABLE, val, winctx->fifo_disable);
	write_hvwc_reg(window, VREG(LDMA_CACHE_CTL), val);

	write_hvwc_reg(window, VREG(LRFIFO_PUSH), 0ULL);
	write_hvwc_reg(window, VREG(CURR_MSG_COUNT), 0ULL);
	write_hvwc_reg(window, VREG(LNOTIFY_AFTER_COUNT), 0ULL);

	val = 0ULL;
	val = SET_FIELD(VAS_LRX_WCRED, val, winctx->wcreds_max);
	write_hvwc_reg(window, VREG(LRX_WCRED), val);

	val = 0ULL;
	val = SET_FIELD(VAS_TX_WCRED, val, winctx->wcreds_max);
	write_hvwc_reg(window, VREG(TX_WCRED), val);

	write_hvwc_reg(window, VREG(LRX_WCRED_ADDER), 0ULL);
	write_hvwc_reg(window, VREG(TX_WCRED_ADDER), 0ULL);

	fifo_size = winctx->rx_fifo_size / 1024;

	val = 0ULL;
	val = SET_FIELD(VAS_LFIFO_SIZE, val, ilog2(fifo_size));
	write_hvwc_reg(window, VREG(LFIFO_SIZE), val);

	/* Update window control and caching control registers last so
	 * we mark the window open only after fully initializing it and
	 * pushing context to cache.
	 */

	write_hvwc_reg(window, VREG(WIN_STATUS), 0ULL);

	init_rsvd_tx_buf_count(window, winctx);

	/* for a send window, point to the matching receive window */
	val = 0ULL;
	val = SET_FIELD(VAS_LRX_WIN_ID, val, winctx->rx_win_id);
	write_hvwc_reg(window, VREG(LRFIFO_WIN_PTR), val);

	write_hvwc_reg(window, VREG(SPARE4), 0ULL);

	val = 0ULL;
	val = SET_FIELD(VAS_NOTIFY_DISABLE, val, winctx->notify_disable);
	val = SET_FIELD(VAS_INTR_DISABLE, val, winctx->intr_disable);
	val = SET_FIELD(VAS_NOTIFY_EARLY, val, winctx->notify_early);
	val = SET_FIELD(VAS_NOTIFY_OSU_INTR, val, winctx->notify_os_intr_reg);
	write_hvwc_reg(window, VREG(LNOTIFY_CTL), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LNOTIFY_PID, val, winctx->lnotify_pid);
	write_hvwc_reg(window, VREG(LNOTIFY_PID), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LNOTIFY_LPID, val, winctx->lnotify_lpid);
	write_hvwc_reg(window, VREG(LNOTIFY_LPID), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LNOTIFY_TID, val, winctx->lnotify_tid);
	write_hvwc_reg(window, VREG(LNOTIFY_TID), val);

	val = 0ULL;
	val = SET_FIELD(VAS_LNOTIFY_MIN_SCOPE, val, winctx->min_scope);
	val = SET_FIELD(VAS_LNOTIFY_MAX_SCOPE, val, winctx->max_scope);
	write_hvwc_reg(window, VREG(LNOTIFY_SCOPE), val);

	/* Skip read-only registers NX_UTIL and NX_UTIL_SE */

	write_hvwc_reg(window, VREG(SPARE5), 0ULL);
	write_hvwc_reg(window, VREG(NX_UTIL_ADDER), 0ULL);
	write_hvwc_reg(window, VREG(SPARE6), 0ULL);

	/* Finally, push window context to memory and... */
	val = 0ULL;
	val = SET_FIELD(VAS_PUSH_TO_MEM, val, 1);
	write_hvwc_reg(window, VREG(WIN_CTX_CACHING_CTL), val);

	/* ... mark the window open for business */
	val = 0ULL;
	val = SET_FIELD(VAS_WINCTL_REJ_NO_CREDIT, val, winctx->rej_no_credit);
	val = SET_FIELD(VAS_WINCTL_PIN, val, winctx->pin_win);
	val = SET_FIELD(VAS_WINCTL_TX_WCRED_MODE, val, winctx->tx_wcred_mode);
	val = SET_FIELD(VAS_WINCTL_RX_WCRED_MODE, val, winctx->rx_wcred_mode);
	val = SET_FIELD(VAS_WINCTL_TX_WORD_MODE, val, winctx->tx_word_mode);
	val = SET_FIELD(VAS_WINCTL_RX_WORD_MODE, val, winctx->rx_word_mode);
	val = SET_FIELD(VAS_WINCTL_FAULT_WIN, val, winctx->fault_win);
	val = SET_FIELD(VAS_WINCTL_NX_WIN, val, winctx->nx_win);
	val = SET_FIELD(VAS_WINCTL_OPEN, val, 1);
	write_hvwc_reg(window, VREG(WINCTL), val);
}

static void vas_release_window_id(struct ida *ida, int winid)
{
	ida_free(ida, winid);
}

static int vas_assign_window_id(struct ida *ida)
{
	int winid = ida_alloc_max(ida, VAS_WINDOWS_PER_CHIP - 1, GFP_KERNEL);

	if (winid == -ENOSPC) {
		/*
		 * Every window id on this chip is in use. Ratelimited and at
		 * warning level: it is reachable by any user opening windows
		 * in a loop, so it must not be a way to fill the log, but an
		 * operator seeing accelerator requests fail needs to know the
		 * chip ran out rather than something being misconfigured.
		 */
		pr_warn_ratelimited("%s[%d]: all %d window ids on this chip are in use\n",
				    current->comm, current->pid,
				    VAS_WINDOWS_PER_CHIP);
		return -EAGAIN;
	}

	return winid;
}

static void vas_window_free(struct pnv_vas_window *window)
{
	struct vas_instance *vinst = window->vinst;
	int winid = window->vas_win.winid;

	unmap_winctx_mmio_bars(window);

	vas_window_free_dbgdir(window);

	vas_fault_ring_free(window);

	if (window->rx_fifo_buf)
		free_pages_exact(window->rx_fifo_buf, window->rx_fifo_len);

	kfree(window);

	vas_release_window_id(&vinst->ida, winid);
}

/* Completes a close the caller could not wait out; defined below. */
static void vas_close_work_fn(struct work_struct *work);

static struct pnv_vas_window *vas_window_alloc(struct vas_instance *vinst)
{
	int winid;
	struct pnv_vas_window *window;

	winid = vas_assign_window_id(&vinst->ida);
	if (winid < 0)
		return ERR_PTR(winid);

	window = kzalloc_obj(*window);
	if (!window)
		goto out_free;

	window->vinst = vinst;
	window->vas_win.winid = winid;
	INIT_DELAYED_WORK(&window->close_work, vas_close_work_fn);

	if (map_winctx_mmio_bars(window))
		goto out_free;

	vas_window_init_dbgdir(window);

	return window;

out_free:
	kfree(window);
	vas_release_window_id(&vinst->ida, winid);
	return ERR_PTR(-ENOMEM);
}

static void put_rx_win(struct pnv_vas_window *rxwin)
{
	/* Better not be a send window! */
	WARN_ON_ONCE(rxwin->tx_win);

	atomic_dec(&rxwin->num_txwins);
}

/*
 * Check that @target is a window a send window may be pointed at:
 *      - it must be an OPEN, FTW, RECEIVE window;
 *      - it must belong to this instance, because the switchboard names the
 *        destination of a send window by an id that means nothing on another.
 *
 * The caller has already established that it is entitled to send here, by
 * presenting the descriptor the window was opened on. This is only the check
 * that what it presented can be what it asked for.
 *
 * NOTE: We access ->windows[] table and assume that vinst->mutex is held.
 */
static struct pnv_vas_window *get_user_rxwin(struct vas_instance *vinst,
					     struct vas_window *target)
{
	struct pnv_vas_window *rxwin;

	rxwin = container_of(target, struct pnv_vas_window, vas_win);

	if (rxwin->vinst != vinst)
		return ERR_PTR(-EINVAL);

	if (rxwin->tx_win || rxwin->vas_win.cop != VAS_COP_TYPE_FTW)
		return ERR_PTR(-EINVAL);

	return rxwin;
}

/*
 * Get the VAS receive window associated with NX engine identified
 * by @cop, or the window @target names for a wake.
 *
 * See also function header of set_vinst_win().
 */
static struct pnv_vas_window *get_vinst_rxwin(struct vas_instance *vinst,
			enum vas_cop_type cop, struct vas_window *target)
{
	struct pnv_vas_window *rxwin;

	mutex_lock(&vinst->mutex);

	if (cop == VAS_COP_TYPE_FTW)
		rxwin = target ? get_user_rxwin(vinst, target) :
				 ERR_PTR(-EINVAL);
	else
		rxwin = vinst->rxwin[cop] ?: ERR_PTR(-EINVAL);

	if (!IS_ERR(rxwin))
		atomic_inc(&rxwin->num_txwins);

	mutex_unlock(&vinst->mutex);

	return rxwin;
}

/*
 * We have two tables of windows in a VAS instance. The first one,
 * ->windows[], contains all the windows in the instance and allows
 * looking up a window by its id. It is used to look up send windows
 * during fault handling and receive windows when pairing user space
 * send/receive windows.
 *
 * The second table, ->rxwin[], contains receive windows that are
 * associated with NX engines. This table has VAS_COP_TYPE_MAX
 * entries and is used to look up a receive window by its
 * coprocessor type.
 *
 * Here, we save @window in the ->windows[] table. If it is a receive
 * window, we also save the window in the ->rxwin[] table.
 */
static void set_vinst_win(struct vas_instance *vinst,
			struct pnv_vas_window *window)
{
	int id = window->vas_win.winid;

	mutex_lock(&vinst->mutex);

	/*
	 * There should only be one receive window for a coprocessor type
	 * unless its a user (FTW) window.
	 */
	if (!window->user_win && !window->tx_win) {
		/*
		 * A leftover pointer is a bug unless it is a window kept
		 * after a failed close; replacing that one is the driver
		 * reloading around a wedge, with a fresh window id.
		 */
		if (vinst->rxwin[window->vas_win.cop] &&
		    !vinst->rxwin[window->vas_win.cop]->retained)
			WARN_ON_ONCE(1);
		vinst->rxwin[window->vas_win.cop] = window;
	}

	WARN_ON_ONCE(vinst->windows[id] != NULL);
	vinst->windows[id] = window;

	mutex_unlock(&vinst->mutex);
}

/*
 * Clear this window from the table(s) of windows for this VAS instance.
 * See also function header of set_vinst_win().
 */
static void clear_vinst_win(struct pnv_vas_window *window)
{
	int id = window->vas_win.winid;
	struct vas_instance *vinst = window->vinst;

	mutex_lock(&vinst->mutex);

	if (!window->user_win && !window->tx_win) {
		WARN_ON_ONCE(!vinst->rxwin[window->vas_win.cop]);
		vinst->rxwin[window->vas_win.cop] = NULL;
	}

	WARN_ON_ONCE(vinst->windows[id] != window);
	vinst->windows[id] = NULL;

	mutex_unlock(&vinst->mutex);
}

static void init_winctx_for_rxwin(struct pnv_vas_window *rxwin,
			struct vas_rx_win_attr *rxattr,
			struct vas_winctx *winctx)
{
	/*
	 * We first zero (memset()) all fields and only set non-zero fields.
	 * Following fields are 0/false but maybe deserve a comment:
	 *
	 *	->notify_os_intr_reg	In powerNV, send intrs to HV
	 *	->notify_disable	False for NX windows
	 *	->intr_disable		False for Fault Windows
	 *	->xtra_write		False for NX windows
	 *	->notify_early		NA for NX windows
	 *	->rsvd_txbuf_count	NA for Rx windows
	 *	->lpid, ->pid, ->tid	NA for Rx windows
	 */

	memset(winctx, 0, sizeof(struct vas_winctx));

	winctx->rx_fifo = rxattr->rx_fifo;
	winctx->rx_fifo_size = rxattr->rx_fifo_size;
	winctx->wcreds_max = rxwin->vas_win.wcreds_max;
	winctx->pin_win = rxattr->pin_win;

	winctx->nx_win = rxattr->nx_win;
	winctx->fault_win = rxattr->fault_win;
	winctx->user_win = rxattr->user_win;
	winctx->rej_no_credit = rxattr->rej_no_credit;
	winctx->rx_word_mode = rxattr->rx_win_ord_mode;
	winctx->tx_word_mode = rxattr->tx_win_ord_mode;
	winctx->rx_wcred_mode = rxattr->rx_wcred_mode;
	winctx->tx_wcred_mode = rxattr->tx_wcred_mode;
	winctx->notify_early = rxattr->notify_early;

	if (winctx->nx_win) {
		winctx->data_stamp = true;
		winctx->intr_disable = true;
		winctx->pin_win = true;

		WARN_ON_ONCE(winctx->fault_win);
		WARN_ON_ONCE(!winctx->rx_word_mode);
		WARN_ON_ONCE(!winctx->tx_word_mode);
		WARN_ON_ONCE(winctx->notify_after_count);
	} else if (winctx->fault_win) {
		winctx->notify_disable = true;
	} else if (winctx->user_win) {
		/*
		 * Section 1.8.1 Low Latency Core-Core Wake up of
		 * the VAS workbook:
		 *
		 *      - disable credit checks ([tr]x_wcred_mode = false)
		 *      - disable FIFO writes
		 *      - enable ASB_Notify, disable interrupt
		 *
		 * The workbook is describing a window that carries nothing but
		 * the wake, and the FIFO is disabled there because there is
		 * nowhere to put what a paste carries. A window given a queue
		 * keeps it: the notify is unaffected, and the 128 bytes that
		 * would have been discarded land where its thread can read
		 * them.
		 *
		 * Credit checking stays off either way. Credits are what would
		 * make a full queue refuse a paste, but they are returned by
		 * whatever consumes the queue, and nothing in the kernel
		 * consumes this one -- a thread returning each by system call
		 * would spend more than the mechanism saves.
		 */
		winctx->fifo_disable = !winctx->rx_fifo;
		winctx->intr_disable = true;
	}

	winctx->lnotify_lpid = rxattr->lnotify_lpid;
	winctx->lnotify_pid = rxattr->lnotify_pid;
	winctx->lnotify_tid = rxattr->lnotify_tid;
	winctx->pswid = rxattr->pswid;
	winctx->dma_type = VAS_DMA_TYPE_INJECT;
	winctx->tc_mode = rxattr->tc_mode;

	winctx->min_scope = VAS_SCOPE_LOCAL;
	winctx->max_scope = VAS_SCOPE_VECTORED_GROUP;
	if (rxwin->vinst->virq)
		winctx->irq_port = rxwin->vinst->irq_port;
}

static bool rx_win_args_valid(enum vas_cop_type cop,
			struct vas_rx_win_attr *attr)
{
	pr_debug("Rxattr: fault %d, notify %d, intr %d, early %d, fifo %d\n",
			attr->fault_win, attr->notify_disable,
			attr->intr_disable, attr->notify_early,
			attr->rx_fifo_size);

	if (cop >= VAS_COP_TYPE_MAX)
		return false;

	if (cop != VAS_COP_TYPE_FTW &&
				attr->rx_fifo_size < VAS_RX_FIFO_SIZE_MIN)
		return false;

	if (attr->rx_fifo_size > VAS_RX_FIFO_SIZE_MAX)
		return false;

	/*
	 * A window the switchboard checks no credits against has no maximum
	 * to state. Section 1.8.1 of the VAS workbook has the wake window
	 * disable credit checking, so requiring one here would refuse the one
	 * window that is meant to run without it.
	 */
	if (cop != VAS_COP_TYPE_FTW && !attr->wcreds_max)
		return false;

	if (attr->nx_win) {
		/* cannot be fault or user window if it is nx */
		if (attr->fault_win || attr->user_win)
			return false;
		/*
		 * Section 3.1.4.32: NX Windows must not disable notification,
		 *	and must not enable interrupts or early notification.
		 */
		if (attr->notify_disable || !attr->intr_disable ||
				attr->notify_early)
			return false;
	} else if (attr->fault_win) {
		/* cannot be both fault and user window */
		if (attr->user_win)
			return false;

		/*
		 * Section 3.1.4.32: Fault windows must disable notification
		 *	but not interrupts.
		 */
		if (!attr->notify_disable || attr->intr_disable)
			return false;

	} else if (attr->user_win) {
		/*
		 * A user receive window either only wakes its thread, in which
		 * case it has no queue at all, or keeps what is pasted to it in
		 * one the kernel allocated -- and then both the address and the
		 * size must be there, since half of either describes nothing.
		 *
		 * Interrupts stay disabled whichever it is. There is no handler
		 * for a user window and the thread is reached by notify.
		 */
		if (!attr->intr_disable)
			return false;
		if (!attr->rx_fifo != !attr->rx_fifo_size)
			return false;
	} else {
		/* Rx window must be one of NX or Fault or User window. */
		return false;
	}

	return true;
}

void vas_init_rx_win_attr(struct vas_rx_win_attr *rxattr, enum vas_cop_type cop)
{
	memset(rxattr, 0, sizeof(*rxattr));

	if (cop == VAS_COP_TYPE_842 || cop == VAS_COP_TYPE_842_HIPRI ||
		cop == VAS_COP_TYPE_GZIP || cop == VAS_COP_TYPE_GZIP_HIPRI ||
		cop == VAS_COP_TYPE_SYM || cop == VAS_COP_TYPE_SYM_HIPRI) {
		rxattr->pin_win = true;
		rxattr->nx_win = true;
		rxattr->fault_win = false;
		rxattr->intr_disable = true;
		rxattr->rx_wcred_mode = true;
		rxattr->tx_wcred_mode = true;
		rxattr->rx_win_ord_mode = true;
		rxattr->tx_win_ord_mode = true;
	} else if (cop == VAS_COP_TYPE_FAULT) {
		rxattr->pin_win = true;
		rxattr->fault_win = true;
		rxattr->notify_disable = true;
		rxattr->rx_wcred_mode = true;
		rxattr->rx_win_ord_mode = true;
		rxattr->rej_no_credit = true;
		rxattr->tc_mode = VAS_THRESH_DISABLED;
	} else if (cop == VAS_COP_TYPE_FTW) {
		rxattr->user_win = true;
		rxattr->intr_disable = true;

		/*
		 * As noted in the VAS Workbook we disable credit checks.
		 * If we enable credit checks in the future, we must also
		 * implement a mechanism to return the user credits or new
		 * paste operations will fail.
		 */
	}
}
EXPORT_SYMBOL_GPL(vas_init_rx_win_attr);

struct vas_window *vas_rx_win_open(int vasid, enum vas_cop_type cop,
			struct vas_rx_win_attr *rxattr)
{
	struct pnv_vas_window *rxwin;
	struct vas_winctx winctx;
	struct vas_instance *vinst;

	trace_vas_rx_win_open(current, vasid, cop, rxattr);

	if (!rx_win_args_valid(cop, rxattr))
		return ERR_PTR(-EINVAL);

	vinst = find_vas_instance(vasid);
	if (!vinst) {
		pr_devel("vasid %d not found!\n", vasid);
		return ERR_PTR(-EINVAL);
	}
	pr_devel("Found instance %d\n", vasid);

	rxwin = vas_window_alloc(vinst);
	if (IS_ERR(rxwin)) {
		pr_devel("Unable to allocate memory for Rx window\n");
		return (struct vas_window *)rxwin;
	}

	rxwin->tx_win = false;
	rxwin->nx_win = rxattr->nx_win;
	rxwin->user_win = rxattr->user_win;
	rxwin->vas_win.cop = cop;
	rxwin->vas_win.wcreds_max = rxattr->wcreds_max;
	rxwin->lnotify_tid = rxattr->lnotify_tid;

	init_winctx_for_rxwin(rxwin, rxattr, &winctx);
	init_winctx_regs(rxwin, &winctx);

	set_vinst_win(vinst, rxwin);

	return &rxwin->vas_win;
}
EXPORT_SYMBOL_GPL(vas_rx_win_open);

void vas_init_tx_win_attr(struct vas_tx_win_attr *txattr, enum vas_cop_type cop)
{
	memset(txattr, 0, sizeof(*txattr));

	if (cop == VAS_COP_TYPE_842 || cop == VAS_COP_TYPE_842_HIPRI ||
		cop == VAS_COP_TYPE_GZIP || cop == VAS_COP_TYPE_GZIP_HIPRI ||
		cop == VAS_COP_TYPE_SYM || cop == VAS_COP_TYPE_SYM_HIPRI) {
		txattr->rej_no_credit = false;
		txattr->rx_wcred_mode = true;
		txattr->tx_wcred_mode = true;
		txattr->rx_win_ord_mode = true;
		txattr->tx_win_ord_mode = true;
	} else if (cop == VAS_COP_TYPE_FTW) {
		txattr->user_win = true;
	}
}
EXPORT_SYMBOL_GPL(vas_init_tx_win_attr);

static void init_winctx_for_txwin(struct pnv_vas_window *txwin,
			struct vas_tx_win_attr *txattr,
			struct vas_winctx *winctx)
{
	/*
	 * We first zero all fields and only set non-zero ones. Following
	 * are some fields set to 0/false for the stated reason:
	 *
	 *	->notify_os_intr_reg	In powernv, send intrs to HV
	 *	->rsvd_txbuf_count	Not supported yet.
	 *	->notify_disable	False for NX windows
	 *	->xtra_write		False for NX windows
	 *	->notify_early		NA for NX windows
	 *	->lnotify_lpid		NA for Tx windows
	 *	->lnotify_pid		NA for Tx windows
	 *	->lnotify_tid		NA for Tx windows
	 *	->tx_win_cred_mode	Ignore for now for NX windows
	 *	->rx_win_cred_mode	Ignore for now for NX windows
	 */
	memset(winctx, 0, sizeof(struct vas_winctx));

	winctx->wcreds_max = txwin->vas_win.wcreds_max;

	winctx->user_win = txattr->user_win;
	winctx->amr = txattr->amr;
	winctx->nx_win = txwin->rxwin->nx_win;
	winctx->pin_win = txattr->pin_win;
	winctx->rej_no_credit = txattr->rej_no_credit;
	winctx->rsvd_txbuf_enable = txattr->rsvd_txbuf_enable;

	winctx->rx_wcred_mode = txattr->rx_wcred_mode;
	winctx->tx_wcred_mode = txattr->tx_wcred_mode;
	winctx->rx_word_mode = txattr->rx_win_ord_mode;
	winctx->tx_word_mode = txattr->tx_win_ord_mode;
	winctx->rsvd_txbuf_count = txattr->rsvd_txbuf_count;

	winctx->intr_disable = true;
	if (winctx->nx_win)
		winctx->data_stamp = true;

	winctx->lpid = txattr->lpid;
	winctx->pidr = txattr->pidr;
	winctx->rx_win_id = txwin->rxwin->vas_win.winid;
	/*
	 * IRQ and fault window setup is successful. Set fault window
	 * for the send window so that ready to handle faults.
	 */
	if (txwin->vinst->virq)
		winctx->fault_win_id = txwin->vinst->fault_win->vas_win.winid;

	winctx->dma_type = VAS_DMA_TYPE_INJECT;
	winctx->tc_mode = txattr->tc_mode;
	winctx->min_scope = VAS_SCOPE_LOCAL;
	winctx->max_scope = VAS_SCOPE_VECTORED_GROUP;
	if (txwin->vinst->virq)
		winctx->irq_port = txwin->vinst->irq_port;

	/*
	 * A window's own name, which NX stamps into the fault CRB so the fault
	 * handler can find it again. The destination of a wake is not carried
	 * here but in rx_win_id above, so a window pointed at another still
	 * answers to itself.
	 */
	winctx->pswid = encode_pswid(txwin->vinst->vas_id,
				     txwin->vas_win.winid);
}

static bool tx_win_args_valid(enum vas_cop_type cop,
			struct vas_tx_win_attr *attr)
{
	if (attr->tc_mode != VAS_THRESH_DISABLED)
		return false;

	if (cop >= VAS_COP_TYPE_MAX)
		return false;

	if (attr->wcreds_max > VAS_TX_WCREDS_MAX)
		return false;

	if (attr->user_win) {
		if (attr->rsvd_txbuf_count)
			return false;

		/*
		 * The types a user window may be opened against. This is a
		 * policy list, not a hardware limit: vas_init_tx_win_attr()
		 * already gives 842 and SYM the same window attributes as
		 * GZIP, and each has a receive window on both FIFO
		 * priorities. A type is listed once the driver registers a
		 * device node for it.
		 */
		switch (cop) {
		case VAS_COP_TYPE_FTW:
		case VAS_COP_TYPE_GZIP:
		case VAS_COP_TYPE_GZIP_HIPRI:
		case VAS_COP_TYPE_842:
		case VAS_COP_TYPE_842_HIPRI:
		case VAS_COP_TYPE_SYM:
		case VAS_COP_TYPE_SYM_HIPRI:
			break;
		default:
			return false;
		}
	}

	return true;
}

struct vas_window *vas_tx_win_open(int vasid, enum vas_cop_type cop,
			struct vas_tx_win_attr *attr)
{
	int rc;
	struct pnv_vas_window *txwin;
	struct pnv_vas_window *rxwin;
	struct vas_winctx winctx;
	struct vas_instance *vinst;

	trace_vas_tx_win_open(current, vasid, cop, attr);

	if (!tx_win_args_valid(cop, attr))
		return ERR_PTR(-EINVAL);

	/*
	 * A window pointed at another has to be on the instance that other is
	 * on, because the id naming a destination means nothing anywhere else.
	 * A caller that asked for no instance in particular is given that one
	 * rather than refused for having asked for the wrong thing.
	 */
	if (vas_instance_is_any(vasid) && attr->target)
		vasid = container_of(attr->target, struct pnv_vas_window,
				     vas_win)->vinst->vas_id;

	vinst = find_vas_instance(vasid);
	if (!vinst) {
		pr_devel("vasid %d not found!\n", vasid);
		return ERR_PTR(-EINVAL);
	}

	rxwin = get_vinst_rxwin(vinst, cop, attr->target);
	if (IS_ERR(rxwin)) {
		pr_devel("No RxWin for vasid %d, cop %d\n", vasid, cop);
		return (struct vas_window *)rxwin;
	}

	txwin = vas_window_alloc(vinst);
	if (IS_ERR(txwin)) {
		rc = PTR_ERR(txwin);
		goto put_rxwin;
	}

	txwin->vas_win.cop = cop;
	txwin->tx_win = 1;
	txwin->rxwin = rxwin;
	txwin->nx_win = txwin->rxwin->nx_win;
	txwin->user_win = attr->user_win;
	txwin->vas_win.wcreds_max = attr->wcreds_max ?: VAS_WCREDS_DEFAULT;

	if (txwin->user_win) {
		rc = vas_fault_ring_alloc(txwin);
		if (rc)
			goto free_window;
	}

	init_winctx_for_txwin(txwin, attr, &winctx);

	init_winctx_regs(txwin, &winctx);

	/*
	 * If its a kernel send window, map the window address into the
	 * kernel's address space. For user windows, user must issue an
	 * mmap() to map the window into their address space.
	 *
	 * NOTE: If kernel ever resubmits a user CRB after handling a page
	 *	 fault, we will need to map this into kernel as well.
	 */
	if (!txwin->user_win) {
		txwin->paste_kaddr = map_paste_region(txwin);
		if (IS_ERR(txwin->paste_kaddr)) {
			rc = PTR_ERR(txwin->paste_kaddr);
			goto free_window;
		}
	} else {
		/*
		 * Interrupt handler or fault window setup failed. Means
		 * NX can not generate fault for page fault. So not
		 * opening for user space tx window.
		 */
		if (!vinst->virq) {
			rc = -ENODEV;
			goto free_window;
		}
		/*
		 * No flags: this platform has no QoS credit pool, so every
		 * window is charged to the default resource. The QoS
		 * capacity is never set here, and a charge against a
		 * zero-capacity resource fails with EINVAL, not EBUSY, so
		 * the flags must not reach this call unless that capacity
		 * is given a value first.
		 */
		rc = get_vas_user_win_ref(&txwin->vas_win.task_ref, 0,
					  attr->amr);
		if (rc)
			goto free_window;
		/* Owned by the reference from here: freed with it. */
		txwin->vas_win.task_ref.nmmu_view = attr->nmmu_view;

		vas_user_win_add_mm_context(&txwin->vas_win.task_ref);
	}

	set_vinst_win(vinst, txwin);

	return &txwin->vas_win;

free_window:
	vas_window_free(txwin);

put_rxwin:
	put_rx_win(rxwin);
	return ERR_PTR(rc);

}
EXPORT_SYMBOL_GPL(vas_tx_win_open);

int vas_copy_crb(void *crb, int offset)
{
	return vas_copy(crb, offset);
}
EXPORT_SYMBOL_GPL(vas_copy_crb);

#define RMA_LSMP_REPORT_ENABLE PPC_BIT(53)
int vas_paste_crb(struct vas_window *vwin, int offset, bool re)
{
	struct pnv_vas_window *txwin;
	int rc;
	void *addr;
	uint64_t val;

	txwin = container_of(vwin, struct pnv_vas_window, vas_win);
	trace_vas_paste_crb(current, txwin);

	/*
	 * Only NX windows are supported for now and hardware assumes
	 * report-enable flag is set for NX windows. Ensure software
	 * complies too.
	 */
	WARN_ON_ONCE(txwin->nx_win && !re);

	addr = txwin->paste_kaddr;
	if (re) {
		/*
		 * Set the REPORT_ENABLE bit (equivalent to writing
		 * to 1K offset of the paste address)
		 */
		val = SET_FIELD(RMA_LSMP_REPORT_ENABLE, 0ULL, 1);
		addr += val;
	}

	/*
	 * Map the raw CR value from vas_paste() to an error code (there
	 * is just pass or fail for now though).
	 */
	rc = vas_paste(addr, offset);
	if (rc == 2)
		rc = 0;
	else
		rc = -EINVAL;

	pr_debug("Txwin #%d: Msg count %llu\n", txwin->vas_win.winid,
			read_hvwc_reg(txwin, VREG(LRFIFO_PUSH)));

	return rc;
}
EXPORT_SYMBOL_GPL(vas_paste_crb);

/*
 * If credit checking is enabled for this window, poll for the return
 * of window credits (i.e for NX engines to process any outstanding CRBs).
 * Since NX-842 waits for the CRBs to be processed before closing the
 * window, we should not have to wait for too long.
 *
 * TODO: We retry in 10ms intervals now. We could/should probably peek at
 *	the VAS_LRFIFO_PUSH_OFFSET register to get an estimate of pending
 *	CRBs on the FIFO and compute the delay dynamically on each retry.
 *	But that is not really needed until we support NX-GZIP access from
 *	user space. (NX-842 driver waits for CSB and Fast thread-wakeup
 *	doesn't use credit checking).
 */
/*
 * How long the close path waits for the hardware before giving the window
 * up: both polls retry every 10ms, so this is one minute. Closing needs
 * the accelerator to hand back every credit it took, which normally
 * takes milliseconds, and can legitimately take as long as the slowest
 * fault the fault thread is resolving in front of ours. What it must not
 * take is forever with the closing task uninterruptible: close runs from
 * exit_task_work, so an unbounded wait here is a process that cannot exit
 * and a SIGKILL that does nothing.
 */
#define VAS_WIN_CLOSE_RETRIES	6000

/*
 * Whether to stop waiting for the hardware because the caller is being killed.
 *
 * Not on the first pass. Credits normally come back in a few milliseconds, and
 * abandoning the wait retains the window -- its id, credits, hardware PID, mm
 * and cgroup charge held until the machine reboots. Giving up the moment a
 * fatal signal is pending would make every killed process leak one, which is
 * something any user can arrange in a loop. Wait out the grace period first,
 * so an ordinary close still completes for a process that is being killed, and
 * only a window the hardware has genuinely not released is retained.
 */
#define VAS_WIN_CLOSE_GRACE	100	/* 10ms each, so one second */

static bool close_wait_aborted(int count)
{
	return count >= VAS_WIN_CLOSE_GRACE && fatal_signal_pending(current);
}

/*
 * How often a deferred close re-checks, and how long it keeps trying. The
 * interval is long enough that a window waiting on a slow accelerator costs
 * almost nothing, and the total is generous because the alternative to waiting
 * is leaking the window for the lifetime of the machine.
 */
#define VAS_CLOSE_DEFER_INTERVAL	msecs_to_jiffies(500)
#define VAS_CLOSE_DEFER_TRIES		2400		/* 20 minutes */
#define VAS_CLOSE_DEFER_MAX		256		/* closes in flight */

struct workqueue_struct *vas_close_wq;

/* Keep a window that will not close, and account for it. */
static void vas_win_retain(struct pnv_vas_window *window)
{
	if (window->retained)
		return;
	window->retained = true;
	atomic_inc(&window->vinst->nr_retained);
	vas_stat_inc(VAS_STAT_WIN_RETAINED);
	pr_err("VAS: window %u (pid %d) never closed; retaining its resources\n",
	       window->vas_win.winid, vas_window_pid(&window->vas_win));
}

/* Single-shot forms of the two waits, for the deferred path. */
static bool window_still_busy(struct pnv_vas_window *window)
{
	u64 val = read_hvwc_reg(window, VREG(WIN_STATUS));

	return GET_FIELD(VAS_WIN_BUSY, val) != 0;
}

static bool window_credits_outstanding(struct pnv_vas_window *window)
{
	u64 val;
	int creds, mode;

	val = read_hvwc_reg(window, VREG(WINCTL));
	if (window->tx_win)
		mode = GET_FIELD(VAS_WINCTL_TX_WCRED_MODE, val);
	else
		mode = GET_FIELD(VAS_WINCTL_RX_WCRED_MODE, val);

	if (!mode)
		return false;

	if (window->tx_win) {
		val = read_hvwc_reg(window, VREG(TX_WCRED));
		creds = GET_FIELD(VAS_TX_WCRED, val);
	} else {
		val = read_hvwc_reg(window, VREG(LRX_WCRED));
		creds = GET_FIELD(VAS_LRX_WCRED, val);
	}

	return creds < window->vas_win.wcreds_max;
}

static int poll_window_credits(struct pnv_vas_window *window)
{
	u64 val;
	int creds, mode;
	int count = 0;

	val = read_hvwc_reg(window, VREG(WINCTL));
	if (window->tx_win)
		mode = GET_FIELD(VAS_WINCTL_TX_WCRED_MODE, val);
	else
		mode = GET_FIELD(VAS_WINCTL_RX_WCRED_MODE, val);

	if (!mode)
		return 0;
retry:
	if (window->tx_win) {
		val = read_hvwc_reg(window, VREG(TX_WCRED));
		creds = GET_FIELD(VAS_TX_WCRED, val);
	} else {
		val = read_hvwc_reg(window, VREG(LRX_WCRED));
		creds = GET_FIELD(VAS_LRX_WCRED, val);
	}

	/*
	 * Takes around few milliseconds to complete all pending requests
	 * and return credits.
	 * TODO: Scan fault FIFO and invalidate CRBs points to this window
	 *       and issue CRB Kill to stop all pending requests. Need only
	 *       if there is a bug in NX or fault handling in kernel.
	 */
	if (creds < window->vas_win.wcreds_max) {
		val = 0;
		if (count >= VAS_WIN_CLOSE_RETRIES || close_wait_aborted(count))
			return -ETIMEDOUT;
		set_current_state(TASK_KILLABLE);
		schedule_timeout(msecs_to_jiffies(10));
		count++;
		/*
		 * Process can not close send window until all credits are
		 * returned.
		 */
		if (!(count % 1000))
			pr_warn_ratelimited("VAS: pid %d stuck. Waiting for credits returned for Window(%d). creds %d, Retries %d\n",
				vas_window_pid(&window->vas_win),
				window->vas_win.winid,
				creds, count);

		goto retry;
	}

	return 0;
}

/*
 * Wait for the window to go to "not-busy" state. It should only take a
 * short time to queue a CRB, so window should not be busy for too long.
 * Trying 5ms intervals.
 */
static int poll_window_busy_state(struct pnv_vas_window *window)
{
	int busy;
	u64 val;
	int count = 0;

retry:
	val = read_hvwc_reg(window, VREG(WIN_STATUS));
	busy = GET_FIELD(VAS_WIN_BUSY, val);
	if (busy) {
		val = 0;
		if (count >= VAS_WIN_CLOSE_RETRIES || close_wait_aborted(count))
			return -ETIMEDOUT;
		set_current_state(TASK_KILLABLE);
		schedule_timeout(msecs_to_jiffies(10));
		count++;
		/*
		 * Takes around few milliseconds to process all pending
		 * requests.
		 */
		if (!(count % 1000))
			pr_warn_ratelimited("VAS: pid %d stuck. Window (ID=%d) is in busy state. Retries %d\n",
				vas_window_pid(&window->vas_win),
				window->vas_win.winid, count);

		goto retry;
	}

	return 0;
}

/*
 * Have the hardware cast a window out of cache and wait for it to
 * be completed.
 *
 * NOTE: It can take a relatively long time to cast the window context
 *	out of the cache. It is not strictly necessary to cast out if:
 *
 *	- we clear the "Pin Window" bit (so hardware is free to evict)
 *
 *	- we re-initialize the window context when it is reassigned.
 *
 *	We do the former in vas_win_close() and latter in vas_win_open().
 *	So, ignoring the cast-out for now. We can add it as needed. If
 *	casting out becomes necessary we should consider offloading the
 *	job to a worker thread, so the window close can proceed quickly.
 */
static void poll_window_castout(struct pnv_vas_window *window)
{
	/* stub for now */
}

/*
 * Unpin and close a window so no new requests are accepted and the
 * hardware can evict this window from cache if necessary.
 */
static void unpin_close_window(struct pnv_vas_window *window)
{
	u64 val;

	val = read_hvwc_reg(window, VREG(WINCTL));
	val = SET_FIELD(VAS_WINCTL_PIN, val, 0);
	val = SET_FIELD(VAS_WINCTL_OPEN, val, 0);
	write_hvwc_reg(window, VREG(WINCTL), val);
}

/*
 * Close a window.
 *
 * See Section 1.12.1 of VAS workbook v1.05 for details on closing window:
 *	- Disable new paste operations (unmap paste address)
 *	- Poll for the "Window Busy" bit to be cleared
 *	- Clear the Open/Enable bit for the Window.
 *	- Poll for return of window Credits (implies FIFO empty for Rx win?)
 *	- Unpin and cast window context out of cache
 *
 * Besides the hardware, kernel has some bookkeeping of course.
 */
/*
 * Finish a close the caller could not wait out.
 *
 * Runs the remaining steps of section 1.12.1 from wherever vas_win_close()
 * stopped, one check per run so no worker is held while the hardware takes its
 * time. The window is unreachable throughout: its paste mapping is gone and
 * its id is not reissued, so nothing can submit to it while this waits.
 */
static void vas_close_work_fn(struct work_struct *work)
{
	struct pnv_vas_window *window = container_of(to_delayed_work(work),
						     struct pnv_vas_window,
						     close_work);
	struct vas_window *vwin = &window->vas_win;

	if (window->close_stage == VAS_CLOSE_BUSY) {
		if (window_still_busy(window))
			goto again;
		unpin_close_window(window);
		window->close_stage = VAS_CLOSE_CREDITS;
	}

	if (window_credits_outstanding(window))
		goto again;

	/*
	 * Everything the hardware owed has come back, so the close can be
	 * completed exactly as the synchronous path would have.
	 *
	 * The address space references are dropped here rather than by
	 * coproc_release(), which released them only for a close that returned
	 * zero. A deferred close did not, so ownership passed to this work
	 * when it was queued, and it ends here.
	 */
	clear_vinst_win(window);
	poll_window_castout(window);

	if (window->tx_win) {
		if (window->user_win) {
			mm_context_remove_vas_window(vwin->task_ref.mm);
			put_vas_user_win_ref(&vwin->task_ref);
		}
		put_rx_win(window->rxwin);
	}

	pr_info("VAS: window %u closed after %d deferred attempt(s)\n",
		vwin->winid, window->close_tries);
	atomic_dec(&window->vinst->nr_deferring);
	vas_window_free(window);
	return;

again:
	if (++window->close_tries < VAS_CLOSE_DEFER_TRIES) {
		queue_delayed_work(vas_close_wq, &window->close_work,
				   VAS_CLOSE_DEFER_INTERVAL);
		return;
	}

	/*
	 * Out of patience: hardware that is not going to give the window
	 * back. Keep it, and let the operator see it.
	 */
	atomic_dec(&window->vinst->nr_deferring);
	vas_win_retain(window);
}

int vas_win_close(struct vas_window *vwin)
{
	struct pnv_vas_window *window;
	int rc;

	if (!vwin)
		return 0;

	window = container_of(vwin, struct pnv_vas_window, vas_win);

	if (!window->tx_win && atomic_read(&window->num_txwins) != 0) {
		/*
		 * With no retained window on the instance this is a caller
		 * bug and deserves the alarm. With one, it is the expected
		 * shadow of that leak: a send window kept after a failed
		 * close still references this receive window, so keep the
		 * pair, quietly and accounted.
		 */
		if (atomic_read(&window->vinst->nr_retained)) {
			if (!window->retained) {
				window->retained = true;
				atomic_inc(&window->vinst->nr_retained);
				vas_stat_inc(VAS_STAT_WIN_RETAINED);
			}
			pr_err("VAS: rx window %u kept; %d tx window(s) still attached\n",
			       vwin->winid, atomic_read(&window->num_txwins));
		} else {
			pr_devel("Attempting to close an active Rx window!\n");
			WARN_ON_ONCE(1);
		}
		return -EBUSY;
	}

	unmap_paste_region(window);

	rc = poll_window_busy_state(window);
	if (rc) {
		window->close_stage = VAS_CLOSE_BUSY;
		goto defer;
	}

	unpin_close_window(window);

	rc = poll_window_credits(window);
	if (rc) {
		window->close_stage = VAS_CLOSE_CREDITS;
		goto defer;
	}

	clear_vinst_win(window);

	poll_window_castout(window);

	/* if send window, drop reference to matching receive window */
	if (window->tx_win)
		put_rx_win(window->rxwin);

	vas_window_free(window);

	return 0;

defer:
	/*
	 * The hardware still owes this window work: requests are in flight or
	 * credits are unreturned, and they can complete at any time by writing
	 * through the window's translation into the address space behind it.
	 * Freeing anything here -- the window id, the window itself, the mm and
	 * pid references -- hands that write to whoever owns the memory next,
	 * which is how a wedged accelerator becomes another process's
	 * corruption.
	 *
	 * So the window cannot be freed now. It does not follow that it can
	 * never be freed: what is outstanding usually completes, just not
	 * within the caller's patience. Hand the rest of the close to a worker
	 * and return. The caller's wait is what was abandoned, not the close.
	 *
	 * close_stage records where this stopped, because the two waits leave
	 * different hardware states. After the busy wait the window is still
	 * open and pinned -- clearing the enable under queued requests is what
	 * the ordering in the workbook exists to prevent. After the credit wait
	 * it is unpinned and closed, and only the writeback of already accepted
	 * requests is outstanding. Neither can be pasted to again: the mappings
	 * are gone and the id is never reissued.
	 */
	window->close_tries = 0;

	/*
	 * Bound the number of closes in flight, for the same reason TCP bounds
	 * TIME_WAIT sockets: a deferred close holds an mm reference and a
	 * window id for as long as it waits, and reaching this path is
	 * something an unprivileged process can arrange in a loop. Past the
	 * limit, fall back to retaining, so the worst behaviour under attack
	 * is a retained window and the common case still recovers its
	 * windows.
	 */
	if (vas_close_wq &&
	    atomic_inc_return(&window->vinst->nr_deferring) <= VAS_CLOSE_DEFER_MAX) {
		queue_delayed_work(vas_close_wq, &window->close_work,
				   VAS_CLOSE_DEFER_INTERVAL);
		return rc;
	}
	atomic_dec(&window->vinst->nr_deferring);

	/*
	 * No worker to hand it to, so keep everything as the only safe option
	 * left. vas_close_finish() reaches this too, once a deferred close has
	 * run out of attempts.
	 */
	vas_win_retain(window);
	return rc;
}
EXPORT_SYMBOL_GPL(vas_win_close);

/*
 * Return credit for the given window.
 * Send windows and fault window uses credit mechanism as follows:
 *
 * Send windows:
 * - The default number of credits available for each send window is
 *   1024. It means 1024 requests can be issued asynchronously at the
 *   same time. If the credit is not available, that request will be
 *   returned with RMA_Busy.
 * - One credit is taken when NX request is issued.
 * - This credit is returned after NX processed that request.
 * - If NX encounters translation error, kernel will return the
 *   credit on the specific send window after processing the fault CRB.
 *
 * Fault window:
 * - The total number credits available is FIFO_SIZE/CRB_SIZE.
 *   Means 4MB/128 in the current implementation. If credit is not
 *   available, RMA_Reject is returned.
 * - A credit is taken when NX pastes CRB in fault FIFO.
 * - The kernel with return credit on fault window after reading entry
 *   from fault FIFO.
 */
void vas_return_credit(struct pnv_vas_window *window, bool tx)
{
	uint64_t val;

	val = 0ULL;
	if (tx) { /* send window */
		val = SET_FIELD(VAS_TX_WCRED, val, 1);
		write_hvwc_reg(window, VREG(TX_WCRED_ADDER), val);
	} else {
		val = SET_FIELD(VAS_LRX_WCRED, val, 1);
		write_hvwc_reg(window, VREG(LRX_WCRED_ADDER), val);
	}
}

struct pnv_vas_window *vas_pswid_to_window(struct vas_instance *vinst,
		uint32_t pswid)
{
	struct pnv_vas_window *window;
	int winid;

	if (!vas_pswid_names_window(pswid)) {
		pr_devel("%s: called with no window named\n", __func__);
		return ERR_PTR(-ESRCH);
	}

	decode_pswid(pswid, NULL, &winid);

	if (winid >= VAS_WINDOWS_PER_CHIP)
		return ERR_PTR(-ESRCH);

	/*
	 * If application closes the window before the hardware
	 * returns the fault CRB, we should wait in vas_win_close()
	 * for the pending requests. so the window must be active
	 * and the process alive.
	 *
	 * If its a kernel process, we should not get any faults and
	 * should not get here.
	 */
	window = vinst->windows[winid];

	if (!window) {
		pr_err("PSWID decode: Could not find window for winid %d pswid %d vinst 0x%p\n",
			winid, pswid, vinst);
		return NULL;
	}

	/*
	 * Do some sanity checks on the decoded window.  Window should be
	 * NX GZIP user send window. FTW windows should not incur faults
	 * since their CRBs are ignored (not queued on FIFO or processed
	 * by NX).
	 */
	if (!window->tx_win || !window->user_win || !window->nx_win ||
			window->vas_win.cop == VAS_COP_TYPE_FAULT ||
			window->vas_win.cop == VAS_COP_TYPE_FTW) {
		pr_err("PSWID decode: id %d, tx %d, user %d, nx %d, cop %d\n",
			winid, window->tx_win, window->user_win,
			window->nx_win, window->vas_win.cop);
		WARN_ON(1);
	}

	return window;
}

static struct vas_window *vas_user_win_open(const struct vas_user_win_req *req)
{
	enum vas_cop_type cop_type = req->cop_type;
	struct vas_tx_win_attr txattr = {};
	struct nmmu_view *view = NULL;
	int vas_id = req->vas_id;
	struct vas_window *win;
	int pid;

	if (!current->mm)
		return ERR_PTR(-EINVAL);

	/*
	 * The nest MMU translates on this window's behalf using the PID the
	 * window carries, so that PID has to name the caller's mm. Take it from
	 * the mm rather than from SPRN_PID. That register is only a copy of the
	 * running mm's PID, kept there for the nest MMU by
	 * hash__switch_mmu_context() because the core itself does not translate
	 * through it under HPT (POWER9 User's Manual 4.10.7, "The PIDR is not
	 * used in this submode in the processor core, but is used by the NMMU").
	 * This is the call that allocates the PID, so it is not in the register
	 * yet. ocxl already sources this from the mm; see
	 * ocxl_context_attach() in drivers/misc/ocxl/context.c.
	 *
	 * LPID is still read from its register: it is per partition and does
	 * not vary between mms.
	 */
	pid = mm_alloc_hw_pid(current->mm);
	if (pid < 0)
		return ERR_PTR(pid);

	/*
	 * A confined window translates through a view of its own, with a
	 * PID of its own, so the nest MMU can be shown less than the mm.
	 * Only the hashed page table has a table per PID to show it in.
	 */
	if (req->flags & VAS_TX_WIN_FLAG_DOMAINS) {
		if (radix_enabled())
			return ERR_PTR(-EOPNOTSUPP);
		view = hash__nmmu_view_new(current->mm);
		if (IS_ERR(view))
			return ERR_CAST(view);
		pid = hash__nmmu_view_pid(view);
	}

	vas_init_tx_win_attr(&txattr, cop_type);

	txattr.lpid = mfspr(SPRN_LPID);
	txattr.pidr = pid;
	txattr.user_win = true;
	txattr.amr = req->amr;
	txattr.nmmu_view = view;
	txattr.rsvd_txbuf_count = false;
	txattr.target = req->target;

	pr_devel("Pid %d: Opening txwin, hardware PID %d\n",
		 task_pid_nr(current), txattr.pidr);

	win = vas_tx_win_open(vas_id, cop_type, &txattr);
	if (IS_ERR(win) && view)
		hash__nmmu_view_free(view);
	return win;
}

static int vas_user_win_domain(struct vas_window *vwin, u64 start, u64 len,
			       bool add)
{
	struct nmmu_view *view = vwin->task_ref.nmmu_view;

	if (!view)
		return -EINVAL;
	return add ? hash__nmmu_view_allow(view, start, len)
		   : hash__nmmu_view_deny(view, start, len);
}

static u64 vas_user_win_paste_addr(struct vas_window *txwin)
{
	struct pnv_vas_window *win;
	u64 paste_addr;

	win = container_of(txwin, struct pnv_vas_window, vas_win);
	vas_win_paste_addr(win, &paste_addr, NULL);

	return paste_addr;
}

static int vas_user_win_close(struct vas_window *txwin)
{
	return vas_win_close(txwin);
}

/*
 * A deferred close still references a window and the mm behind it, so it
 * has to finish before anything here goes away. Cancelling would leave
 * exactly the leak this work exists to avoid.
 */
/*
 * A receive window for the calling thread, which a send window may then be
 * pointed at. The switchboard addresses it by the same partition, process
 * and thread identity that names an accelerator's queue: firmware gives the
 * engines a synthetic identity because they have none of their own, and a
 * thread is named by the identity it already has.
 *
 * The thread identity register is what distinguishes threads of one process,
 * and it is set lazily, so ask for it before the window is built with it.
 *
 * The window may outlive the descriptor its opener holds, because a sender
 * keeps one too, so it takes the same references on the opening thread that a
 * send window takes. The hardware PID it wakes is the opener's, and holding
 * the mm is what stops that PID being handed to another process while a
 * window still names it.
 */
/*
 * As many entries as the workbook gives an engine's queue, which is the only
 * size the hardware is known to be exercised at, rounded to something that can
 * be mapped whole.
 */
#define VAS_USER_FIFO_DEFAULT	(256 * CRB_SIZE)

/*
 * Somewhere for a receive window to keep what is pasted to it.
 *
 * Page-aligned and physically contiguous because the switchboard is given a
 * real address and the pages are handed to the process that opened the window,
 * and every entry left invalid because that is how a reader tells an entry that
 * has arrived from one that never has.
 */
static void *vas_user_fifo_alloc(u32 want, u32 *len)
{
	u32 size = want ? want : VAS_USER_FIFO_DEFAULT;
	void *fifo;

	size = ALIGN(size, PAGE_SIZE);
	if (size > VAS_RX_FIFO_SIZE_MAX)
		return ERR_PTR(-EINVAL);

	fifo = alloc_pages_exact(size, GFP_KERNEL);
	if (!fifo)
		return ERR_PTR(-ENOMEM);

	memset(fifo, FIFO_INVALID_ENTRY, size);
	*len = size;

	return fifo;
}

static struct vas_window *vas_user_rx_win_open(const struct vas_user_win_req *req)
{
	struct vas_rx_win_attr rxattr;
	struct pnv_vas_window *pnv_win;
	struct vas_window *win;
	void *fifo = NULL;
	u32 fifo_len = 0;
	int rc;

	rc = set_thread_tidr(current);
	if (rc)
		return ERR_PTR(rc);

	vas_init_rx_win_attr(&rxattr, req->cop_type);
	rxattr.user_win = true;
	rxattr.lnotify_lpid = mfspr(SPRN_LPID);
	rxattr.lnotify_pid = mfspr(SPRN_PID);
	rxattr.lnotify_tid = current->thread.tidr;

	/*
	 * Joining an existing destination: take its thread identity as this
	 * thread's own, so one notify matches both and one paste wakes both.
	 *
	 * Only within one address space. The switchboard addresses a
	 * destination by partition, process and thread, and the process part
	 * is the hardware identifier of the address space -- a thread cannot
	 * present another's, so a group that spanned processes would never be
	 * matched however the thread part were arranged.
	 *
	 * This gives up the one thing the identity otherwise guarantees. It is
	 * how an accelerator names the thread that submitted to it, and two
	 * threads answering to one name means either may be resumed in the
	 * other's place. A group is for threads waiting on the same event, not
	 * for threads with work of their own outstanding.
	 */
	if (req->target) {
		struct pnv_vas_window *group =
			container_of(req->target, struct pnv_vas_window, vas_win);

		if (group->vas_win.cop != VAS_COP_TYPE_FTW || group->tx_win)
			return ERR_PTR(-EINVAL);
		if (group->vas_win.task_ref.mm != current->mm)
			return ERR_PTR(-EPERM);

		rxattr.lnotify_tid = group->lnotify_tid;
		current->thread.tidr = rxattr.lnotify_tid;
		mtspr(SPRN_TIDR, current->thread.tidr);
	}

	/*
	 * Allocated before the window, so a window is never opened writing
	 * into a queue that could not be had.
	 */
	if (req->rx_fifo) {
		fifo = vas_user_fifo_alloc(req->rx_fifo_size, &fifo_len);
		if (IS_ERR(fifo))
			return ERR_CAST(fifo);

		rxattr.rx_fifo = __pa(fifo);
		rxattr.rx_fifo_size = fifo_len;
		/*
		 * How many entries the queue holds, which the switchboard is
		 * told as a receive credit count. Without it the count is zero
		 * and the hardware has nowhere to put a paste: the notify still
		 * arrives and the bytes do not, which measured as one delivery
		 * in eight.
		 *
		 * Checking stays off, as it is for every window of this type.
		 * With it on a full queue would refuse the paste, and nothing
		 * returns a credit here -- the queue would take as many
		 * messages as it has entries and then stop for good.
		 */
		rxattr.wcreds_max = fifo_len / CRB_SIZE;
	}

	win = vas_rx_win_open(req->vas_id, req->cop_type, &rxattr);
	if (IS_ERR(win)) {
		if (fifo)
			free_pages_exact(fifo, fifo_len);
		return win;
	}

	pnv_win = container_of(win, struct pnv_vas_window, vas_win);
	/*
	 * Handed over before anything else can fail: from here the window owns
	 * it and vas_window_free() is what releases it.
	 */
	pnv_win->rx_fifo_buf = fifo;
	pnv_win->rx_fifo_len = fifo_len;

	rc = get_vas_user_win_ref(&win->task_ref, req->flags, 0);
	if (rc) {
		vas_win_close(win);
		return ERR_PTR(rc);
	}

	pr_devel("Pid %d: receive window %d on vas %d, notify %d:%d:%d\n",
		 task_pid_nr(current), win->winid, pnv_win->vinst->vas_id,
		 rxattr.lnotify_lpid, rxattr.lnotify_pid, rxattr.lnotify_tid);

	return win;
}

static void vas_user_win_drain_closes(void)
{
	if (vas_close_wq)
		flush_workqueue(vas_close_wq);
}

/*
 * Where a receive window keeps what was pasted to it, for the api layer to map
 * into the process that opened it. NULL for a window that is only woken.
 */
static void *vas_user_win_rx_fifo(struct vas_window *win, u32 *len)
{
	struct pnv_vas_window *window =
		container_of(win, struct pnv_vas_window, vas_win);

	if (!window->rx_fifo_buf)
		return NULL;

	*len = window->rx_fifo_len;

	return window->rx_fifo_buf;
}

static const struct vas_user_win_ops vops =  {
	.open_win	=	vas_user_win_open,
	.paste_addr	=	vas_user_win_paste_addr,
	.close_win	=	vas_user_win_close,
	.drain_closes	=	vas_user_win_drain_closes,
	.domain		=	vas_user_win_domain,
	.open_rx_win	=	vas_user_rx_win_open,
	.rx_fifo	=	vas_user_win_rx_fifo,
};

int __init vas_user_win_ops_register(void)
{
	return vas_register_backend(VAS_BACKEND_POWERNV, &vops);
}
