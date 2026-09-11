/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2016-17 IBM Corp.
 */

#ifndef _ASM_POWERPC_VAS_H
#define _ASM_POWERPC_VAS_H
#include <linux/sched/mm.h>
#include <linux/mmu_context.h>
#include <asm/icswx.h>
#include <uapi/asm/vas-api.h>

/*
 * Min and max FIFO sizes are based on Version 1.05 Section 3.1.4.25
 * (Local FIFO Size Register) of the VAS workbook.
 */
#define VAS_RX_FIFO_SIZE_MIN	(1 << 10)	/* 1KB */
#define VAS_RX_FIFO_SIZE_MAX	(8 << 20)	/* 8MB */

/*
 * Threshold Control Mode: Have paste operation fail if the number of
 * requests in receive FIFO exceeds a threshold.
 *
 * NOTE: No special error code yet if paste is rejected because of these
 *	 limits. So users can't distinguish between this and other errors.
 */
#define VAS_THRESH_DISABLED		0
#define VAS_THRESH_FIFO_GT_HALF_FULL	1
#define VAS_THRESH_FIFO_GT_QTR_FULL	2
#define VAS_THRESH_FIFO_GT_EIGHTH_FULL	3

/*
 * VAS window Linux status bits
 */
#define VAS_WIN_ACTIVE		0x0	/* Used in platform independent */
					/* vas mmap() */
/* Window is closed in the hypervisor due to lost credit */
#define VAS_WIN_NO_CRED_CLOSE	0x00000001
/* Window is closed due to migration */
#define VAS_WIN_MIGRATE_CLOSE	0x00000002
/* Hypervisor refused to deallocate; window and refs retained, off the lists */
#define VAS_WIN_HV_RETAINED	0x00000004

/*
 * Get/Set bit fields
 */
#define GET_FIELD(m, v)                (((v) & (m)) >> MASK_LSH(m))
#define MASK_LSH(m)            (__builtin_ffsl(m) - 1)
#define SET_FIELD(m, v, val)   \
		(((v) & ~(m)) | ((((typeof(v))(val)) << MASK_LSH(m)) & (m)))

/*
 * Co-processor Engine type.
 */
enum vas_cop_type {
	VAS_COP_TYPE_FAULT,
	VAS_COP_TYPE_842,
	VAS_COP_TYPE_842_HIPRI,
	VAS_COP_TYPE_GZIP,
	VAS_COP_TYPE_GZIP_HIPRI,
	VAS_COP_TYPE_SYM,
	VAS_COP_TYPE_SYM_HIPRI,
	VAS_COP_TYPE_FTW,
	VAS_COP_TYPE_MAX,
};

/*
 * User space VAS windows are opened by tasks and take references
 * to pid and mm until windows are closed.
 * Stores pid, mm, and tgid for each window.
 */
struct misc_cg;
struct nmmu_view;

struct vas_user_win_ref {
	struct pid *pid;	/* PID of owner */
	struct pid *tgid;	/* Thread group ID of owner */
	struct mm_struct *mm;	/* Linux process mm_struct */
	struct mutex mmap_mutex;	/* protects paste address mmap() */
					/* with DLPAR close/open windows */
	struct vm_area_struct *vma;	/* Save VMA and used in DLPAR ops */
	struct misc_cg *misc_cg;	/* cgroup the window is charged to */
	bool qos_win;			/* charged as a QoS window */
	u64 amr;			/* opener's AMR, for the CSB write */
	struct nmmu_view *nmmu_view;	/* the window's own view, or NULL */
};

/*
 * Common VAS window struct on PowerNV and PowerVM
 */
struct vas_window {
	u32 winid;
	u32 wcreds_max;	/* Window credits */
	u32 status;	/* Window status used in OS */
	enum vas_cop_type cop;
	struct vas_user_win_ref task_ref;
	char *dbgname;
	struct dentry *dbgdir;
};

