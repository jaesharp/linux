// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Does one paste wake more than one thread?
 *
 * A notify carries an identity rather than a recipient: the switchboard puts a
 * partition, process and thread number on the interconnect, and a core matches
 * it against whatever it is running. Nothing in that says only one thread may
 * answer to a given identity, so several threads given the same one should all
 * be matched by a single notify -- and one paste would wake a group rather
 * than a thread. Neither the workbook nor the architecture says whether the
 * hardware does that, so this asks it.
 *
 * Answering needs an instrument, not just an experiment. A thread in wait
 * resumes on its own about every microsecond on this machine and a notify
 * arrives in a few hundred nanoseconds, so "did it wake" has no useful answer:
 * every thread wakes, always, for one reason or another. What separates the
 * two is when.
 *
 * So each thread has a cell of its own, seeded to the largest value there is,
 * and on resuming folds its timebase into that cell with an atomic Store
 * Minimum Unsigned. The earliest resume wins, the comparison happens at memory
 * rather than in the waking thread, and a thread that resumes several times in
 * one trial cannot be counted twice. The sender then reads the cells and knows
 * for each thread how long after the paste it first came back, or that it
 * never did.
 *
 * --no-join is the control and the result means nothing without it. With it
 * each thread opens a destination of its own, so only the thread pasted to can
 * be reached and every other prompt resume is a coincidence. The rate of those
 * coincidences is what a joined run has to beat.
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

#define JOINERS_MAX 8

/*
 * A resume later than this was not the notify. A wake crossing chips takes a
 * few hundred nanoseconds with a ninetieth percentile barely above its median,
 * so this leaves an order of magnitude of headroom -- enough that the core
 * clock moving underneath it does not change which side of the line a resume
 * falls on. Widening it only admits more of the incidental resumes the control
 * is there to count.
 */
#define PROMPT_NS 3000.0

/* Store Atomic function codes, Power ISA 3.0B Figure 4. */
enum vas_store_atomic {
	STORE_ATOMIC_ADD = 0,
	STORE_ATOMIC_MAX_UNSIGNED = 4,
	STORE_ATOMIC_MIN_UNSIGNED = 6,
};

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

/* mem = min(mem, value), decided at memory rather than by this thread. */
static inline void store_min(volatile uint64_t *mem, uint64_t value)
{
	asm volatile("stdat %0, %1, %2"
		     :: "r"(value), "b"(mem), "i"(STORE_ATOMIC_MIN_UNSIGNED)
		     : "memory");
}

/* Its own line, so one thread's atomic does not slow another's. */
struct cell {
	volatile uint64_t earliest;
	char pad[120];
};

struct joiner {
	pthread_t thread;
	int cpu;
	int join_fd;			/* -1 for the first, which opens it */
	struct vas_destination *dest;
	struct cell *cell;
	volatile int ready;
	int prompt;			/* trials it resumed inside the window */
	int reached;			/* trials it resumed at all */
	int rc;
};

static volatile uint64_t trial;
static volatile int stop;

/*
 * The CPUs the kernel was told to keep work off. A joiner has to sit on one:
 * this counts resumes and calls the prompt ones wakes, so a core the scheduler
 * still uses produces resumes that have nothing to do with a paste, at a rate
 * that swamps what is being looked for.
 */
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

static void *joiner_main(void *arg)
{
	struct joiner *j = arg;
	uint64_t seen = 0;

	pin_to(j->cpu);

	j->rc = j->join_fd < 0 ?
		vas_destination_open(vas_instance_any(), &j->dest) :
		vas_destination_join(j->join_fd, &j->dest);
	if (j->rc) {
		j->ready = -1;
		return NULL;
	}

	j->ready = 1;

	while (!stop) {
		uint64_t resumed, outstanding;

		vas_wait();

		/*
		 * The clock before anything else. Deciding whether this resume
		 * belongs to the outstanding trial means loading a line the
		 * sender last wrote, which across chips costs as much as the
		 * wake being measured.
		 */
		resumed = now_tb();

		outstanding = __atomic_load_n(&trial, __ATOMIC_ACQUIRE);
		if (outstanding == seen)
			continue;

		store_min(&j->cell->earliest, resumed);
		seen = outstanding;
	}

	return NULL;
}

