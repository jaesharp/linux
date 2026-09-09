// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A tiling systolic ring, clocked by the switchboard.
 *
 * Each worker owns a row block of the result and holds one column block of the
 * right-hand matrix. At every step it multiplies what it holds into what it
 * owns, hands its column block to the next worker in the ring, and takes one
 * from the previous. After as many steps as there are workers, every column
 * block has visited every worker and the result is complete.
 *
 * That is a systolic schedule rather than a partition: no worker ever holds
 * the whole of the right-hand matrix, the working set is one tile, and the
 * arrangement is the same one a hardware array uses -- except that here the
 * cells are threads running independently, each on its own instruction stream,
 * advancing when its data arrives rather than on a shared clock.
 *
 * The handoff is what this is for. A step ends by telling the next worker that
 * a block is ready, and the ways of saying so differ by more than style:
 *
 *   - a flag the neighbour spins on, which costs a core to watch;
 *   - a paste to the neighbour's destination, which costs the sender about
 *     160 nanoseconds and lets the neighbour wait in a way that gives its
 *     core partner the whole machine.
 *
 * Both are here, with a static partition beside them as the baseline anybody
 * would write, and a single thread beneath that. A systolic schedule is only
 * worth its handoffs if it beats the partition, so the partition runs too.
 *
 * The result is checked against a straightforward triple loop. A pipeline that
 * drops a step is fast and wrong, and that is the failure this shape invites.
 *
 * Timing is in cycles: this machine moves its core frequency by a factor of
 * 1.78 between runs without asking, so nanoseconds describe two things at once.
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

#include "cycles.h"
#include "report.h"

#define WORKERS_MAX 8

enum handoff {
	HANDOFF_NONE,		/* static partition: no block ever moves */
	HANDOFF_SPIN,		/* a flag the neighbour polls */
	HANDOFF_WAKE,		/* a paste to the neighbour's destination */
};

