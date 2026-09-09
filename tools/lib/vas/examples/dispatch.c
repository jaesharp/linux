// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Waking a working set, round after round.
 *
 * The pieces have been measured apart: a paste reaches every thread sharing a
 * destination on the chip that issued the notify, a thread parked in wait
 * costs its core partner nothing, and work claimed against a bounded counter
 * scales where a fixed partition does not. This puts them together and asks
 * whether they still hold when there are thirty-two workers rather than four.
 *
 * The arrangement follows from what the notify will and will not do. It does
 * not cross chips, so a machine of two chips is two groups, each with its own
 * destination and its own paste -- not a detail to be discovered later, since
 * a single group would silently leave half the workers asleep and the run
 * would merely look slow.
 *
 * What is measured is two things that a single figure would confuse. Dispatch
 * is how long after the paste each worker came back, recorded by the worker
 * itself with an atomic Store Minimum so that the earliest resume wins and no
 * worker can be counted twice. Throughput is how long the round took from the
 * paste to the last worker finishing. A dispatch that is prompt for the first
 * worker and late for the thirty-second is a different thing from one that is
 * uniformly slower, and only the spread shows which.
 *
 * How many arrived late matters as much as how late the last one was. A
 * maximum cannot tell one straggler from a dozen, and the difference is the
 * whole question when cores hold two waiters each: a notify that reaches one
 * thread of a core and not its partner would leave exactly as many stragglers
 * as there are shared cores.
 *
 * A woken worker does not yet know what to do. It has to read the descriptor
 * the sender wrote, which is a miss on a line another chip may own, and only
 * then can it start. That read is measured separately here because it is the
 * part a payload would remove: the hundred and twenty-eight bytes a paste
 * carries are discarded by this window type and by nothing in the hardware, so
 * what the read costs is what a message-carrying window would save.
 *
 * The control starts the same workers on a flag they poll instead. It costs a
 * core to watch, which is the thing the wake is supposed to save, so the
 * comparison is not which is faster but what the wake costs to be free.
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

#define WORKERS_MAX 128
#define GROUPS_MAX 8

/* Atomic function codes, Power ISA 3.0B Figures 3 and 4. */
enum { LOAD_ATOMIC_INCREMENT_BOUNDED = 24 };
enum { STORE_ATOMIC_ADD = 0, STORE_ATOMIC_MIN_UNSIGNED = 6 };

struct work_queue {
	volatile uint64_t next;
	volatile uint64_t limit;
	char pad[112];
};

struct cell {
	volatile uint64_t value;
	char pad[120];
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

static inline void fold(volatile uint64_t *m, uint64_t v)
{
	asm volatile("stdat %0, %1, %2"
		     :: "r"(v), "b"(m), "i"(STORE_ATOMIC_ADD) : "memory");
}

static inline void keep_earliest(volatile uint64_t *m, uint64_t v)
{
	asm volatile("stdat %0, %1, %2"
		     :: "r"(v), "b"(m), "i"(STORE_ATOMIC_MIN_UNSIGNED)
		     : "memory");
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

struct placement {
	int cpu;
	int chip;
	bool secondary;
	bool isolated;
};

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

static void pin_to(int cpu)
{
	cpu_set_t set;

	if (cpu < 0)
		return;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

/* A resume later than this did not come from the paste. */
#define PROMPT_NS 10000.0

static long pieces = 8192;
static long steps = 2000;
static bool by_wake;

static struct work_queue queue __attribute__((aligned(128)));
static struct cell finished __attribute__((aligned(128)));
static struct cell *woke_at;
static struct cell *knew_at;	/* when the descriptor had been read */
static volatile uint64_t round_number;
static volatile int stop;

static uint64_t piece_work(long i)
{
	uint64_t x = (uint64_t)i + 1;
	long n = ((i / 256) % 10) == 0 ? steps * 10 : steps;

	while (n--)
		x = x * 6364136223846793005UL + 1442695040888963407UL;

	return x;
}

struct worker {
	pthread_t thread;
	int index;
	int cpu;
	int chip;
	int group;
	int join_fd;			/* -1 for the thread that opens it */
	struct vas_destination *dest;
	volatile int ready;
	uint64_t checksum;
	int rc;
};

static struct worker workers[WORKERS_MAX];
static int workers_n = 8;

static void *worker_main(void *arg)
{
	struct worker *w = arg;
	uint64_t seen = 0;

	pin_to(w->cpu);

	if (by_wake) {
		w->rc = w->join_fd < 0 ?
			vas_destination_open(vas_instance_any(), &w->dest) :
			vas_destination_join(w->join_fd, &w->dest);
		if (w->rc) {
			w->ready = -1;
			return NULL;
		}
	}

	w->ready = 1;

	while (!stop) {
		uint64_t resumed, now;

		if (by_wake)
			vas_wait();

		/* The clock first: deciding costs a line the sender just wrote. */
		resumed = now_tb();

		now = __atomic_load_n(&round_number, __ATOMIC_ACQUIRE);
		if (now == seen)
			continue;
		seen = now;
		if (stop)
			break;

		keep_earliest(&woke_at[w->index].value, resumed);
		keep_earliest(&knew_at[w->index].value, now_tb());

		for (;;) {
			uint64_t i = claim(&queue);

			if (claim_spent(i))
				break;
			w->checksum += piece_work((long)i);
		}

		fold(&finished.value, 1);
	}

	return NULL;
}

int main(int argc, char **argv)
{
	static struct placement order[WORKERS_MAX * 4];
	struct vas_window *group_window[GROUPS_MAX] = { NULL };
	int group_chip[GROUPS_MAX];
	int isolated[WORKERS_MAX * 4], all[WORKERS_MAX * 4];
	int isolated_n, online, groups = 0;
	int rounds = 20;
	double ns = tb_ns();
	int arm, i, w, g, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--workers") && i + 1 < argc)
			workers_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--pieces") && i + 1 < argc)
			pieces = atol(argv[++i]);
		else if (!strcmp(argv[i], "--steps") && i + 1 < argc)
			steps = atol(argv[++i]);
		else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
			rounds = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: dispatch [--workers N] [--pieces N] [--steps N]"
				" [--rounds N]\n");
			return 2;
		}
	}
	if (workers_n < 1 || workers_n > WORKERS_MAX || pieces < 1)
		return 2;

