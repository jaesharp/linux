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
 * Placement is chosen and named rather than passed in. Where the two ends sit
 * decides where each arm reads from: the descriptor is written by the sender
 * and so lives in the sender's cache, while the queue is written by the
 * switchboard into memory the kernel allocated near the reader. That asymmetry
 * is the effect, not a flaw, but a run that does not say which pair of CPUs it
 * used cannot be compared with another.
 *
 * Where the bytes are when the reader gets to them is a separate question, and
 * the one that decides whether a queue can ever be the faster path. If the
 * switchboard's write lands in a cache the reader can hit, touching the entry
 * costs what a hit costs; if it goes to memory, the reader pays a miss for
 * data that was already on its chip. So the first touch of the entry is timed
 * on its own, against two references taken on the same thread: a line it has
 * just written, and a line pushed out of every cache with dcbf.
 *
 * And the wait for it is bounded. A reader that never answers is the most
 * likely thing to go wrong when the mechanism under test is new, and an
 * unbounded wait turns that into a hang with two threads spinning and nothing
 * said -- which is exactly what it did the first time it was run.
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

/* How long one trial may take before it is called unanswered. */
#define PATIENCE_US 1000

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
 * What a hit and a miss cost this thread, measured on its own memory so the
 * entry's first touch has something to be compared with. The flushed arm uses
 * dcbf, which pushes the line out of every cache that holds it, so what
 * follows is a fetch from memory.
 */
#define REFERENCE_RUNS 512

