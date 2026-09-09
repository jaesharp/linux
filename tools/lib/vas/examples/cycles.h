/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A cycle count for the calling thread.
 *
 * Wall-clock time on this machine is a statement about the instruction and
 * about whatever frequency the core chose, which varies by a factor of 1.78
 * between runs and is not settable. Cycles are the part a design can rely on.
 */

#ifndef _VAS_EXAMPLES_CYCLES_H
#define _VAS_EXAMPLES_CYCLES_H

struct cycle_counter {
	int fd;
};

#define CYCLE_COUNTER_INIT { .fd = -1 }

/* Returns 0, or a negative errno if the counter is unavailable. */
int cycles_open(struct cycle_counter *c);
void cycles_close(struct cycle_counter *c);

void cycles_start(struct cycle_counter *c);
unsigned long long cycles_stop(struct cycle_counter *c);

/*
 * Print cycles and nanoseconds per operation and the frequency implied by
 * their ratio, so a run at an unexpected clock is visible rather than folded
 * into the result. Returns cycles per operation.
 */
double cycles_report(const char *what, unsigned long long cycles,
		     unsigned long long ticks, unsigned long operations,
		     double tick_ns);

#endif /* _VAS_EXAMPLES_CYCLES_H */
