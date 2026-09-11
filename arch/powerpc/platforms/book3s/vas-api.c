// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VAS user space API for its accelerators
 * Copyright (C) 2019 Haren Myneni, IBM Corp
 */

#define pr_fmt(fmt)	"vas-api: " fmt

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/cdev.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/misc_cgroup.h>
#include <linux/pkeys.h>
#include <linux/seq_file.h>
#include <linux/sched/signal.h>
#include <linux/mmu_context.h>
#include <linux/io.h>
#include <asm/vas.h>
#include <uapi/asm/vas-api.h>

/*
 * The driver creates the device node that can be used as follows:
 * For NX-GZIP
 *
 *	fd = open("/dev/crypto/nx-gzip", O_RDWR);
 *	rc = ioctl(fd, VAS_TX_WIN_OPEN, &attr);
 *	paste_addr = mmap(NULL, PAGE_SIZE, prot, MAP_SHARED, fd, 0ULL).
 *	vas_copy(&crb, 0, 1);
 *	vas_paste(paste_addr, 0, 1);
 *	close(fd) or exit process to close window.
 *
 * where "vas_copy" and "vas_paste" are defined in copy-paste.h.
 * copy/paste returns to the user space directly. So refer NX hardware
 * documentation for exact copy/paste usage and completion / error
 * conditions.
 */

/*
 * One coprocessor type registered with the user window driver: its
 * description, a minor of the character major all types share, its own
 * class, the platform's window operations, and its own copy of the file
 * operations so that an open descriptor pins the module that registered
 * this type and no other. coproc_open() finds it from the cdev it was opened
 * through.
 */
struct coproc_dev {
	struct cdev cdev;
	struct device *device;
	dev_t devt;
	struct class *class;
	const struct vas_user_type *type;
	const struct vas_user_win_ops *vops;
	struct file_operations fops;
	struct list_head node;
};

static LIST_HEAD(coproc_devices);
static DEFINE_MUTEX(coproc_devices_lock);
/* The shared major, allocated with the first type and released with the last. */
static dev_t coproc_devt;
/* One set of window operations per backend; NULL until it registers. */
static const struct vas_user_win_ops *coproc_backends[VAS_BACKEND_MAX];

/*
 * The backend a node asking for VAS_BACKEND_DEFAULT is given. Set by the
 * first backend to register, which on a machine with only one is the only
 * answer; vas_backend= on the command line overrides that, for bringing a
 * machine up on a backend other than its own.
 */
static enum vas_backend coproc_default_backend = VAS_BACKEND_DEFAULT;
static enum vas_backend coproc_wanted_backend = VAS_BACKEND_DEFAULT;

static const char * const coproc_backend_names[VAS_BACKEND_MAX] = {
	[VAS_BACKEND_DEFAULT]	= "default",
	[VAS_BACKEND_POWERNV]	= "powernv",
	[VAS_BACKEND_POWERVM]	= "powervm",
	[VAS_BACKEND_KERNEL]	= "kernel",
};

const char *vas_backend_name(enum vas_backend backend)
{
	if (backend >= VAS_BACKEND_MAX || !coproc_backend_names[backend])
		return "unknown";

	return coproc_backend_names[backend];
}

static int __init vas_backend_setup(char *str)
{
	int i;

	for (i = VAS_BACKEND_DEFAULT + 1; i < VAS_BACKEND_MAX; i++) {
		if (coproc_backend_names[i] && !strcmp(str, coproc_backend_names[i])) {
			coproc_wanted_backend = i;
			return 1;
		}
	}

	pr_warn("vas_backend=%s is not a backend this kernel has\n", str);

	return 1;
}
early_param("vas_backend", vas_backend_setup);

/* The operations a node's windows are opened against. */
static const struct vas_user_win_ops *coproc_dev_ops(const struct coproc_dev *dev)
{
	enum vas_backend backend = dev->type->backend;

	if (backend == VAS_BACKEND_DEFAULT)
		backend = coproc_default_backend;

	if (backend >= VAS_BACKEND_MAX)
		return NULL;

	return coproc_backends[backend];
}

struct coproc_instance {
	struct coproc_dev *coproc;
	struct vas_window *txwin;
	/*
	 * A descriptor holds one window, and which kind it is depends on the
	 * ioctl the caller used: a send window to paste to, or a receive
	 * window that makes this thread somewhere another window can send.
	 */
	struct vas_window *rxwin;
	/*
	 * The descriptor whose receive window this send window delivers to,
	 * held so that window outlives every window pointed at it: the target
	 * is named in hardware by a window id, which must not be reissued
	 * while a sender still carries it.
	 */
	struct file *target;
	/*
	 * Serialises the open ioctl against itself. One descriptor may be
	 * used by several threads, and the one-window-per-descriptor rule is
	 * enforced by a test on txwin that is otherwise separated from the
	 * assignment by the whole of open_win().
	 */
	struct mutex mutex;
};

static char *coproc_devnode(const struct device *dev, umode_t *mode)
{
	const struct coproc_dev *coproc = dev_get_drvdata(dev);

	return kasprintf(GFP_KERNEL, "%s/%s", coproc->type->dir, dev_name(dev));
}

static ssize_t cop_type_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	const struct coproc_dev *coproc = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", coproc->type->cop_type);
}
static DEVICE_ATTR_RO(cop_type);

static ssize_t req_max_processed_len_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	const struct coproc_dev *coproc = dev_get_drvdata(dev);
	const struct vas_user_caps *caps = coproc->type->caps;

	return sysfs_emit(buf, "%llu\n", caps ? caps->req_max_processed_len : 0);
}
static DEVICE_ATTR_RO(req_max_processed_len);

static struct attribute *coproc_dev_attrs[] = {
	&dev_attr_cop_type.attr,
	&dev_attr_req_max_processed_len.attr,
	NULL,
};
ATTRIBUTE_GROUPS(coproc_dev);

atomic_long_t vas_stats[VAS_STAT_NR];

/*
 * The name of a counter is its path: the combined file prints it as it
 * stands, and the debugfs tree makes a directory of each component.
 */
const char * const vas_stat_names[VAS_STAT_NR] = {
	[VAS_STAT_FAULT_CRBS]		= "faults/crbs",
	[VAS_STAT_FAULT_BAD_PSWID]	= "faults/bad_pswid",

	[VAS_STAT_FIXUP]		= "faults/fixup/attempted",
	[VAS_STAT_FIXUP_MM_GONE]	= "faults/fixup/outcome/mm_gone",
	[VAS_STAT_FIXUP_NOT_USER_EA]	= "faults/fixup/outcome/not_user_ea",
	[VAS_STAT_FIXUP_REFUSED_DOMAIN]	= "faults/fixup/outcome/refused_domain",
	[VAS_STAT_FIXUP_REFUSED_LOAD]	= "faults/fixup/outcome/refused_load",
	[VAS_STAT_FIXUP_REFUSED_STORE]	= "faults/fixup/outcome/refused_store",
	[VAS_STAT_FIXUP_WALKED]		= "faults/fixup/outcome/walked",

	[VAS_STAT_WALK_COMPLETED]	= "faults/walk/completed",
	[VAS_STAT_WALK_BUDGET]		= "faults/walk/budget",
	[VAS_STAT_WALK_PAGE_ERR]	= "faults/walk/page_err",

	[VAS_STAT_PAGES_FAULTED]	= "faults/pages/faulted",
	[VAS_STAT_PAGES_HASH_ERR]	= "faults/pages/hash_err",
	[VAS_STAT_PAGES_HASH_NOINSERT]	= "faults/pages/hash_noinsert",
	[VAS_STAT_PAGES_STE_ERR]	= "faults/pages/ste_err",

	[VAS_STAT_STAMP_DIR_DISAGREE]	= "faults/stamp/direction_disagree",
	[VAS_STAT_STAMP_PROT_DISAGREE]	= "faults/stamp/protection_disagree",
	[VAS_STAT_STAMP_UNKNOWN]	= "faults/stamp/unknown",

	[VAS_STAT_CSB]			= "completions/attempted",
	[VAS_STAT_CSB_WRITTEN]		= "completions/outcome/written",
	[VAS_STAT_CSB_TASK_GONE]	= "completions/outcome/task_gone",
	[VAS_STAT_CSB_MM_REPLACED]	= "completions/outcome/mm_replaced",
	[VAS_STAT_CSB_PKEY_DENIED]	= "completions/outcome/pkey_denied",
	[VAS_STAT_CSB_COPY_FAIL]	= "completions/outcome/copy_fail",

	[VAS_STAT_WIN_RETAINED]		= "windows/retained",
};

