/* SPDX-License-Identifier: GPL-2.0+ */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM	vas

#if !defined(_VAS_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)

#define _VAS_TRACE_H
#include <linux/tracepoint.h>
#include <linux/sched.h>
#include <asm/vas.h>

TRACE_EVENT(	vas_rx_win_open,

		TP_PROTO(struct task_struct *tsk,
			 int vasid,
			 int cop,
			 struct vas_rx_win_attr *rxattr),

		TP_ARGS(tsk, vasid, cop, rxattr),

		TP_STRUCT__entry(
			__field(struct task_struct *, tsk)
			__field(int, pid)
			__field(int, cop)
			__field(int, vasid)
			__field(struct vas_rx_win_attr *, rxattr)
			__field(int, lnotify_lpid)
			__field(int, lnotify_pid)
			__field(int, lnotify_tid)
		),

		TP_fast_assign(
			__entry->pid = tsk->pid;
			__entry->vasid = vasid;
			__entry->cop = cop;
			__entry->lnotify_lpid = rxattr->lnotify_lpid;
			__entry->lnotify_pid = rxattr->lnotify_pid;
			__entry->lnotify_tid = rxattr->lnotify_tid;
		),

		TP_printk("pid=%d, vasid=%d, cop=%d, lpid=%d, pid=%d, tid=%d",
			__entry->pid, __entry->vasid, __entry->cop,
			__entry->lnotify_lpid, __entry->lnotify_pid,
			__entry->lnotify_tid)
);

TRACE_EVENT(	vas_tx_win_open,

		TP_PROTO(struct task_struct *tsk,
			 int vasid,
			 int cop,
			 struct vas_tx_win_attr *txattr),

		TP_ARGS(tsk, vasid, cop, txattr),

		TP_STRUCT__entry(
			__field(struct task_struct *, tsk)
			__field(int, pid)
			__field(int, cop)
			__field(int, vasid)
			__field(struct vas_tx_win_attr *, txattr)
			__field(int, lpid)
			__field(int, pidr)
		),

		TP_fast_assign(
			__entry->pid = tsk->pid;
			__entry->vasid = vasid;
			__entry->cop = cop;
			__entry->lpid = txattr->lpid;
			__entry->pidr = txattr->pidr;
		),

		TP_printk("pid=%d, vasid=%d, cop=%d, lpid=%d, pidr=%d",
			__entry->pid, __entry->vasid, __entry->cop,
			__entry->lpid, __entry->pidr)
);

TRACE_EVENT(	vas_paste_crb,

		TP_PROTO(struct task_struct *tsk,
			struct pnv_vas_window *win),

		TP_ARGS(tsk, win),

		TP_STRUCT__entry(
			__field(struct task_struct *, tsk)
			__field(struct vas_window *, win)
			__field(int, pid)
			__field(int, vasid)
			__field(int, winid)
			__field(unsigned long, paste_kaddr)
		),

		TP_fast_assign(
			__entry->pid = tsk->pid;
			__entry->vasid = win->vinst->vas_id;
			__entry->winid = win->vas_win.winid;
			__entry->paste_kaddr = (unsigned long)win->paste_kaddr
		),

		TP_printk("pid=%d, vasid=%d, winid=%d, paste_kaddr=0x%016lx\n",
			__entry->pid, __entry->vasid, __entry->winid,
			__entry->paste_kaddr)
);

TRACE_EVENT(	vas_fault_fixup,

		TP_PROTO(int pid,
			 unsigned long ea,
			 unsigned long end,
			 unsigned long pgsz,
			 bool is_write,
			 u8 fs,
			 u8 flags),

		TP_ARGS(pid, ea, end, pgsz, is_write, fs, flags),

		TP_STRUCT__entry(
			__field(int, pid)
			__field(unsigned long, ea)
			__field(unsigned long, end)
			__field(unsigned long, pgsz)
			__field(bool, is_write)
			__field(u8, fs)
			__field(u8, flags)
		),

		TP_fast_assign(
			__entry->pid = pid;
			__entry->ea = ea;
			__entry->end = end;
			__entry->pgsz = pgsz;
			__entry->is_write = is_write;
			__entry->fs = fs;
			__entry->flags = flags;
		),

		TP_printk("pid %d ea 0x%lx end 0x%lx extent %lu pgsz %lu %s fs 0x%02x flags 0x%02x",
			  __entry->pid, __entry->ea, __entry->end,
			  __entry->end - __entry->ea, __entry->pgsz,
			  __entry->is_write ? "write" : "read",
			  __entry->fs, __entry->flags)
);

TRACE_EVENT(	vas_fault_done,

		TP_PROTO(int pid,
			 unsigned long ea,
			 int pages,
			 int budget_left,
			 int hash_rc,
			 int ste_rc,
			 u8 cc),

		TP_ARGS(pid, ea, pages, budget_left, hash_rc, ste_rc, cc),

		TP_STRUCT__entry(
			__field(int, pid)
			__field(unsigned long, ea)
			__field(int, pages)
			__field(int, budget_left)
			__field(int, hash_rc)
			__field(int, ste_rc)
			__field(u8, cc)
		),

		TP_fast_assign(
			__entry->pid = pid;
			__entry->ea = ea;
			__entry->pages = pages;
			__entry->budget_left = budget_left;
			__entry->hash_rc = hash_rc;
			__entry->ste_rc = ste_rc;
			__entry->cc = cc;
		),

		/*
		 * Both outcomes of the last page, because they are separate
		 * insertions: hash_rc is the page table entry, ste_rc the
		 * segment table entry. A single field reported only the
		 * second.
		 */
		TP_printk("pid %d ea 0x%lx resolved %d page(s) budget_left %d%s hash_rc %d ste_rc %d cc %u",
			  __entry->pid, __entry->ea, __entry->pages,
			  __entry->budget_left,
			  __entry->budget_left <= 0 ? " (BUDGET EXHAUSTED)" : "",
			  __entry->hash_rc, __entry->ste_rc, __entry->cc)
);

#endif /* _VAS_TRACE_H */

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../arch/powerpc/platforms/powernv
#define TRACE_INCLUDE_FILE vas-trace
#include <trace/define_trace.h>
