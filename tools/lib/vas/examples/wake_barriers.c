// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What the parts of a wake cost.
 *
 * The sequence a sender executes is a barrier, a copy, a paste and a barrier.
 * Most of a same-chip wake is this, not the fabric, so the division matters:
 * a trailing barrier that a caller does not need is latency given away on
 * every wake.
 *
 * The variants live in wake_barriers.S. Writing them in C put the compiler
 * between the intent and the instructions once already, and a sequence whose
 * whole point is the ordering of four instructions is not one to leave to a
 * scheduler.
 *
 * Whose destination is pasted to turns out to matter. A sender pasting to a
 * destination it opened itself is naming a thread that is running -- itself --
 * and the notify then arrives at a core that is not waiting for it. --peer
 * puts the destination on another thread that is parked in wait, which is what
 * a sender actually does, and the two differ by more than the barriers do.
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

#include <vas/vas.h>

#include "report.h"

static unsigned long iterations = 200000;

uint64_t paste_full(void *target, void *block, uint64_t n);
uint64_t paste_no_trailing(void *target, void *block, uint64_t n);
uint64_t paste_bare(void *target, void *block, uint64_t n);
uint64_t paste_lwsync(void *target, void *block, uint64_t n);
uint64_t barriers_only(void *target, void *block, uint64_t n);
uint64_t paste_accepted(void *target, void *block, uint64_t n);

struct variant {
	const char *name;
	const char *sequence;
	uint64_t (*run)(void *, void *, uint64_t);
};

static const struct variant variants[] = {
	{ "as sent today", "hwsync copy paste. hwsync", paste_full },
	{ "no trailing", "hwsync copy paste.", paste_no_trailing },
	{ "no barriers", "copy paste.", paste_bare },
	{ "lwsync leading", "lwsync copy paste.", paste_lwsync },
	{ "barriers alone", "hwsync hwsync", barriers_only },
};

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

struct peer {
	int cpu;
	struct vas_destination *dest;
	volatile int ready;
	volatile int stop;
	int rc;
};

static void *peer_main(void *arg)
{
	struct peer *p = arg;
	cpu_set_t set;

	if (p->cpu >= 0) {
		CPU_ZERO(&set);
		CPU_SET(p->cpu, &set);
		sched_setaffinity(0, sizeof(set), &set);
	}

	p->rc = vas_destination_open(vas_instance_any(), &p->dest);
	if (p->rc) {
		p->ready = -1;
		return NULL;
	}

	p->ready = 1;
	while (!p->stop)
		vas_wait();

	return NULL;
}

int main(int argc, char **argv)
{
	static _Alignas(128) char block[128];
	struct peer peer = { .cpu = -1 };
	pthread_t peer_thread;
	bool use_peer = false;
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	struct vas_destination *dest = NULL;
	double ns = tb_ns();
	void *target;
	size_t v;
	int cpu = -1, i, rc;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
			cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
			iterations = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--peer") && i + 1 < argc) {
			peer.cpu = atoi(argv[++i]);
			use_peer = true;
		} else {
			fprintf(stderr,
				"usage: wake_barriers [--cpu N] [--peer N]"
				" [--iterations N]\n");
			return 2;
		}
	}

	if (cpu >= 0) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		sched_setaffinity(0, sizeof(set), &set);
	}

	if (use_peer) {
		if (pthread_create(&peer_thread, NULL, peer_main, &peer)) {
			report_errno("pthread_create", -errno);
			return 1;
		}
		while (!peer.ready)
			;
		if (peer.ready < 0) {
			report_errno("peer could not open a destination", peer.rc);
			return peer.rc == -ENODEV ? 77 : 1;
		}
	} else {
		rc = vas_destination_open(vas_instance_any(), &dest);
		if (rc) {
			report_errno("open a destination", rc);
			return rc == -ENODEV ? 77 : 1;
		}
	}

	vas_window_attr_init(&attr, VAS_COP_FTW);
	attr.wake_target = vas_destination_fd(use_peer ? peer.dest : dest);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("open a window", rc);
		return 1;
	}

	target = vas_window_paste_target(window);

	printf("%lu pastes per variant", iterations);
	if (cpu >= 0)
		printf(", sending from cpu %d", cpu);
	if (use_peer)
		printf(", to a thread waiting on cpu %d", peer.cpu);
	else
		printf(", to this thread's own destination");
	printf("\n\n");
	/*
	 * Before any timing means anything: were the pastes taken? A refused
	 * paste is a different operation with a different cost, and CR0 is the
	 * only thing that says which one was measured.
	 */
	{
		uint64_t taken = paste_accepted(target, block, 10000);

		if (taken != 10000) {
			fprintf(stderr,
				"switchboard accepted %llu of 10000 pastes;"
				" the timings below would not be of a wake\n",
				(unsigned long long)taken);
			return 1;
		}
		printf("all of 10000 trial pastes accepted\n\n");
	}

	printf("  %-16s %-28s %12s\n", "variant", "sequence", "per paste");

	for (v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
		uint64_t ticks;

		variants[v].run(target, block, iterations / 10 + 1);
		ticks = variants[v].run(target, block, iterations);

		printf("  %-16s %-28s %9.1f ns\n", variants[v].name,
		       variants[v].sequence,
		       (double)ticks * ns / (double)iterations);
	}

	vas_window_close(&window);
	if (use_peer) {
		peer.stop = 1;
		pthread_join(peer_thread, NULL);
		vas_destination_close(&peer.dest);
	} else {
		vas_destination_close(&dest);
	}

	return 0;
}
