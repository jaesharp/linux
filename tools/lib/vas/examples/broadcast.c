// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * One tile to many places, in one request.
 *
 * A systolic ring wants the same block in several workers at once, and the
 * obvious way to get it is one move per worker. The engine walks a source list
 * and a target list in order and requires only that the totals agree, which
 * leaves room for something better: name the same source span as many times as
 * there are destinations, give as many target spans, and one request lands the
 * block everywhere.
 *
 * That is not a broadcast in the sense of one read serving many writes -- the
 * engine reads the source once per repetition, and after the first it is
 * reading something it has just read. What it does save is the per-request
 * cost, which is about 1.2 microseconds of submission and completion, and
 * which would otherwise be paid once per destination.
 *
 * So this measures both ways of doing the same thing, checks that every
 * destination actually received the data, and then reads the destinations back
 * to see whether the engine left them anywhere useful. A copy that lands only
 * in memory still has to be fetched by the worker that wanted it; one that
 * lands in cache does not, and the difference is worth knowing before building
 * a pipeline on it.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vas/nx.h>
#include <vas/nx842.h>
#include <vas/vas.h>

#include "cycles.h"
#include "report.h"

#define COPIES_MAX 16

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

static void pin_to(int cpu)
{
	cpu_set_t set;

	if (cpu < 0)
		return;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

/* Read every byte and return a value the compiler cannot discard. */
static uint64_t sweep(const unsigned char *p, size_t len)
{
	uint64_t sum = 0;
	size_t i;

	for (i = 0; i < len; i += 128)
		sum += p[i];

	return sum;
}

static volatile uint64_t sink;

int main(int argc, char **argv)
{
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	struct nx_request *request = NULL;
	struct nx_source_list *sources = NULL;
	struct nx_target_list *targets = NULL;
	unsigned char *source = NULL;
	unsigned char *copies[COPIES_MAX] = { NULL };
	unsigned char *cold = NULL;
	size_t len = 65536;
	int copies_n = 8;
	double ns = tb_ns();
	uint64_t t0, t1, one_request = 0, many_requests = 0;
	int cpu = -1;
	int i, c, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
			cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--copies") && i + 1 < argc)
			copies_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bytes") && i + 1 < argc)
			len = strtoull(argv[++i], NULL, 0);
		else {
			fprintf(stderr,
				"usage: broadcast [--cpu N] [--copies N] [--bytes N]\n");
			return 2;
		}
	}
	if (copies_n < 1 || copies_n > COPIES_MAX)
		return 2;

	pin_to(cpu);

	vas_window_attr_init(&attr, VAS_COP_842);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("open an 842 window", rc);
		return rc == -ENODEV ? 77 : 1;
	}

	source = aligned_alloc(NX_842_BUFFER_ALIGN, len);
	cold = aligned_alloc(NX_842_BUFFER_ALIGN, len);
	if (!source || !cold)
		return 1;
	for (i = 0; i < (int)len; i++)
		source[i] = (unsigned char)(i * 31 + 7);
	memset(cold, 0, len);

	for (c = 0; c < copies_n; c++) {
		copies[c] = aligned_alloc(NX_842_BUFFER_ALIGN, len);
		if (!copies[c])
			return 1;
		memset(copies[c], 0, len);
	}

	if (nx_request_create(&request) ||
	    nx_source_list_create(COPIES_MAX, &sources) ||
	    nx_target_list_create(COPIES_MAX, &targets)) {
		report_errno("create the request and its lists", -ENOMEM);
		return 1;
	}

	printf("%d copies of %zu bytes", copies_n, len);
	if (cpu >= 0)
		printf(", on cpu %d", cpu);
	printf("\n\n");

	/* One request: the same source named once per destination. */
	nx_request_reset(request);
	nx_source_list_reset(sources);
	nx_target_list_reset(targets);
	nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
	for (c = 0; c < copies_n; c++) {
		if (nx_source_list_add(sources, nx_source(source, len)) ||
		    nx_target_list_add(targets, nx_target(copies[c], len))) {
			report_errno("build the lists", -EINVAL);
			return 1;
		}
	}
	if (nx_request_set_source_list(request, sources) ||
	    nx_request_set_target_list(request, targets)) {
		report_errno("attach the lists", -EINVAL);
		return 1;
	}

	t0 = now_tb();
	rc = nx_execute(window, request, NULL);
	t1 = now_tb();
	one_request = t1 - t0;

	if (rc) {
		printf("  one request with a repeated source: refused, %s\n",
		       strerror(-rc));
		printf("  (the engine will not read one span more than once)\n");
	} else {
		bool all = true;

		for (c = 0; c < copies_n; c++)
			if (memcmp(copies[c], source, len))
				all = false;

		printf("  one request, source repeated %d times: %8.3f us, %s\n",
		       copies_n, (double)one_request * ns / 1000.0,
		       all ? "every copy correct" : "COPIES DIFFER");
		if (!all)
			return 1;
	}

	/* One request per destination, which is what it is being compared with. */
	for (c = 0; c < copies_n; c++)
		memset(copies[c], 0, len);

	t0 = now_tb();
	for (c = 0; c < copies_n; c++) {
		nx_request_reset(request);
		nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
		nx_request_set_source(request, nx_source(source, len));
		nx_request_set_target(request, nx_target(copies[c], len));
		rc = nx_execute(window, request, NULL);
		if (rc)
			break;
	}
	t1 = now_tb();
	many_requests = t1 - t0;

	if (rc) {
		report_errno("a separate request per destination", rc);
		return 1;
	}

	{
		bool all = true;

		for (c = 0; c < copies_n; c++)
			if (memcmp(copies[c], source, len))
				all = false;

		printf("  %d separate requests:%*s %8.3f us, %s\n", copies_n,
		       24 - 20, "", (double)many_requests * ns / 1000.0,
		       all ? "every copy correct" : "COPIES DIFFER");
	}

	if (one_request && many_requests)
		printf("\n  one request is %.2fx the cost of %d\n",
		       (double)one_request / (double)many_requests, copies_n);

	/*
	 * Where the engine left them. A destination the engine has just written
	 * is read back and timed against a buffer nothing has touched: if the
	 * write landed somewhere close, the first read is cheaper.
	 */
	t0 = now_tb();
	sink = sweep(cold, len);
	t1 = now_tb();
	printf("\n  reading a buffer the engine never touched: %8.3f us\n",
	       (double)(t1 - t0) * ns / 1000.0);

	t0 = now_tb();
	sink = sweep(copies[0], len);
	t1 = now_tb();
	printf("  reading one the engine just wrote:          %8.3f us\n",
	       (double)(t1 - t0) * ns / 1000.0);

	nx_source_list_destroy(&sources);
	nx_target_list_destroy(&targets);
	nx_request_destroy(&request);
	vas_window_close(&window);
	free(source);
	free(cold);
	for (c = 0; c < copies_n; c++)
		free(copies[c]);

	return 0;
}