/*
 * The two groups that partition their denominator. A reader may check that
 * the parts sum to the whole; so may a test.
 */
static const enum vas_stat_item vas_fixup_outcomes[] = {
	VAS_STAT_FIXUP_MM_GONE,
	VAS_STAT_FIXUP_NOT_USER_EA,
	VAS_STAT_FIXUP_REFUSED_DOMAIN,
	VAS_STAT_FIXUP_REFUSED_LOAD,
	VAS_STAT_FIXUP_REFUSED_STORE,
	VAS_STAT_FIXUP_WALKED,
};

static const enum vas_stat_item vas_walk_outcomes[] = {
	VAS_STAT_WALK_COMPLETED,
	VAS_STAT_WALK_BUDGET,
	VAS_STAT_WALK_PAGE_ERR,
};

static const enum vas_stat_item vas_csb_outcomes[] = {
	VAS_STAT_CSB_WRITTEN,
	VAS_STAT_CSB_TASK_GONE,
	VAS_STAT_CSB_MM_REPLACED,
	VAS_STAT_CSB_PKEY_DENIED,
	VAS_STAT_CSB_COPY_FAIL,
};

static long vas_stat_sum(const enum vas_stat_item *items, size_t n)
{
	long total = 0;
	size_t i;

	for (i = 0; i < n; i++)
		total += atomic_long_read(&vas_stats[items[i]]);

	return total;
}

void vas_stats_show(struct seq_file *s)
{
	int i;

	for (i = 0; i < VAS_STAT_NR; i++)
		seq_printf(s, "%-40s %ld\n", vas_stat_names[i],
			   atomic_long_read(&vas_stats[i]));

	/*
	 * Printed rather than merely assertable, because a reader looking at
	 * a live machine wants to know the parts still account for the whole
	 * before drawing anything from them. A non-zero difference means a
	 * path returns without counting how it ended.
	 */
	seq_printf(s, "\n%-40s %ld\n", "faults/fixup/unaccounted",
		   atomic_long_read(&vas_stats[VAS_STAT_FIXUP]) -
		   vas_stat_sum(vas_fixup_outcomes, ARRAY_SIZE(vas_fixup_outcomes)));
	seq_printf(s, "%-40s %ld\n", "faults/walk/unaccounted",
		   atomic_long_read(&vas_stats[VAS_STAT_FIXUP_WALKED]) -
		   vas_stat_sum(vas_walk_outcomes, ARRAY_SIZE(vas_walk_outcomes)));
	seq_printf(s, "%-40s %ld\n", "completions/unaccounted",
		   atomic_long_read(&vas_stats[VAS_STAT_CSB]) -
		   vas_stat_sum(vas_csb_outcomes, ARRAY_SIZE(vas_csb_outcomes)));
}

/*
 * Take reference to pid and mm
 */
static enum misc_res_type vas_win_misc_res(struct vas_user_win_ref *ref)
{
	return ref->qos_win ? MISC_CG_RES_VAS_WIN_QOS : MISC_CG_RES_VAS_WIN;
}

int get_vas_user_win_ref(struct vas_user_win_ref *task_ref, u64 flags,
			 u64 amr)
{
	int rc;

	/*
	 * Window opened by a child thread may not be closed when
	 * it exits. So take reference to its pid and release it
	 * when the window is free by parent thread.
	 * Acquire a reference to the task's pid to make sure
	 * pid will not be re-used - needed only for multithread
	 * applications.
	 */
	/*
	 * Initialised here and not by the ioctl that opened the window,
	 * because by the time open_win() returns, the window is published:
	 * the pseries DLPAR walker can already be holding this mutex when
	 * the ioctl would have re-initialised it under the walker's feet. A
	 * platform takes the references before it publishes, so this is the
	 * one place that is early enough on both.
	 */
	mutex_init(&task_ref->mmap_mutex);

	/*
	 * The window is a charge against the cgroup that opens it, one unit
	 * per window, on the pool the caller chose. The cgroup is recorded
	 * in the window and the uncharge goes to the recorded cgroup, not
	 * the closer's: a descriptor can outlive the opener or be passed to
	 * another process, and the charge has to stay where the documented
	 * ownership rule puts it -- with whoever used the resource first --
	 * until the window really is gone. Same pattern as SEV ASIDs.
	 */
	task_ref->qos_win = !!(flags & VAS_TX_WIN_FLAG_QOS_CREDIT);
	rc = misc_cg_charge_current(vas_win_misc_res(task_ref),
				    &task_ref->misc_cg, 1);
	if (rc)
		return rc;

	task_ref->amr = amr;
	task_ref->pid = get_task_pid(current, PIDTYPE_PID);
	/*
	 * Acquire a reference to the task's mm.
	 */
	task_ref->mm = get_task_mm(current);
	if (!task_ref->mm) {
		put_pid(task_ref->pid);
		task_ref->pid = NULL;
		misc_cg_uncharge_put(vas_win_misc_res(task_ref),
				     &task_ref->misc_cg, 1);
		pr_debug("%s[%d]: no address space to attach a window to\n",
			 current->comm, current->pid);
		return -EPERM;
	}

	mmgrab(task_ref->mm);
	mmput(task_ref->mm);
	/*
	 * Process closes window during exit. In the case of
	 * multithread application, the child thread can open
	 * window and can exit without closing it. So takes tgid
	 * reference until window closed to make sure tgid is not
	 * reused.
	 */
	task_ref->tgid = find_get_pid(task_tgid_vnr(current));

	return 0;
}

void put_vas_user_win_ref(struct vas_user_win_ref *ref)
{
	/* The hardware is done with the window; its view can go before the mm. */
	if (ref->nmmu_view) {
		hash__nmmu_view_free(ref->nmmu_view);
		ref->nmmu_view = NULL;
	}
	/*
	 * Everything dropped is also cleared, so a struct that has been
	 * through here holds no half-dead pointers: either a field is live
	 * or it is NULL, and a second call is a no-op rather than a
	 * double-put.
	 */
	put_pid(ref->pid);
	ref->pid = NULL;
	put_pid(ref->tgid);
	ref->tgid = NULL;
	if (ref->mm) {
		mmdrop(ref->mm);
		ref->mm = NULL;
	}

	misc_cg_uncharge_put(vas_win_misc_res(ref), &ref->misc_cg, 1);
}

/*
 * Successful return must release the task reference with
 * put_task_struct
 */
