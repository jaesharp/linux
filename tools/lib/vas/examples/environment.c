// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Refusing to measure a machine that is not ready, and writing down the one
 * that was.
 *
 * Each check here exists because a measurement was taken without it and read
 * as a result. The core clock moved by a factor of 1.78 between runs with
 * nothing in sysfs changing, so the same loop timed 129 ns and 233 ns and
 * neither was wrong. Threads were placed on CPUs the scheduler still used, and
 * a wake that should never be lost went missing eight per cent of the time.
 * Workers were pinned to CPU numbers the machine does not bring up, where
 * pinning fails quietly and the thread stays where it was.
 *
 * None of those announced themselves. Each produced a plausible number, which
 * is why these are refusals rather than warnings: a warning on a machine that
 * is measured unattended is a line of output nobody reads, and the number
 * beside it looks exactly like a good one.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cycles.h"
#include "environment.h"

#define CPUFREQ "/sys/devices/system/cpu/cpufreq"

/* Long enough that the count is not dominated by starting and stopping it. */
#define CLOCK_SAMPLE_US 20000

/* The measured clock must agree with what the governor was told this closely. */
#define CLOCK_TOLERANCE 0.05

static inline uint64_t now_tb(void)
{
	uint64_t tb;

	asm volatile("mfspr %0, 268" : "=r"(tb));

	return tb;
}

static double tb_hz(void)
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

	return hz;
}

static int read_line(const char *path, char *out, size_t len)
{
	FILE *f = fopen(path, "r");
	size_t n;

	out[0] = '\0';
	if (!f)
		return -ENOENT;
	if (!fgets(out, (int)len, f)) {
		fclose(f);
		return -EIO;
	}
	fclose(f);

	n = strlen(out);
	while (n && (out[n - 1] == '\n' || out[n - 1] == ' '))
		out[--n] = '\0';

	return 0;
}

static long read_long(const char *path, long fallback)
{
	char buf[64];

	if (read_line(path, buf, sizeof(buf)))
		return fallback;

	return strtol(buf, NULL, 10);
}

/*
 * A range list as sysfs writes them. The numbering is what matters and not the
 * count: this machine numbers CPUs to 141 and brings up 72, so anything that
 * walks a count places work on CPUs that are not there.
 */
static int parse_list(const char *text, int *out, int max)
{
	const char *p = text;
	int n = 0;

	while (*p && n < max) {
		int lo, hi, len = 0;

		if (sscanf(p, "%d-%d%n", &lo, &hi, &len) == 2)
			;
		else if (sscanf(p, "%d%n", &lo, &len) == 1)
			hi = lo;
		else
			break;
		for (; lo <= hi && n < max; lo++)
			out[n++] = lo;
		p += len;
		if (*p == ',')
			p++;
	}

	return n;
}

static int read_list(const char *path, int *out, int max)
{
	char buf[4096];

	if (read_line(path, buf, sizeof(buf)))
		return 0;

	return parse_list(buf, out, max);
}

static bool holds(const int *set, int n, int value)
{
	int i;

	for (i = 0; i < n; i++)
		if (set[i] == value)
			return true;

	return false;
}

/*
 * Every reason a chip has given for running slower than it was asked, added
 * up. The turbo counters are excluded: they record the clock going up, which
 * is not a broken measurement, only a different one.
 */
static unsigned long long throttle_events(const struct environment *env)
{
	unsigned long long total = 0;
	int i;

	for (i = 0; i < env->online_n; i++) {
		static const char * const reason[] = {
			"powercap", "overtemp", "supply_fault",
			"overcurrent", "occ_reset",
		};
		size_t r;

		for (r = 0; r < sizeof(reason) / sizeof(reason[0]); r++) {
			char path[160];

			snprintf(path, sizeof(path),
				 "/sys/devices/system/cpu/cpu%d/cpufreq/throttle_stats/%s",
				 env->online[i], reason[r]);
			total += (unsigned long long)read_long(path, 0);
		}
	}

	return total;
}

