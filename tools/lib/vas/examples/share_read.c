// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Does the fabric already broadcast a tile to everyone who wants it?
 *
 * The switchboard broadcasts notification and not data: an ASB_Notify carries
 * an identity that every thread sharing it matches, but the hundred and
 * twenty-eight bytes of a paste land in the FIFO of the one window the paste
 * was addressed to. So a group cannot be handed a tile the way it can be
 * handed a wake.
 *
 * It may not need to be. Coherence is a broadcast medium of its own: when
 * several caches want the same line, the first request fetches it from memory
 * and the rest can be served from a cache that already holds it, without the
 * memory controller being asked again. If that is what happens here, then
 * putting one tile where several workers read it is the broadcast, and no
 * mechanism has to be invented.
 *
 * If instead each reader pays a full fetch, then sharing a tile costs what
 * copying it would have, and a pipeline should give every worker its own.
 *
 * So: N threads read the same buffer, and N threads read buffers of their own,
 * with the same total work either way. The arms differ only in whether the
 * lines are shared, and the aggregate rate says which regime the machine is
 * in. Every buffer is flushed before every pass, so neither arm starts with an
 * advantage the other does not have.
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

#include "report.h"

#define READERS_MAX 8

/* A cache line here, and the unit a flush works on. */
#define LINE 128

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
 * Push a range out of every cache that holds it. Without this the second arm
 * measured a buffer the setup had just written, which is not a cold read of
 * anything.
 */
static void flush(const void *p, size_t len)
{
	const char *q = p;
	size_t i;

	asm volatile("sync" ::: "memory");
	for (i = 0; i < len; i += LINE)
		asm volatile("dcbf 0,%0" :: "r"(q + i) : "memory");
	asm volatile("sync" ::: "memory");
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

static volatile uint64_t sink;
static volatile int go;
static volatile int done;

struct reader {
	pthread_t thread;
	int cpu;
	const unsigned char *buffer;
	size_t len;
	uint64_t ticks;
};

static void *reader_main(void *arg)
{
	struct reader *r = arg;
	uint64_t sum = 0;
	uint64_t t0, t1;
	size_t i;

	pin_to(r->cpu);

	while (!go)
		;

	t0 = now_tb();
	for (i = 0; i < r->len; i += LINE)
		sum += r->buffer[i];
	t1 = now_tb();

	r->ticks = t1 - t0;
	sink = sum;
	__atomic_fetch_add(&done, 1, __ATOMIC_RELEASE);

	return NULL;
}

int main(int argc, char **argv)
{
	struct reader readers[READERS_MAX];
	unsigned char *shared = NULL;
	unsigned char *private_copy[READERS_MAX] = { NULL };
	int isolated[READERS_MAX * 4];
	int isolated_n;
	size_t len = 262144;
	int readers_n = 4;
	int rounds = 10;
	double ns = tb_ns();
	int arm, i, r, c;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--readers") && i + 1 < argc)
			readers_n = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bytes") && i + 1 < argc)
			len = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--rounds") && i + 1 < argc)
			rounds = atoi(argv[++i]);
		else {
			fprintf(stderr,
				"usage: share_read [--readers N] [--bytes N] [--rounds N]\n");
			return 2;
		}
	}
	if (readers_n < 1 || readers_n > READERS_MAX)
		return 2;

	isolated_n = isolated_cpus(isolated, READERS_MAX * 4);
	if (isolated_n < readers_n) {
		fprintf(stderr, "refusing to run: %d isolated cpus, %d readers\n",
			isolated_n, readers_n);
		return 1;
	}

	shared = aligned_alloc(LINE, len);
	if (!shared)
		return 1;
	memset(shared, 0x5a, len);
	for (c = 0; c < readers_n; c++) {
		private_copy[c] = aligned_alloc(LINE, len);
		if (!private_copy[c])
			return 1;
		memset(private_copy[c], 0x5a, len);
	}

	printf("%d readers, %zu bytes each, %d rounds\n\n", readers_n, len,
	       rounds);
	printf("  %-24s %14s %14s\n", "arm", "us per reader", "aggregate GB/s");

	for (arm = 0; arm < 2; arm++) {
		bool share = arm == 0;
		double total = 0.0;

		for (r = 0; r < rounds; r++) {
			uint64_t slowest = 0;

			/* Cold on both arms, so neither starts warm. */
			flush(shared, len);
			for (c = 0; c < readers_n; c++)
				flush(private_copy[c], len);

			go = 0;
			done = 0;
			for (c = 0; c < readers_n; c++) {
				readers[c].cpu = isolated[c];
				readers[c].buffer = share ? shared :
							    private_copy[c];
				readers[c].len = len;
				readers[c].ticks = 0;
				if (pthread_create(&readers[c].thread, NULL,
						   reader_main, &readers[c]))
					return 1;
			}
			/* Let every reader reach its spin before any of them start. */
			usleep(2000);
			go = 1;

			for (c = 0; c < readers_n; c++)
				pthread_join(readers[c].thread, NULL);
			for (c = 0; c < readers_n; c++)
				if (readers[c].ticks > slowest)
					slowest = readers[c].ticks;

			total += (double)slowest * ns / 1000.0;
		}

		{
			double us = total / (double)rounds;
			double bytes = (double)len * (double)readers_n;

			printf("  %-24s %14.3f %14.2f\n",
			       share ? "one buffer, shared" : "a buffer each",
			       us, bytes / (us * 1000.0));
		}
		fflush(stdout);
	}

	free(shared);
	for (c = 0; c < readers_n; c++)
		free(private_copy[c]);

	return 0;
}
