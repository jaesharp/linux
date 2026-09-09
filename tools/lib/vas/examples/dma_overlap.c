// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Does a tile move for free while the core computes?
 *
 * The 842 engine will move memory to memory without the core touching it, and
 * a pipeline that keeps its next tile arriving while it works on the current
 * one wants exactly that. But an accelerator is not free merely because it is
 * elsewhere: it reads and writes through the same memory controllers the core
 * does, and translates through the same nest MMU, so it can slow the very work
 * it was meant to overlap with.
 *
 * That is the question here, and it is answered by measuring the compute, not
 * the transfer. The same fixed block of arithmetic runs three ways:
 *
 *   - alone, which is the baseline;
 *   - with a move in flight, submitted before the arithmetic and waited for
 *     after it, which is the double-buffered shape;
 *   - after a memcpy of the same bytes, which is what it costs to move them on
 *     the core instead.
 *
 * If the first two agree, the move was free and the pipeline should use it. If
 * the second is slower, the difference is what the engine takes from the core,
 * and it has to be set against what the memcpy would have cost.
 *
 * The arithmetic is the same hand-written FMA loop the instruction rates were
 * measured with, so its cost alone is already known and any change is the
 * engine's doing.
 *
 * Whether the move actually finished inside the compute window is reported
 * too. A transfer that had not started would also cost nothing.
 *
 * The arms are interleaved and repeated, because a single pass of each cannot
 * answer this: run once, the baseline itself moved by eleven per cent between
 * sizes, which is larger than the effect being looked for. Rounds of all three
 * in turn put any drift on all of them, and the spread is printed so a reader
 * can see whether a difference is bigger than the noise it sits in.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vas/nx.h>
#include <vas/nx842.h>
#include <vas/vas.h>

#include "cycles.h"
#include "report.h"

/* The FMA loop from vsx_loops.S: 8 independent chains, returns timebase ticks. */
uint64_t thr_fma(uint64_t iterations);

static inline uint64_t now_tb(void)
{
	uint64_t tb;

	asm volatile("mfspr %0, 268" : "=r"(tb));

	return tb;
}

static double tb_ns(void)
{
	uint32_t be;
	double hz = 512000000.0;
	FILE *f;

	f = fopen("/proc/device-tree/cpus/timebase-frequency", "rb");
	if (f) {
		if (fread(&be, sizeof(be), 1, f) == 1)
			hz = (double)__builtin_bswap32(be);
		fclose(f);
	}

	return 1000000000.0 / hz;
}

