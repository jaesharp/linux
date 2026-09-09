// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Does one paste wake more than one thread?
 *
 * A notify carries an identity rather than a recipient: the switchboard puts
 * a partition, process and thread number on the interconnect, and a core
 * matches it against whatever it is running. Nothing in that says only one
 * thread may answer to a given identity, so several threads given the same
 * one should all be matched by a single notify -- and one paste would wake a
 * group rather than a thread.
 *
 * Neither the workbook nor the architecture says whether the hardware does
 * that. So this asks it, by having several threads join one destination and
 * counting how many a single paste resumes.
 *
 * The counting is the delicate part. A thread in wait also resumes on any
 * exception it happens to take, so a thread that wakes proves nothing on its
 * own; what distinguishes a wake from an interruption is when it arrives. So
 * each joiner records the moment it resumed, and only those inside a window
 * far shorter than the interval between incidental resumes are counted. On a
 * quiet, isolated core that interval is long and the distinction is clean; on
 * a busy one this measures the machine rather than the mechanism, which is why
 * it reports the spread as well as the count.
 *
 * A run where every joiner resumes is only interesting if they resume
 * together. One that reports all of them, spread over milliseconds, found the
 * timer.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vas/vas.h>

#include "report.h"

#define JOINERS_MAX 8

/* A resume this long after the paste was something else, not the wake. */
#define TOGETHER_US 200.0

struct joiner {
	pthread_t thread;
	int index;
	int cpu;
	int join_fd;		/* -1 for the first, which opens the group */
	struct vas_destination *dest;
	volatile int ready;
	volatile long long woke_ns;
	int rc;
};

static volatile int released;
static long long pasted_ns;

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
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

	pin_to(j->cpu);

	j->rc = j->join_fd < 0 ?
		vas_destination_open(vas_instance_any(), &j->dest) :
		vas_destination_join(j->join_fd, &j->dest);
	if (j->rc) {
		j->ready = -1;
		return NULL;
	}

	j->ready = 1;

	/*
	 * Waited on once and recorded, rather than looped on a condition: the
	 * question is what a single notify reaches, so a thread that resumes
	 * must say when and not go back to sleep.
	 */
	while (!released)
		vas_wait();

	j->woke_ns = now_ns();

	return NULL;
}

int main(int argc, char **argv)
{
	struct joiner joiners[JOINERS_MAX];
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	int joiners_n = 4;
	int base_cpu = -1;
	int i, rc, woken = 0;
	double first = 0.0, last = 0.0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--joiners") && i + 1 < argc)
			joiners_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--base-cpu") && i + 1 < argc)
			base_cpu = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: multicast [--joiners N] [--base-cpu N]\n");
			return 2;
		}
	}
	if (joiners_n < 2 || joiners_n > JOINERS_MAX)
		return 2;

	memset(joiners, 0, sizeof(joiners));

	/* The first opens the group; the rest join what it opened. */
	for (i = 0; i < joiners_n; i++) {
		joiners[i].index = i;
		joiners[i].cpu = base_cpu < 0 ? -1 : base_cpu + i * 4;
		joiners[i].join_fd = -1;
		joiners[i].woke_ns = 0;
	}

	joiners[0].join_fd = -1;
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
		joiners[i].join_fd = vas_destination_fd(joiners[0].dest);
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

	/* One sender, pointed at the destination the group shares. */
	vas_window_attr_init(&attr, VAS_COP_FTW);
	attr.wake_target = vas_destination_fd(joiners[0].dest);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("open a window onto the group", rc);
		return 1;
	}

	/* Long enough that every joiner is certainly suspended. */
	for (volatile long s = 0; s < 200000; s++)
		;

	released = 1;
	pasted_ns = now_ns();
	rc = vas_wake(window);
	if (rc)
		report_errno("wake the group", rc);

	for (i = 0; i < joiners_n; i++)
		pthread_join(joiners[i].thread, NULL);

	printf("one paste, %d threads sharing a destination:\n", joiners_n);
	for (i = 0; i < joiners_n; i++) {
		double us = (double)(joiners[i].woke_ns - pasted_ns) / 1000.0;

		printf("  thread %d on cpu %-4d resumed %+.3f us\n",
		       i, joiners[i].cpu, us);
		if (us >= 0.0 && us < TOGETHER_US) {
			woken++;
			if (!first || us < first)
				first = us;
			if (us > last)
				last = us;
		}
		vas_destination_close(&joiners[i].dest);
	}

	printf("  %d of %d resumed within %.0f us, spread %.3f us\n",
	       woken, joiners_n, TOGETHER_US, last - first);
	if (woken > 1)
		printf("  one notify reached more than one thread\n");
	else
		printf("  only one thread was reached: the notify is delivered once\n");

	vas_window_close(&window);

	return 0;
}