/* What the core actually ran at, which is not what sysfs says it will. */
static double measure_clock(void)
{
	struct cycle_counter counter = CYCLE_COUNTER_INIT;
	double ns = 1000000000.0 / tb_hz();
	uint64_t ticks = (uint64_t)(CLOCK_SAMPLE_US * 1000.0 / ns);
	uint64_t began, spin = 0;
	unsigned long long spent;

	if (cycles_open(&counter))
		return 0.0;

	cycles_start(&counter);
	began = now_tb();
	while (now_tb() - began < ticks)
		spin++;
	spent = cycles_stop(&counter);
	cycles_close(&counter);

	(void)spin;

	return (double)spent / ((double)CLOCK_SAMPLE_US * 1000.0);
}

static int count_chips_and_cores(struct environment *env)
{
	int chips[ENVIRONMENT_CPUS_MAX];
	int i;

	env->chips = 0;
	env->cores = 0;

	for (i = 0; i < env->online_n; i++) {
		char path[160];
		char buf[256];
		int siblings[8], sn, chip;

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/topology/physical_package_id",
			 env->online[i]);
		chip = (int)read_long(path, 0);
		/* Chip numbers are not a dense range: the two here are 0 and 8. */
		if (!holds(chips, env->chips, chip) &&
		    env->chips < ENVIRONMENT_CPUS_MAX)
			chips[env->chips++] = chip;

		snprintf(path, sizeof(path),
			 "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
			 env->online[i]);
		if (read_line(path, buf, sizeof(buf)))
			continue;
		sn = parse_list(buf, siblings, 8);
		/* Count a core once, at its lowest-numbered thread. */
		if (sn && siblings[0] == env->online[i])
			env->cores++;
	}

	return 0;
}

static void explain(const char *what, const char *why, const char *fix)
{
	fprintf(stderr, "  %s\n    %s\n    help: %s\n", what, why, fix);
}

int environment_require(struct environment *env, int workers,
			unsigned int needed)
{
	int raw[ENVIRONMENT_CPUS_MAX];
	int raw_n, i;
	int refused = 0;

	memset(env, 0, sizeof(*env));
	env->needed = needed;

	read_line("/proc/sys/kernel/osrelease", env->release, sizeof(env->release));
	read_line("/proc/cmdline", env->cmdline, sizeof(env->cmdline));

	env->online_n = read_list("/sys/devices/system/cpu/online", env->online,
				  ENVIRONMENT_CPUS_MAX);
	if (!env->online_n) {
		fprintf(stderr, "cannot read which CPUs are online\n");
		return -ENOENT;
	}
	count_chips_and_cores(env);

	/*
	 * Isolated and online, not merely isolated: a command line may name
	 * CPUs the machine never brings up, and pinning to one of those fails
	 * without saying so.
	 */
	raw_n = read_list("/sys/devices/system/cpu/isolated", raw,
			  ENVIRONMENT_CPUS_MAX);
	for (i = 0; i < raw_n; i++)
		if (holds(env->online, env->online_n, raw[i]))
			env->isolated[env->isolated_n++] = raw[i];

	env->asked_khz_min = (unsigned long)read_long(CPUFREQ "/policy0/scaling_min_freq", 0);
	env->asked_khz_max = (unsigned long)read_long(CPUFREQ "/policy0/scaling_max_freq", 0);
	env->boost = read_long(CPUFREQ "/boost", 0) != 0;
	env->idle_states_present =
		access("/sys/devices/system/cpu/cpu0/cpuidle/state0", F_OK) == 0;
	env->throttle_at_start = throttle_events(env);
	env->measured_ghz = measure_clock();

	fprintf(stderr, "measuring on %s\n", env->release);

	if (needed & ENVIRONMENT_STEADY_CLOCK) {
		double asked = (double)env->asked_khz_max / 1e6;

		if (env->boost) {
			explain("the core clock is free to move",
				"boost is on, so the firmware raises the clock above the "
				"pstate table using headroom it does not report",
				"bench/pin-clock.sh hold");
			refused++;
		} else if (env->asked_khz_min != env->asked_khz_max) {
			explain("the core clock is free to move",
				"the governor may pick anything between its minimum and "
				"its maximum, and does",
				"bench/pin-clock.sh hold");
			refused++;
		} else if (asked > 0.0 &&
			   (env->measured_ghz < asked * (1.0 - CLOCK_TOLERANCE) ||
			    env->measured_ghz > asked * (1.0 + CLOCK_TOLERANCE))) {
			fprintf(stderr,
				"  the clock was pinned and did not hold\n"
				"    asked for %.3f GHz, measured %.3f GHz\n"
				"    help: check cpufreq/throttle_stats; a chip over its "
				"power or thermal budget ignores the governor\n",
				asked, env->measured_ghz);
			refused++;
		}
	}

	if (needed & ENVIRONMENT_ISOLATED_CPUS) {
		if (env->isolated_n < workers) {
			fprintf(stderr,
				"  not enough CPUs are kept clear\n"
				"    %d are isolated and online, %d workers were asked for\n"
				"    help: boot with isolcpus= covering more, or lower the "
				"worker count\n",
				env->isolated_n, workers);
			refused++;
		}
	}

	if ((needed & ENVIRONMENT_NO_IDLE_STATES) && env->idle_states_present) {
		explain("cores may enter a stop state",
			"a wake that has to bring a core back costs milliseconds, "
			"which lands on whichever CPUs are least used and reads "
			"like a distance effect",
			"boot with powersave=off, or disable every cpuidle state");
		refused++;
	}

	if (refused) {
		fprintf(stderr,
			"refusing to measure: %d precondition%s unmet\n",
			refused, refused == 1 ? "" : "s");
		return -EINVAL;
	}

	return 0;
}