int main(int argc, char **argv)
{
	struct joiner joiners[JOINERS_MAX];
	struct cell *cells;
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	int isolated[JOINERS_MAX * 4];
	int isolated_n;
	int joiners_n = 4;
	int stride = 1;
	int trials = 400;
	bool join = true;
	bool reverse = false;
	double hz, ns;
	int i, t, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--joiners") && i + 1 < argc)
			joiners_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--stride") && i + 1 < argc)
			stride = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--trials") && i + 1 < argc)
			trials = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--no-join"))
			join = false;
		else if (!strcmp(argv[i], "--reverse"))
			reverse = true;
		else {
			fprintf(stderr,
				"usage: multicast [--joiners N] [--stride N] [--trials N]"
				" [--no-join] [--reverse]\n");
			return 2;
		}
	}
	if (joiners_n < 2 || joiners_n > JOINERS_MAX || trials < 1)
		return 2;

	hz = tb_hz();
	ns = 1000000000.0 / hz;

	memset(joiners, 0, sizeof(joiners));

	isolated_n = isolated_cpus(isolated, JOINERS_MAX * 4);
	if (isolated_n < joiners_n * stride) {
		fprintf(stderr,
			"refusing to run: %d isolated cpus, %d threads at stride %d need %d\n"
			"  boot with isolcpus= covering them, or lower --joiners\n",
			isolated_n, joiners_n, stride, joiners_n * stride);
		return 1;
	}

	cells = aligned_alloc(128, (size_t)joiners_n * sizeof(*cells));
	if (!cells)
		return 1;
	memset(cells, 0, (size_t)joiners_n * sizeof(*cells));

	/*
	 * The sender sleeps rather than spins through each trial's window, so
	 * it is off its core for the interval being measured and cannot slow a
	 * joiner it shares a core with.
	 */
	pin_to(reverse ? isolated[0] : isolated[isolated_n - 1]);

	/*
	 * Which end of the isolated list the group is built from. It decides
	 * which chip the first thread sits on, and so which chip's switchboard
	 * opens the shared destination and issues the notify -- the thing the
	 * rest of the group is or is not reached by.
	 */
	for (i = 0; i < joiners_n; i++) {
		joiners[i].cpu = reverse ?
			isolated[isolated_n - 1 - i * stride] :
			isolated[i * stride];
		joiners[i].join_fd = -1;
		joiners[i].cell = &cells[i];
	}

	if (pthread_create(&joiners[0].thread, NULL, joiner_main, &joiners[0])) {
		report_errno("pthread_create", -errno);
		return 1;
	}
	while (!joiners[0].ready)
		;
	if (joiners[0].ready < 0) {
		report_errno("open the group", joiners[0].rc);
		return joiners[0].rc == -ENODEV ? 77 : 1;
	}

	for (i = 1; i < joiners_n; i++) {
		joiners[i].join_fd = join ? vas_destination_fd(joiners[0].dest) : -1;
		if (pthread_create(&joiners[i].thread, NULL, joiner_main,
				   &joiners[i])) {
			report_errno("pthread_create", -errno);
			return 1;
		}
		while (!joiners[i].ready)
			;
		if (joiners[i].ready < 0) {
			report_errno("join the group", joiners[i].rc);
			return 1;
		}
	}

	/* One sender, pointed at the destination the first thread opened. */
	vas_window_attr_init(&attr, VAS_COP_FTW);
	attr.wake_target = vas_destination_fd(joiners[0].dest);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("open a window onto the group", rc);
		return 1;
	}

	for (t = 0; t < trials; t++) {
		uint64_t sent;

		for (i = 0; i < joiners_n; i++)
			cells[i].earliest = UINT64_MAX;

		/* Settle: every joiner suspended, no trial outstanding. */
		usleep(200);

		sent = now_tb();
		__atomic_store_n(&trial, (uint64_t)(t + 1), __ATOMIC_RELEASE);

		rc = vas_wake(window);
		if (rc) {
			report_errno("wake the group", rc);
			break;
		}

		/* Long enough that a notify would have arrived many times over. */
		usleep(500);

		for (i = 0; i < joiners_n; i++) {
			uint64_t got = cells[i].earliest;

			if (got == UINT64_MAX || got < sent)
				continue;
			joiners[i].reached++;
			if ((double)(got - sent) * ns < PROMPT_NS)
				joiners[i].prompt++;
		}
	}

	stop = 1;
	/* A last bump and paste, so anything still suspended can leave. */
	__atomic_store_n(&trial, (uint64_t)(trials + 2), __ATOMIC_RELEASE);
	vas_wake(window);
	for (i = 0; i < joiners_n; i++)
		pthread_join(joiners[i].thread, NULL);

	printf("{\"label\":\"%s%s\",\"joiners\":%d,\"trials\":%d,\"prompt\":[",
	       join ? "joined" : "separate", reverse ? "-reverse" : "",
	       joiners_n, trials);
	for (i = 0; i < joiners_n; i++)
		printf("%s%d", i ? "," : "", joiners[i].prompt);
	printf("]}\n");
	fflush(stdout);

	fprintf(stderr, "%d trials, %d threads %s:\n", trials, joiners_n,
		join ? "sharing a destination" :
		       "with destinations of their own (control)");
	for (i = 0; i < joiners_n; i++) {
		fprintf(stderr,
			"  %s on cpu %-4d resumed within %.0f ns in %4d of %d trials\n",
			i ? "follower  " : "the target",
			joiners[i].cpu, PROMPT_NS, joiners[i].prompt, trials);
		vas_destination_close(&joiners[i].dest);
	}

	vas_window_close(&window);
	free(cells);

	return 0;
}