/*
 * What a type publishes about itself, as attributes of its node's device in
 * sysfs. A zero limit means none is configured: the engine takes any length
 * its request format can express.
 */
struct vas_user_caps {
	u64 req_max_processed_len;	/* bytes one request may process */
};

/*
 * Which switchboard instance a window is opened on. There is one per chip,
 * and a caller that does not choose asks for the one local to the running
 * thread rather than naming a number.
 */
#define VAS_INSTANCE_ANY	(-1)

static inline bool vas_instance_is_any(int vasid)
{
	return vasid == VAS_INSTANCE_ANY;
}

/*
 * A send window that delivers to another window rather than to an engine
 * names its receive window by that window's packed switchboard and window
 * id. No such id names no such window, which is the case for every window
 * bound to an engine's own receive window.
 */
#define VAS_PSWID_NONE		0

static inline bool vas_pswid_names_window(int pswid)
{
	return pswid != VAS_PSWID_NONE;
}

/*
 * Who runs a window's requests. Two of these have always existed: a machine
 * was PowerNV or it was pseries, and installed one set of window operations
 * to suit. What is new is more than one being registered at once, so that a
 * node names the backend its windows are opened against instead of the
 * machine deciding for every window on it.
 */
enum vas_backend {
	/* Whichever the machine chose. The node the plain name points at. */
	VAS_BACKEND_DEFAULT = 0,
	VAS_BACKEND_POWERNV = 1,
	VAS_BACKEND_POWERVM = 2,
	/* The kernel runs the request itself, in software. */
	VAS_BACKEND_KERNEL = 3,
	VAS_BACKEND_MAX,
};

const char *vas_backend_name(enum vas_backend backend);

/*
 * A coprocessor type may offer more than one node, so that the interface a
 * node presents can be fixed for the life of that node: an engine keeps the
 * node and the semantics user space already has, and gains what comes later
 * on a node of its own.
 */
enum vas_node_variant {
	/* The name and the interface user space had before this kernel. */
	VAS_NODE_LEGACY = 0,
	/* This platform's node, where later features are offered. */
	VAS_NODE_PLATFORM = 1,
	VAS_NODE_VARIANT_MAX,
};

/*
 * The minor number is split like an address under a prefix length: the
 * coprocessor type selects a block, the interface variant a sub-block, and
 * the backend a place within that. A node's minor is fixed by what the node
 * is, so adding a node cannot renumber another, every block stays whole
 * whether or not it is fully occupied, and the number itself says which
 * engine, which interface and which backend a descriptor reached.
 */
#define VAS_MINOR_BACKEND_BITS	4
#define VAS_MINOR_VARIANT_BITS	4
#define VAS_MINOR_NODE_BITS	(VAS_MINOR_VARIANT_BITS + VAS_MINOR_BACKEND_BITS)
#define VAS_MINOR_COUNT		((unsigned int)VAS_COP_TYPE_MAX << VAS_MINOR_NODE_BITS)

static inline unsigned int vas_node_minor(enum vas_cop_type cop,
					  enum vas_node_variant variant,
					  enum vas_backend backend)
{
	return ((unsigned int)cop << VAS_MINOR_NODE_BITS) |
	       ((unsigned int)variant << VAS_MINOR_BACKEND_BITS) |
	       (unsigned int)backend;
}

/*
 * One node user space may open windows through. A driver registers each type
 * it has a receive window for, and may register more than one node for a
 * type; the user window driver creates /dev/<dir>/<name>, names the node's
 * class after it (udev rules match on it), publishes caps under the node's
 * device, and binds every window opened through the node to the type.
 */
struct vas_user_type {
	const char *name;
	const char *dir;
	enum vas_cop_type cop_type;
	enum vas_node_variant variant;
	enum vas_backend backend;
	const struct vas_user_caps *caps;	/* optional */
};

