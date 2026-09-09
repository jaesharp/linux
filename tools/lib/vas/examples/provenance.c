// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What machine this was, written down before anything is measured on it.
 *
 * A latency without the clock it was taken at is an anecdote, and a scaling
 * curve without the isolated set is a curve about the scheduler. Both have
 * been produced here and read as results.
 *
 * So a collection begins with this: it checks the machine is in the state the
 * measurements need, refuses if it is not, and writes one record naming the
 * kernel, the command line, the topology, the isolated CPUs and the clock it
 * actually measured -- not the one sysfs promised, which on this hardware
 * reads the boost ceiling while the cores run a fifth slower.
 *
 * The record goes at the head of the file the runs append to, so a reader
 * months later has the machine and the numbers together.
 *
 * Copyright 2026 J Lynn
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "environment.h"

int main(int argc, char **argv)
{
	struct environment env;
	unsigned int need = ENVIRONMENT_STEADY_CLOCK;
	int workers = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--workers") && i + 1 < argc) {
			workers = atoi(argv[++i]);
			need |= ENVIRONMENT_ISOLATED_CPUS;
		} else if (!strcmp(argv[i], "--quiet-clock")) {
			need &= ~(unsigned int)ENVIRONMENT_STEADY_CLOCK;
		} else if (!strcmp(argv[i], "--no-idle")) {
			need |= ENVIRONMENT_NO_IDLE_STATES;
		} else {
			fprintf(stderr,
				"usage: provenance [--workers N] [--no-idle] [--quiet-clock]\n");
			return 2;
		}
	}

	if (environment_require(&env, workers, need))
		return 1;

	environment_print(&env);
	environment_record(&env, stdout);

	return 0;
}
