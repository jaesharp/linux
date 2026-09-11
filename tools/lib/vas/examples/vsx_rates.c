// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What the vector pack, permute and multiply-accumulate operations cost.
 *
 * Two numbers, not one, and a pipeline is designed against whichever binds.
 *
 *   - Latency: each operation waits for the previous one's result. An
 *     accumulation is that shape, so a recurrence cannot go faster than the
 *     latency however wide the machine is.
 *   - Throughput: independent operations, enough in flight to fill the
 *     pipeline. A stream of unrelated elements is that shape.
 *
 * They differ by the pipeline depth, so quoting one where the other applies is
 * wrong by that factor.
 *
 * The reason to have them: the atomic memory operations that make a reduction
 * cheap are integer only and eight bytes wide, so a floating-point pipeline
 * must either accumulate in the core and convert, or work in fixed point
 * throughout. Which is right depends on what a convert costs against a
 * multiply-accumulate, and neither is guessable.
 *
 * The loops themselves are in vsx_loops.S, hand-written, because every C form
 * of them spilled the accumulators into the loop and measured the spill.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Enough that the loop branch is a rounding error against the operation. */
#define ITERATIONS 200000000UL

/* Independent chains in the throughput arm, matching vsx_loops.S. */
#define WIDTH 8

#define DECLARE(name)					\
	uint64_t lat_##name(uint64_t iterations);	\
	uint64_t thr_##name(uint64_t iterations)

DECLARE(fma);
DECLARE(fmad);
DECLARE(imac);
DECLARE(imacd);
DECLARE(perm);
DECLARE(pack);
DECLARE(unpack);
DECLARE(cvt_f2i);
DECLARE(cvt_i2f);
DECLARE(cvt_f2h);
DECLARE(cvt_h2f);

struct measurement {
	const char *name;
	const char *what;
	uint64_t (*latency)(uint64_t);
	uint64_t (*throughput)(uint64_t);
};

static const struct measurement measurements[] = {
	{ "xvmaddasp", "float multiply-add, single", lat_fma, thr_fma },
	{ "xvmaddadp", "float multiply-add, double", lat_fmad, thr_fmad },
	{ "vmsumuhm", "integer multiply-sum, half", lat_imac, thr_imac },
	{ "vmsumudm", "integer multiply-sum, double", lat_imacd, thr_imacd },
	{ "xxperm", "byte permute", lat_perm, thr_perm },
	{ "vpkudum", "pack, narrowing", lat_pack, thr_pack },
	{ "vupkhsw", "unpack, widening", lat_unpack, thr_unpack },
	{ "xvcvspsxws", "single to integer", lat_cvt_f2i, thr_cvt_f2i },
	{ "xvcvsxwsp", "integer to single", lat_cvt_i2f, thr_cvt_i2f },
	{ "xvcvsphp", "single to half", lat_cvt_f2h, thr_cvt_f2h },
	{ "xvcvhpsp", "half to single", lat_cvt_h2f, thr_cvt_h2f },
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

static void pin_to(int cpu)
{
	cpu_set_t set;

	if (cpu < 0)
		return;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	sched_setaffinity(0, sizeof(set), &set);
}

int main(int argc, char **argv)
{
	double ns = tb_ns();
	int cpu = -1;
	size_t m;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--cpu") && i + 1 < argc)
			cpu = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: vsx_rates [--cpu N]\n");
			return 2;
		}
	}

	pin_to(cpu);

	printf("%lu operations each, %d independent chains for throughput",
	       ITERATIONS, WIDTH);
	if (cpu >= 0)
		printf(", on cpu %d", cpu);
	printf("\n\n");
	printf("  %-12s %-30s %11s %12s %10s\n",
	       "instruction", "", "latency", "throughput", "in flight");

	for (m = 0; m < sizeof(measurements) / sizeof(measurements[0]); m++) {
		double lat, thr;

		/* Once to warm, then the run that counts. */
		measurements[m].latency(ITERATIONS / 100);
		lat = (double)measurements[m].latency(ITERATIONS) * ns /
		      (double)ITERATIONS;

		measurements[m].throughput(ITERATIONS / 100);
		thr = (double)measurements[m].throughput(ITERATIONS) * ns /
		      (double)((ITERATIONS / WIDTH) * WIDTH);

		printf("  %-12s %-30s %8.3f ns %9.3f ns %8.1f\n",
		       measurements[m].name, measurements[m].what, lat, thr,
		       thr > 0.0 ? lat / thr : 0.0);
	}

	return 0;
}