int environment_verify(const struct environment *env)
{
	unsigned long long now = throttle_events(env);
	double ghz;

	if (now != env->throttle_at_start) {
		fprintf(stderr,
			"discard this run: the machine throttled while measuring\n"
			"  %llu events during the run, against %llu before it\n",
			now - env->throttle_at_start, env->throttle_at_start);
		return -EIO;
	}

	if (!(env->needed & ENVIRONMENT_STEADY_CLOCK))
		return 0;

	ghz = measure_clock();
	if (env->measured_ghz > 0.0 &&
	    (ghz < env->measured_ghz * (1.0 - CLOCK_TOLERANCE) ||
	     ghz > env->measured_ghz * (1.0 + CLOCK_TOLERANCE))) {
		fprintf(stderr,
			"discard this run: the clock moved while measuring\n"
			"  %.3f GHz before, %.3f GHz after\n",
			env->measured_ghz, ghz);
		return -EIO;
	}

	return 0;
}

void environment_print(const struct environment *env)
{
	fprintf(stderr,
		"  %d cpus online over %d cores of %d chips, %d isolated\n",
		env->online_n, env->cores, env->chips, env->isolated_n);
	fprintf(stderr, "  clock %.3f GHz measured", env->measured_ghz);
	if (env->asked_khz_min == env->asked_khz_max && !env->boost)
		fprintf(stderr, ", held at %.3f\n",
			(double)env->asked_khz_max / 1e6);
	else
		fprintf(stderr, ", free between %.3f and %.3f%s\n",
			(double)env->asked_khz_min / 1e6,
			(double)env->asked_khz_max / 1e6,
			env->boost ? " with boost above that" : "");
}

void environment_record(const struct environment *env, FILE *out)
{
	int i;

	fprintf(out, "{\"kind\":\"environment\",\"release\":\"%s\",", env->release);
	fprintf(out, "\"cmdline\":\"%s\",", env->cmdline);
	fprintf(out, "\"online\":%d,\"cores\":%d,\"chips\":%d,",
		env->online_n, env->cores, env->chips);
	fprintf(out, "\"measured_ghz\":%.4f,\"asked_khz\":[%lu,%lu],\"boost\":%s,",
		env->measured_ghz, env->asked_khz_min, env->asked_khz_max,
		env->boost ? "true" : "false");
	fprintf(out, "\"idle_states\":%s,\"throttle\":%llu,",
		env->idle_states_present ? "true" : "false",
		env->throttle_at_start);
	fprintf(out, "\"isolated\":[");
	for (i = 0; i < env->isolated_n; i++)
		fprintf(out, "%s%d", i ? "," : "", env->isolated[i]);
	fprintf(out, "]}\n");
}