static bool ref_get_pid_and_task(struct vas_user_win_ref *task_ref,
			  struct task_struct **tskp, struct pid **pidp)
{
	struct task_struct *tsk;
	struct pid *pid;

	pid = task_ref->pid;
	tsk = get_pid_task(pid, PIDTYPE_PID);
	if (!tsk) {
		pid = task_ref->tgid;
		tsk = get_pid_task(pid, PIDTYPE_PID);
		/*
		 * Both are gone: the process exited with requests of its own
		 * still in the fault window. There is no one left to report
		 * to, which is a state userspace can reach at will and not an
		 * assertion to make.
		 */
		if (!tsk)
			return false;
	}

	/* Return if the task is exiting. */
	if (tsk->flags & PF_EXITING) {
		put_task_struct(tsk);
		return false;
	}

	*tskp = tsk;
	*pidp = pid;

	return true;
}

/*
 * Does the requester's own AMR allow a write to the page holding its CSB?
 *
 * arch_vma_access_permitted() cannot answer this: it declines to enforce
 * keys on a foreign vma, and it reads the running thread's AMR, which here
 * belongs to whichever thread is draining the fault window.
 */
#ifdef CONFIG_PPC_MEM_KEYS
static bool csb_write_permitted(struct mm_struct *mm, void __user *addr,
				u64 amr, int *pkey)
{
	struct vm_area_struct *vma;
	bool ok = true;

	*pkey = 0;
	mmap_read_lock(mm);
	vma = find_vma(mm, (unsigned long)addr);
	if (vma && (unsigned long)addr >= vma->vm_start) {
		*pkey = vma_pkey(vma);
		ok = pkey_amr_access_permitted(amr, *pkey, true);
	}
	mmap_read_unlock(mm);

	return ok;
}
#else
static bool csb_write_permitted(struct mm_struct *mm, void __user *addr,
				u64 amr, int *pkey)
{
	*pkey = 0;
	return true;
}
#endif

/*
 * Update the CSB to indicate a translation error.
 *
 * User space will be polling on CSB after the request is issued.
 * If NX can handle the request without any issues, it updates CSB.
 * Whereas if NX encounters page fault, the kernel will handle the
 * fault and update CSB with translation error.
 *
 * If we are unable to update the CSB means copy_to_user failed due to
 * invalid csb_addr, send a signal to the process.
 */
void vas_update_csb(struct coprocessor_request_block *crb,
		    struct vas_user_win_ref *task_ref, u8 cc)
{
	struct coprocessor_status_block csb;
	struct kernel_siginfo info;
	struct task_struct *tsk;
	void __user *csb_addr;
	struct mm_struct *mm;
	struct pid *pid;
	int pkey, rc;

	/*
	 * NX user space windows can not be opened for task->mm=NULL
	 * and faults will not be generated for kernel requests.
	 */
	if (WARN_ON_ONCE(!task_ref->mm))
		return;

	csb_addr = (void __user *)be64_to_cpu(crb->csb_addr);

	memset(&csb, 0, sizeof(csb));
	csb.cc = cc;
	csb.ce = CSB_CE_TERMINATION;
	csb.cs = 0;
	csb.count = 0;

	/*
	 * NX operates and returns in BE format as defined CRB struct.
	 * So saves fault_storage_addr in BE as NX pastes in FIFO and
	 * expects user space to convert to CPU format.
	 */
	csb.address = crb->stamp.nx.fault_storage_addr;
	csb.flags = 0;

	/*
	 * Process closes send window after all pending NX requests are
	 * completed. In multi-thread applications, a child thread can
	 * open a window and can exit without closing it. May be some
	 * requests are pending or this window can be used by other
	 * threads later. We should handle faults if NX encounters
	 * pages faults on these requests. Update CSB with translation
	 * error and fault address. If csb_addr passed by user space is
	 * invalid, send SEGV signal to pid saved in window. If the
	 * child thread is not running, send the signal to tgid.
	 * Parent thread (tgid) will close this window upon its exit.
	 *
	 * pid and mm references are taken when window is opened by
	 * process (pid). So tgid is used only when child thread opens
	 * a window and exits without closing it.
	 */

	vas_stat_inc(VAS_STAT_CSB);

	if (!ref_get_pid_and_task(task_ref, &tsk, &pid)) {
		vas_stat_inc(VAS_STAT_CSB_TASK_GONE);
		return;
	}

	/*
	 * The window pins the mm with mmgrab(), which keeps the struct but
	 * not the address space: exit_mmap() runs once the last user
	 * reference goes. get_task_mm() takes a user reference and reads
	 * the task's current mm, so a mismatch means the task exec'd or
	 * exited and csb_addr names an address space that is gone.
	 * Threads share an mm, so the tgid fallback above still matches.
	 */
	mm = get_task_mm(tsk);
	if (mm != task_ref->mm) {
		vas_stat_inc(VAS_STAT_CSB_MM_REPLACED);
		if (mm)
			mmput(mm);
		put_task_struct(tsk);
		return;
	}

	/*
	 * The copy below runs with the kernel's AMR, which grants every key.
	 * The address is one the requester chose, so the requester's keys
	 * decide whether it may be written -- not the kernel's, and not those
	 * of whatever thread happens to be draining the fault window.
	 */
	if (!csb_write_permitted(mm, csb_addr, task_ref->amr, &pkey)) {
		vas_stat_inc(VAS_STAT_CSB_PKEY_DENIED);
		mmput(mm);
		/*
		 * The block the process polls for its completion is the one
		 * its own keys forbid this write to, so the answer cannot go
		 * through it. The core reports the same event as SIGSEGV with
		 * the key; so does this, for the same reason the invalid-CSB
		 * case below signals: a poller has no other way to learn.
		 */
		clear_siginfo(&info);
		info.si_signo = SIGSEGV;
		info.si_errno = 0;
		info.si_code = SEGV_PKUERR;
		info.si_addr = csb_addr;
		info.si_pkey = pkey;
		rcu_read_lock();
		kill_pid_info(SIGSEGV, &info, pid);
		rcu_read_unlock();
		put_task_struct(tsk);
		return;
	}

	kthread_use_mm(mm);
	rc = copy_to_user(csb_addr, &csb, sizeof(csb));
	/*
	 * User space polls on csb.flags (first byte). So add barrier
	 * then copy first byte with csb flags update.
	 */
	if (!rc) {
		csb.flags = CSB_V;
		/* Make sure update to csb.flags is visible now */
		smp_mb();
		rc = copy_to_user(csb_addr, &csb, sizeof(u8));
	}
	kthread_unuse_mm(mm);
	mmput(mm);
	put_task_struct(tsk);

	/* Success */
	if (!rc) {
		vas_stat_inc(VAS_STAT_CSB_WRITTEN);
		return;
	}

	vas_stat_inc(VAS_STAT_CSB_COPY_FAIL);

	pr_debug("Invalid CSB address 0x%p signalling pid(%d)\n",
			csb_addr, pid_vnr(pid));

	clear_siginfo(&info);
	info.si_signo = SIGSEGV;
	info.si_errno = EFAULT;
	info.si_code = SEGV_MAPERR;
	info.si_addr = csb_addr;
	/*
	 * process will be polling on csb.flags after request is sent to
	 * NX. So generally CSB update should not fail except when an
	 * application passes invalid csb_addr. So an error message will
	 * be displayed and leave it to user space whether to ignore or
	 * handle this signal.
	 */
	rcu_read_lock();
	rc = kill_pid_info(SIGSEGV, &info, pid);
	rcu_read_unlock();

