// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * One paste, one copy, every reader.
 *
 * A notify carries an identity rather than a recipient, so several threads
 * given the same one are all woken by a single paste -- that is measured, and
 * multicast.c is where. This asks what those threads can be told, rather than
 * only that they were called.
 *
 * A paste carries 128 bytes whether or not anyone keeps them. Turned into a
 * queue, the switchboard writes those bytes once, into the window that owns
 * the identity, and every thread sharing that identity is woken to read that
 * one entry. Nobody is sent a copy. What the group gets is a place the fabric
 * has already put the content, and the second reader is answered from wherever
 * the first was, by the coherence protocol, not by another trip to memory.
 *
 * The switchboard's write is a cache inject, which is what makes that worth
 * anything: the receive queue is not written out to memory and read back N
 * times, it is installed in a cache and served to the group from there.
 *
 * The control is the same group, the same placement and the same one paste,
 * but the paste is a bare wake and the content is a line the sender wrote in
 * its own memory. That is what a program would do without a queue, and it is
 * also one line read by N threads -- the difference is only where the line is
 * and who put it there. Anything the queue is worth has to show up against it.
 *
 * A round is not over until every reader has the content, so the group's cost
 * is the last reader's and not the average of them; the mean of a broadcast
 * flatters it by exactly the amount the stragglers matter.
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

#define READERS_MAX 32

/* How long a round may take before the readers that did not answer are lost. */
#define PATIENCE_US 2000

/*
 * Rounds run but not counted. The first arm in a process pays for its thread
 * stacks and the first touch of every line it uses, and has measured three
 * times its own steady value.
 */
#define WARMUP_ROUNDS 50

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

/* Store Min Unsigned, Power ISA 3.0B Figure 4. */
enum { STORE_ATOMIC_MIN_UNSIGNED = 6 };

/*
 * The earliest of what is there and @v, decided at the memory controller.
 *
 * Which is the point, and not the folding: the sender spins on this cell
 * waiting for an answer, so a reader that stored to it normally would have to
 * take the line off a hot spinner, and would still be doing that when the next
 * paste arrived -- a notify reaches a thread only if it is already suspended,
 * so the reader would miss it and come back on its own a microsecond later.
 * An atomic leaves the line where it is. Storing normally here reported 1174ns
 * against the 396ns the same pair delivers.
 */
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
 * What the sender says. The trial number is checked, because a group that was
 * woken and read nothing would otherwise be the fastest arrangement there is.
 */
struct payload {
	uint64_t trial;
	uint64_t pattern[15];
} __attribute__((aligned(128)));

static uint64_t payload_trial(const void *p)
{
	return __atomic_load_n(&((const struct payload *)p)->trial,
			       __ATOMIC_ACQUIRE);
}

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
	/* The trial last, so a reader that sees it can rely on the rest. */
	__atomic_store_n(&p->trial, trial, __ATOMIC_RELEASE);
}

/*
 * One reader's answer to one round, on a line of its own.
 *
 * The reader's own count of extra resumes is kept in the reader, not here:
 * this line is the one the sender spins on, and a second field on it would be
 * another store the reader has to take it away to make.
 */
struct answer {
	volatile uint64_t when;
	char pad[120];
};

static volatile uint64_t round_number;
static volatile int stop;
static volatile int wrong;
/*
 * Whether the sender also publishes a sequence number.
 *
 * The arm that leaves the content in memory has to: a reader needs to be told
 * the block is written, and that telling is the store the paste replaces. The
 * arm that pastes does not, since the entry says for itself that it arrived --
 * so making it publish one too charges it for the thing it exists to avoid.
 * Left on by default because it is also what tells this program which round a
 * reader answered; --no-sequence takes it away from the paste arm and lets the
 * readers count for themselves, which is the comparison a program would face.
 */
static bool sequenced = true;
static struct payload shared __attribute__((aligned(128)));
static struct answer answers[READERS_MAX] __attribute__((aligned(128)));

