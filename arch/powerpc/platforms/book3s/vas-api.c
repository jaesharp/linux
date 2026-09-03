// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * VAS user space API for its accelerators (Only NX-GZIP is supported now)
 * Copyright (C) 2019 Haren Myneni, IBM Corp
 */

#define pr_fmt(fmt)	"vas-api: " fmt

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/cdev.h>
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
 * Wrapper object for the nx-gzip device - there is just one instance of
 * this node for the whole system.
 */
struct coproc_dev {
	struct cdev cdev;
	struct device *device;
	char *name;
	dev_t devt;
	struct class *class;
	enum vas_cop_type cop_type;
	const struct vas_user_win_ops *vops;
	struct list_head node;
};

/*
 * One of these per coprocessor type a driver registers. coproc_open() already
 * finds its own from the cdev it was opened through, so the only thing that
 * limited this to a single type was storing it in one static instance.
 */
static LIST_HEAD(coproc_devices);
static DEFINE_MUTEX(coproc_devices_lock);

struct coproc_instance {
	struct coproc_dev *coproc;
	struct vas_window *txwin;
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
	return kasprintf(GFP_KERNEL, "crypto/%s", dev_name(dev));
}

atomic_t vas_stats[VAS_STAT_NR];

const char * const vas_stat_names[VAS_STAT_NR] = {
	[VAS_STAT_FAULT_CRBS]		= "fault_crbs",
	[VAS_STAT_FAULT_BAD_PSWID]	= "fault_bad_pswid",
	[VAS_STAT_FIXUP]		= "fixup",
	[VAS_STAT_FIXUP_NOT_USER_EA]	= "fixup_not_user_ea",
	[VAS_STAT_FIXUP_MM_GONE]	= "fixup_mm_gone",
	[VAS_STAT_FIXUP_PAGES]		= "fixup_pages",
	[VAS_STAT_FIXUP_PAGE_ERR]	= "fixup_page_err",
	[VAS_STAT_FIXUP_HASH_ERR]	= "fixup_hash_err",
	[VAS_STAT_FIXUP_HASH_NOINSERT]	= "fixup_hash_noinsert",
	[VAS_STAT_FIXUP_STE_ERR]	= "fixup_ste_err",
	[VAS_STAT_FIXUP_BUDGET]		= "fixup_budget",
	[VAS_STAT_CSB]			= "csb",
	[VAS_STAT_CSB_TASK_GONE]	= "csb_task_gone",
	[VAS_STAT_CSB_MM_REPLACED]	= "csb_mm_replaced",
	[VAS_STAT_CSB_PKEY_DENIED]	= "csb_pkey_denied",
	[VAS_STAT_CSB_COPY_FAIL]	= "csb_copy_fail",
	[VAS_STAT_CSB_SIGNAL]		= "csb_signal",
	[VAS_STAT_WIN_RETAINED]		= "win_retained",
};

void vas_stats_show(struct seq_file *s)
{
	int i;

	for (i = 0; i < VAS_STAT_NR; i++)
		seq_printf(s, "%-20s %d\n", vas_stat_names[i],
			   atomic_read(&vas_stats[i]));
}

/*
 * Take reference to pid and mm
 */
static enum misc_res_type vas_win_misc_res(struct vas_user_win_ref *ref)
{
	return ref->qos_win ? MISC_CG_RES_VAS_WIN_QOS : MISC_CG_RES_VAS_WIN;
}

int get_vas_user_win_ref(struct vas_user_win_ref *task_ref, u64 flags)
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

#ifdef CONFIG_PPC_PKEY
	task_ref->amr = current_thread_amr();
#endif
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
				u64 amr)
{
	struct vm_area_struct *vma;
	bool ok = true;

	mmap_read_lock(mm);
	vma = find_vma(mm, (unsigned long)addr);
	if (vma && (unsigned long)addr >= vma->vm_start)
		ok = pkey_amr_access_permitted(amr, vma_pkey(vma), true);
	mmap_read_unlock(mm);

	return ok;
}
#else
static bool csb_write_permitted(struct mm_struct *mm, void __user *addr,
				u64 amr)
{
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
		    struct vas_user_win_ref *task_ref)
{
	struct coprocessor_status_block csb;
	struct kernel_siginfo info;
	struct task_struct *tsk;
	void __user *csb_addr;
	struct mm_struct *mm;
	struct pid *pid;
	int rc;

	/*
	 * NX user space windows can not be opened for task->mm=NULL
	 * and faults will not be generated for kernel requests.
	 */
	if (WARN_ON_ONCE(!task_ref->mm))
		return;

	csb_addr = (void __user *)be64_to_cpu(crb->csb_addr);