	isolated_n = cpu_list("/sys/devices/system/cpu/isolated", isolated,
			      WORKERS_MAX * 4);
	online = cpu_list("/sys/devices/system/cpu/online", all,
			  WORKERS_MAX * 4);
	for (i = 0; i < online; i++) {
		int k;

		order[i].cpu = all[i];
		order[i].chip = chip_of(all[i]);
		order[i].secondary = is_secondary(all[i]);
		order[i].isolated = false;
		for (k = 0; k < isolated_n; k++)
			if (isolated[k] == all[i])
				order[i].isolated = true;
	}
	qsort(order, (size_t)online, sizeof(order[0]), by_contention);
	if (online < workers_n) {
		fprintf(stderr, "refusing to run: %d cpus, %d workers\n",
			online, workers_n);
		return 1;
	}

	woke_at = aligned_alloc(128, (size_t)workers_n * sizeof(*woke_at));
	knew_at = aligned_alloc(128, (size_t)workers_n * sizeof(*knew_at));
	if (!woke_at || !knew_at)
		return 1;

	/* One group per chip, because a notify does not leave the chip that made it. */
	for (w = 0; w < workers_n; w++) {
		workers[w].index = w;
		workers[w].cpu = order[w].cpu;
		workers[w].chip = order[w].chip;
		workers[w].group = -1;
		for (g = 0; g < groups; g++)
			if (group_chip[g] == workers[w].chip)
				workers[w].group = g;
		if (workers[w].group < 0 && groups < GROUPS_MAX) {
			group_chip[groups] = workers[w].chip;
			workers[w].group = groups++;
		}
	}

	printf("%d workers over %d chips, so %d group%s and %d paste%s a round\n",
	       workers_n, groups, groups, groups == 1 ? "" : "s",
	       groups, groups == 1 ? "" : "s");
	printf("%ld pieces of %ld steps, %d rounds\n\n", pieces, steps, rounds);
	printf("  %-14s %11s %11s %11s %10s %14s  %s\n", "start", "round us",
	       "first ns", "last ns", "spread ns", "descriptor ns", "result");

