/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The state of the machine a measurement was taken on.
 *
 * Two jobs, and they are the same job at different times. Before a run it
 * refuses to start unless the machine is in the state the measurement needs --
 * the clock held still, enough CPUs the scheduler will not touch, no
 * throttling in progress. After a run it checks that none of that changed
 * underneath, because a machine that started throttling halfway through
 * produces numbers that look exactly like numbers from one that did not.
 *
 * And it writes down what it found, so that a record can be read months later
 * by someone who does not have the machine. A latency without the clock it was
 * taken at is not a measurement, it is an anecdote.
 */

#ifndef _VAS_EXAMPLES_ENVIRONMENT_H
#define _VAS_EXAMPLES_ENVIRONMENT_H

#include <stdbool.h>
#include <stdio.h>

/* What a measurement needs of the machine. Asked for, not assumed. */
enum environment_need {
	/* The core clock must not move: a time is otherwise two facts at once. */
	ENVIRONMENT_STEADY_CLOCK	= 1 << 0,
	/* Workers must sit where the scheduler will not put anything else. */
	ENVIRONMENT_ISOLATED_CPUS	= 1 << 1,
	/* No core may drop into a stop state and be slow to come back. */
	ENVIRONMENT_NO_IDLE_STATES	= 1 << 2,
};

#define ENVIRONMENT_CPUS_MAX 256

struct environment {
	char release[80];
	char cmdline[1024];

	int online[ENVIRONMENT_CPUS_MAX];
	int online_n;
	int isolated[ENVIRONMENT_CPUS_MAX];	/* isolated and online */
	int isolated_n;
	int chips;
	int cores;

	/* What the governor was told, and what the machine actually ran at. */
	unsigned long asked_khz_min;
	unsigned long asked_khz_max;
	bool boost;
	double measured_ghz;

	unsigned long long throttle_at_start;
	bool idle_states_present;

	unsigned int needed;
};

/*
 * Read the machine, check it against @needed, and record the answer. Returns 0
 * when the machine will do, or a negative errno after explaining on stderr
 * what is wrong and what would fix it. @workers is how many CPUs the caller
 * intends to place threads on, or 0 if it does not care.
 */
int environment_require(struct environment *env, int workers,
			unsigned int needed);

/*
 * Check nothing moved. Call after the measured work, before reporting: returns
 * 0, or a negative errno having said what changed. A run that fails here has
 * results, and they should be thrown away.
 */
int environment_verify(const struct environment *env);

/*
 * How two CPUs are related, which is what a latency between them is about.
 * Ordered by distance, so a comparison is meaningful.
 */
enum environment_relation {
	ENVIRONMENT_SAME_THREAD,
	ENVIRONMENT_SAME_CORE,		/* threads of one core */
	ENVIRONMENT_SHARED_CACHE,	/* different cores, one last-level cache */
	ENVIRONMENT_SAME_CHIP,		/* one chip, no cache in common */
	ENVIRONMENT_OTHER_CHIP,
};

const char *environment_relation_name(enum environment_relation r);
enum environment_relation environment_relation(int cpu_a, int cpu_b);

/*
 * A CPU standing in the given relation to @from, or -1 if the machine offers
 * none. Isolated CPUs are preferred, since a measurement placed where the
 * scheduler still works is a measurement of the scheduler.
 */
int environment_peer(const struct environment *env, int from,
		     enum environment_relation want);

/* For a person reading the terminal, and for a record read later. */
void environment_print(const struct environment *env);
void environment_record(const struct environment *env, FILE *out);

#endif /* _VAS_EXAMPLES_ENVIRONMENT_H */