/*
 * What the user window driver asks the platform to open: the attribute as
 * validated, and the key mask the window translates under. The platform
 * gives the mask to the hardware or the hypervisor; the kernel's own write
 * of the status block obeys the same mask.
 */
struct vas_user_win_req {
	int vas_id;
	u64 flags;
	enum vas_cop_type cop_type;
	u64 amr;
	/*
	 * The receive window a send window is to deliver to, if any. Resolved
	 * from the caller's descriptor before the platform sees it, so a
	 * platform is never handed a window the caller could not reach.
	 */
	struct vas_window *target;
};

/*
 * The running platform's user window operations, installed once at its init.
 */
struct vas_user_win_ops {
	struct vas_window * (*open_win)(const struct vas_user_win_req *req);
	u64 (*paste_addr)(struct vas_window *);
	int (*close_win)(struct vas_window *);
	/* Optional: finish every deferred close once the last type is gone. */
	void (*drain_closes)(void);
	/* Optional: add or drop a domain of a window opened with domains. */
	int (*domain)(struct vas_window *, u64 start, u64 len, bool add);
	/*
	 * Optional: a receive window for the calling thread, which a send
	 * window may be pointed at. The descriptor it is opened on is what
	 * names it to a sender, so nothing is returned but the window itself.
	 * Closed through ->close_win() like any other window.
	 */
	struct vas_window *(*open_rx_win)(const struct vas_user_win_req *req);
};

void put_vas_user_win_ref(struct vas_user_win_ref *ref);

static inline void vas_user_win_add_mm_context(struct vas_user_win_ref *ref)
{
	mm_context_add_vas_window(ref->mm);
	/*
	 * Even a process that has no foreign real address mapping can
	 * use an unpaired COPY instruction (to no real effect). Issue
	 * CP_ABORT to clear any pending COPY and prevent a covert
	 * channel.
	 *
	 * __switch_to() will issue CP_ABORT on future context switches
	 * if process / thread has any open VAS window (Use
	 * current->mm->context.vas_windows).
	 */
	asm volatile(PPC_CP_ABORT);
}

/*
 * Receive window attributes specified by the (in-kernel) owner of window.
 */
struct vas_rx_win_attr {
	u64 rx_fifo;
	int rx_fifo_size;
	int wcreds_max;

	bool pin_win;
	bool rej_no_credit;
	bool tx_wcred_mode;
	bool rx_wcred_mode;
	bool tx_win_ord_mode;
	bool rx_win_ord_mode;
	bool data_stamp;
	bool nx_win;
	bool fault_win;
	bool user_win;
	bool notify_disable;
	bool intr_disable;
	bool notify_early;

	int lnotify_lpid;
	int lnotify_pid;
	int lnotify_tid;
	u32 pswid;

	int tc_mode;
};

/*
 * Window attributes specified by the in-kernel owner of a send window.
 */
struct vas_tx_win_attr {
	enum vas_cop_type cop;
	int wcreds_max;
	int lpid;
	int pidr;		/* hardware PID (from SPRN_PID) */
	/* The receive window a wake is delivered to, instead of an engine. */
	struct vas_window *target;
	int rsvd_txbuf_count;
	int tc_mode;

	bool user_win;
	bool pin_win;
	bool rej_no_credit;
	bool rsvd_txbuf_enable;
	bool tx_wcred_mode;
	bool rx_wcred_mode;
	bool tx_win_ord_mode;
	bool rx_win_ord_mode;
	u64 amr;		/* user windows: the mask settled at open */
	struct nmmu_view *nmmu_view;	/* user windows: a view of their own */
};

#ifdef CONFIG_PPC_POWERNV
/*
 * Helper to map a chip id to VAS id.
 * For POWER9, this is a 1:1 mapping. In the future this maybe a 1:N
 * mapping in which case, we will need to update this helper.
 *
 * Return the VAS id or -1 if no matching vasid is found.
 */
int chip_to_vas_id(int chipid);

