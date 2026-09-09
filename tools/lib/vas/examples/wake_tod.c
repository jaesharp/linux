// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What a wake costs, with nothing on the measured path but the wake.
 *
 * The first attempt at this timed each wait with clock_gettime and concluded
 * that the wait instruction never suspends. It does. clock_gettime costs about
 * a microsecond on this machine and sat immediately before every wait, so it
 * consumed the very interval being measured and left a residue of sixty
 * nanoseconds that looked like an instruction doing nothing. An instrument
 * that costs as much as its subject does not measure it.
 *
 * So the clock here is the timebase, read with mftb: one instruction, four
 * nanoseconds, no system call. And the arithmetic is not done by the waking
 * thread at all. Power ISA 3.0B section 4.5 puts an execution unit next to
 * memory and gives stdat to reach it, so the sender leaves the negated send
 * time in a cell and the receiver adds its own timebase to that cell with a
 * single Store Add. The subtraction happens at memory. What the receiver
 * executes between resuming and being finished is mftb and stdat -- about as
 * close to nothing as this can be made.
 *
 * That matters beyond tidiness. The quantity is a few hundred nanoseconds, so
 * anything the receiver does after resuming is not overhead to be subtracted
 * later but a delay that changes what is being measured: work on the resume
 * path makes the wake look slower and, worse, makes a lost wake look like a
 * slow one.
 *
 * The clock the two threads share is the timebase, so the latency itself can
 * only be a time: cycles are counted per thread and a cross-thread difference
 * has no meaning in them. What the cycle counter is for is the conversion. The
 * core clock here moves by a factor of 1.78 between runs and is not settable,
 * so a figure in nanoseconds is also a statement about which clock the run got.
 * Counting the sender's cycles over the whole run gives the frequency it
 * actually ran at, and the same latency is then reported in cycles as well --
 * which is the part that does not move.
 *
 * --no-wake is the control and the run is not interpretable without it. It
 * leaves out only the paste. The receiver then has nothing but its own return
 * from wait, so whatever the two runs share is not the wake.
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
#include <unistd.h>

#include <vas/vas.h>

#include "cycles.h"
#include "environment.h"
#include "report.h"

/*
 * Store Atomic function codes, Power ISA 3.0B Figure 4. Only Store Add is
 * used here; the others are named because a reader checking the encoding
 * should not have to find the table to know that 0 is not arbitrary.
 */
enum vas_store_atomic {
	STORE_ATOMIC_ADD = 0,
	STORE_ATOMIC_MAX_UNSIGNED = 4,
	STORE_ATOMIC_MIN_UNSIGNED = 6,
};

/*
 * How long to give one trial before sending again, and how many times. The
 * bound is far longer than a wake and far shorter than a person waiting.
 */
#define PATIENCE_US 200
#define RESEND_MAX 8

/*
 * What counts as delivered by the notify rather than by whatever else resumes
 * a thread. The wake is a few hundred nanoseconds even across chips and the
 * incidental resumes sit around a microsecond, so this separates them with
 * room to spare -- and the control measures how often a coincidence lands
 * inside it anyway, which is the only reason the number means anything.
 */
#define DELIVERED_NS 700.0

/* The timebase, and the frequency the kernel publishes for it. */
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

/* mem += value, performed at memory rather than by this thread. */
static inline void store_add(volatile uint64_t *mem, uint64_t value)
{
	asm volatile("stdat %0, %1, %2"
		     :: "r"(value), "b"(mem), "i"(STORE_ATOMIC_ADD)
		     : "memory");
}

/*
 * One cell per quantity, each on its own cache line: the sender writes the
 * seed and the receiver's atomic lands on the same line, and sharing a line
 * with the sequence number would put the two in each other's way.
 */
struct shared {
	uint64_t delta;			/* seeded -t_send, receiver adds t_recv */
	char pad0[120];
	uint64_t answered;		/* the trial the receiver has answered */
	char pad1[120];
	uint64_t trial;			/* the trial the sender is asking about */
	char pad2[120];
};

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

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/*
 * The receiver. Suspends, and on every resume folds its timebase into the
 * cell the sender seeded. It cannot tell a wake from anything else and does
 * not try: it answers whatever trial is outstanding and the sender decides.
 */