static const char *handoff_name(enum handoff h)
{
	switch (h) {
	case HANDOFF_NONE:
		return "static partition";
	case HANDOFF_SPIN:
		return "ring, spin handoff";
	case HANDOFF_WAKE:
		return "ring, paste handoff";
	}

	return "unknown";
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

/* Square, and a multiple of the worker count so blocks divide evenly. */
static long order;
static long block;		/* order / workers */
static const float *matrix_a;
static const float *matrix_b;
static float *matrix_c;

/*
 * One worker's mailbox, on a line of its own: the step it may begin, written
 * by the worker behind it in the ring.
 */
struct mailbox {
	volatile uint64_t released;
	char pad[120];
};

static struct mailbox mailboxes[WORKERS_MAX];

struct worker {
	pthread_t thread;
	int index;
	int cpu;
	struct vas_destination *dest;	/* this worker waits here */
	struct vas_window *ahead;	/* window onto the next worker */
	enum handoff handoff;
	volatile int ready;
	volatile int go;
	int rc;
};

static struct worker workers[WORKERS_MAX];
static int workers_n = 4;
static volatile int stop;

/*
 * C[rows of this worker][columns of block bcol] += A[rows] x B[all][columns].
 *
 * Straightforward, and deliberately so: the schedule is what is being measured
 * and a hand-tuned kernel would only make the handoffs harder to see. The
 * innermost loop walks contiguous columns of both B and C, so it vectorises
 * and the tile stays in cache.
 */
static void multiply_block(int worker, long bcol)
{
	long row0 = (long)worker * block;
	long col0 = bcol * block;
	long i, j, k;

	for (i = row0; i < row0 + block; i++) {
		for (k = 0; k < order; k++) {
			float a = matrix_a[i * order + k];
			const float *brow = &matrix_b[k * order + col0];
			float *crow = &matrix_c[i * order + col0];

			for (j = 0; j < block; j++)
				crow[j] += a * brow[j];
		}
	}
}

static void *worker_main(void *arg)
{
	struct worker *w = arg;

	pin_to(w->cpu);

	if (w->handoff == HANDOFF_WAKE) {
		w->rc = vas_destination_open(vas_instance_any(), &w->dest);
		if (w->rc) {
			w->ready = -1;
			return NULL;
		}
	}

	w->ready = 1;

	while (!stop) {
		long step;

		while (!w->go && !stop) {
			if (w->handoff == HANDOFF_WAKE)
				vas_wait();
		}
		if (stop)
			break;

		for (step = 0; step < workers_n; step++) {
			/*
			 * The block this worker holds at this step. Starting
			 * each worker on a different one is what keeps every
			 * worker busy: no two ever want the same block.
			 */
			long bcol = (w->index + step) % workers_n;

			if (w->handoff != HANDOFF_NONE && step > 0) {
				/* Wait until the worker behind has released this step. */
				while (__atomic_load_n(&mailboxes[w->index].released,
						       __ATOMIC_ACQUIRE) <
				       (uint64_t)step) {
					if (w->handoff == HANDOFF_WAKE)
						vas_wait();
				}
			}

			multiply_block(w->index, bcol);

			/*
			 * The static arm does the same work in the same order
			 * and simply never synchronises: every worker walks
			 * every column block of its own rows. Anything else
			 * would compare different amounts of arithmetic.
			 */
			if (w->handoff == HANDOFF_NONE)
				continue;

			/* Release the worker ahead, then tell it so. */
			{
				int next = (w->index + 1) % workers_n;

				__atomic_store_n(&mailboxes[next].released,
						 (uint64_t)step + 1,
						 __ATOMIC_RELEASE);
				if (w->handoff == HANDOFF_WAKE)
					vas_wake(w->ahead);
			}
		}

		w->go = 0;
		__atomic_store_n(&mailboxes[w->index].released, 0,
				 __ATOMIC_RELEASE);
	}

	return NULL;
}

static void reference(float *out)
{
	long i, j, k;

	for (i = 0; i < order; i++)
		for (k = 0; k < order; k++) {
			float a = matrix_a[i * order + k];

			for (j = 0; j < order; j++)
				out[i * order + j] += a * matrix_b[k * order + j];
		}
}

/* No math library for one absolute value, and no doubt about what it does. */
static inline float magnitude(float x)
{
	return x < 0.0f ? -x : x;
}

/*
 * Relative, because the arms sum in different orders and floating point
 * addition is not associative -- an exact comparison would fail on a correct
 * result. Loose enough for reordering, far tighter than a dropped step.
 */
static bool matches(const float *got, const float *want)
{
	long n = order * order;
	long i;

	for (i = 0; i < n; i++) {
		float scale = magnitude(want[i]) + 1.0f;

		if (magnitude(got[i] - want[i]) > 1e-3f * scale)
			return false;
	}

	return true;
}

int main(int argc, char **argv)
{
	enum handoff arms[] = { HANDOFF_NONE, HANDOFF_SPIN, HANDOFF_WAKE };
	struct cycle_counter counter = CYCLE_COUNTER_INIT;
	float *a = NULL, *b = NULL, *c = NULL, *want = NULL;
	int isolated[WORKERS_MAX * 4];
	int isolated_n;
	double ns = tb_ns();
	size_t arm;
	long i;
	int w, rc;

	order = 512;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--workers") && i + 1 < argc)
			workers_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--order") && i + 1 < argc)
			order = atol(argv[++i]);
		else {
			fprintf(stderr,
				"usage: systolic [--workers N] [--order N]\n");
			return 2;
		}
	}
	if (workers_n < 1 || workers_n > WORKERS_MAX)
		return 2;
	if (order % workers_n) {
		fprintf(stderr,
			"order %ld is not a multiple of %d workers; blocks would not divide\n",
			order, workers_n);
		return 2;
	}
	block = order / workers_n;

	isolated_n = isolated_cpus(isolated, WORKERS_MAX * 4);
	if (isolated_n < workers_n) {
		fprintf(stderr, "refusing to run: %d isolated cpus, %d workers\n",
			isolated_n, workers_n);
		return 1;
	}

	a = aligned_alloc(128, (size_t)order * order * sizeof(*a));
	b = aligned_alloc(128, (size_t)order * order * sizeof(*b));
	c = aligned_alloc(128, (size_t)order * order * sizeof(*c));
	want = aligned_alloc(128, (size_t)order * order * sizeof(*want));
	if (!a || !b || !c || !want)
		return 1;

	for (i = 0; i < order * order; i++) {
		a[i] = (float)((i * 7 + 3) % 17) - 8.0f;
		b[i] = (float)((i * 5 + 11) % 13) - 6.0f;
	}
	matrix_a = a;
	matrix_b = b;
	matrix_c = c;

	memset(want, 0, (size_t)order * order * sizeof(*want));
	reference(want);

	if (cycles_open(&counter))
		printf("no cycle counter available; reporting time only\n\n");

	printf("%ld by %ld, blocks of %ld, %d workers on cpus",
	       order, order, block, workers_n);
	for (w = 0; w < workers_n; w++)
		printf(" %d", isolated[w]);
	printf("\n\n  %-22s %16s %12s %8s  %s\n", "arm", "cycles/multiply-add",
	       "ns/element", "GHz", "result");

	/* One thread, for scale. */
	{
		unsigned long long cycles, t0, t1;

		memset(c, 0, (size_t)order * order * sizeof(*c));
		cycles_start(&counter);
		t0 = now_tb();
		reference(c);
		t1 = now_tb();
		cycles = cycles_stop(&counter);

		printf("  %-22s %16.3f %12.3f %8.2f  %s\n", "one thread",
		       (double)cycles / (double)(order * order * order),
		       (double)(t1 - t0) * ns / (double)(order * order),
		       (t1 - t0) ? (double)cycles / ((double)(t1 - t0) * ns) : 0.0,
		       matches(c, want) ? "correct" : "WRONG");
	}

	for (arm = 0; arm < sizeof(arms) / sizeof(arms[0]); arm++) {
		enum handoff this = arms[arm];
		unsigned long long cycles, t0, t1;

		memset(c, 0, (size_t)order * order * sizeof(*c));
		memset(mailboxes, 0, sizeof(mailboxes));
		stop = 0;

		for (w = 0; w < workers_n; w++) {
			workers[w].index = w;
			workers[w].cpu = isolated[w];
			workers[w].handoff = this;
			workers[w].ready = 0;
			workers[w].go = 0;
			workers[w].ahead = NULL;
			if (pthread_create(&workers[w].thread, NULL,
					   worker_main, &workers[w]))
				return 1;
			while (!workers[w].ready)
				;
			if (workers[w].ready < 0) {
				report_errno("open a destination", workers[w].rc);
				return workers[w].rc == -ENODEV ? 77 : 1;
			}
		}

		/* Each worker gets a window onto the one ahead of it. */
		if (this == HANDOFF_WAKE) {
			for (w = 0; w < workers_n; w++) {
				struct vas_window_attr attr;
				int next = (w + 1) % workers_n;

				vas_window_attr_init(&attr, VAS_COP_FTW);
				attr.wake_target =
					vas_destination_fd(workers[next].dest);
				rc = vas_window_open(&attr, &workers[w].ahead);
				if (rc) {
					report_errno("open a window onto the next worker",
						     rc);
					return 1;
				}
			}
		}

		cycles_start(&counter);
		t0 = now_tb();
		for (w = 0; w < workers_n; w++)
			workers[w].go = 1;
		if (this == HANDOFF_WAKE)
			for (w = 0; w < workers_n; w++)
				vas_wake(workers[w].ahead);
		for (w = 0; w < workers_n; w++)
			while (workers[w].go)
				;
		t1 = now_tb();
		cycles = cycles_stop(&counter);

		stop = 1;
		for (w = 0; w < workers_n; w++) {
			if (this == HANDOFF_WAKE && workers[w].ahead)
				vas_wake(workers[w].ahead);
			pthread_join(workers[w].thread, NULL);
			if (workers[w].ahead)
				vas_window_close(&workers[w].ahead);
			if (workers[w].dest)
				vas_destination_close(&workers[w].dest);
		}

		printf("  %-22s %16.3f %12.3f %8.2f  %s\n", handoff_name(this),
		       (double)cycles / (double)(order * order * order),
		       (double)(t1 - t0) * ns / (double)(order * order),
		       (t1 - t0) ? (double)cycles / ((double)(t1 - t0) * ns) : 0.0,
		       matches(c, want) ? "correct" : "WRONG");
		fflush(stdout);
	}

	cycles_close(&counter);
	free(a);
	free(b);
	free(c);
	free(want);

	return 0;
}