static int by_value(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

static void reference_costs(double ns, double *hot, double *cold)
{
	static _Alignas(128) volatile uint64_t line[16];
	uint64_t warm[REFERENCE_RUNS], flushed[REFERENCE_RUNS], t0;
	int i;

	for (i = 0; i < REFERENCE_RUNS; i++) {
		uint64_t v;

		line[0] = (uint64_t)i;			/* now certainly held */
		t0 = now_tb();
		v = line[0];
		warm[i] = now_tb() - t0;

		asm volatile("sync" ::: "memory");
		asm volatile("dcbf 0,%0" :: "r"(&line[0]) : "memory");
		asm volatile("sync" ::: "memory");
		t0 = now_tb();
		v += line[0];
		flushed[i] = now_tb() - t0;
		line[1] = v;
	}

	qsort(warm, REFERENCE_RUNS, sizeof(warm[0]), by_value);
	qsort(flushed, REFERENCE_RUNS, sizeof(flushed[0]), by_value);
	*hot = (double)warm[REFERENCE_RUNS / 2] * ns;
	*cold = (double)flushed[REFERENCE_RUNS / 2] * ns;
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
/*
 * Timing the first touch costs two reads of the timebase on every poll, which
 * is more than the load they bracket, so the arrival time and the first touch
 * are never measured in the same run.
 */
static bool measure_touch;
static struct arrival arrived __attribute__((aligned(128)));
/*
 * The first touch of round N, written only by the reader and read only once
 * the run is over. A cell the sender also wrote each round would be a second
 * line moving between the two threads on the path being timed.
 */
static uint64_t *touch_by_round;
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
			const void *msg;
			uint64_t t0, t1;

			/*
			 * The first touch of the entry's line, timed alone.
			 * next() reads one word of it and nothing else, so
			 * this is what it costs to reach the switchboard's
			 * write -- a hit if it landed in a cache here, a miss
			 * if it went to memory.
			 */
			t0 = measure_touch ? now_tb() : 0;
			msg = vas_destination_next(r->dest);
			t1 = measure_touch ? now_tb() : 0;

			if (!msg)
				continue;
			if (measure_touch)
				touch_by_round[round_number] = t1 - t0;
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
			 * left, and only then does it hold the content. Its
			 * first touch is bracketed the same way and reads the
			 * same one word, so the two arms are compared on where
			 * the content was found and not on how much of it each
			 * goes on to read.
			 */
			if (measure_touch) {
				uint64_t t0 = now_tb();

				(void)*(volatile const uint64_t *)&shared.trial;
				touch_by_round[now] = now_tb() - t0;
			}
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
	double hot = 0.0, cold = 0.0;
	enum environment_relation places[] = {
		ENVIRONMENT_SAME_CORE, ENVIRONMENT_SHARED_CACHE,
		ENVIRONMENT_SAME_CHIP, ENVIRONMENT_OTHER_CHIP,
	};
	int trials = 2000;
	int send_cpu = -1, wait_cpu = -1;
	size_t place;
	int arm, i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--trials") && i + 1 < argc)
			trials = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--send-cpu") && i + 1 < argc)
			send_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--wait-cpu") && i + 1 < argc)
			wait_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--everywhere"))
			send_cpu = -2;
		else if (!strcmp(argv[i], "--touch"))
			measure_touch = true;
		else {
			fprintf(stderr,
				"usage: message [--trials N] [--send-cpu N] [--wait-cpu N] [--touch]\n");
			return 2;
		}
	}

	if (environment_require(&env, 0, ENVIRONMENT_STEADY_CLOCK))
		return 1;

	/*
	 * Every relationship the machine offers, from an anchor that can fill
	 * them, unless a pair was named outright.
	 */
	if (send_cpu == -2) {
		send_cpu = env.isolated_n ? env.isolated[0] : env.online[0];
		wait_cpu = -1;
	}

	if (measure_touch) {
		reference_costs(ns, &hot, &cold);
		printf("  on this thread a line already held reads in %.0f ns,"
		       " one flushed out of every cache in %.0f ns\n\n",
		       hot, cold);
	}

	printf("  %-38s %-22s %10s %10s %8s %10s  %s\n", "placement",
	       "how the content arrives", "median ns", "90th ns", "trials",
	       "1st touch", "content");

	for (place = 0; place < sizeof(places) / sizeof(places[0]); place++) {
	int peer = wait_cpu;

	if (wait_cpu < 0) {
		peer = environment_peer(&env, send_cpu, places[place]);
		if (peer < 0)
			continue;
	} else if (place) {
		break;		/* a pair was named; run it once */
	}

	pin_to(send_cpu);

	for (arm = 0; arm < 2; arm++) {
		uint64_t *samples, *touches;
		int taken = 0, unanswered = 0;

		memset(&reader, 0, sizeof(reader));
		reader.cpu = peer;
		reader.queued = arm == 1;
		stop = 0;
		wrong = 0;
		round_number = 0;

		samples = calloc((size_t)trials, sizeof(*samples));
		touches = calloc((size_t)trials, sizeof(*touches));
		/* Indexed by round, and the last round is trials + 2. */
		touch_by_round = calloc((size_t)trials + 3,
					sizeof(*touch_by_round));
		if (!samples || !touches || !touch_by_round)
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

			/*
			 * Bounded: far longer than any delivery and short
			 * enough that a run which is not working says so
			 * rather than spinning.
			 */
			{
				uint64_t deadline = now_tb() +
					(uint64_t)(PATIENCE_US * 1000.0 / ns);

				while (arrived.when == UINT64_MAX &&
				       now_tb() < deadline)
					;
			}
			if (arrived.when == UINT64_MAX)
				unanswered++;
			else if (arrived.when >= sent) {
				touches[taken] = touch_by_round[i + 1];
				samples[taken++] = arrived.when - sent;
			}
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
			char touch[16] = "";
			char where[48];
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

			if (measure_touch) {
				for (j = 1; j < taken; j++) {
					uint64_t v = touches[j];

					for (k = j - 1;
					     k >= 0 && touches[k] > v; k--)
						touches[k + 1] = touches[k];
					touches[k + 1] = v;
				}
				snprintf(touch, sizeof(touch), "%.0f ns",
					 (double)touches[taken / 2] * ns);
			}
			/*
			 * With the pair, because the relation names a class of
			 * placements and a reading is of one of them.
			 */
			snprintf(where, sizeof(where), "%s (%d to %d)",
				 environment_relation_name(
					 environment_relation(send_cpu, peer)),
				 send_cpu, peer);

			printf("  %-38s %-22s %10.0f %10.0f %8d %10s  %s%s\n",
			       arm ? "" : where,
			       reader.queued ? "in the paste" : "read from memory",
			       (double)a * ns, (double)b * ns, taken,
			       touch, wrong ? "WRONG" : "correct",
			       unanswered ? ", some lost" : "");
		}
		if (unanswered)
			printf("  %-38s %-22s %d of %d trials went unanswered\n",
			       "", "", unanswered, trials);
		free(samples);
		free(touches);
		free(touch_by_round);
		touch_by_round = NULL;
	}

	}

	if (environment_verify(&env))
		return 1;

	return 0;
}