	memset(&csb, 0, sizeof(csb));
	csb.cc = CSB_CC_FAULT_ADDRESS;
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
	if (!csb_write_permitted(mm, csb_addr, task_ref->amr)) {
		vas_stat_inc(VAS_STAT_CSB_PKEY_DENIED);
		mmput(mm);
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
	if (!rc)
		return;

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
	vas_stat_inc(VAS_STAT_CSB_SIGNAL);
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

static int coproc_ioc_tx_win_open(struct file *fp, unsigned long arg)
{
	void __user *uptr = (void __user *)arg;
	struct vas_tx_win_open_attr uattr;
	struct coproc_instance *cp_inst;
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
	}

	if (!cp_inst->coproc->vops || !cp_inst->coproc->vops->open_win) {
		pr_err("VAS API is not registered\n");
		return -EACCES;
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
		return -EEXIST;
	}

	txwin = cp_inst->coproc->vops->open_win(uattr.vas_id, uattr.flags,
						cp_inst->coproc->cop_type);
	if (IS_ERR(txwin)) {
		rc = PTR_ERR(txwin);
		mutex_unlock(&cp_inst->mutex);
		pr_warn_ratelimited("%s[%d]: window open failed: %s (%d)\n",
				    current->comm, current->pid,
				    vas_open_why(rc), rc);
		return rc;
	}

	cp_inst->txwin = txwin;
	mutex_unlock(&cp_inst->mutex);

	return 0;
}

static int coproc_release(struct inode *inode, struct file *fp)
{
	struct coproc_instance *cp_inst = fp->private_data;
	int rc;

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

static int coproc_mmap(struct file *fp, struct vm_area_struct *vma)
{
	struct coproc_instance *cp_inst = fp->private_data;
	struct vas_window *txwin;
	unsigned long pfn;
	u64 paste_addr;
	pgprot_t prot;
	int rc;

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

static long coproc_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case VAS_TX_WIN_OPEN:
		return coproc_ioc_tx_win_open(fp, arg);
	default:
		return -EINVAL;
	}
}

static struct file_operations coproc_fops = {
	.open = coproc_open,
	.release = coproc_release,
	.mmap = coproc_mmap,
	.unlocked_ioctl = coproc_ioctl,
};

/*
 * Supporting only nx-gzip coprocessor type now, but this API code
 * extended to other coprocessor types later.
 */
int vas_register_coproc_api(struct module *mod, enum vas_cop_type cop_type,
			    const char *name,
			    const struct vas_user_win_ops *vops)
{
	struct coproc_dev *dev;
	int rc = -EINVAL;
	dev_t devno;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	rc = alloc_chrdev_region(&dev->devt, 1, 1, name);
	if (rc) {
		pr_err("Unable to allocate coproc major number: %i\n", rc);
		kfree(dev);
		return rc;
	}

	pr_devel("%s device allocated, dev [%i,%i]\n", name,
			MAJOR(dev->devt), MINOR(dev->devt));

	dev->class = class_create(name);
	if (IS_ERR(dev->class)) {
		rc = PTR_ERR(dev->class);
		pr_err("Unable to create %s class %d\n", name, rc);
		goto err_class;
	}
	dev->class->devnode = coproc_devnode;
	dev->cop_type = cop_type;
	dev->vops = vops;

	coproc_fops.owner = mod;
	cdev_init(&dev->cdev, &coproc_fops);

	devno = MKDEV(MAJOR(dev->devt), 0);
	rc = cdev_add(&dev->cdev, devno, 1);
	if (rc) {
		pr_err("cdev_add() failed %d\n", rc);
		goto err_cdev;
	}

	dev->device = device_create(dev->class, NULL, devno, NULL, name,
				    MINOR(devno));
	if (IS_ERR(dev->device)) {
		rc = PTR_ERR(dev->device);
		pr_err("Unable to create coproc-%d %d\n", MINOR(devno), rc);
		goto err;
	}

	mutex_lock(&coproc_devices_lock);
	list_add_tail(&dev->node, &coproc_devices);
	mutex_unlock(&coproc_devices_lock);

	pr_devel("Added dev [%d,%d]\n", MAJOR(devno), MINOR(devno));

	return 0;

err:
	cdev_del(&dev->cdev);
err_cdev:
	class_destroy(dev->class);
err_class:
	unregister_chrdev_region(dev->devt, 1);
	kfree(dev);
	return rc;
}

void vas_unregister_coproc_api(void)
{
	struct coproc_dev *dev, *tmp;
	dev_t devno;

	mutex_lock(&coproc_devices_lock);
	list_for_each_entry_safe(dev, tmp, &coproc_devices, node) {
		list_del(&dev->node);

		cdev_del(&dev->cdev);
		devno = MKDEV(MAJOR(dev->devt), 0);
		device_destroy(dev->class, devno);

		class_destroy(dev->class);
		unregister_chrdev_region(dev->devt, 1);
		kfree(dev);
	}
	mutex_unlock(&coproc_devices_lock);
}