struct reader {
	pthread_t thread;
	int cpu;
	int index;
	bool queued;
	struct vas_destination *dest;
	volatile int ready;
	int rc;
	/* Written by this reader alone, read between rounds and after. */
	long rounds;
	long late;
	volatile uint64_t last_round;
} __attribute__((aligned(128)));	/* one reader per line */

/*
 * The destination the group shares, opened by the thread at index 0 and
 * joined by the rest. Which thread opens it is not an implementation detail:
 * the switchboard addresses a destination by the thread running where the
 * window was opened, so a window opened by the sender names the sender, and
 * every paste would wake the thread that sent it and no reader at all. That
 * mistake reads as a working group with every reader a microsecond late,
 * because a thread in wait resumes on its own about that often.
 */
static volatile int owner_open;		/* 0 pending, 1 open, -1 failed */
static struct vas_destination *owner_dest;
static size_t queue_bytes;

static void *reader_main(void *arg)
{
	struct reader *r = arg;
	unsigned int resumes = 0;
	bool answered = false;
	uint64_t seen = 0, taken = 0;

	pin_to(r->cpu);

	if (r->index == 0) {
		r->rc = r->queued ?
			vas_destination_open_queued(vas_instance_any(),
						    queue_bytes, &r->dest) :
			vas_destination_open(vas_instance_any(), &r->dest);
		if (r->rc) {
			__atomic_store_n(&owner_open, -1, __ATOMIC_RELEASE);
			r->ready = -1;
			return NULL;
		}
		owner_dest = r->dest;
		__atomic_store_n(&owner_open, 1, __ATOMIC_RELEASE);
	} else {
		while (!__atomic_load_n(&owner_open, __ATOMIC_ACQUIRE))
			;
		if (owner_open < 0) {
			r->ready = -1;
			return NULL;
		}
		r->rc = r->queued ?
			vas_destination_join_queue(owner_dest, &r->dest) :
			vas_destination_join(vas_destination_fd(owner_dest),
					     &r->dest);
		if (r->rc) {
			r->ready = -1;
			return NULL;
		}
	}
	r->ready = 1;

	while (!stop) {
		uint64_t now, at;

		vas_wait();

		/*
		 * With nothing to publish, the reader's own count is the round
		 * number: entries are never freed here, so the switchboard
		 * cannot pass a reader, and the Nth entry a reader takes is
		 * the Nth that was sent. Being late costs it nothing but time.
		 */
		if (r->queued && !sequenced) {
			const void *msg;

			resumes++;
			msg = vas_destination_next(r->dest);
			if (!msg)
				continue;
			taken++;
			if (!payload_correct(msg, taken))
				wrong = 1;
			at = now_tb();
			if (taken > WARMUP_ROUNDS)
				r->rounds++;
			resumes = 0;
			r->last_round = taken;
			keep_earliest(&answers[r->index].when, at);
			vas_destination_advance(r->dest);
			continue;
		}

		now = __atomic_load_n(&round_number, __ATOMIC_ACQUIRE);
		if (!now)		/* nothing has been sent yet */
			continue;
		if (now != seen) {
			seen = now;
			resumes = 0;
			answered = false;
		}
		resumes++;
		if (answered)		/* this round is already in hand */
			continue;

		if (r->queued) {
			const void *msg;

			/*
			 * Past any round this reader was not woken for. A
			 * notify that arrives while the thread is off a core
			 * is not delivered and not queued, so a reader can
			 * miss one; without this its cursor would sit a round
			 * behind the switchboard for the rest of the run and
			 * every later entry would look like the wrong round.
			 */
			while ((msg = vas_destination_next(r->dest)) &&
			       payload_trial(msg) < now)
				vas_destination_advance(r->dest);

			if (!msg || payload_trial(msg) != now)
				continue;
			if (!payload_correct(msg, now))
				wrong = 1;
			at = now_tb();	/* the clock before anything else */
			answered = true;
			if (now > WARMUP_ROUNDS) {
				r->rounds++;
				if (resumes > 1)
					r->late++;
			}
			keep_earliest(&answers[r->index].when, at);
			/*
			 * Advance without freeing. The entry is the group's,
			 * and a reader that freed it would take it from the
			 * sharers that had not looked yet. The queue is sized
			 * to the run instead, so no slot is ever needed twice.
			 */
			vas_destination_advance(r->dest);
		} else {
			/*
			 * Ordered by the round number and nothing else: the
			 * sender fills the block before publishing it, so a
			 * reader that sees the new number sees the block, and
			 * one that reads on any other cue can catch the block
			 * half written.
			 */
			if (!payload_correct(&shared, now))
				wrong = 1;
			at = now_tb();	/* the clock before anything else */
			answered = true;
			if (now > WARMUP_ROUNDS) {
				r->rounds++;
				if (resumes > 1)
					r->late++;
			}
			keep_earliest(&answers[r->index].when, at);
		}
	}

	return NULL;
}

