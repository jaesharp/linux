// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What one core-to-core wake costs, measured so the answer is about the wake.
 *
 * Timing a single wake does not work. The wake itself is a fabric operation of
 * a few hundred nanoseconds, and everything around it -- two sync instructions,
 * the copy and paste pair, resuming from wait, reading a clock twice -- costs
 * several microseconds. A difference the size of a fabric traversal disappears
 * under the fixed cost and the run-to-run spread, which is why a single-shot
 * measurement of this shows no difference between chips when there must be one.
 *
 * So this bounces: two processes wake each other in turn, N times, and only
 * the total is timed. The fixed cost of starting is paid once and divided by
 * N, and what is left per round trip is the part that varies with where the
 * two ends sit. One run reports one estimate; the distribution comes from
 * repeating the run, which is the caller's business rather than this program's.
 *
 * Each side needs both halves: a destination the peer wakes, and a window onto
 * the peer's destination. Four windows for two processes, and the descriptors
 * cross in both directions.
 *
 * There is a precondition without which none of it measures a wake, and
 * --settle is how it is met. A notify is matched against the thread the
 * switchboard finds running: one that arrives before the peer has executed its
 * wait instruction finds a running thread, does nothing, and is not queued.
 * In a bounce the two sides race to that instruction, and with no delay the
 * wake is lost nearly every time -- measured here at 365 us per wake against
 * 8.5 us with a two-hundred-iteration spin, the difference being the interval
 * at which some unrelated exception happens to rescue the sleeper.
 *
 * So --settle spins before each paste, long enough that the peer is certainly
 * suspended. It is a constant added to every round trip, which is why it must
 * be held fixed and why only differences between conditions mean anything: the
 * figure is not the cost of a wake, and comparing two runs at different settle
 * values compares the settles.
 *
 * The race cannot be closed. Power ISA 3.0B book II gives
 * "while (not condition), wait" as the idiom and offers no armed state, so
 * there is no atomic check-and-wait. A program that needs a wake to be
 * reliable has to re-send it: a lost notify is harmless and a repeated paste
 * is cheap, so the sender polls and pastes again. That is a different shape
 * from this one, which suspends on both sides in order to time the suspension.
 *
 * --no-wake is the control, and it is not optional reading. It runs everything
 * else unchanged -- the same windows, the same settle, the same sequence
 * numbers, the same wait loop -- and only leaves out the paste. The peer then
 * has nothing to resume it but its own return from wait, so whatever the two
 * runs have in common is not the wake. A figure quoted without its --no-wake
 * companion says nothing about the switchboard.
 *
 * The record on stdout is one JSON object, because that is what a later
 * analysis reads; the human summary goes to stderr, because that is what a
 * person watching reads. Neither is the other's format.
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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <vas/vas.h>

#include "report.h"

#define ITERATIONS_DEFAULT 10000

/*
 * A round trip that takes longer than this was not a pair of wakes. The
 * waiting side also resumes on any exception it happens to take, so a run
 * whose average lands here measured the machine's interrupt rate instead.
 */
#define ROUND_TRIP_LIMIT_US 100.0

/*
 * The two sequence numbers, in memory both processes share. Each side stores
 * its own with release ordering and reads the peer's with acquire, so the
 * store the peer made before waking is visible once its number is seen.
 */
struct pingpong {
	int a_seq;
	int b_seq;
	int ready;
};

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int pin_to(int cpu)
{
	cpu_set_t set;

	if (cpu < 0)
		return 0;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);

	return sched_setaffinity(0, sizeof(set), &set) < 0 ? -errno : 0;
}

static int send_fd(int sock, int fd)
{
	char control[CMSG_SPACE(sizeof(int))] = { 0 };
	char byte = 'd';
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control),
	};
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

	return sendmsg(sock, &msg, 0) < 0 ? -errno : 0;
}

static int recv_fd(int sock)
{
	char control[CMSG_SPACE(sizeof(int))] = { 0 };
	char byte = 0;
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control),
	};
	struct cmsghdr *cmsg;
	int fd;

	if (recvmsg(sock, &msg, 0) < 0)
		return -errno;

	cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS)
		return -EPROTO;

	memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));

	return fd;
}

/* Wait until *@seq reaches @want, suspending between looks. */
static void await(const int *seq, int want)
{
	while (__atomic_load_n(seq, __ATOMIC_ACQUIRE) < want)
		vas_wait();
}

/* Store @value into *@seq so a peer that reads it sees everything before it. */
static void advance(int *seq, int value)
{
	__atomic_store_n(seq, value, __ATOMIC_RELEASE);
}

/*
 * One side of the pair. @mine and @theirs are the two sequence numbers, and
 * @first says which side opens the round trip: A wakes and then waits, B
 * waits and then wakes, so exactly one wake is in flight at a time.
 */
static int bounce(struct vas_window *window, int *mine, const int *theirs,
		  int iterations, bool first, long settle, bool paste)
{
	volatile long spin;
	int i;
	int rc;

	for (i = 1; i <= iterations; i++) {
		if (first) {
			advance(mine, i);
			for (spin = 0; spin < settle; spin++)
				;
			if (paste) {
				rc = vas_wake(window);
				if (rc)
					return rc;
			}
			await(theirs, i);
		} else {
			await(theirs, i);
			advance(mine, i);
			for (spin = 0; spin < settle; spin++)
				;
			if (paste) {
				rc = vas_wake(window);
				if (rc)
					return rc;
			}
		}
	}

	return 0;
}