/*
 * Helper to initialize receive window attributes to defaults for an
 * NX window.
 */
void vas_init_rx_win_attr(struct vas_rx_win_attr *rxattr, enum vas_cop_type cop);

/*
 * Open a VAS receive window for the instance of VAS identified by @vasid
 * Use @attr to initialize the attributes of the window.
 *
 * Return a handle to the window or ERR_PTR() on error.
 */
struct vas_window *vas_rx_win_open(int vasid, enum vas_cop_type cop,
				   struct vas_rx_win_attr *attr);

/*
 * Helper to initialize send window attributes to defaults for an NX window.
 */
extern void vas_init_tx_win_attr(struct vas_tx_win_attr *txattr,
			enum vas_cop_type cop);

/*
 * Open a VAS send window for the instance of VAS identified by @vasid
 * and the co-processor type @cop. Use @attr to initialize attributes
 * of the window.
 *
 * Note: The instance of VAS must already have an open receive window for
 * the coprocessor type @cop.
 *
 * Return a handle to the send window or ERR_PTR() on error.
 */
struct vas_window *vas_tx_win_open(int vasid, enum vas_cop_type cop,
			struct vas_tx_win_attr *attr);

/*
 * Close the send or receive window identified by @win. For receive windows
 * return -EAGAIN if there are active send windows attached to this receive
 * window.
 */
int vas_win_close(struct vas_window *win);

/*
 * Copy the co-processor request block (CRB) @crb into the local L2 cache.
 */
int vas_copy_crb(void *crb, int offset);

/*
 * Paste a previously copied CRB (see vas_copy_crb()) from the L2 cache to
 * the hardware address associated with the window @win. @re is expected/
 * assumed to be true for NX windows.
 */
int vas_paste_crb(struct vas_window *win, int offset, bool re);
#endif

#ifdef CONFIG_PPC_PSERIES

/* VAS Capabilities */
#define VAS_GZIP_QOS_FEAT	0x1
#define VAS_GZIP_DEF_FEAT	0x2
#define VAS_GZIP_QOS_FEAT_BIT	PPC_BIT(VAS_GZIP_QOS_FEAT) /* Bit 1 */
#define VAS_GZIP_DEF_FEAT_BIT	PPC_BIT(VAS_GZIP_DEF_FEAT) /* Bit 2 */

/* NX Capabilities */
#define VAS_NX_GZIP_FEAT	0x1
#define VAS_NX_GZIP_FEAT_BIT	PPC_BIT(VAS_NX_GZIP_FEAT) /* Bit 1 */

/*
 * These structs are used to retrieve overall VAS capabilities that
 * the hypervisor provides.
 */
struct hv_vas_all_caps {
	__be64  descriptor;
	__be64  feat_type;
} __packed __aligned(0x1000);

struct vas_all_caps {
	u64     descriptor;
	u64     feat_type;
};

int h_query_vas_capabilities(const u64 hcall, u8 query_type, u64 result);
#endif

/*
 * The VAS API exported to user space: a window per open descriptor, through
 * which requests go to a coprocessor directly. The platform installs its
 * window operations once at init; a driver registers each type it has a
 * receive window for, and names no platform. What limits the set of types
 * is that a type needs a receive window before a send window can attach to
 * one.
 */
int vas_register_backend(enum vas_backend backend,
			 const struct vas_user_win_ops *ops);
int vas_user_type_register(struct module *mod,
			   const struct vas_user_type *type);
void vas_user_type_unregister(const struct vas_user_type *type);

int get_vas_user_win_ref(struct vas_user_win_ref *task_ref, u64 flags,
			 u64 amr);
void vas_update_csb(struct coprocessor_request_block *crb,
		    struct vas_user_win_ref *task_ref, u8 cc);
void vas_dump_crb(struct coprocessor_request_block *crb);

/*
 * Fault path outcomes. Indexed by enumerator rather than held in named
 * fields so the debugfs output cannot drift from the counters: adding an
 * outcome is one enumerator and one string, and vas_stats_show() walks
 * the array.
 */
