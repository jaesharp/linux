// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Saying something in the operation that wakes the reader.
 *
 * A paste carries a hundred and twenty-eight bytes whether or not anyone keeps
 * them. A destination that only wakes discards them, so a sender with
 * something to say leaves it in memory and the woken thread fetches it -- and
 * that fetch was measured here at more than the wake itself, and growing with
 * the group where the wake does not, because the notify is a broadcast and a
 * line the sender wrote is not.
 *
 * A destination that keeps a queue is given those bytes instead. This measures
 * whether that is worth having, by timing the same thing both ways:
 *
 *   - the reader is woken, then reads a descriptor the sender left in shared
 *     memory, which is what a program has to do today;
 *   - the reader is woken and finds what the sender said already in its queue.
 *
 * What is timed is not the wake but the arrival of the content -- from the
 * sender's paste to the reader holding the bytes -- because that is what a
 * program waits for, and the two differ by exactly the fetch that one of them
 * does not do.
 *
 * The content is checked, not assumed. A queue that delivered nothing would
 * look like a very fast one.
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

#include "environment.h"
#include "report.h"

/* Store Atomic function code, Power ISA 3.0B Figure 4. */
enum { STORE_ATOMIC_MIN_UNSIGNED = 6 };

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

static inline void keep_earliest(volatile uint64_t *m, uint64_t v)
{
	asm volatile("stdat %0, %1, %2"
		     :: "r"(v), "b"(m), "i"(STORE_ATOMIC_MIN_UNSIGNED)
		     : "memory");
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

/*
 * What the sender says. The first word is the trial, so the reader can tell a
 * message of this round from one left over; the rest is a pattern it checks,
 * because a queue that delivered nothing would otherwise look like the fastest
 * of the two.
 */
struct payload {
	uint64_t trial;
	uint64_t pattern[15];
} __attribute__((aligned(128)));

struct arrival {
	volatile uint64_t when;
	char pad[120];
};

static volatile uint64_t round_number;
static volatile int stop;
static struct arrival arrived __attribute__((aligned(128)));
static struct payload shared __attribute__((aligned(128)));
static volatile int wrong;

struct reader {
	pthread_t thread;
	int cpu;
	bool queued;
	struct vas_destination *dest;
	volatile int ready;
	int rc;
};

static bool payload_correct(const struct payload *p, uint64_t trial)
{
	int i;

	if (p->trial != trial)
		return false;
	for (i = 0; i < 15; i++)
		if (p->pattern[i] != trial * 1000003UL + (uint64_t)i)
			return false;

	return true;
}

static void payload_fill(struct payload *p, uint64_t trial)
{
	int i;

	for (i = 0; i < 15; i++)
		p->pattern[i] = trial * 1000003UL + (uint64_t)i;
	/*
	 * The trial last, so a reader that sees it can rely on the rest --
	 * for the shared-memory arm, where nothing else orders them.
	 */
	__atomic_store_n(&p->trial, trial, __ATOMIC_RELEASE);
}

static void *reader_main(void *arg)
{
	struct reader *r = arg;
	uint64_t seen = 0;

	pin_to(r->cpu);

	r->rc = r->queued ?
		vas_destination_open_queued(vas_instance_any(), 0, &r->dest) :
		vas_destination_open(vas_instance_any(), &r->dest);
	if (r->rc) {
		r->ready = -1;
		return NULL;
	}

	r->ready = 1;

	while (!stop) {
		vas_wait();

		if (r->queued) {
			const void *msg = vas_destination_next(r->dest);

			if (!msg)
				continue;
			/*
			 * Stamped after reading the content, exactly as the
			 * other arm is. Stamping when the entry is merely
			 * found would time finding it against the other arm's
			 * finding *and* reading, and flatter this one by the
			 * difference.
			 */
			if (!payload_correct(msg, round_number))
				wrong = 1;
			keep_earliest(&arrived.when, now_tb());
			vas_destination_release(r->dest, msg);
			seen = round_number;
		} else {
			uint64_t now = __atomic_load_n(&round_number,
						       __ATOMIC_ACQUIRE);

			if (now == seen)
				continue;
			/*
			 * The other arm has to go and read what the sender
			 * left, and only then does it hold the content.
			 */
			if (!payload_correct(&shared, now))
				wrong = 1;
			keep_earliest(&arrived.when, now_tb());
			seen = now;
		}
	}

	return NULL;
}

int main(int argc, char **argv)
{
	static struct payload outgoing __attribute__((aligned(128)));
	struct environment env;
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	struct reader reader;
	double ns = tb_ns();
	int trials = 2000;
	int send_cpu = -1, wait_cpu = -1;
	int arm, i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--trials") && i + 1 < argc)
			trials = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--send-cpu") && i + 1 < argc)
			send_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--wait-cpu") && i + 1 < argc)
			wait_cpu = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: message [--trials N] [--send-cpu N] [--wait-cpu N]\n");
			return 2;
		}
	}

	if (environment_require(&env, 0, ENVIRONMENT_STEADY_CLOCK))
		return 1;

	pin_to(send_cpu);

	printf("  %-22s %12s %12s %10s  %s\n", "how the content arrives",
	       "median ns", "90th ns", "trials", "content");

	for (arm = 0; arm < 2; arm++) {
		uint64_t *samples;
		int taken = 0;

		memset(&reader, 0, sizeof(reader));
		reader.cpu = wait_cpu;
		reader.queued = arm == 1;
		stop = 0;
		wrong = 0;
		round_number = 0;

		samples = calloc((size_t)trials, sizeof(*samples));
		if (!samples)
			return 1;

		if (pthread_create(&reader.thread, NULL, reader_main, &reader)) {
			report_errno("pthread_create", -errno);
			return 1;
		}
		while (!reader.ready)
			;
		if (reader.ready < 0) {
			report_errno("open a destination", reader.rc);
			return reader.rc == -ENODEV ? 77 : 1;
		}

		vas_window_attr_init(&attr, VAS_COP_FTW);
		attr.wake_target = vas_destination_fd(reader.dest);
		rc = vas_window_open(&attr, &window);
		if (rc) {
			report_errno("open a window onto the destination", rc);
			return 1;
		}

		for (i = 0; i < trials; i++) {
			uint64_t sent;
			volatile long spin;

			arrived.when = UINT64_MAX;
			for (spin = 0; spin < 200; spin++)
				;

			payload_fill(&outgoing, (uint64_t)(i + 1));
			if (!reader.queued)
				payload_fill(&shared, (uint64_t)(i + 1));

			sent = now_tb();
			__atomic_store_n(&round_number, (uint64_t)(i + 1),
					 __ATOMIC_RELEASE);
			rc = reader.queued ? vas_send(window, &outgoing) :
					     vas_wake(window);
			if (rc) {
				report_errno("send", rc);
				break;
			}

			while (arrived.when == UINT64_MAX)
				;
			if (arrived.when >= sent)
				samples[taken++] = arrived.when - sent;
		}

		stop = 1;
		round_number = (uint64_t)trials + 2;
		payload_fill(&shared, round_number);
		payload_fill(&outgoing, round_number);
		vas_send(window, &outgoing);
		pthread_join(reader.thread, NULL);
		vas_window_close(&window);
		vas_destination_close(&reader.dest);

		if (taken) {
			uint64_t a, b;
			int j, k;

			/* Small and only sorted to take two quantiles. */
			for (j = 1; j < taken; j++) {
				uint64_t v = samples[j];

				for (k = j - 1; k >= 0 && samples[k] > v; k--)
					samples[k + 1] = samples[k];
				samples[k + 1] = v;
			}
			a = samples[taken / 2];
			b = samples[(taken * 9) / 10];
			printf("  %-22s %12.0f %12.0f %10d  %s\n",
			       reader.queued ? "in the paste" : "read from memory",
			       (double)a * ns, (double)b * ns, taken,
			       wrong ? "WRONG" : "correct");
		}
		free(samples);
	}

	if (environment_verify(&env))
		return 1;

	return 0;
}