static int by_value(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/*
 * Readers one per core, outward from the sender, so that adding a reader adds
 * a core rather than a thread beside one already listening. A group placed by
 * CPU number would cross a chip boundary at whatever number the machine
 * happened to use.
 */
static int place_readers(const struct environment *env, int from, int *cpu,
			 int want)
{
	enum environment_relation order[] = {
		ENVIRONMENT_SHARED_CACHE,
		ENVIRONMENT_SAME_CHIP,
		ENVIRONMENT_OTHER_CHIP,
	};
	int taken = 0;
	size_t k;
	int i, j;

	/*
	 * Isolated only. A reader the scheduler may put something else on
	 * measures the scheduler: placed on a shared CPU this reports whole
	 * microseconds where the machine delivers hundreds of nanoseconds.
	 */
	for (k = 0; k < sizeof(order) / sizeof(order[0]) && taken < want; k++) {
		for (i = 0; i < env->isolated_n && taken < want; i++) {
			int c = env->isolated[i];

			if (c == from ||
			    environment_relation(from, c) != order[k])
				continue;
			/* One per core while there are cores left. */
			for (j = 0; j < taken; j++)
				if (cpu[j] == c ||
				    environment_relation(cpu[j], c) ==
					    ENVIRONMENT_SAME_CORE)
					break;
			if (j < taken)
				continue;
			cpu[taken++] = c;
		}
	}

	/* Only then a second thread on a core already listening. */
	for (i = 0; i < env->isolated_n && taken < want; i++) {
		int c = env->isolated[i];

		if (c == from)
			continue;
		for (j = 0; j < taken; j++)
			if (cpu[j] == c)
				break;
		if (j < taken)
			continue;
		cpu[taken++] = c;
	}

	return taken;
}

int main(int argc, char **argv)
{
	struct reader readers[READERS_MAX];
	int cpu[READERS_MAX];
	struct environment env;
	struct payload outgoing __attribute__((aligned(128)));
	double ns = tb_ns();
	int trials = 500, want = 8, send_cpu;
	long settle = 4000;
	int arm, i, n, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--trials") && i + 1 < argc)
			trials = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--readers") && i + 1 < argc)
			want = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--settle") && i + 1 < argc)
			settle = atol(argv[++i]);
		else if (!strcmp(argv[i], "--no-sequence"))
			sequenced = false;
		else {
			fprintf(stderr,
				"usage: share_message [--trials N] [--readers N] [--settle N] [--no-sequence]\n");
			return 2;
		}
	}
	if (want < 1 || want > READERS_MAX || trials < 1)
		return 2;

	if (environment_require(&env, want + 1,
				ENVIRONMENT_STEADY_CLOCK |
					ENVIRONMENT_ISOLATED_CPUS))
		return 1;

	send_cpu = env.isolated_n ? env.isolated[0] : env.online[0];
	n = place_readers(&env, send_cpu, cpu, want);
	if (n < 1) {
		fprintf(stderr, "  no reader placement available\n");
		return 1;
	}
	pin_to(send_cpu);

	printf("  one paste from cpu %d to %d reader%s, %d trials\n", send_cpu,
	       n, n == 1 ? "" : "s", trials);
	printf("  readers on cpu");
	for (i = 0; i < n; i++)
		printf(" %d", cpu[i]);
	printf("\n\n");
	printf("  %-24s %9s %9s %9s %9s %8s %6s  %s\n",
	       "how the content arrives", "10th first", "first ns", "last ns",
	       "90th last", "woke late", "lost", "content");

	for (arm = 0; arm < 2; arm++) {
		bool queued = arm == 1;
		uint64_t *first, *last;
		long pairs = 0, late = 0;
		int taken = 0, unanswered = 0;

		first = calloc((size_t)trials, sizeof(*first));
		last = calloc((size_t)trials, sizeof(*last));
		if (!first || !last)
			return 1;

		stop = 0;
		wrong = 0;
		round_number = 0;
		owner_open = 0;
		owner_dest = NULL;
		memset(readers, 0, sizeof(readers));

		/*
		 * Sized to the whole run, so that no slot is reused and no
		 * reader has to free one out from under the others.
		 */
		queue_bytes = (size_t)(trials + WARMUP_ROUNDS + 4) *
			      VAS_MESSAGE_BYTES;

		for (i = 0; i < n; i++) {
			readers[i].cpu = cpu[i];
			readers[i].index = i;
			readers[i].queued = queued;
			if (pthread_create(&readers[i].thread, NULL,
					   reader_main, &readers[i])) {
				report_errno("pthread_create", -errno);
				return 1;
			}
		}

		/* Each reader opens or joins for itself; wait for all of it. */
		for (i = 0; i < n; i++) {
			while (!readers[i].ready)
				;
			if (readers[i].ready < 0) {
				report_errno("open a destination",
					     readers[i].rc);
				return readers[i].rc == -ENODEV ? 77 : 1;
			}
		}

		{
			struct vas_window_attr attr;
			struct vas_window *window;

			vas_window_attr_init(&attr, VAS_COP_FTW);
			attr.wake_target = vas_destination_fd(owner_dest);
			rc = vas_window_open(&attr, &window);
			if (rc) {
				report_errno("open a window", rc);
				return 1;
			}

			for (i = 0; i < trials + WARMUP_ROUNDS; i++) {
				uint64_t sent, deadline;
				uint64_t worst = 0, best = UINT64_MAX;
				volatile long spin;
				bool counted = i >= WARMUP_ROUNDS;
				int answered, j;

				/*
				 * Cleared here and read by the readers, which
				 * puts n lines on the path being timed -- the
				 * same n in both arms, so the comparison holds
				 * even though neither reading is a floor.
				 */
				for (j = 0; j < n; j++)
					answers[j].when = UINT64_MAX;
				/*
				 * Long enough that every reader is back in
				 * wait. A notify reaches a thread only if it
				 * is already suspended, so a group pasted to
				 * while one of them is still on its way back
				 * measures how fast that one returns: at 200
				 * iterations the median was 1238ns against a
				 * tenth percentile of 367, one mode for the
				 * notify and one for the resume that rescues
				 * a reader the notify missed. This is before
				 * the clock is read, so it costs wall time
				 * and nothing else.
				 */
				for (spin = 0; spin < settle; spin++)
					;

				/*
				 * The clock starts before the content is
				 * written. Writing it is not the same work in
				 * the two arms and with a group the
				 * difference grows: the arm that leaves it in
				 * memory writes a line every reader has been
				 * reading, and must take it from all of them
				 * to do so, while the arm that pastes writes
				 * a line nobody else has ever held and lets
				 * the switchboard distribute it. Timing from
				 * after both had written charged neither for
				 * it, which is the whole of what a queue is
				 * supposed to save.
				 *
				 * Each arm fills only what it sends.
				 */
				sent = now_tb();
				if (queued)
					payload_fill(&outgoing,
						     (uint64_t)(i + 1));
				else
					payload_fill(&shared,
						     (uint64_t)(i + 1));
				if (sequenced || !queued)
					__atomic_store_n(&round_number,
							 (uint64_t)(i + 1),
							 __ATOMIC_RELEASE);
				rc = queued ? vas_send(window, &outgoing) :
					      vas_wake(window);
				if (rc) {
					report_errno("send", rc);
					break;
				}

				deadline = sent +
					(uint64_t)(PATIENCE_US * 1000.0 / ns);
				do {
					answered = 0;
					for (j = 0; j < n; j++)
						if (answers[j].when !=
						    UINT64_MAX)
							answered++;
				} while (answered < n && now_tb() < deadline);

				if (answered < n) {
					if (counted)
						unanswered++;
					continue;
				}
				/*
				 * With no sequence number the readers count
				 * for themselves, so check they counted the
				 * round that was just sent rather than an
				 * earlier one they were still catching up on.
				 */
				if (queued && !sequenced) {
					for (j = 0; j < n; j++)
						if (readers[j].last_round !=
						    (uint64_t)(i + 1))
							break;
					if (j < n) {
						if (counted)
							unanswered++;
						continue;
					}
				}
				for (j = 0; j < n; j++) {
					uint64_t t = __atomic_load_n(
						&answers[j].when,
						__ATOMIC_ACQUIRE);

					if (t < sent)
						continue;
					if (t - sent < best)
						best = t - sent;
					if (t - sent > worst)
						worst = t - sent;
				}
				if (!counted || best == UINT64_MAX)
					continue;
				first[taken] = best;
				last[taken++] = worst;
			}

			stop = 1;
			round_number = (uint64_t)(trials + WARMUP_ROUNDS) + 2;
			payload_fill(&shared, round_number);
			/* One paste per reader, so none is left in wait. */
			for (i = 0; i < n + 2; i++) {
				if (!queued) {
					vas_wake(window);
					continue;
				}
				/*
				 * Carrying on the count, so a reader that is
				 * counting for itself and takes one of these
				 * on its way out does not call it wrong.
				 */
				payload_fill(&outgoing,
					     (uint64_t)(trials +
							WARMUP_ROUNDS + 1 + i));
				vas_send(window, &outgoing);
			}
			for (i = 0; i < n; i++) {
				pthread_join(readers[i].thread, NULL);
				pairs += readers[i].rounds;
				late += readers[i].late;
			}
			vas_window_close(&window);
		}

		for (i = n - 1; i >= 0; i--)
			vas_destination_close(&readers[i].dest);

		if (taken) {
			qsort(first, (size_t)taken, sizeof(*first), by_value);
			qsort(last, (size_t)taken, sizeof(*last), by_value);
			char woke[12];

			/*
			 * Only meaningful where a reader can tell which
			 * resume followed the round being sent, which is the
			 * sequence number it does not otherwise have.
			 */
			if (queued && !sequenced)
				snprintf(woke, sizeof(woke), "%8s", "-");
			else
				snprintf(woke, sizeof(woke), "%7.1f%%",
					 pairs ? 100.0 * (double)late /
							 (double)pairs :
						 0.0);

			printf("  %-24s %9.0f %9.0f %9.0f %9.0f %s %6d  %s\n",
			       queued ? "in the paste" : "read from memory",
			       (double)first[taken / 10] * ns,
			       (double)first[taken / 2] * ns,
			       (double)last[taken / 2] * ns,
			       (double)last[(taken * 9) / 10] * ns, woke,
			       unanswered, wrong ? "WRONG" : "correct");
		} else {
			printf("  %-24s nothing was delivered\n",
			       queued ? "in the paste" : "read from memory");
		}

		free(first);
		free(last);
	}

	if (environment_verify(&env))
		return 1;

	return 0;
}