/*
 * Open this side's destination, trade descriptors with the peer, and open a
 * window onto the peer's. Returns 0 with both handles set.
 */
static int rendezvous(int sock, int cpu, int32_t instance,
		      struct vas_destination **dest, struct vas_window **window)
{
	struct vas_window_attr attr;
	int target;
	int rc;

	rc = pin_to(cpu);
	if (rc)
		return rc;

	rc = vas_destination_open(instance < 0 ? vas_instance_any() :
						 vas_instance(instance), dest);
	if (rc)
		return rc;

	rc = send_fd(sock, vas_destination_fd(*dest));
	if (rc)
		return rc;

	target = recv_fd(sock);
	if (target < 0)
		return target;

	vas_window_attr_init(&attr, VAS_COP_FTW);
	attr.wake_target = target;

	rc = vas_window_open(&attr, window);
	close(target);

	return rc;
}

int main(int argc, char **argv)
{
	int iterations = ITERATIONS_DEFAULT;
	int a_cpu = -1, b_cpu = -1;
	int32_t a_instance = -1, b_instance = -1;
	const char *label = "unlabelled";
	bool paste = true;
	long settle = 0;
	struct vas_destination *dest = NULL;
	struct vas_window *window = NULL;
	struct pingpong *shared;
	long long start, elapsed;
	double round_trip_us, one_way_us;
	int sv[2];
	int waited, rc, i;
	pid_t pid;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--iterations") && i + 1 < argc)
			iterations = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--a-cpu") && i + 1 < argc)
			a_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--b-cpu") && i + 1 < argc)
			b_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--a-instance") && i + 1 < argc)
			a_instance = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--b-instance") && i + 1 < argc)
			b_instance = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--settle") && i + 1 < argc)
			settle = atol(argv[++i]);
		else if (!strcmp(argv[i], "--no-wake"))
			paste = false;
		else if (!strcmp(argv[i], "--label") && i + 1 < argc)
			label = argv[++i];
		else {
			fprintf(stderr,
				"usage: pingpong [--iterations N] [--a-cpu N] [--b-cpu N]\n"
				"                [--a-instance N] [--b-instance N]\n"
				"                [--settle SPINS] [--label S] [--no-wake]\n"
				"\n"
				"--settle spins before each paste so the peer is certainly\n"
				"suspended when the wake arrives. It adds a constant to every\n"
				"round trip: hold it fixed and compare conditions, never\n"
				"compare runs that used different values.\n");
			return 2;
		}
	}

	if (iterations < 1)
		return 2;

	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		report_errno("map the shared counters", -errno);
		return 1;
	}
	shared->a_seq = 0;
	shared->b_seq = 0;
	shared->ready = 0;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		report_errno("socketpair", -errno);
		return 1;
	}

	pid = fork();
	if (pid < 0) {
		report_errno("fork", -errno);
		return 1;
	}

	if (pid == 0) {
		close(sv[0]);
		rc = rendezvous(sv[1], b_cpu, b_instance, &dest, &window);
		if (rc) {
			report_errno("B: rendezvous", rc);
			/* Unblock A, which is waiting on a peer that failed. */
			advance(&shared->b_seq, iterations);
			_exit(1);
		}
		advance(&shared->ready, 1);
		rc = bounce(window, &shared->b_seq, &shared->a_seq, iterations,
			    false, settle, paste);
		if (rc)
			report_errno("B: wake", rc);
		vas_window_close(&window);
		vas_destination_close(&dest);
		_exit(rc ? 1 : 0);
	}

	close(sv[1]);
	rc = rendezvous(sv[0], a_cpu, a_instance, &dest, &window);
	if (rc) {
		report_errno("A: rendezvous", rc);
		advance(&shared->a_seq, iterations);
		waitpid(pid, &waited, 0);
		return 1;
	}

	/* Both sides open before the clock starts; setup is not the subject. */
	while (!__atomic_load_n(&shared->ready, __ATOMIC_ACQUIRE))
		;

	start = now_ns();
	rc = bounce(window, &shared->a_seq, &shared->b_seq, iterations, true,
		    settle, paste);
	elapsed = now_ns() - start;

	vas_window_close(&window);
	vas_destination_close(&dest);

	if (waitpid(pid, &waited, 0) > 0 &&
	    (!WIFEXITED(waited) || WEXITSTATUS(waited) != 0))
		rc = rc ? rc : -EIO;

	if (rc) {
		report_errno("A: wake", rc);
		return 1;
	}

	round_trip_us = (double)elapsed / 1000.0 / iterations;
	one_way_us = round_trip_us / 2.0;

	/* The record. */
	printf("{\"label\":\"%s\",\"iterations\":%d,\"a_cpu\":%d,\"b_cpu\":%d,"
	       "\"a_instance\":%d,\"b_instance\":%d,\"settle\":%ld,\"paste\":%s,"
	       "\"elapsed_ns\":%lld,\"round_trip_us\":%.4f,\"one_way_us\":%.4f}\n",
	       label, iterations, a_cpu, b_cpu, a_instance, b_instance, settle,
	       paste ? "true" : "false", elapsed, round_trip_us, one_way_us);
	fflush(stdout);

	/* The report. */
	fprintf(stderr, "%s: %.3f us per %s, %d round trips\n",
		label, one_way_us, paste ? "wake" : "hop with no wake sent",
		iterations);

	if (round_trip_us > ROUND_TRIP_LIMIT_US) {
		fprintf(stderr,
			"  too slow to have been wakes: the loops resumed on something else\n");
		return 1;
	}

	return 0;
}