static void pin_to(int cpu)
{
	cpu_set_t set;

	if (cpu < 0)
		return;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

enum arm {
	ARM_ALONE,
	ARM_WITH_MOVE,
	ARM_AFTER_MEMCPY,
};

static const char *arm_name(enum arm a)
{
	switch (a) {
	case ARM_ALONE:
		return "compute alone";
	case ARM_WITH_MOVE:
		return "842 move in flight";
	case ARM_AFTER_MEMCPY:
		return "after memcpy";
	}

	return "unknown";
}

int main(int argc, char **argv)
{
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	struct nx_request *request = NULL;
	struct cycle_counter counter = CYCLE_COUNTER_INIT;
	size_t sizes[] = { 4096, 16384, 65536, 262144, 1048576 };
	uint64_t fma_ops = 4000000;
	int rounds = 10;
	char *source = NULL, *target = NULL;
	size_t largest = 1048576;
	double ns = tb_ns();
	int cpu = -1;
	size_t s;
	int i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
			cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--operations") && i + 1 < argc)
			fma_ops = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
			rounds = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: dma_overlap [--cpu N] [--operations N]"
				" [--rounds N]\n");
			return 2;
		}
	}

	pin_to(cpu);

	vas_window_attr_init(&attr, VAS_COP_842);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("open an 842 window", rc);
		return rc == -ENODEV ? 77 : 1;
	}

	source = aligned_alloc(NX_842_BUFFER_ALIGN, largest);
	target = aligned_alloc(NX_842_BUFFER_ALIGN, largest);
	if (!source || !target)
		return 1;
	memset(source, 0xa5, largest);
	memset(target, 0, largest);

	if (nx_request_create(&request)) {
		report_errno("create a request", -ENOMEM);
		return 1;
	}

	if (cycles_open(&counter))
		printf("no cycle counter available\n");

	printf("%llu vector multiply-adds per measurement",
	       (unsigned long long)fma_ops);
	if (cpu >= 0)
		printf(", on cpu %d", cpu);
	printf(", %d rounds interleaved", rounds);
	printf("\n\n  %-10s %-22s %12s %10s %12s  %s\n", "bytes", "arm",
	       "cycles/FMA", "spread", "vs alone", "move");

	for (s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
		size_t len = sizes[s];
		double alone = 0.0;
		size_t a;

		nx_request_reset(request);
		nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
		if (nx_request_set_source(request, nx_source(source, len)) ||
		    nx_request_set_target(request, nx_target(target, len))) {
			printf("  %zu bytes: request refused\n", len);
			continue;
		}

		/* Once, so a translation fault is not counted as overlap. */
		rc = nx_execute(window, request, NULL);
		if (rc) {
			printf("  %zu bytes: %s\n", len, strerror(-rc));
			continue;
		}

		double total[3] = { 0.0, 0.0, 0.0 };
		double low[3] = { 1e30, 1e30, 1e30 };
		double high[3] = { 0.0, 0.0, 0.0 };
		bool finished[3] = { false, false, false };
		int round;

		/* Warm once, so the first round is not the odd one out. */
		thr_fma(fma_ops / 10);

		for (round = 0; round < rounds; round++)
		for (a = 0; a < 3; a++) {
			enum arm this = (enum arm)a;
			unsigned long long cycles, t0, t1;
			bool moved = false;
			double per;

			switch (this) {
			case ARM_ALONE:
				cycles_start(&counter);
				t0 = now_tb();
				thr_fma(fma_ops);
				t1 = now_tb();
				cycles = cycles_stop(&counter);
				break;
			case ARM_WITH_MOVE:
				nx_request_reset(request);
				nx_request_set_ccw(request,
						   nx_842_ccw(NX_842_MOVE));
				nx_request_set_source(request,
						      nx_source(source, len));
				nx_request_set_target(request,
						      nx_target(target, len));
				rc = nx_submit(window, request);
				cycles_start(&counter);
				t0 = now_tb();
				thr_fma(fma_ops);
				t1 = now_tb();
				cycles = cycles_stop(&counter);
				/*
				 * Asked without blocking: whether the engine
				 * had finished by the time the arithmetic did
				 * is the thing worth knowing, and waiting for
				 * it would answer a different question.
				 */
				moved = !rc && !nx_wait(request, 0);
				if (!rc)
					nx_wait(request, 1000);
				break;
			case ARM_AFTER_MEMCPY:
				memcpy(target, source, len);
				cycles_start(&counter);
				t0 = now_tb();
				thr_fma(fma_ops);
				t1 = now_tb();
				cycles = cycles_stop(&counter);
				break;
			}

			per = (double)cycles / (double)fma_ops;
			total[a] += per;
			if (per < low[a])
				low[a] = per;
			if (per > high[a])
				high[a] = per;
			finished[a] = moved;
			(void)t0;
			(void)t1;
			(void)ns;
		}

		for (a = 0; a < 3; a++) {
			double mean = total[a] / (double)rounds;

			if (a == (size_t)ARM_ALONE)
				alone = mean;

			printf("  %-10zu %-22s %12.4f %9.1f%% %11.1f%%  %s\n",
			       len, arm_name((enum arm)a), mean,
			       100.0 * (high[a] - low[a]) / mean,
			       alone > 0.0 ? 100.0 * (mean - alone) / alone : 0.0,
			       a == (size_t)ARM_WITH_MOVE ?
				       (finished[a] ? "finished first" : "still running") :
				       "");
		}
		fflush(stdout);
	}

	cycles_close(&counter);
	nx_request_destroy(&request);
	vas_window_close(&window);
	free(source);
	free(target);

	return 0;
}
