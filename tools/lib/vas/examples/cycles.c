// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Counting cycles, because nanoseconds are not stable here.
 *
 * This machine runs its cores between 2.366 and 4.2 GHz and decides which
 * without asking the operating system -- the governor is already at
 * performance and scaling_cur_freq already reads the maximum, so there is
 * nothing to set. The effect is not subtle: the same paste sequence measured
 * either 129 ns or 233 ns depending on the run, a ratio of 1.81 against a
 * frequency range of 1.78, and which one a run got was not predictable from
 * anything the run did.
 *
 * A duration in nanoseconds is therefore a statement about two things at once.
 * Counting cycles separates them: the instruction's cost in cycles is what the
 * design depends on, and the frequency it happened to run at is a separate
 * fact worth reporting beside it rather than multiplied into it.
 *
 * The counter is the performance monitor's cycle event, opened on the calling
 * thread only. Reading it needs no privilege because it counts nothing but
 * this thread in user mode.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <errno.h>
#include <linux/perf_event.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "cycles.h"

int cycles_open(struct cycle_counter *c)
{
	struct perf_event_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.type = PERF_TYPE_HARDWARE;
	attr.config = PERF_COUNT_HW_CPU_CYCLES;
	attr.size = sizeof(attr);
	attr.disabled = 1;
	/* This thread in user mode: no privilege needed, and nothing else counted. */
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;

	c->fd = syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0);
	if (c->fd < 0)
		return -errno;

	return 0;
}

void cycles_close(struct cycle_counter *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
}

void cycles_start(struct cycle_counter *c)
{
	if (c->fd < 0)
		return;
	ioctl(c->fd, PERF_EVENT_IOC_RESET, 0);
	ioctl(c->fd, PERF_EVENT_IOC_ENABLE, 0);
}

unsigned long long cycles_stop(struct cycle_counter *c)
{
	unsigned long long count = 0;

	if (c->fd < 0)
		return 0;

	ioctl(c->fd, PERF_EVENT_IOC_DISABLE, 0);
	if (read(c->fd, &count, sizeof(count)) != sizeof(count))
		return 0;

	return count;
}

double cycles_report(const char *what, unsigned long long cycles,
		     unsigned long long ticks, unsigned long operations,
		     double tick_ns)
{
	double ns = (double)ticks * tick_ns / (double)operations;
	double per = (double)cycles / (double)operations;
	double ghz = ticks ? (double)cycles / ((double)ticks * tick_ns) : 0.0;

	if (what)
		printf("  %-16s %8.1f cycles %9.1f ns   at %.2f GHz\n",
		       what, per, ns, ghz);

	return per;
}
