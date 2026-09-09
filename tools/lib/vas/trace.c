// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The uprobe anchors, built only when tracing is on and USDT is not
 * available. Each exists to have an address and a signature; a tracer
 * attaches to the symbol and reads the arguments.
 */

#include <vas/trace.h>

#if VAS_TRACE && !defined(VAS_TRACE_USDT)

VAS_TRACE_ANCHOR void vas_trace_window_open(int cop, int node, int instance,
					    uint64_t flags)
{
	(void)cop;
	(void)node;
	(void)instance;
	(void)flags;
}

VAS_TRACE_ANCHOR void vas_trace_window_close(int cop, int node)
{
	(void)cop;
	(void)node;
}

VAS_TRACE_ANCHOR void vas_trace_submit(const void *target, uint32_t ccw,
				       uint64_t source_len, uint64_t target_len)
{
	(void)target;
	(void)ccw;
	(void)source_len;
	(void)target_len;
}

VAS_TRACE_ANCHOR void vas_trace_paste_refused(const void *target,
					      unsigned int attempt)
{
	(void)target;
	(void)attempt;
}

VAS_TRACE_ANCHOR void vas_trace_complete(const void *target, int cc,
					 uint32_t processed)
{
	(void)target;
	(void)cc;
	(void)processed;
}

VAS_TRACE_ANCHOR void vas_trace_fault_retry(const void *address, int status,
					    int on_write)
{
	(void)address;
	(void)status;
	(void)on_write;
}

#endif /* VAS_TRACE && !VAS_TRACE_USDT */
