// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Does an accelerator request that moves nothing complete, and what does it
 * cost to use one as a doorbell?
 *
 * The question is whether a wake can be had from the accelerator path rather
 * than from a window-to-window notify. A notify is edge triggered against a
 * thread the switchboard finds running, so one that arrives early is lost and
 * a waiter can sleep until an unrelated exception rescues it. A request, by
 * contrast, leaves a coprocessor status block behind: the CSB's valid bit is
 * durable state, so a loop on it terminates whether or not any signal
 * arrived. Pairing the two would give a wake that is fast when it lands and
 * correct when it does not.
 *
 * That only works if a request with nothing in it is accepted. This asks:
 *
 *   - a move of zero bytes, which the length rules may well reject;
 *   - a move of the smallest length the rules do allow, as the fallback;
 *
 * and reports what each costs, so the price of using the engine as a doorbell
 * can be compared against the notify it would be replacing.
 *
 * The cost is not only the microseconds. A request of any size occupies an
 * entry in the receive queue that real work would otherwise use, so a doorbell
 * built this way is spending accelerator capacity to avoid a race.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vas/nx.h>
#include <vas/nx842.h>
#include <vas/vas.h>

#include "report.h"

#define REPEATS 200

/*
 * The timebase, read directly. A batch timed with clock_gettime and divided is
 * honest about the mean and silent about everything else, and whether a
 * completion is deterministic or merely fast on average is the whole question
 * for anything that would route work here at fine granularity.
 */
static inline unsigned long long now_tb(void)
{
	unsigned long long tb;

	asm volatile("mfspr %0, 268" : "=r"(tb));

	return tb;
}

static double tb_ns(void)
{
	unsigned int be;
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

static int cmp_ull(const void *a, const void *b)
{
	unsigned long long x = *(const unsigned long long *)a;
	unsigned long long y = *(const unsigned long long *)b;

	return (x > y) - (x < y);
}

/*
 * Submit one move of @len bytes and report what came back. @len of zero is the
 * case in question; anything else is the control that says the path works.
 */
static int try_length(struct vas_window *window, void *source, void *target,
		      size_t len, const char *what)
{
	struct nx_completion completion;
	struct nx_request *request = NULL;
	int rc;

	rc = nx_request_create(&request);
	if (rc) {
		report_errno("nx_request_create", rc);
		return rc;
	}

	nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
	rc = nx_request_set_source(request, nx_source(source, len));
	if (!rc)
		rc = nx_request_set_target(request, nx_target(target, len));
	if (rc) {
		printf("  %-18s refused when described: %s\n", what, strerror(-rc));
		nx_request_destroy(&request);
		return rc;
	}

	rc = nx_execute(window, request, NULL);
	if (rc) {
		printf("  %-18s refused at submission: %s\n", what, strerror(-rc));
		nx_request_destroy(&request);
		return rc;
	}

	rc = nx_request_completion(request, &completion);
	if (rc) {
		printf("  %-18s no completion: %s\n", what, strerror(-rc));
		nx_request_destroy(&request);
		return rc;
	}

	printf("  %-18s completed, cc %u, %llu bytes into the target\n", what,
	       (unsigned)completion.cc,
	       (unsigned long long)completion.processed_bytes);

	nx_request_destroy(&request);

	return 0;
}

/* Time @repeats round trips of a move of @len bytes. */
static int time_length(struct vas_window *window, void *source, void *target,
		       size_t len, const char *what)
{
	struct nx_completion completion;
	struct nx_request *request = NULL;
	unsigned long long start_tb, *held;
	double each_us;
	int rc = 0;
	int i;

	rc = nx_request_create(&request);
	if (rc)
		return rc;

	nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
	rc = nx_request_set_source(request, nx_source(source, len));
	if (!rc)
		rc = nx_request_set_target(request, nx_target(target, len));
	if (rc) {
		nx_request_destroy(&request);
		return rc;
	}

	/* Once first, so a translation fault is not counted as latency. */
	rc = nx_execute(window, request, NULL);
	if (rc) {
		nx_request_destroy(&request);
		return rc;
	}

	held = calloc(REPEATS, sizeof(*held));
	if (!held) {
		nx_request_destroy(&request);
		return -ENOMEM;
	}

	for (i = 0; i < REPEATS; i++) {
		start_tb = now_tb();
		rc = nx_execute(window, request, NULL);
		held[i] = now_tb() - start_tb;
		if (rc)
			break;
	}

	if (rc) {
		report_errno("during the timed repeats", rc);
	} else if (!nx_request_completion(request, &completion)) {
		double ns = tb_ns();

		qsort(held, REPEATS, sizeof(*held), cmp_ull);
		each_us = held[REPEATS / 2] * ns / 1000.0;
		printf("  %-18s median %.3f us, 90th %.3f us, fastest %.3f us,"
		       " slowest %.3f us, cc %u\n", what, each_us,
		       held[(REPEATS * 9) / 10] * ns / 1000.0,
		       held[0] * ns / 1000.0,
		       held[REPEATS - 1] * ns / 1000.0,
		       (unsigned)completion.cc);
	}

	free(held);

	nx_request_destroy(&request);

	return rc;
}

int main(void)
{
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	void *source, *target;
	size_t smallest;
	int rc;

	vas_window_attr_init(&attr, VAS_COP_842);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("vas_window_open", rc);
		return (rc == -ENODEV || rc == -ENOENT) ? 77 : 1;
	}

	source = alloc_engine_buffer(NX_842_BUFFER_ALIGN);
	target = alloc_engine_buffer(NX_842_BUFFER_ALIGN);
	if (!source || !target) {
		report_errno("allocating buffers", -ENOMEM);
		vas_window_close(&window);
		return 1;
	}
	memset(source, 0xa5, NX_842_BUFFER_ALIGN);

	/* The smallest length the last-descriptor rule admits. */
	smallest = NX_842_LAST_LENGTH_MULTIPLE;

	printf("does a request that moves nothing complete?\n");
	try_length(window, source, target, 0, "zero bytes");
	try_length(window, source, target, smallest, "smallest allowed");

	printf("\nwhat a doorbell would cost, %d repeats each:\n", REPEATS);
	time_length(window, source, target, 0, "zero bytes");
	time_length(window, source, target, smallest, "smallest allowed");

	free(source);
	free(target);
	vas_window_close(&window);

	return 0;
}