	for (arm = 0; arm < 2; arm++) {
		double round_us = 0.0, first_ns = 0.0, last_ns = 0.0;
		double read_ns = 0.0;
		uint64_t expected = 0, total = 0;
		int r, reached_total = 0, late_total = 0;

		by_wake = arm == 1;
		stop = 0;
		round_number = 0;
		groups = 0;

		for (i = 0; i < pieces; i++)
			expected += piece_work(i);

		for (w = 0; w < workers_n; w++) {
			workers[w].ready = 0;
			workers[w].checksum = 0;
			workers[w].join_fd = -1;
			workers[w].group = -1;
			for (g = 0; g < groups; g++)
				if (group_chip[g] == workers[w].chip)
					workers[w].group = g;
			if (workers[w].group < 0) {
				group_chip[groups] = workers[w].chip;
				workers[w].group = groups++;
			}
		}

		/* The first worker of a chip opens; the rest of that chip join. */
		for (g = 0; g < groups; g++) {
			int leader = -1;

			for (w = 0; w < workers_n; w++) {
				if (workers[w].group != g)
					continue;
				/* Only the wake arm has destinations to join. */
				workers[w].join_fd = (!by_wake || leader < 0) ? -1 :
					vas_destination_fd(workers[leader].dest);
				if (pthread_create(&workers[w].thread, NULL,
						   worker_main, &workers[w]))
					return 1;
				while (!workers[w].ready)
					;
				if (workers[w].ready < 0) {
					report_errno("open or join a destination",
						     workers[w].rc);
					return workers[w].rc == -ENODEV ? 77 : 1;
				}
				if (leader < 0)
					leader = w;
			}

			if (by_wake) {
				struct vas_window_attr attr;

				vas_window_attr_init(&attr, VAS_COP_FTW);
				attr.wake_target =
					vas_destination_fd(workers[leader].dest);
				rc = vas_window_open(&attr, &group_window[g]);
				if (rc) {
					report_errno("open a window onto a group", rc);
					return 1;
				}
			}
		}

		for (r = 0; r < rounds; r++) {
			uint64_t sent, done_at;
			double earliest = 0.0, latest = 0.0, read_sum = 0.0;
			int reached = 0, late = 0;

			for (w = 0; w < workers_n; w++) {
				woke_at[w].value = UINT64_MAX;
				knew_at[w].value = UINT64_MAX;
			}
			finished.value = 0;
			queue.next = 0;
			queue.limit = (uint64_t)pieces;

			usleep(1000);

			sent = now_tb();
			__atomic_store_n(&round_number, (uint64_t)(r + 1),
					 __ATOMIC_RELEASE);
			if (by_wake)
				for (g = 0; g < groups; g++)
					vas_wake(group_window[g]);

			while (__atomic_load_n(&finished.value,
					       __ATOMIC_ACQUIRE) <
			       (uint64_t)workers_n)
				;
			done_at = now_tb();

			for (w = 0; w < workers_n; w++) {
				double us;

				if (woke_at[w].value == UINT64_MAX ||
				    woke_at[w].value < sent)
					continue;
				us = (double)(woke_at[w].value - sent) * ns;
				reached++;
				if (us > PROMPT_NS)
					late++;
				if (!earliest || us < earliest)
					earliest = us;
				if (us > latest)
					latest = us;
				if (knew_at[w].value != UINT64_MAX &&
				    knew_at[w].value >= woke_at[w].value)
					read_sum += (double)(knew_at[w].value -
							     woke_at[w].value) * ns;
			}

			/* The first round pays for what the rest do not. */
			if (r) {
				round_us += (double)(done_at - sent) * ns / 1000.0;
				first_ns += earliest;
				last_ns += latest;
				reached_total += reached;
				late_total += late;
				if (reached)
					read_ns += read_sum / reached;
			}
		}

		stop = 1;
		__atomic_store_n(&round_number, (uint64_t)(rounds + 2),
				 __ATOMIC_RELEASE);
		for (g = 0; g < groups; g++)
			if (group_window[g])
				vas_wake(group_window[g]);
		for (w = 0; w < workers_n; w++) {
			pthread_join(workers[w].thread, NULL);
			total += workers[w].checksum;
			if (workers[w].dest)
				vas_destination_close(&workers[w].dest);
		}
		for (g = 0; g < groups; g++)
			if (group_window[g])
				vas_window_close(&group_window[g]);

		printf("  %-14s %11.1f %11.0f %11.0f %10.0f %14.0f  %s\n",
		       by_wake ? "paste" : "flag (control)",
		       round_us / (rounds - 1),
		       first_ns / (rounds - 1), last_ns / (rounds - 1),
		       (last_ns - first_ns) / (rounds - 1),
		       read_ns / (rounds - 1),
		       total == expected * (uint64_t)rounds ? "correct" : "WRONG");
		printf("  %-14s %d of %d reached, %.1f of them later than %.0f us\n",
		       "", reached_total / (rounds - 1), workers_n,
		       (double)late_total / (rounds - 1), PROMPT_NS / 1000.0);
		fflush(stdout);
	}

	free(woke_at);
	free(knew_at);

	return 0;
}
