// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What irregular work costs when every element claims its own.
 *
 * A schedule fixed before the run has to guess how long each piece will take.
 * When the pieces are equal that guess is free; when they are not, the run
 * ends when the unluckiest worker ends, and the rest wait. Machines that
 * execute in lockstep have no way out of this, which is why work with
 * data-dependent cost is awkward on them.
 *
 * Here the workers are independent threads and claiming a piece is one
 * instruction: Fetch and Increment Bounded reads a counter and the limit
 * beside it, hands back an index if there is one and an unmistakable value if
 * there is not, and does the comparison in the memory controller so that no
 * two workers serialise against each other to get their next piece. A worker
 * that finishes early simply takes more.
 *
 * So the question is what that is worth, and the answer needs both halves of a
 * two-by-two: a fixed partition and a claim, over work that is even and work
 * that is not. Three of those four cells should agree. If the fixed partition
 * over uneven work is the only one that suffers, the claim is what rescued it;
 * if all four agree, the imbalance was never large enough to see and the
 * measurement says nothing.
 *
 * The unevenness has to be clustered to mean anything. A first version made
 * every tenth piece heavy, which a contiguous partition balances perfectly by
 * construction -- each share got the same count of heavy pieces and all four
 * cells agreed, testing nothing. Real skew arrives in runs: a routed layer
 * sends a burst to one expert, a sparse block has dense neighbourhoods. So the
 * heavy pieces come in runs here, and a partition can be unlucky in the way it
 * would be unlucky in earnest.
 *
 * The first arm a process runs is slow -- thread stacks, first touch, the core
 * winding up -- by enough to be mistaken for a schedule effect: it came out
 * three times its steady value twice before a warm-up pass was added. So one
 * unmeasured pass runs before any arm is timed.
 *
 * Every arm must produce the same checksum. A schedule that drops or repeats a
 * piece is fast and wrong, and with work claimed rather than assigned that is
 * exactly the failure to look for.
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

#include "cycles.h"
#include "report.h"

#define WORKERS_MAX 8

/* Load Atomic function code, Power ISA 3.0B Figure 3. */
enum { LOAD_ATOMIC_INCREMENT_BOUNDED = 24 };

/*
 * The counter and its limit adjacent, in that order, because Fetch and
 * Increment Bounded reads both: the limit is one doubleword above the counter
 * and nothing may sit between them.
 */
struct work_queue {
	volatile uint64_t next;
	volatile uint64_t limit;
	char pad[112];
};

static inline uint64_t claim(struct work_queue *q)
{
	uint64_t got;

	asm volatile("ldat %0, %1, %2"
		     : "=r"(got)
		     : "b"(&q->next), "i"(LOAD_ATOMIC_INCREMENT_BOUNDED)
		     : "memory");

	return got;
}

static inline bool claim_spent(uint64_t got)
{
	return (got >> 63) != 0;
}

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

static long pieces = 4096;
static long light_steps = 2000;
static long cluster = 256;
static bool uneven;
static bool claimed;

/*
 * How much work piece @i carries. A tenth of them carry ten times the rest,
 * arriving in runs of @cluster so that where a piece sits decides which worker
 * is unlucky.
 */
static long steps_for(long i)
{
	if (uneven && ((i / cluster) % 10) == 0)
		return light_steps * 10;

	return light_steps;
}

/*
 * A dependent chain, so the cost is the step count and not something the
 * machine can widen, and the result feeds a checksum so it cannot be deleted.
 */
static uint64_t piece_work(long i)
{
	uint64_t x = (uint64_t)i + 1;
	long n = steps_for(i);

	while (n--)
		x = x * 6364136223846793005UL + 1442695040888963407UL;

	return x;
}

static struct work_queue queue __attribute__((aligned(128)));

struct worker {
	pthread_t thread;
	int index;
	int cpu;
	uint64_t checksum;
	uint64_t pieces_done;
	uint64_t ticks;
	uint64_t cycles;
};

static struct worker workers[WORKERS_MAX];
static int workers_n = 4;
static volatile int go;

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	struct cycle_counter counter = CYCLE_COUNTER_INIT;
	uint64_t sum = 0, count = 0, t0, t1;

	pin_to(w->cpu);
	/* Its own, because the counter follows the thread that opened it. */
	cycles_open(&counter);

	while (!go)
		;

	cycles_start(&counter);
	t0 = now_tb();
	if (claimed) {
		for (;;) {
			uint64_t i = claim(&queue);

			if (claim_spent(i))
				break;
			sum += piece_work((long)i);
			count++;
		}
	} else {
		/* A partition decided before anything ran. */
		long share = pieces / workers_n;
		long first = (long)w->index * share;
		long last = w->index == workers_n - 1 ? pieces : first + share;
		long i;

		for (i = first; i < last; i++) {
			sum += piece_work(i);
			count++;
		}
	}
	t1 = now_tb();
	w->cycles = cycles_stop(&counter);
	cycles_close(&counter);

	w->checksum = sum;
	w->pieces_done = count;
	w->ticks = t1 - t0;

	return NULL;
}