	pr_devel("pid %d kill_proc_info() rc %d\n", pid_vnr(pid), rc);
}

void vas_dump_crb(struct coprocessor_request_block *crb)
{
	struct data_descriptor_entry *dde;
	struct nx_fault_stamp *nx;

	dde = &crb->source;
	pr_devel("SrcDDE: addr 0x%llx, len %d, count %d, idx %d, flags %d\n",
		be64_to_cpu(dde->address), be32_to_cpu(dde->length),
		dde->count, dde->index, dde->flags);

	dde = &crb->target;
	pr_devel("TgtDDE: addr 0x%llx, len %d, count %d, idx %d, flags %d\n",
		be64_to_cpu(dde->address), be32_to_cpu(dde->length),
		dde->count, dde->index, dde->flags);

	nx = &crb->stamp.nx;
	pr_devel("NX Stamp: PSWID 0x%x, FSA 0x%llx, flags 0x%x, FS 0x%x\n",
		be32_to_cpu(nx->pswid),
		be64_to_cpu(crb->stamp.nx.fault_storage_addr),
		nx->flags, nx->fault_status);
}

static int coproc_open(struct inode *inode, struct file *fp)
{
	struct coproc_instance *cp_inst;

	cp_inst = kzalloc_obj(*cp_inst);
	if (!cp_inst)
		return -ENOMEM;

	cp_inst->coproc = container_of(inode->i_cdev, struct coproc_dev,
					cdev);
	mutex_init(&cp_inst->mutex);
	fp->private_data = cp_inst;

	return 0;
}

/*
 * Why a window open failed, in terms an operator can act on.
 *
 * The errno alone does not separate the causes that matter: -EBUSY covers a
 * partition that has handed out all its credits, windows a reconfiguration
 * closed and has not reopened, and a cgroup at its limit, and each of those
 * needs a different response. The layer that knows which one it was logs it
 * with the numbers; this says where to look. Kept to one line each, because
 * a message that wraps is a message nobody greps.
 *
 * Documentation/arch/powerpc/vas-api.rst describes the counters.
 */
static const char *vas_open_why(long rc)
{
	switch (rc) {
	case -EBUSY:
		return "no credit or at cgroup limit; see nr_used_credits in sysfs and misc.max";
	case -EAGAIN:
		return "every window id on the chip is in use";
	case -ENOMEM:
		return "out of memory";
	case -EINVAL:
		return "bad argument or no such VAS instance";
	case -ENOTSUPP:
		return "hypervisor offers no user mode copy/paste";
	case -EPERM:
		return "caller has no address space";
	default:
		return "see preceding messages";
	}
}

/*
 * The key mask a window translates under: the opening thread's own, or the
 * one the opener named, which may only withhold rights the thread has. A set
 * bit denies, so the thread's denials must all be present in a named mask.
 */
static int vas_user_win_amr(struct vas_user_win_req *req,
			    const struct vas_tx_win_open_attr *uattr)
{
	u64 own = 0;

#ifdef CONFIG_PPC_PKEY
	own = current_thread_amr();
#endif
	req->amr = own;
	if (!(req->flags & VAS_TX_WIN_FLAG_AMR))
		return 0;
	if (!mmu_has_feature(MMU_FTR_PKEY)) {
		pr_debug("%s[%d]: a key mask was named, but keys are not in effect\n",
			 current->comm, current->pid);
		return -EOPNOTSUPP;
	}
	if (own & ~uattr->amr) {
		pr_debug("%s[%d]: mask 0x%llx grants what the thread's 0x%llx denies\n",
			 current->comm, current->pid, uattr->amr, own);
		return -EPERM;
	}
	req->amr = uattr->amr;
	return 0;
}

static long coproc_ioctl(struct file *fp, unsigned int cmd, unsigned long arg);

/*
 * The receive window @fd was opened on, and a reference to the descriptor
 * holding it, which the caller releases with fput().
 *
 * Holding the descriptor is the whole of the right to send to that window.
 * There is no identifier a process could name one by, so a window is
 * reachable only by a process that was handed the right to reach it, and a
 * thread revokes what it handed out by closing its own descriptor once every
 * sender has gone.
 *
 * Each coproc_dev holds its own copy of coproc_fops, so the address of the
 * table does not tell one of ours from any other file. A member of it does.
 */
static struct vas_window *get_target_win(int fd, struct file **filep)
{
	struct coproc_instance *target;

	CLASS(fd, f)(fd);
	if (fd_empty(f))
		return ERR_PTR(-EBADF);

	if (fd_file(f)->f_op->unlocked_ioctl != coproc_ioctl) {
		pr_debug("%s[%d]: target descriptor is not a VAS window\n",
			 current->comm, current->pid);
		return ERR_PTR(-EINVAL);
	}

	target = fd_file(f)->private_data;
	if (!target || !target->rxwin) {
		pr_debug("%s[%d]: target descriptor has no receive window\n",
			 current->comm, current->pid);
		return ERR_PTR(-EINVAL);
	}

	*filep = get_file(fd_file(f));

	return target->rxwin;
}