/*
 * What the fault and completion paths count. Two groups partition their
 * denominator exactly: every fixup ends in one of the outcomes below it, and
 * every walk ends one of three ways. A reader can therefore check the sums,
 * and vas_stats_selftest() does. The rest are observations, counted where
 * they happen and in their own units.
 *
 * The name of each is a path, and it is the path the counter appears under
 * in debugfs as well as the name in the combined file.
 */
enum vas_stat_item {
	/* fault FIFO */
	VAS_STAT_FAULT_CRBS,		/* CRBs taken from the fault FIFO */
	VAS_STAT_FAULT_BAD_PSWID,	/* CRB named a window we cannot find */

	/* fixups entered: the denominator of the outcomes below */
	VAS_STAT_FIXUP,

	/* exactly one of these per fixup */
	VAS_STAT_FIXUP_MM_GONE,		/* address space already torn down */
	VAS_STAT_FIXUP_NOT_USER_EA,	/* fault address outside user region */
	VAS_STAT_FIXUP_REFUSED_DOMAIN,	/* outside the window's domains */
	VAS_STAT_FIXUP_REFUSED_LOAD,	/* a right the mapping does not grant */
	VAS_STAT_FIXUP_REFUSED_STORE,	/* likewise, for a store */
	VAS_STAT_FIXUP_WALKED,		/* reached the page walk */

	/* exactly one of these per walk */
	VAS_STAT_WALK_COMPLETED,	/* walked the whole run */
	VAS_STAT_WALK_BUDGET,		/* cut short by the page budget */
	VAS_STAT_WALK_PAGE_ERR,		/* cut short: a page could not be faulted */

	/* pages, inside walks; not outcomes and not one per fixup */
	VAS_STAT_PAGES_FAULTED,		/* pages faulted in */
	VAS_STAT_PAGES_HASH_ERR,	/* hash table would not take a page */
	VAS_STAT_PAGES_HASH_NOINSERT,	/* hash walk found nothing to insert */
	VAS_STAT_PAGES_STE_ERR,		/* no segment table entry inserted */

	/* what the hardware stamped against what the kernel found */
	VAS_STAT_STAMP_DIR_DISAGREE,	/* stamp and descriptor differ on direction */
	VAS_STAT_STAMP_PROT_DISAGREE,	/* stamp says refused, mapping does not */
	VAS_STAT_STAMP_UNKNOWN,		/* stamp status is none we know */

	/* completion writes entered: the denominator of the outcomes below */
	VAS_STAT_CSB,

	/* exactly one of these per completion write */
	VAS_STAT_CSB_WRITTEN,		/* the block reached the requester */
	VAS_STAT_CSB_TASK_GONE,		/* task exiting or already gone */
	VAS_STAT_CSB_MM_REPLACED,	/* task exec'd; not its address space */
	VAS_STAT_CSB_PKEY_DENIED,	/* opener's AMR denies the CSB page */
	VAS_STAT_CSB_COPY_FAIL,		/* copy_to_user() of the CSB failed */

	/* window close */
	VAS_STAT_WIN_RETAINED,		/* closes that timed out, resources held */

	VAS_STAT_NR,
};

/*
 * Long, because these only ever rise and a signed 32-bit count of pages
 * wraps into negative numbers on a machine left running.
 */
extern atomic_long_t vas_stats[VAS_STAT_NR];
extern const char * const vas_stat_names[VAS_STAT_NR];

static inline void vas_stat_inc(enum vas_stat_item item)
{
	atomic_long_inc(&vas_stats[item]);
}

static inline void vas_stat_add(enum vas_stat_item item, long n)
{
	atomic_long_add(n, &vas_stats[item]);
}

struct seq_file;
void vas_stats_show(struct seq_file *s);
#endif /* __ASM_POWERPC_VAS_H */
