// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What a reduction costs, and where it stops scaling.
 *
 * Atomic memory operations run in the memory controller rather than the core.
 * POWER9 UM section 10.8 is explicit: each MCU has an arithmetic logic unit
 * over the store and fetch data, several AMOs to one address are queued
 * without being serialised by retries on the SMP fabric, and a 31 entry
 * 128-byte read-modify-write buffer keeps the datum so the next AMO to that
 * address finds it there. So a shared counter should cost no more than a
 * private one, however many threads are folding into it.
 *
 * That is a claim about silicon, and it decides a design rather than
 * decorating one, so this measures it rather than repeating it. Two operations
 * crossed with two sharing patterns:
 *
 *   - stdat Store Add, into one cell and into a cell per thread;
 *   - a load-reserve loop, the same two ways, which certainly moves the line.
 *
 * The crossing is what makes it readable. An operation's cost and its
 * behaviour under contention are separate questions, and a table with only the
 * shared column cannot tell a slow instruction from a contended one -- the
 * private arms are what say which is which.
 *
 * The reason to care: a worker accumulating a tile in registers and folding
 * the total in with one atomic pays the atomic once per tile. Whether that
 * scales to many workers is the difference between a reduction tree and a
 * single shared cell, so the number this prints is a design input and not a
 * curiosity.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "report.h"

/* From linux/mempolicy.h, so that asking does not need libnuma. */
#define MPOL_F_NODE  (1 << 0)
#define MPOL_F_ADDR  (1 << 1)

/*
 * Which memory the cell is actually in. The whole question here is whether an
 * operation performed at the memory controller costs more when that controller
 * is on another chip, and a run under numactl that quietly allocated locally
 * anyway would answer it with a confident no.
 */
static int node_of(const void *addr)
{
	int node = -1;

	if (syscall(__NR_get_mempolicy, &node, NULL, 0UL, (unsigned long)addr,
		    MPOL_F_NODE | MPOL_F_ADDR))
		return -1;

	return node;
}

#define WORKERS_MAX 8

/* Store Atomic function codes, Power ISA 3.0B Figure 4. */
enum vas_store_atomic {
	STORE_ATOMIC_ADD = 0,
	STORE_ATOMIC_MAX_UNSIGNED = 4,
	STORE_ATOMIC_MIN_UNSIGNED = 6,
};

/*
 * Two operations crossed with two sharing patterns. The crossing is the point:
 * an operation's cost and its behaviour under contention are separate
 * questions, and a table with only the shared column cannot tell a slow
 * instruction from a contended one.
 */
enum reduce_method {
	REDUCE_ATOMIC_MEMORY,	/* stdat, one cell: the operation done at memory */
	REDUCE_ATOMIC_PRIVATE,	/* stdat, a cell each: the same with no sharing */
	REDUCE_COMPARE_SWAP,	/* a load-reserve loop on one cell, in the core */
	REDUCE_SWAP_PRIVATE,	/* the same loop with no sharing */
};

static const char *method_name(enum reduce_method m)
{
	switch (m) {
	case REDUCE_ATOMIC_MEMORY:
		return "stdat, shared";
	case REDUCE_ATOMIC_PRIVATE:
		return "stdat, private";
	case REDUCE_COMPARE_SWAP:
		return "cas, shared";
	case REDUCE_SWAP_PRIVATE:
		return "cas, private";
	}

	return "unknown";
}

static bool shares(enum reduce_method m)
{
	return m == REDUCE_ATOMIC_MEMORY || m == REDUCE_COMPARE_SWAP;
}

static inline uint64_t now_tb(void)
{
	uint64_t tb;

	asm volatile("mfspr %0, 268" : "=r"(tb));

	return tb;
}

static double tb_hz(void)
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

	return hz;
}

static inline void store_add(volatile uint64_t *mem, uint64_t value)
{
	asm volatile("stdat %0, %1, %2"
		     :: "r"(value), "b"(mem), "i"(STORE_ATOMIC_ADD)
		     : "memory");
}

struct cell {
	volatile uint64_t value;
	char pad[120];
};

struct worker {
	pthread_t thread;
	int cpu;
	struct cell *target;
	enum reduce_method method;
	uint64_t operations;
};

static volatile int go;

static int isolated_cpus(int *cpus, int max)
{
	char buf[4096];
	int n = 0;
	FILE *f;
	char *p;

	f = fopen("/sys/devices/system/cpu/isolated", "r");
	if (!f)
		return 0;
	if (!fgets(buf, sizeof(buf), f)) {
		fclose(f);
		return 0;
	}
	fclose(f);

	for (p = buf; *p && n < max; ) {
		int lo, hi, len = 0;

		if (sscanf(p, "%d-%d%n", &lo, &hi, &len) == 2)
			;
		else if (sscanf(p, "%d%n", &lo, &len) == 1)
			hi = lo;
		else
			break;
		for (; lo <= hi && n < max; lo++)
			cpus[n++] = lo;
		p += len;
		if (*p == ',')
			p++;
	}

	return n;
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

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	uint64_t i;

	pin_to(w->cpu);

	while (!go)
		;

	for (i = 0; i < w->operations; i++) {
		switch (w->method) {
		case REDUCE_ATOMIC_MEMORY:
		case REDUCE_ATOMIC_PRIVATE:
			store_add(&w->target->value, 1);
			break;
		case REDUCE_COMPARE_SWAP:
		case REDUCE_SWAP_PRIVATE:
			__atomic_fetch_add(&w->target->value, 1,
					   __ATOMIC_RELAXED);
			break;
		}
	}

	return NULL;
}