static int coproc_ioc_tx_win_open(struct file *fp, unsigned long arg)
{
	void __user *uptr = (void __user *)arg;
	struct vas_tx_win_open_attr uattr;
	struct coproc_instance *cp_inst;
	struct vas_user_win_req req;
	struct file *target = NULL;
	struct vas_window *txwin;
	int rc, i;

	cp_inst = fp->private_data;

	/*
	 * One window for file descriptor
	 */
	if (cp_inst->txwin)
		return -EEXIST;

	rc = copy_from_user(&uattr, uptr, sizeof(uattr));
	if (rc) {
		pr_debug("%s[%d]: bad attribute pointer\n", current->comm,
			 current->pid);
		return -EFAULT;
	}

	if (uattr.version != VAS_TX_WIN_OPEN_V1 &&
	    uattr.version != VAS_TX_WIN_OPEN_V2) {
		pr_debug("%s[%d]: window open version %u, expected %u or %u\n",
			 current->comm, current->pid, uattr.version,
			 VAS_TX_WIN_OPEN_V1, VAS_TX_WIN_OPEN_V2);
		return -EINVAL;
	}

	/*
	 * A legacy node offers the interface it offered before this kernel and
	 * no more, so that what it means cannot drift under a program written
	 * against it. Anything later is asked for through the platform's own
	 * node, which no such program opens.
	 */
	if (cp_inst->coproc->type->variant == VAS_NODE_LEGACY &&
	    uattr.version > VAS_TX_WIN_OPEN_V1) {
		pr_debug("%s[%d]: %s offers version %u only, not %u\n",
			 current->comm, current->pid, cp_inst->coproc->type->name,
			 VAS_TX_WIN_OPEN_V1, uattr.version);
		return -EOPNOTSUPP;
	}

	/* Version 1 does not check these. */
	if (uattr.version >= VAS_TX_WIN_OPEN_V2) {
		if (uattr.reserved1 || uattr.flags & ~VAS_TX_WIN_FLAGS_ALL) {
			pr_debug("%s[%d]: reserved1 %u flags 0x%llx: must be 0 / known\n",
				 current->comm, current->pid, uattr.reserved1,
				 uattr.flags);
			return -EINVAL;
		}

		for (i = 0; i < ARRAY_SIZE(uattr.reserved2); i++) {
			if (uattr.reserved2[i]) {
				pr_debug("%s[%d]: reserved2[%d] must be 0\n",
					 current->comm, current->pid, i);
				return -EINVAL;
			}
		}
		if (uattr.amr && !(uattr.flags & VAS_TX_WIN_FLAG_AMR)) {
			pr_debug("%s[%d]: amr must be 0 without VAS_TX_WIN_FLAG_AMR\n",
				 current->comm, current->pid);
			return -EINVAL;
		}
		if (uattr.reserved3 ||
		    (uattr.target_fd && !(uattr.flags & VAS_TX_WIN_FLAG_TARGET))) {
			pr_debug("%s[%d]: target_fd must be 0 without VAS_TX_WIN_FLAG_TARGET\n",
				 current->comm, current->pid);
			return -EINVAL;
		}
		if ((uattr.flags & VAS_TX_WIN_FLAG_DOMAINS) &&
		    !cp_inst->coproc->vops->domain) {
			pr_debug("%s[%d]: no domains on this platform\n",
				 current->comm, current->pid);
			return -EOPNOTSUPP;
		}
	}

	req.vas_id = uattr.vas_id;
	/*
	 * Version 1 carries the flags it always has and ignores the rest, so
	 * that a flag added later cannot be asked for through a version that
	 * does not check the fields carrying it.
	 */
	req.flags = uattr.flags & (uattr.version >= VAS_TX_WIN_OPEN_V2 ?
				   VAS_TX_WIN_FLAGS_ALL : VAS_TX_WIN_FLAGS_V1);
	req.cop_type = cp_inst->coproc->type->cop_type;
	req.target = NULL;
	rc = vas_user_win_amr(&req, &uattr);
	if (rc)
		return rc;

	if (!cp_inst->coproc->vops || !cp_inst->coproc->vops->open_win) {
		pr_err("VAS API is not registered\n");
		return -EACCES;
	}

	if (req.flags & VAS_TX_WIN_FLAG_TARGET) {
		req.target = get_target_win(uattr.target_fd, &target);
		if (IS_ERR(req.target))
			return PTR_ERR(req.target);
	}

	/*
	 * The test above is only advisory: it runs before this and cannot
	 * exclude another thread on the same descriptor. Retest under the
	 * mutex, so that two openers cannot both install a window and leave
	 * one of them unreferenced, with its id, charge and mm held until
	 * the machine reboots.
	 */
	mutex_lock(&cp_inst->mutex);
	if (cp_inst->txwin) {
		mutex_unlock(&cp_inst->mutex);
		rc = -EEXIST;
		goto put_target;
	}

	txwin = cp_inst->coproc->vops->open_win(&req);
	if (IS_ERR(txwin)) {
		rc = PTR_ERR(txwin);
		mutex_unlock(&cp_inst->mutex);
		pr_warn_ratelimited("%s[%d]: window open failed: %s (%d)\n",
				    current->comm, current->pid,
				    vas_open_why(rc), rc);
		goto put_target;
	}

	cp_inst->txwin = txwin;
	/* Handed over: released with the window, in coproc_release(). */
	cp_inst->target = target;
	mutex_unlock(&cp_inst->mutex);

	return 0;

put_target:
	if (target)
		fput(target);

	return rc;
}

static int coproc_release(struct inode *inode, struct file *fp)
{
	struct coproc_instance *cp_inst = fp->private_data;
	int rc;

	/*
	 * A receive window has no paste mapping, so the close and the
	 * references it took on the thread that is woken through it are the
	 * whole of its teardown.
	 */
	if (cp_inst->rxwin && cp_inst->coproc->vops &&
	    cp_inst->coproc->vops->close_win) {
		rc = cp_inst->coproc->vops->close_win(cp_inst->rxwin);
		if (rc)
			pr_err("VAS: pid %d receive window not closed (%d)\n",
			       current->pid, rc);
		else
			put_vas_user_win_ref(&cp_inst->rxwin->task_ref);
		cp_inst->rxwin = NULL;
	}

	if (cp_inst->txwin) {
		if (cp_inst->coproc->vops &&
			cp_inst->coproc->vops->close_win) {
			rc = cp_inst->coproc->vops->close_win(cp_inst->txwin);
			/*
			 * The window's references on the address space are
			 * released here, and only on success: a non-zero
			 * return means the platform retained the window,
			 * the hardware may still write through its
			 * translation, and what that translation needs has
			 * to stay. Every platform has to obey that, so it
			 * is stated once here rather than repeated in each.
			 *
			 * There is no aborting this path over a failure:
			 * the VFS discards the return value and frees the
			 * file regardless, so an early return only leaks
			 * cp_inst and reports nothing.
			 */
			if (rc) {
				pr_err("VAS: pid %d window not closed (%d)\n",
				       current->pid, rc);
			} else {
				mm_context_remove_vas_window(cp_inst->txwin->task_ref.mm);
				put_vas_user_win_ref(&cp_inst->txwin->task_ref);
			}
		}
		cp_inst->txwin = NULL;
	}

	/*
	 * Released after the window, not before: until the window is closed
	 * its context still names the target's window id, and the reference
	 * is what keeps that id from being reissued to anyone else.
	 */
	if (cp_inst->target) {
		fput(cp_inst->target);
		cp_inst->target = NULL;
	}

	kfree(cp_inst);
	fp->private_data = NULL;

	/*
	 * We don't know here if user has other receive windows
	 * open, so we can't really call clear_thread_tidr().
	 * So, once the process calls set_thread_tidr(), the
	 * TIDR value sticks around until process exits, resulting
	 * in an extra copy in restore_sprs().
	 */

	return 0;
}

/*
 * If the executed instruction that caused the fault was a paste, then
 * clear regs CR0[EQ], advance NIP, and return 0. Else return error code.
 */
static int do_fail_paste(void)
{
	struct pt_regs *regs = current->thread.regs;
	u32 instword;

	if (WARN_ON_ONCE(!regs))
		return -EINVAL;

	if (WARN_ON_ONCE(!user_mode(regs)))
		return -EINVAL;

	/*
	 * If we couldn't translate the instruction, the driver should
	 * return success without handling the fault, it will be retried
	 * or the instruction fetch will fault.
	 */
	if (get_user(instword, (u32 __user *)(regs->nip)))
		return -EAGAIN;

	/*
	 * Not a paste instruction, driver may fail the fault.
	 */
	if ((instword & PPC_INST_PASTE_MASK) != PPC_INST_PASTE)
		return -ENOENT;

	regs->ccr &= ~0xe0000000;	/* Clear CR0[0-2] to fail paste */
	regs_add_return_ip(regs, 4);	/* Emulate the paste */

	return 0;
}

/*
 * This fault handler is invoked when the core generates page fault on
 * the paste address. Happens if the kernel closes window in hypervisor
 * (on pseries) due to lost credit or the paste address is not mapped.
 */
