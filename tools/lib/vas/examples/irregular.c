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

/* Every thread this machine brings up, with room for a larger one. */
#define WORKERS_MAX 128

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

/*
 * Where the workers go.
 *
 * The order matters as much as the count. Filling the isolated CPUs first and
 * then the rest by number puts the first two workers on two threads of one
 * core and the seventh on the far chip, so a scaling curve built that way
 * measures the accidents of CPU numbering: four workers on two cores is not
 * four cores, and a run that crosses a chip at seven workers has a step in it
 * that has nothing to do with scaling.
 *
 * So workers are placed on distinct cores first, one chip before the next, and
 * only then on the second thread of a core already in use. Each added worker
 * is the next least contended place to put one, and the curve is about the
 * work rather than the enumeration.
 *
 * Isolation matters most when the quantity is a wake, because a resume that
 * was really an interrupt is indistinguishable from one that was not. Here the
 * quantity is how long a fixed amount of arithmetic takes, so a housekeeping
 * CPU adds variance rather than a false signal -- worth having in order to
 * reach a worker count the isolated set alone cannot.
 */
struct placement {
	int cpu;
	int chip;		/* physical package */
	bool secondary;		/* not the first thread of its core */
	bool isolated;
};

static int read_first_int(const char *path, int *out)
{
	FILE *f = fopen(path, "r");
	int got;

	if (!f)
		return -1;
	got = fscanf(f, "%d", out);
	fclose(f);

	return got == 1 ? 0 : -1;
}

/* The lowest-numbered thread of a core is the one to fill first. */
static bool is_secondary(int cpu)
{
	char path[128];
	int first = cpu;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
		 cpu);
	if (read_first_int(path, &first))
		return false;

	return first != cpu;
}

static int chip_of(int cpu)
{
	char path[128];
	int chip = 0;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
		 cpu);
	read_first_int(path, &chip);

	return chip;
}

/*
 * Distinct cores before shared ones, isolated before not, one chip before the
 * next. The isolated test has to come before the chip and the number, or the
 * lowest-numbered CPU wins -- and the lowest-numbered CPUs are exactly the
 * ones left for housekeeping, so the first worker would land on the busiest
 * place in the machine.
 */
static int by_contention(const void *a, const void *b)
{
	const struct placement *x = a, *y = b;

	if (x->secondary != y->secondary)
		return x->secondary - y->secondary;
	if (x->isolated != y->isolated)
		return y->isolated - x->isolated;
	if (x->chip != y->chip)
		return x->chip - y->chip;

	return x->cpu - y->cpu;
}
static int cpu_list(const char *path, int *cpus, int max)
{
	char buf[4096];
	int n = 0;
	FILE *f;
	char *p;

	f = fopen(path, "r");
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
	static struct placement order[WORKERS_MAX * 4];
	int isolated[WORKERS_MAX * 4];
	int all[WORKERS_MAX * 4];
	int isolated_n, placed, online;
	uint64_t expected = 0;
	double ns = tb_ns();
	long i;
	int arm, w, c;

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


	isolated_n = cpu_list("/sys/devices/system/cpu/isolated", isolated,
			      WORKERS_MAX * 4);
	/*
	 * The online numbers, not a count of them: this machine numbers its
	 * CPUs to 141 and brings up 72 of them, so counting would place
	 * workers on CPUs that are not there -- and pinning to one of those
	 * fails quietly, leaving the thread wherever it already was.
	 */
	online = cpu_list("/sys/devices/system/cpu/online", all, WORKERS_MAX * 4);
	for (c = 0; c < online; c++) {
		int k;

		order[c].cpu = all[c];
		order[c].chip = chip_of(all[c]);
		order[c].secondary = is_secondary(all[c]);
		order[c].isolated = false;
		for (k = 0; k < isolated_n; k++)
			if (isolated[k] == all[c])
				order[c].isolated = true;
	}
	qsort(order, (size_t)online, sizeof(order[0]), by_contention);
	placed = online;

	if (placed < workers_n) {
		fprintf(stderr, "refusing to run: %d cpus placed, %d workers\n",
			placed, workers_n);
		return 1;
	}

	{
		int cores = 0, chips = 0, isolated_used = 0;
		int seen[WORKERS_MAX];

		/*
		 * Chip numbers are not a dense range -- this machine calls its
		 * two chips 0 and 8 -- so they are collected rather than used
		 * as indices. Indexing by them counted one chip of two.
		 */
		for (w = 0; w < workers_n; w++) {
			int k;
			bool known = false;

			if (!order[w].secondary)
				cores++;
			if (order[w].isolated)
				isolated_used++;
			for (k = 0; k < chips; k++)
				if (seen[k] == order[w].chip)
					known = true;
			if (!known)
				seen[chips++] = order[w].chip;
		}
		printf("%ld pieces over %d workers on %d cores of %d chips,"
		       " %d sharing a core, %d on isolated cpus",
		       pieces, workers_n, cores, chips, workers_n - cores,
		       isolated_used);
	}
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
		workers[w].cpu = order[w].cpu;
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
			workers[w].cpu = order[w].cpu;
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