int main(int argc, char **argv)
{
	int isolated[WORKERS_MAX * 4];
	int isolated_n;
	uint64_t expected = 0;
	double ns = tb_ns();
	long i;
	int arm, w;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--workers") && i + 1 < argc)
			workers_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pieces") && i + 1 < argc)
			pieces = atol(argv[++i]);
		else if (!strcmp(argv[i], "--steps") && i + 1 < argc)
			light_steps = atol(argv[++i]);
		else if (!strcmp(argv[i], "--cluster") && i + 1 < argc)
			cluster = atol(argv[++i]);
		else {
			fprintf(stderr,
				"usage: irregular [--workers N] [--pieces N] [--steps N]"
				" [--cluster N]\n");
			return 2;
		}
	}
	if (workers_n < 1 || workers_n > WORKERS_MAX || pieces < workers_n ||
	    cluster < 1)
		return 2;

	isolated_n = isolated_cpus(isolated, WORKERS_MAX * 4);
	if (isolated_n < workers_n) {
		fprintf(stderr, "refusing to run: %d isolated cpus, %d workers\n",
			isolated_n, workers_n);
		return 1;
	}

	printf("%ld pieces over %d workers on cpus", pieces, workers_n);
	for (w = 0; w < workers_n; w++)
		printf(" %d", isolated[w]);
	printf("\n  a light piece is %ld steps; where uneven, a tenth carry ten times that\n\n",
	       light_steps);
	printf("  %-10s %-12s %12s %14s %10s  %s\n", "work", "schedule",
	       "us to finish", "worker Gcycles", "idle", "checksum");

	/*
	 * One pass nobody looks at, so that whatever the first arm would have
	 * paid for is already paid.
	 */
	uneven = false;
	claimed = true;
	queue.next = 0;
	queue.limit = (uint64_t)pieces;
	go = 0;
	for (w = 0; w < workers_n; w++) {
		workers[w].index = w;
		workers[w].cpu = isolated[w];
		if (pthread_create(&workers[w].thread, NULL, worker_main,
				   &workers[w]))
			return 1;
	}
	usleep(5000);
	go = 1;
	for (w = 0; w < workers_n; w++)
		pthread_join(workers[w].thread, NULL);

	for (arm = 0; arm < 4; arm++) {
		uint64_t cycles = 0, t0, t1, first_done = ~0ULL, last_done = 0;
		uint64_t sum = 0, done = 0;

		uneven = (arm & 2) != 0;
		claimed = (arm & 1) != 0;

		/* The answer does not depend on the schedule; check that it does not. */
		expected = 0;
		for (i = 0; i < pieces; i++)
			expected += piece_work(i);

		queue.next = 0;
		queue.limit = (uint64_t)pieces;
		go = 0;

		for (w = 0; w < workers_n; w++) {
			workers[w].index = w;
			workers[w].cpu = isolated[w];
			workers[w].checksum = 0;
			workers[w].pieces_done = 0;
			workers[w].ticks = 0;
			if (pthread_create(&workers[w].thread, NULL,
					   worker_main, &workers[w]))
				return 1;
		}
		usleep(5000);

		t0 = now_tb();
		go = 1;
		for (w = 0; w < workers_n; w++)
			pthread_join(workers[w].thread, NULL);
		t1 = now_tb();

		for (w = 0; w < workers_n; w++) {
			cycles += workers[w].cycles;
			sum += workers[w].checksum;
			done += workers[w].pieces_done;
			if (workers[w].ticks < first_done)
				first_done = workers[w].ticks;
			if (workers[w].ticks > last_done)
				last_done = workers[w].ticks;
		}

		printf("  %-10s %-12s %12.1f %14.3f %9.1f%%  %s\n",
		       uneven ? "uneven" : "even",
		       claimed ? "claimed" : "partitioned",
		       (double)(t1 - t0) * ns / 1000.0,
		       (double)cycles / 1e9,
		       last_done ? 100.0 * (double)(last_done - first_done) /
				   (double)last_done : 0.0,
		       (sum == expected && done == (uint64_t)pieces) ?
			       "correct" : "WRONG");
		if (sum != expected || done != (uint64_t)pieces)
			printf("      %llu pieces done of %ld\n",
			       (unsigned long long)done, pieces);
		fflush(stdout);
	}

	return 0;
}