static vm_fault_t vas_mmap_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct file *fp = vma->vm_file;
	struct coproc_instance *cp_inst = fp->private_data;
	struct vas_window *txwin;
	vm_fault_t fault;
	u64 paste_addr;
	int ret;

	/*
	 * window is not opened. Shouldn't expect this error.
	 */
	if (!cp_inst || !cp_inst->txwin) {
		pr_debug("%s[%d]: fault on a paste address whose window is closed\n",
			 current->comm, current->pid);
		return VM_FAULT_SIGBUS;
	}

	txwin = cp_inst->txwin;

	/*
	 * The window may be inactive due to lost credit (Ex: core
	 * removal with DLPAR). If the window is active again when
	 * the credit is available, map the new paste address at the
	 * window virtual address.
	 */
	scoped_guard(mutex, &txwin->task_ref.mmap_mutex) {
		/*
		 * When the LPAR lost credits due to core removal or during
		 * migration, invalidate the existing mapping for the current
		 * paste addresses and set windows in-active (zap_vma() in
		 * reconfig_close_windows()).
		 * New mapping will be done later after migration or new
		 * credits available. So continue to receive faults if the
		 * user space issue NX request.
		 *
		 * Compared under the mutex every writer of the field holds;
		 * outside it the read races the mmap and close paths that
		 * change it.
		 */
		if (txwin->task_ref.vma != vmf->vma) {
			pr_debug("%s[%d]: paste fault from a different mapping\n",
				 current->comm, current->pid);
			return VM_FAULT_SIGBUS;
		}

		if (txwin->status == VAS_WIN_ACTIVE) {
			paste_addr = cp_inst->coproc->vops->paste_addr(txwin);
			if (paste_addr) {
				/*
				 * The same protection coproc_mmap() used,
				 * dirty included: paste writes go over the
				 * bus, nothing ever dirties the PTE, and
				 * without the bit the first paste after a
				 * reopen takes one more fault just to set
				 * it.
				 */
				pgprot_t prot =
					__pgprot(pgprot_val(vma->vm_page_prot) |
						 _PAGE_DIRTY);

				fault = vmf_insert_pfn_prot(vma,
						vma->vm_start,
						paste_addr >> PAGE_SHIFT,
						prot);
				return fault;
			}
		}
	}

	/*
	 * Received this fault due to closing the actual window.
	 * It can happen during migration or lost credits.
	 * Since no mapping, return the paste instruction failure
	 * to the user space.
	 */
	ret = do_fail_paste();
	/*
	 * The user space can retry several times until success (needed
	 * for migration) or should fallback to SW compression or
	 * manage with the existing open windows if available.
	 * Looking at sysfs interface, it can determine whether these
	 * failures are coming during migration or core removal:
	 * nr_used_credits > nr_total_credits when lost credits
	 */
	if (!ret || (ret == -EAGAIN))
		return VM_FAULT_NOPAGE;

	return VM_FAULT_SIGBUS;
}

/*
 * During mmap() paste address, mapping VMA is saved in VAS window
 * struct which is used to unmap during migration if the window is
 * still open. But the user space can remove this mapping with
 * munmap() before closing the window and the VMA address will
 * be invalid. Set VAS window VMA to NULL in this function which
 * is called before VMA free.
 */
static void vas_mmap_close(struct vm_area_struct *vma)
{
	struct file *fp = vma->vm_file;
	struct coproc_instance *cp_inst = fp->private_data;
	struct vas_window *txwin;

	/* Should not happen */
	if (!cp_inst || !cp_inst->txwin) {
		pr_debug("%s[%d]: mmap without a window; issue VAS_TX_WIN_OPEN first\n",
			 current->comm, current->pid);
		return;
	}

	txwin = cp_inst->txwin;
	/*
	 * task_ref.vma is set in coproc_mmap() during mmap paste
	 * address. So it has to be the same VMA that is getting freed.
	 */
	if (WARN_ON(txwin->task_ref.vma != vma)) {
		pr_debug("%s[%d]: paste mmap must be one page at offset 0\n",
			 current->comm, current->pid);
		return;
	}

	scoped_guard(mutex, &txwin->task_ref.mmap_mutex)
		txwin->task_ref.vma = NULL;
}

static const struct vm_operations_struct vas_vm_ops = {
	.close = vas_mmap_close,
	.fault = vas_mmap_fault,
};

/*
 * Map the queue a receive window keeps what was pasted to it in.
 *
 * Ordinary memory rather than a paste page, so the mapping is writable: a
 * reader marks an entry free again once it has taken a copy, and that is the
 * only way the switchboard's next writer knows the slot may be reused.
 *
 * The mapping keeps the window alive without any help, because a VMA holds a
 * reference to the file it was mapped from and the window is released when the
 * last reference to that file goes. Closing the descriptor while the queue is
 * mapped therefore cannot free the memory under the mapping.
 */
static int coproc_mmap_rx_fifo(struct coproc_instance *cp_inst,
			       struct vm_area_struct *vma)
{
	struct vas_window *rxwin = cp_inst->rxwin;
	unsigned long size = vma->vm_end - vma->vm_start;
	u32 len = 0;
	void *fifo;

	if (!rxwin) {
		pr_debug("%s[%d]: no receive window open on this descriptor\n",
			 current->comm, current->pid);
		return -EINVAL;
	}

	if (!cp_inst->coproc->vops->rx_fifo)
		return -EOPNOTSUPP;

	fifo = cp_inst->coproc->vops->rx_fifo(rxwin, &len);
	if (!fifo) {
		pr_debug("%s[%d]: window keeps nothing; open it with VAS_RX_WIN_FLAG_FIFO\n",
			 current->comm, current->pid);
		return -EINVAL;
	}

	/*
	 * The window translates and delivers for the address space that opened
	 * it, so its queue is only meaningful there.
	 */
	if (rxwin->task_ref.mm != current->mm)
		return -EACCES;

	if (size > len)
		return -EINVAL;

	/*
	 * Not copied on fork: a child would share one queue with its parent
	 * and both would take entries the other was owed.
	 */
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTCOPY);
	vma->vm_page_prot = pgprot_cached(vma->vm_page_prot);

	return remap_pfn_range(vma, vma->vm_start, virt_to_pfn(fifo),
			       size, vma->vm_page_prot);
}

