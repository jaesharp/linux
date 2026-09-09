// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What a wake costs when many windows are in use at once.
 *
 * The switchboard caches window contexts -- 128 of them per chip -- and casts
 * out the ones it has not needed. A program using a handful of windows never
 * notices. A program using more than fit must have contexts fetched back
 * before each paste can be routed, and the cost of that appears nowhere in the
 * single-window figures every other measurement here reports.
 *
 * So this opens N destinations and N windows onto them, pastes to each in
 * turn, and reports the cost per wake as N grows. Cycling through all of them
 * is what creates the pressure: touching one window repeatedly keeps its
 * context resident however many others exist.
 *
 * Which is also the control. The same N windows are opened either way, so the
 * kernel has done the same work and the same credits are drawn; the two arms
 * differ only in whether the sender walks the set or stays on one window. A
 * difference between them is contexts being fetched, and anything they share
 * is not.
 *
 * The receivers are threads sharing whatever CPUs are isolated, so past a
 * handful they are oversubscribed and their latency means little. That is
 * deliberate -- the quantity here is the sender's cost to route a paste, which
 * does not depend on the receiver being scheduled, and measuring it needs many
 * windows rather than many cores.
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

#define WINDOWS_MAX 512

/* Pastes per window per measurement, once the set is built. */
#define PASTES 2000

static volatile int stop;

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

struct waiter {
	pthread_t thread;
	int cpu;
	struct vas_destination *dest;
	volatile int ready;
	int rc;
};

static void *waiter_main(void *arg)
{
	struct waiter *w = arg;

	pin_to(w->cpu);

	w->rc = vas_destination_open(vas_instance_any(), &w->dest);
	if (w->rc) {
		w->ready = -1;
		return NULL;
	}

	w->ready = 1;

	while (!stop)
		vas_wait();

	return NULL;
}

int main(int argc, char **argv)
{
	static struct waiter waiters[WINDOWS_MAX];
	static struct vas_window *windows[WINDOWS_MAX];
	int isolated[64];
	int isolated_n;
	int steps[] = { 1, 2, 4, 8, 16, 32, 64, 96, 128, 160, 192, 256 };
	int windows_max = 256;
	double ns = tb_ns();
	size_t s;
	int i, rc, opened = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--windows") && i + 1 < argc)
			windows_max = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: wake_scale [--windows N]\n");
			return 2;
		}
	}
	if (windows_max < 1 || windows_max > WINDOWS_MAX)
		return 2;

	isolated_n = isolated_cpus(isolated, 64);
	if (isolated_n < 1) {
		fprintf(stderr, "refusing to run: no isolated cpus\n");
		return 1;
	}

	printf("windows opened in pairs, receivers spread over %d isolated cpus\n\n",
	       isolated_n);
	printf("  %-10s %14s %14s %12s\n", "windows", "cycling", "one window",
	       "difference");

	for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
		uint64_t t0, t1;
		double walk, stay;
		int want = steps[s];
		int p, w;

		if (want > windows_max)
			break;

		/* Grow the set to `want`, keeping what is already open. */
		while (opened < want) {
			struct vas_window_attr attr;

			waiters[opened].cpu = isolated[opened % isolated_n];
			if (pthread_create(&waiters[opened].thread, NULL,
					   waiter_main, &waiters[opened])) {
				report_errno("pthread_create", -errno);
				goto done;
			}
			while (!waiters[opened].ready)
				;
			if (waiters[opened].ready < 0) {
				report_errno("open a destination",
					     waiters[opened].rc);
				goto done;
			}

			vas_window_attr_init(&attr, VAS_COP_FTW);
			attr.wake_target = vas_destination_fd(waiters[opened].dest);
			rc = vas_window_open(&attr, &windows[opened]);
			if (rc) {
				printf("  stopped at %d windows: %s\n", opened,
				       strerror(-rc));
				goto report;
			}
			opened++;
		}

		/* Cycling: every paste lands on a different window's context. */
		t0 = now_tb();
		for (p = 0; p < PASTES; p++)
			for (w = 0; w < want; w++)
				vas_wake(windows[w]);
		t1 = now_tb();
		walk = (double)(t1 - t0) * ns / ((double)PASTES * want);

		/* One window: the same count of pastes, one resident context. */
		t0 = now_tb();
		for (p = 0; p < PASTES; p++)
			for (w = 0; w < want; w++)
				vas_wake(windows[0]);
		t1 = now_tb();
		stay = (double)(t1 - t0) * ns / ((double)PASTES * want);

		printf("  %-10d %11.1f ns %11.1f ns %9.1f ns\n",
		       want, walk, stay, walk - stay);
		fflush(stdout);
	}

report:
done:
	stop = 1;
	for (i = 0; i < opened; i++) {
		if (windows[i]) {
			vas_wake(windows[i]);
			vas_window_close(&windows[i]);
		}
	}
	for (i = 0; i < opened; i++) {
		pthread_join(waiters[i].thread, NULL);
		vas_destination_close(&waiters[i].dest);
	}

	return 0;
}
