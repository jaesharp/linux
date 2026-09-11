// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What a waiting thread leaves for the other thread of its core.
 *
 * Power ISA 3.0B says of wait that it "frees computational resources which
 * might be allocated to another program or converted into power savings".
 * That is a claim about the partner thread, so the partner is what has to be
 * measured: how much work it gets done while this thread is spinning, while
 * this thread is parked in wait, and while this thread is not there at all.
 *
 * It matters for anything built on the switchboard's wake. A receiver parked
 * in wait is idle by intent, and if that idleness costs its partner half a
 * core then the mechanism is not free -- a design would then want receivers on
 * cores of their own, which is a different placement policy from putting them
 * wherever is convenient. If instead a waiting thread takes almost nothing,
 * receivers can share cores with real work and the placement question mostly
 * goes away.
 *
 * The work is a dependent integer chain, so it cannot be widened by the
 * machine finding parallelism in it, and the count is fixed so that the time
 * is the measurement. Reported as the partner's rate under each condition,
 * against the rate it reaches with the core to itself.
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
#include <unistd.h>

#include <vas/vas.h>

#include "report.h"

/* Long enough to swamp start-up, short enough to run the whole matrix. */
#define CHAIN_STEPS 200000000UL

enum neighbour_mode {
	NEIGHBOUR_ABSENT,	/* nothing on the partner thread */
	NEIGHBOUR_SPINNING,	/* the partner running flat out */
	NEIGHBOUR_WAITING,	/* the partner parked in wait */
};

static const char *mode_name(enum neighbour_mode mode)
{
	switch (mode) {
	case NEIGHBOUR_ABSENT:
		return "core to itself";
	case NEIGHBOUR_SPINNING:
		return "partner spinning";
	case NEIGHBOUR_WAITING:
		return "partner in wait";
	}

	return "unknown";
}

static volatile int stop;

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

static int pin_to(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);

	return sched_setaffinity(0, sizeof(set), &set) < 0 ? -errno : 0;
}

/*
 * Somewhere for the result to go. Without it the chain is dead code and the
 * compiler deletes the whole loop, which does not fail -- it reports a rate,
 * a plausible one, and the same figure for conditions that must differ.
 */
static volatile uint64_t sink;

/*
 * A chain of dependent operations: each step needs the previous result, so the
 * core cannot widen it, and the rate is what one thread gets of the core's
 * issue slots rather than what its instruction mix happens to allow.
 */
static uint64_t chain(uint64_t steps)
{
	uint64_t x = 1;

	while (steps--)
		x = x * 6364136223846793005UL + 1442695040888963407UL;

	return x;
}

struct neighbour {
	pthread_t thread;
	int cpu;
	enum neighbour_mode mode;
};

static void *neighbour_main(void *arg)
{
	struct neighbour *n = arg;

	if (pin_to(n->cpu))
		return NULL;

	while (!stop) {
		if (n->mode == NEIGHBOUR_WAITING)
			vas_wait();
		else
			sink = chain(1000);
	}

	return NULL;
}

static double measure(int neighbour_cpu, enum neighbour_mode mode, double hz)
{
	struct neighbour n = { .cpu = neighbour_cpu, .mode = mode };
	uint64_t t0, t1;
	bool paired = mode != NEIGHBOUR_ABSENT;

	stop = 0;
	if (paired && pthread_create(&n.thread, NULL, neighbour_main, &n))
		return 0.0;

	/* Let the partner reach its steady state before timing anything. */
	if (paired)
		usleep(50000);

	t0 = now_tb();
	sink = chain(CHAIN_STEPS);
	t1 = now_tb();

	stop = 1;
	if (paired)
		pthread_join(n.thread, NULL);

	return (double)CHAIN_STEPS / ((double)(t1 - t0) / hz) / 1e6;
}

int main(int argc, char **argv)
{
	enum neighbour_mode modes[] = {
		NEIGHBOUR_ABSENT, NEIGHBOUR_SPINNING, NEIGHBOUR_WAITING,
	};
	int worker_cpu = -1, neighbour_cpu = -1;
	double hz = tb_hz();
	double alone = 0.0;
	size_t m;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--worker-cpu") && i + 1 < argc)
			worker_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--partner-cpu") && i + 1 < argc)
			neighbour_cpu = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: smt_share --worker-cpu N --partner-cpu N\n");
			return 2;
		}
	}
	if (worker_cpu < 0 || neighbour_cpu < 0) {
		fprintf(stderr,
			"both cpus are required, and they must be threads of one core\n");
		return 2;
	}

	if (pin_to(worker_cpu)) {
		report_errno("pin the worker", -errno);
		return 1;
	}

	printf("dependent chain on cpu %d, partner thread on cpu %d:\n",
	       worker_cpu, neighbour_cpu);

	for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		double rate = measure(neighbour_cpu, modes[m], hz);

		if (!m)
			alone = rate;

		printf("  %-18s %8.1f Mops/s   %5.1f%% of the core to itself\n",
		       mode_name(modes[m]), rate,
		       alone > 0.0 ? 100.0 * rate / alone : 0.0);
	}

	return 0;
}
