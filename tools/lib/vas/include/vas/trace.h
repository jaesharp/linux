/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Static trace points for the library.
 *
 * The kernel cannot see a request. Submission is a copy and paste pair
 * executed in user mode, and an ordinary completion is a store the engine
 * makes straight into the requester's status block; the kernel is involved
 * only when a translation faults or when it has to write a completion on the
 * engine's behalf. Kernel tracing of VAS therefore shows only what went
 * wrong, and on a healthy machine shows nothing at all. These are where the
 * ordinary path becomes visible.
 *
 * Compiled out unless VAS_TRACE is 1, which the Makefile's TRACE setting
 * chooses. When it is on, the probes are USDT notes where <sys/sdt.h> is
 * available, and otherwise functions that exist only to be a place to put a
 * uprobe. Both are reachable from perf and bpftrace; the note costs nothing
 * when nobody is attached, while the function costs a call either way, so
 * the note is preferred when it can be had.
 *
 * The fault probe is the one worth correlating: the same event appears in
 * the kernel as vas_fault_fixup and vas_fault_done on the same address, so
 * a trace holding both sides shows the whole latency of a fault, which
 * neither side can see alone.
 */

#ifndef _VAS_TRACE_H
#define _VAS_TRACE_H

#include <stdint.h>

#ifndef VAS_TRACE
#define VAS_TRACE 0
#endif

#if VAS_TRACE

#if defined(__has_include)
#if __has_include(<sys/sdt.h>)
#define VAS_TRACE_USDT 1
#endif
#endif

#ifdef VAS_TRACE_USDT

#include <sys/sdt.h>

#define vas_trace_window_open(cop, node, instance, flags) \
	STAP_PROBE4(vas, window_open, cop, node, instance, flags)
#define vas_trace_window_close(cop, node) \
	STAP_PROBE2(vas, window_close, cop, node)
#define vas_trace_submit(target, ccw, source_len, target_len) \
	STAP_PROBE4(vas, submit, target, ccw, source_len, target_len)
#define vas_trace_paste_refused(target, attempt) \
	STAP_PROBE2(vas, paste_refused, target, attempt)
#define vas_trace_complete(target, cc, processed) \
	STAP_PROBE3(vas, complete, target, cc, processed)
#define vas_trace_fault_retry(address, status, on_write) \
	STAP_PROBE3(vas, fault_retry, address, status, on_write)

#else /* !VAS_TRACE_USDT */

/*
 * Somewhere to attach a uprobe. Not static, so the symbol survives into the
 * archive and a tracer can find it by name; not inlined, so there is one
 * address to attach to; the arguments are the payload, read from registers.
 */
#define VAS_TRACE_ANCHOR __attribute__((noinline, used))

void vas_trace_window_open(int cop, int node, int instance, uint64_t flags);
void vas_trace_window_close(int cop, int node);
void vas_trace_submit(const void *target, uint32_t ccw, uint64_t source_len,
		      uint64_t target_len);
void vas_trace_paste_refused(const void *target, unsigned int attempt);
void vas_trace_complete(const void *target, int cc, uint32_t processed);
void vas_trace_fault_retry(const void *address, int status, int on_write);

#endif /* VAS_TRACE_USDT */

#else /* !VAS_TRACE */

#define vas_trace_window_open(cop, node, instance, flags) do { } while (0)
#define vas_trace_window_close(cop, node) do { } while (0)
#define vas_trace_submit(target, ccw, source_len, target_len) do { } while (0)
#define vas_trace_paste_refused(target, attempt) do { } while (0)
#define vas_trace_complete(target, cc, processed) do { } while (0)
#define vas_trace_fault_retry(address, status, on_write) do { } while (0)

#endif /* VAS_TRACE */

#endif /* _VAS_TRACE_H */