static void receive(struct shared *shared, struct vas_destination *dest,
		    int trials)
{
	uint64_t answered = 0;

	(void)dest;

	while (answered < (uint64_t)trials) {
		uint64_t resumed;

		vas_wait();

		/*
		 * The clock before anything else. Deciding whether this resume
		 * is the one being waited for means loading a line the sender
		 * last wrote, which on another chip is a fabric round trip of
		 * the same order as the wake -- read the clock after that and
		 * the miss is inside every measurement.
		 */
		resumed = now_tb();

		/*
		 * The trial number, not a flag. A flag the sender clears after
		 * reading is still set when the receiver comes round again, so
		 * it answers twice, runs the count past what the sender is
		 * waiting for, and both sides stop. Answering trial n only
		 * while the sender is asking for exactly n cannot do that.
		 */
		if (__atomic_load_n(&shared->trial, __ATOMIC_ACQUIRE) !=
		    answered + 1)
			continue;

		store_add(&shared->delta, resumed);
		answered++;
		__atomic_store_n(&shared->answered, answered, __ATOMIC_RELEASE);
	}
}

int main(int argc, char **argv)
{
	struct shared *shared;
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	struct vas_destination *dest = NULL;
	uint64_t *samples;
	const char *label = "unlabelled";
	int sock[2];
	int trials = 2000;
	int a_cpu = -1, b_cpu = -1;
	long settle = 200;
	bool paste = true;
	double hz, ns;
	struct cycle_counter counter = CYCLE_COUNTER_INIT;
	struct environment env;
	unsigned long long spent = 0;
	uint64_t patience, began, ended;
	double ghz = 0.0;
	pid_t child;
	int i, rc, resent, taken = 0, lost = 0, delivered = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--trials") && i + 1 < argc)
			trials = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--send-cpu") && i + 1 < argc)
			a_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--wait-cpu") && i + 1 < argc)
			b_cpu = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--settle") && i + 1 < argc)
			settle = atol(argv[++i]);
		else if (!strcmp(argv[i], "--label") && i + 1 < argc)
			label = argv[++i];
		else if (!strcmp(argv[i], "--no-wake"))
			paste = false;
		else {
			fprintf(stderr,
				"usage: wake_tod [--trials N] [--send-cpu N] [--wait-cpu N]\n"
				"                [--settle SPINS] [--label S] [--no-wake]\n");
			return 2;
		}
	}
	if (trials < 1)
		return 2;

	/*
	 * Before anything: a clock that moves makes every figure below two
	 * facts at once, and nothing in the output would distinguish them.
	 */
	if (environment_require(&env, 0, ENVIRONMENT_STEADY_CLOCK))
		return 1;

	hz = tb_hz();
	ns = 1000000000.0 / hz;
	patience = (uint64_t)(PATIENCE_US * hz / 1000000.0);

	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		report_errno("map the shared page", -errno);
		return 1;
	}
	memset(shared, 0, sizeof(*shared));

	/*
	 * Whether the atomic works at all, asked before anything depends on
	 * the answer: an atomic that traps or is quietly ignored would leave
	 * every delta reading as the seed and look like a wake that never
	 * arrived.
	 */
	shared->delta = 40;
	store_add(&shared->delta, 2);
	if (shared->delta != 42) {
		fprintf(stderr,
			"store atomic did not take: cell reads %llu, expected 42\n",
			(unsigned long long)shared->delta);
		return 1;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sock)) {
		report_errno("socketpair", -errno);
		return 1;
	}

	samples = calloc(trials, sizeof(*samples));
	if (!samples)
		return 1;

	child = fork();
	if (child < 0) {
		report_errno("fork", -errno);
		return 1;
	}

	if (!child) {
		close(sock[0]);
		if (pin_to(b_cpu))
			_exit(1);
		rc = vas_destination_open(vas_instance_any(), &dest);
		if (rc) {
			report_errno("open a destination", rc);
			close(sock[1]);
			_exit(rc == -ENODEV ? 77 : 1);
		}
		if (send_fd(sock[1], vas_destination_fd(dest))) {
			close(sock[1]);
			_exit(1);
		}
		receive(shared, dest, trials);
		vas_destination_close(&dest);
		_exit(0);
	}

	close(sock[1]);
	if (pin_to(a_cpu))
		return 1;

	rc = recv_fd(sock[0]);
	if (rc < 0) {
		report_errno("receive the destination", rc);
		waitpid(child, NULL, 0);
		return 1;
	}

	vas_window_attr_init(&attr, VAS_COP_FTW);
	attr.wake_target = rc;
	if (vas_window_open(&attr, &window)) {
		report_errno("open a window onto the destination", -EINVAL);
		waitpid(child, NULL, 0);
		return 1;
	}
	close(rc);

	cycles_open(&counter);
	cycles_start(&counter);
	began = now_tb();

	for (i = 0; i < trials; i++) {
		volatile long spin;
		uint64_t sent;

		/* Long enough that the receiver is certainly suspended. */
		for (spin = 0; spin < settle; spin++)
			;

		sent = now_tb();
		/* Seeded negative, so the receiver's Store Add leaves a delta. */
		__atomic_store_n(&shared->delta, 0 - sent, __ATOMIC_RELAXED);
		__atomic_store_n(&shared->trial, (uint64_t)(i + 1),
				 __ATOMIC_RELEASE);

		if (paste) {
			rc = vas_wake(window);
			if (rc) {
				report_errno("wake the peer", rc);
				break;
			}
		}

		/*
		 * Bounded, and re-sent when the bound is reached. A notify is
		 * matched against the thread the switchboard finds running, so
		 * one that arrives before the receiver reaches its wait is
		 * neither delivered nor queued -- and an unbounded wait for a
		 * lost one is a spin that never ends. Re-sending is cheap and
		 * a duplicate is harmless, which is what makes this the shape
		 * a program should use rather than a concession to the harness.
		 */
		for (resent = 0; resent <= RESEND_MAX; resent++) {
			uint64_t deadline = now_tb() + patience;

			while (__atomic_load_n(&shared->answered,
					       __ATOMIC_ACQUIRE) !=
			       (uint64_t)(i + 1)) {
				if (now_tb() > deadline)
					break;
			}
			if (__atomic_load_n(&shared->answered,
					    __ATOMIC_ACQUIRE) ==
			    (uint64_t)(i + 1))
				break;
			if (paste)
				vas_wake(window);
		}

		if (__atomic_load_n(&shared->answered, __ATOMIC_ACQUIRE) !=
		    (uint64_t)(i + 1)) {
			fprintf(stderr,
				"trial %d unanswered after %d resends; giving up\n",
				i, RESEND_MAX);
			break;
		}

		if (resent)
			lost += resent;

		samples[taken++] = __atomic_load_n(&shared->delta,
						   __ATOMIC_ACQUIRE);
	}

	ended = now_tb();
	spent = cycles_stop(&counter);
	cycles_close(&counter);
	if (ended > began)
		ghz = (double)spent / ((double)(ended - began) * ns);

	vas_window_close(&window);
	waitpid(child, NULL, 0);

	if (!taken) {
		fprintf(stderr, "no trials completed\n");
		return 1;
	}

	/* And after: a machine that throttled mid-run has results to discard. */
	if (environment_verify(&env))
		return 1;

	for (i = 0; i < taken; i++)
		if ((double)samples[i] * ns < DELIVERED_NS)
			delivered++;

	qsort(samples, taken, sizeof(*samples), cmp_u64);

	printf("{\"label\":\"%s\",\"trials\":%d,\"send_cpu\":%d,\"wait_cpu\":%d,"
	       "\"settle\":%ld,\"paste\":%s,\"resends\":%d,\"delivered\":%d,"
	       "\"ghz\":%.3f,\"median_cycles\":%.1f,"
	       "\"median_ns\":%.1f,\"p10_ns\":%.1f,"
	       "\"p90_ns\":%.1f,\"min_ns\":%.1f,\"max_ns\":%.1f}\n",
	       label, taken, a_cpu, b_cpu, settle, paste ? "true" : "false", lost,
	       delivered, ghz, samples[taken / 2] * ns * ghz,
	       samples[taken / 2] * ns, samples[taken / 10] * ns,
	       samples[(taken * 9) / 10] * ns, samples[0] * ns,
	       samples[taken - 1] * ns);
	fflush(stdout);

	fprintf(stderr,
		"%s: %s, median %.0f ns (%.0f cycles at %.2f GHz), 90th %.0f ns,"
		" within %.0f ns in %.1f%% of %d trials, %d resends\n",
		label, paste ? "wake sent" : "no wake sent",
		samples[taken / 2] * ns, samples[taken / 2] * ns * ghz, ghz,
		samples[(taken * 9) / 10] * ns,
		DELIVERED_NS, 100.0 * delivered / taken, taken, lost);

	free(samples);

	return 0;
}