static double run(int workers_n, const int *cpus, enum reduce_method method,
		  uint64_t per_worker, struct cell *cells, double hz)
{
	struct worker workers[WORKERS_MAX];
	uint64_t t0, t1;
	int i;

	memset(cells, 0, (size_t)WORKERS_MAX * sizeof(*cells));
	go = 0;

	for (i = 0; i < workers_n; i++) {
		workers[i].cpu = cpus[i];
		workers[i].method = method;
		workers[i].operations = per_worker;
		workers[i].target = shares(method) ? &cells[0] : &cells[i];
		if (pthread_create(&workers[i].thread, NULL, worker_main,
				   &workers[i]))
			return 0.0;
	}

	/* Started together, so the contention is real rather than staggered. */
	usleep(20000);
	t0 = now_tb();
	go = 1;
	for (i = 0; i < workers_n; i++)
		pthread_join(workers[i].thread, NULL);
	t1 = now_tb();

	if (shares(method) &&
	    cells[0].value != per_worker * (uint64_t)workers_n) {
		fprintf(stderr,
			"  %s lost updates: cell reads %llu, expected %llu\n",
			method_name(method),
			(unsigned long long)cells[0].value,
			(unsigned long long)(per_worker * (uint64_t)workers_n));
		return 0.0;
	}

	return (double)(per_worker * (uint64_t)workers_n) /
	       ((double)(t1 - t0) / hz) / 1e6;
}

int main(int argc, char **argv)
{
	enum reduce_method methods[] = {
		REDUCE_ATOMIC_MEMORY, REDUCE_ATOMIC_PRIVATE,
		REDUCE_COMPARE_SWAP, REDUCE_SWAP_PRIVATE,
	};
	struct cell *cells;
	int isolated[WORKERS_MAX * 4];
	int isolated_n;
	enum reduce_method only = REDUCE_ATOMIC_MEMORY;
	bool chosen = false;
	uint64_t per_worker = 2000000;
	int max_workers = 4;
	double hz = tb_hz();
	size_t m;
	int i, n;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--workers") && i + 1 < argc)
			max_workers = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--operations") && i + 1 < argc)
			per_worker = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--only") && i + 1 < argc) {
			/*
			 * One method per process, so an external counter can
			 * attribute what crossed the memory controller to it.
			 * Run together, the four are indistinguishable to
			 * anything watching from outside.
			 */
			const char *want = argv[++i];

			if (!strcmp(want, "stdat-shared"))
				only = REDUCE_ATOMIC_MEMORY;
			else if (!strcmp(want, "stdat-private"))
				only = REDUCE_ATOMIC_PRIVATE;
			else if (!strcmp(want, "cas-shared"))
				only = REDUCE_COMPARE_SWAP;
			else if (!strcmp(want, "cas-private"))
				only = REDUCE_SWAP_PRIVATE;
			else {
				fprintf(stderr, "unknown method %s\n", want);
				return 2;
			}
			chosen = true;
		}
		else {
			fprintf(stderr,
				"usage: amo_reduce [--workers N] [--operations N]"
				" [--only stdat-shared|stdat-private|cas-shared|cas-private]\n");
			return 2;
		}
	}
	if (max_workers < 1 || max_workers > WORKERS_MAX)
		return 2;

	isolated_n = isolated_cpus(isolated, WORKERS_MAX * 4);
	if (isolated_n < max_workers) {
		fprintf(stderr,
			"refusing to run: %d isolated cpus, %d workers wanted\n",
			isolated_n, max_workers);
		return 1;
	}

	cells = aligned_alloc(128, (size_t)WORKERS_MAX * sizeof(*cells));
	if (!cells)
		return 1;

	/*
	 * Whether the atomic works at all, asked before anything depends on it.
	 * An instruction that quietly did nothing would report the fastest rate
	 * in the table.
	 */
	cells[0].value = 40;
	store_add(&cells[0].value, 2);
	if (cells[0].value != 42) {
		fprintf(stderr, "store atomic did not take: cell reads %llu\n",
			(unsigned long long)cells[0].value);
		return 1;
	}

	printf("cells are in node %d memory; ", node_of((const void *)&cells[0].value));
	printf("%llu operations per worker, workers on cpus",
	       (unsigned long long)per_worker);
	for (i = 0; i < max_workers; i++)
		printf(" %d", isolated[i]);
	printf("\n\n");
	printf("  %-10s", "workers");
	for (m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
		if (chosen && methods[m] != only)
			continue;
		printf("  %16s", method_name(methods[m]));
	}
	printf("\n");

	for (n = 1; n <= max_workers; n++) {
		printf("  %-10d", n);
		for (m = 0; m < sizeof(methods) / sizeof(methods[0]); m++) {
			double rate;

			if (chosen && methods[m] != only)
				continue;
			rate = run(n, isolated, methods[m], per_worker, cells, hz);
			printf("  %10.1f Mops/s", rate);
		}
		printf("\n");
	}

	free(cells);

	return 0;
}
