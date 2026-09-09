// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Does wait suspend this thread, and for how long?
 *
 * Everything built on the switchboard's wake assumes a waiting thread is off
 * the core until something arrives. If instead wait returns straight away, a
 * loop around it is a spin loop wearing a costume: it still works, it still
 * looks fast, and every latency measured through it is the time for a store to
 * become visible rather than the time for a notify to be delivered. The two
 * are the same order of magnitude on this machine, so nothing about the
 * numbers gives it away.
 *
 * So this asks the question on its own, with no sender at all. A thread calls
 * wait repeatedly and records how long each call took. Nothing is arranged to
 * wake it, so:
 *
 *   - if wait suspends, the times are the intervals between whatever
 *     incidental exceptions the core takes -- microseconds at least, and on an
 *     isolated core far more;
 *   - if wait does not suspend, the times are a few nanoseconds and the whole
 *     run finishes immediately.
 *
 * Except that the times came back at a microsecond, which is neither: too long
 * for an instruction that did nothing, too short and far too regular to be the
 * interval between incidental exceptions on an isolated core. So the same loop
 * is also run with the wait removed, and the two are reported together. The
 * difference between them is what the instruction costs; whether that time was
 * spent suspended is then answered by the exception counters, which the caller
 * reads around this.
 *
 * --spin sweeps the phase. If something periodic is resuming the thread, the
 * time spent in wait is the remainder of that period, so spinning for a while
 * first shortens it by however long the spin took, and sweeping the spin walks
 * the duration up and down. If instead wait is completing on a condition that
 * is always already true, the duration does not move at all. One sweep tells
 * the two apart, and they call for entirely different repairs.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vas/vas.h>

/* BESCR bit 63: external event-based exception enable. Bit 0, GE, stays 0. */
#define BESCR_EE 0x1UL

#include "report.h"

/*
 * The timebase, read directly. clock_gettime costs about a microsecond here --
 * an order of magnitude more than the thing being measured, and sitting
 * immediately before every wait, which is the worst place for a confound to
 * sit. mftb is a single instruction readable in problem state and ticks at a
 * frequency the kernel publishes, so it measures this without being part of
 * it.
 */
static inline unsigned long long now_tb(void)
{
	unsigned long long tb;

	asm volatile("mfspr %0, 268" : "=r"(tb));

	return tb;
}

/* Ticks per second, from the device tree, so no frequency is assumed here. */
static double tb_hz(void)
{
	unsigned int be;
	double hz = 512000000.0;
	FILE *f;

	f = fopen("/proc/device-tree/cpus/timebase-frequency", "rb");
	if (f) {
		if (fread(&be, sizeof(be), 1, f) == 1)
			hz = (double)__builtin_bswap32(be);
		fclose(f);
	}

	return hz;
}

static int cmp_ll(const void *a, const void *b)
{
	long long x = *(const long long *)a, y = *(const long long *)b;

	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	long long *held, *bare;
	int samples = 200;
	int cpu = -1;
	long spin_before = 0;
	bool arm_bescr = false;
	long long total = 0;
	double hz, ns;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--samples") && i + 1 < argc)
			samples = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
			cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--spin") && i + 1 < argc)
			spin_before = atol(argv[++i]);
		else if (!strcmp(argv[i], "--bescr"))
			arm_bescr = true;
		else {
			fprintf(stderr,
				"usage: suspend [--samples N] [--cpu N] [--spin N]"
				" [--bescr]\n");
			return 2;
		}
	}
	if (samples < 1)
		return 2;

	if (cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set)) {
			report_errno("pin to the requested cpu", -1);
			return 1;
		}
	}

	/*
	 * What Power ISA 3.0B tells an application to do before waiting for an
	 * external event: enable the exception, disable the branch. With the
	 * branch enabled the event would be taken before the wait executed and
	 * the thread would hang; with the exception disabled there is nothing
	 * to resume it. Neither is what we have been doing, which is neither.
	 */
	if (arm_bescr)
		asm volatile("mtspr 806,%0" :: "r"(BESCR_EE) : "memory");

	hz = tb_hz();
	ns = 1000000000.0 / hz;

	held = calloc(samples, sizeof(*held));
	bare = calloc(samples, sizeof(*bare));
	if (!held || !bare)
		return 1;

	/*
	 * Interleaved, so that anything drifting over the run -- frequency,
	 * another tenant of the core -- lands on both arms rather than on
	 * whichever was measured second.
	 */
	for (i = 0; i < samples; i++) {
		unsigned long long before, after;
		volatile long sp;

		for (sp = 0; sp < spin_before; sp++)
			;

		before = now_tb();
		vas_wait();
		after = now_tb();
		held[i] = (long long)(after - before);
		total += held[i];

		before = now_tb();
		after = now_tb();
		bare[i] = (long long)(after - before);
	}

	qsort(held, samples, sizeof(*held), cmp_ll);
	qsort(bare, samples, sizeof(*bare), cmp_ll);

	printf("%d calls to wait, nothing arranged to wake this thread", samples);
	if (cpu >= 0)
		printf(", on cpu %d", cpu);
	if (spin_before)
		printf(", %ld spins before each", spin_before);
	if (arm_bescr)
		printf(", BESCR_EE set");
	printf(":\n");
	printf("  timebase %.0f Hz, %.3f ns per tick\n", hz, ns);
	printf("  with wait     shortest %8.1f ns  median %8.1f ns  90th %8.1f ns  longest %10.1f ns\n",
	       held[0] * ns, held[samples / 2] * ns,
	       held[(samples * 9) / 10] * ns, held[samples - 1] * ns);
	printf("  reading time  shortest %8.1f ns  median %8.1f ns\n",
	       bare[0] * ns, bare[samples / 2] * ns);
	printf("  wait costs %.1f ns at the median over %d calls\n",
	       (held[samples / 2] - bare[samples / 2]) * ns, samples);

	free(bare);
	free(held);

	return 0;
}