static int coproc_mmap(struct file *fp, struct vm_area_struct *vma)
{
	struct coproc_instance *cp_inst = fp->private_data;
	struct vas_window *txwin;
	unsigned long pfn;
	u64 paste_addr;
	pgprot_t prot;
	int rc;

	/*
	 * One descriptor can carry both a send window and a receive window,
	 * so which mapping is wanted is said by the offset rather than
	 * guessed from which window happens to be open.
	 */
	if (vma->vm_pgoff == (VAS_RX_FIFO_OFFSET >> PAGE_SHIFT))
		return coproc_mmap_rx_fifo(cp_inst, vma);

	txwin = cp_inst->txwin;

	if ((vma->vm_end - vma->vm_start) > PAGE_SIZE) {
		pr_debug("size 0x%zx, PAGE_SIZE 0x%zx\n",
				(vma->vm_end - vma->vm_start), PAGE_SIZE);
		return -EINVAL;
	}

	/*
	 * Map complete page to the paste address. So the user
	 * space should pass 0ULL to the offset parameter.
	 */
	if (vma->vm_pgoff) {
		pr_debug("Page offset unsupported to map paste address\n");
		return -EINVAL;
	}

	/* Ensure instance has an open send window */
	if (!txwin) {
		pr_debug("%s[%d]: no send window open on this descriptor\n",
			 current->comm, current->pid);
		return -EINVAL;
	}

	if (!cp_inst->coproc->vops || !cp_inst->coproc->vops->paste_addr) {
		pr_err("VAS API is not registered\n");
		return -EACCES;
	}

	/*
	 * The window translates with the address space that opened it, so
	 * the paste address is only meaningful there. The descriptor can be
	 * inherited across exec, and a mapping made from the new address
	 * space would submit requests the nest MMU translates through the
	 * old one.
	 */
	if (txwin->task_ref.mm != current->mm)
		return -EACCES;

	/*
	 * The initial mmap is done after the window is opened
	 * with ioctl. But before mmap(), this window can be closed in
	 * the hypervisor due to lost credit (core removal on pseries).
	 * So if the window is not active, return mmap() failure with
	 * -EACCES and expects the user space reissue mmap() when it
	 * is active again or open new window when the credit is available.
	 * mmap_mutex protects the paste address mmap() with DLPAR
	 * close/open event and allows mmap() only when the window is
	 * active.
	 */
	guard(mutex)(&txwin->task_ref.mmap_mutex);
	if (txwin->status != VAS_WIN_ACTIVE) {
		pr_debug("%s[%d]: window is not active; it will be remapped when credits return\n",
			 current->comm, current->pid);
		return -EACCES;
	}

	/*
	 * One mapping per window. A second would replace task_ref.vma, and
	 * the first mapping's close would then be for a VMA the window no
	 * longer knows. After a credit loss the existing mapping is refilled
	 * by vas_mmap_fault(), not replaced, so this refuses nothing that
	 * path needs.
	 */
	if (txwin->task_ref.vma)
		return -EBUSY;

	paste_addr = cp_inst->coproc->vops->paste_addr(txwin);
	if (!paste_addr) {
		pr_debug("%s[%d]: window has no paste address\n",
			 current->comm, current->pid);
		return -EINVAL;
	}

	pfn = paste_addr >> PAGE_SHIFT;

	/*
	 * flags, page_prot from cxl_mmap(), except we want cachable. And not
	 * copied on fork: the child has a different address space, so a
	 * paste from it would be translated through the parent's, and its
	 * exit would close a VMA the window never recorded.
	 */
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTCOPY);
	vma->vm_page_prot = pgprot_cached(vma->vm_page_prot);

	prot = __pgprot(pgprot_val(vma->vm_page_prot) | _PAGE_DIRTY);

	rc = remap_pfn_range(vma, vma->vm_start, pfn + vma->vm_pgoff,
			vma->vm_end - vma->vm_start, prot);

	pr_devel("paste addr %llx at %lx, rc %d\n", paste_addr,
			vma->vm_start, rc);

	/*
	 * Only record the VMA once it is certain there is one to record. A
	 * ->mmap that fails is cleaned up by the caller, which frees the VMA
	 * without calling ->close, so a pointer stored here on the failing
	 * path is left aimed at freed memory with nothing to clear it.
	 */
	if (rc)
		return rc;

	txwin->task_ref.vma = vma;
	vma->vm_ops = &vas_vm_ops;

	return 0;
}

/* Add or drop a domain of the descriptor's window; nothing without one. */
static int coproc_ioc_domain(struct file *fp, unsigned long arg, bool add)
{
	struct coproc_instance *cp_inst = fp->private_data;
	struct vas_win_domain d;
	int rc;

	if (copy_from_user(&d, (void __user *)arg, sizeof(d)))
		return -EFAULT;
	if (d.reserved[0] || d.reserved[1] || !d.len)
		return -EINVAL;
	if (!cp_inst->coproc->vops->domain)
		return -EOPNOTSUPP;

	mutex_lock(&cp_inst->mutex);
	if (!cp_inst->txwin)
		rc = -ENXIO;
	else
		rc = cp_inst->coproc->vops->domain(cp_inst->txwin, d.start,
						   d.len, add);
	mutex_unlock(&cp_inst->mutex);
	return rc;
}

/*
 * Make this thread somewhere a send window can deliver to. The window is
 * bound to the calling thread, not to the process: a paste to a send window
 * pointed here wakes this thread and no other, so a process wanting several
 * destinations opens one window per thread.
 */
static int coproc_ioc_rx_win_open(struct file *fp, unsigned long arg)
{
	void __user *uptr = (void __user *)arg;
	struct vas_rx_win_open_attr uattr;
	struct coproc_instance *cp_inst;
	struct vas_user_win_req req = {};
	struct file *joined = NULL;
	struct vas_window *rxwin;
	int rc;
	int i;

	cp_inst = fp->private_data;

	if (!cp_inst->coproc->vops || !cp_inst->coproc->vops->open_rx_win)
		return -EOPNOTSUPP;

	/*
	 * Only the switchboard's own node makes a thread a destination. Every
	 * other node stands in front of an engine, and a receive window there
	 * is the one the kernel opened to hold that engine's queue.
	 */
	if (cp_inst->coproc->type->cop_type != VAS_COP_TYPE_FTW)
		return -EOPNOTSUPP;

	if (copy_from_user(&uattr, uptr, sizeof(uattr)))
		return -EFAULT;

	/*
	 * One version only. There was no earlier interface to be compatible
	 * with, so there is nothing to accept but the current shape.
	 */
	if (uattr.version != VAS_TX_WIN_OPEN_V2)
		return -EINVAL;
	if (uattr.reserved1 ||
	    (uattr.flags & ~(u64)(VAS_RX_WIN_FLAG_JOIN | VAS_RX_WIN_FLAG_FIFO)))
		return -EINVAL;
	if (uattr.join_fd && !(uattr.flags & VAS_RX_WIN_FLAG_JOIN))
		return -EINVAL;
	if (uattr.fifo_size && !(uattr.flags & VAS_RX_WIN_FLAG_FIFO))
		return -EINVAL;
	/*
	 * Nothing on this platform can hold what a paste carries, so a caller
	 * asking for it is told rather than quietly given a window that only
	 * wakes -- which would look like the queue working and losing
	 * everything.
	 */
	if ((uattr.flags & VAS_RX_WIN_FLAG_FIFO) &&
	    !cp_inst->coproc->vops->rx_fifo) {
		pr_debug("%s[%d]: no receive queue on this platform\n",
			 current->comm, current->pid);
		return -EOPNOTSUPP;
	}
	for (i = 0; i < ARRAY_SIZE(uattr.reserved2); i++)
		if (uattr.reserved2[i])
			return -EINVAL;

	req.vas_id = uattr.vas_id;
	req.cop_type = cp_inst->coproc->type->cop_type;
	/*
	 * Not req.flags: a receive window's flag values are a send window's
	 * flag values, and whatever reads that field cannot tell the two
	 * apart. VAS_RX_WIN_FLAG_JOIN would arrive as a request for QoS
	 * credit and be accounted as one.
	 */
	if (uattr.flags & VAS_RX_WIN_FLAG_FIFO) {
		req.rx_fifo = true;
		req.rx_fifo_size = uattr.fifo_size;
	}

	/*
	 * Joining is asked for the same way sending is: by presenting the
	 * descriptor the destination was opened on. Holding one is what
	 * entitles a thread both to wake that destination and to become it.
	 */
	if (uattr.flags & VAS_RX_WIN_FLAG_JOIN) {
		req.target = get_target_win(uattr.join_fd, &joined);
		if (IS_ERR(req.target))
			return PTR_ERR(req.target);
	}

	scoped_guard(mutex, &cp_inst->mutex) {
		if (cp_inst->txwin || cp_inst->rxwin) {
			rc = -EEXIST;
			goto put_joined;
		}

		rxwin = cp_inst->coproc->vops->open_rx_win(&req);
		if (IS_ERR(rxwin)) {
			rc = PTR_ERR(rxwin);
			goto put_joined;
		}

		cp_inst->rxwin = rxwin;
	}

	/*
	 * The identity was copied out of the joined window, not borrowed from
	 * it: this descriptor's window carries its own now, and the one it was
	 * taken from can close without disturbing it.
	 */
	if (joined)
		fput(joined);

	return 0;

put_joined:
	if (joined)
		fput(joined);

	return rc;
}

static long coproc_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case VAS_TX_WIN_OPEN:
		return coproc_ioc_tx_win_open(fp, arg);
	case VAS_RX_WIN_OPEN:
		return coproc_ioc_rx_win_open(fp, arg);
	case VAS_WIN_DOMAIN_ADD:
		return coproc_ioc_domain(fp, arg, true);
	case VAS_WIN_DOMAIN_DROP:
		return coproc_ioc_domain(fp, arg, false);
	default:
		return -EINVAL;
	}
}

/* Copied into each type's coproc_dev, which sets the owner. */
static const struct file_operations coproc_fops = {
	.open = coproc_open,
	.release = coproc_release,
	.mmap = coproc_mmap,
	.unlocked_ioctl = coproc_ioctl,
};

/*
 * Register one node with the user window driver. A coprocessor type may have
 * a node per variant, and the minor is fixed by the pair, so each registers
 * once. Called under coproc_devices_lock.
 */
static int coproc_dev_add(struct coproc_dev *dev, struct module *mod)
{
	const char *name = dev->type->name;
	const char *legacy_note = dev->type->variant == VAS_NODE_LEGACY ?
		", the name this interface had before nodes carried the platform" : "";
	/*
	 * Resolved the way coproc_dev_ops() resolves it: a node asking for
	 * VAS_BACKEND_DEFAULT is served by whatever the default is, so it has
	 * no backend of its own to report.
	 */
	enum vas_backend backend = dev->type->backend == VAS_BACKEND_DEFAULT ?
		coproc_default_backend : dev->type->backend;
	struct coproc_dev *other;
	int rc;

	list_for_each_entry(other, &coproc_devices, node)
		if (other->type->cop_type == dev->type->cop_type &&
		    other->type->variant == dev->type->variant &&
		    other->type->backend == dev->type->backend)
			return -EEXIST;

	if (list_empty(&coproc_devices)) {
		rc = alloc_chrdev_region(&coproc_devt, 0, VAS_MINOR_COUNT,
					 "vas");
		if (rc) {
			pr_err("Unable to allocate the coproc major number: %d\n",
			       rc);
			return rc;
		}
	}
	dev->devt = MKDEV(MAJOR(coproc_devt),
			  vas_node_minor(dev->type->cop_type, dev->type->variant,
					 dev->type->backend));

	dev->class = class_create(name);
	if (IS_ERR(dev->class)) {
		rc = PTR_ERR(dev->class);
		pr_err("Unable to create %s class %d\n", name, rc);
		goto err_region;
	}
	dev->class->devnode = coproc_devnode;

	dev->fops = coproc_fops;
	dev->fops.owner = mod;
	cdev_init(&dev->cdev, &dev->fops);
	dev->cdev.owner = mod;
	rc = cdev_add(&dev->cdev, dev->devt, 1);
	if (rc) {
		pr_err("cdev_add() failed %d\n", rc);
		goto err_class;
	}

	dev->device = device_create_with_groups(dev->class, NULL, dev->devt, dev,
						coproc_dev_groups, "%s", name);
	if (IS_ERR(dev->device)) {
		rc = PTR_ERR(dev->device);
		pr_err("Unable to create %s %d\n", name, rc);
		goto err_cdev;
	}

	list_add_tail(&dev->node, &coproc_devices);

	/*
	 * Said rather than traced. A machine may offer an engine, offer the
	 * same engine under two names with different interfaces, or run
	 * requests in software at a fraction of the speed, and the three are
	 * indistinguishable to anyone who was not told which happened.
	 *
	 * Only what the name does not already carry: the numbers a udev rule
	 * and ls(1) show, a backend that is not the one every other node uses,
	 * and why a node exists whose name does not say which machine it is
	 * for. The engine and its priority are in the name.
	 */
	if (backend == coproc_default_backend)
		pr_info("/dev/%s/%s %u:%u%s\n",
			dev->type->dir, name,
			MAJOR(dev->devt), MINOR(dev->devt), legacy_note);
	else
		pr_info("/dev/%s/%s %u:%u, served by the %s backend%s\n",
			dev->type->dir, name,
			MAJOR(dev->devt), MINOR(dev->devt),
			vas_backend_name(backend), legacy_note);

	return 0;

err_cdev:
	cdev_del(&dev->cdev);
err_class:
	class_destroy(dev->class);
err_region:
	if (list_empty(&coproc_devices)) {
		unregister_chrdev_region(coproc_devt, VAS_MINOR_COUNT);
		coproc_devt = 0;
	}
	return rc;
}

int vas_register_backend(enum vas_backend backend,
			 const struct vas_user_win_ops *ops)
{
	int rc = 0;

	if (backend <= VAS_BACKEND_DEFAULT || backend >= VAS_BACKEND_MAX)
		return -EINVAL;
	if (!ops || !ops->open_win || !ops->close_win || !ops->paste_addr)
		return -EINVAL;

	mutex_lock(&coproc_devices_lock);
	if (coproc_backends[backend]) {
		rc = -EBUSY;
	} else {
		coproc_backends[backend] = ops;

		/*
		 * The first to register is the default unless the command
		 * line named one, which is how a machine is brought up on a
		 * backend that is not its own.
		 */
		if (coproc_default_backend == VAS_BACKEND_DEFAULT ||
		    backend == coproc_wanted_backend)
			coproc_default_backend = backend;

		pr_info("%s backend registered%s\n", vas_backend_name(backend),
			coproc_default_backend == backend ? ", and is the default" : "");
	}
	mutex_unlock(&coproc_devices_lock);

	return rc;
}

int vas_user_type_register(struct module *mod, const struct vas_user_type *type)
{
	struct coproc_dev *dev;
	int rc;

	if (!type || !type->name || !type->dir ||
	    type->cop_type >= VAS_COP_TYPE_MAX ||
	    type->variant >= VAS_NODE_VARIANT_MAX ||
	    type->backend >= VAS_BACKEND_MAX)
		return -EINVAL;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->type = type;

	mutex_lock(&coproc_devices_lock);
	dev->vops = coproc_dev_ops(dev);
	if (!dev->vops)
		rc = -ENODEV;
	else
		rc = coproc_dev_add(dev, mod);
	mutex_unlock(&coproc_devices_lock);
	if (rc)
		kfree(dev);
	return rc;
}
EXPORT_SYMBOL_GPL(vas_user_type_register);

void vas_user_type_unregister(const struct vas_user_type *type)
{
	struct coproc_dev *dev, *found = NULL;
	bool last = false;

	mutex_lock(&coproc_devices_lock);
	list_for_each_entry(dev, &coproc_devices, node) {
		if (dev->type == type) {
			found = dev;
			break;
		}
	}
	if (found) {
		list_del(&found->node);
		device_destroy(found->class, found->devt);
		cdev_del(&found->cdev);
		class_destroy(found->class);
		kfree(found);
	}
	if (list_empty(&coproc_devices) && coproc_devt) {
		unregister_chrdev_region(coproc_devt, VAS_MINOR_COUNT);
		coproc_devt = 0;
		last = true;
	}
	mutex_unlock(&coproc_devices_lock);

	if (last) {
		int i;

		/*
		 * Every backend that registered, not just the default: a
		 * window opened against one still has closes to finish
		 * whichever node the last type to go belonged to.
		 */
		for (i = VAS_BACKEND_DEFAULT + 1; i < VAS_BACKEND_MAX; i++)
			if (coproc_backends[i] && coproc_backends[i]->drain_closes)
				coproc_backends[i]->drain_closes();
	}
}
EXPORT_SYMBOL_GPL(vas_user_type_unregister);
