// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Whether a paste to a send window wakes the thread that opened the receive
 * window it points at.
 *
 * Power ISA 3.0B book II, the wait instruction: a platform notify is one of
 * the events that resumes a waiting thread, and the note there says the
 * values in LPIDR, PIDR and TIDR identify a thread so that an accelerator
 * can resume the one that submitted to it. This exercises that path with a
 * window in place of an accelerator.
 *
 * The measurement rather than the outcome is the point. wait also resumes on
 * any interrupt, so a thread that loops on a condition will eventually make
 * progress whether or not a notify ever arrives -- the timer tick alone will
 * see to it. What distinguishes the two is how long it takes: a notify
 * arrives in the time a fabric operation takes, and a tick arrives in a
 * millisecond or so. So the test times it and fails on a figure that could
 * only be the tick.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <asm/vas-api.h>

#include "utils.h"

#define DEVICE "/dev/vas/ibm-power9-nv-vas-ftw"

/*
 * A notify crosses the fabric; a timer tick is a millisecond. Anything past
 * this could not have been the notify, whatever else it was.
 */
#define WAKE_LIMIT_US 500

/* The paste address carries the report-enable bit, as for any window. */
#define PASTE_REPORT_ENABLE 0x400

static atomic_int armed;
static atomic_int poked;
/*
 * The waiter's descriptor is the destination. Two threads of one process
 * share it by having it in scope; unrelated processes would pass it over a
 * unix socket, which is the whole of the difference.
 */
static int rx_fd = -1;
static double wake_us;

static double now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* wait 0: resume on an exception, an event-based branch, or a platform notify. */
static inline void wait_for_notify(void)
{
	asm volatile(".long 0x7c00003c" ::: "memory");
}

static void *waiter(void *unused)
{
	struct vas_rx_win_open_attr attr;
	int fd;

	(void)unused;

	fd = open(DEVICE, O_RDWR);
	if (fd < 0) {
		perror("waiter: " DEVICE);
		atomic_store(&armed, -1);
		return NULL;
	}

	memset(&attr, 0, sizeof(attr));
	attr.version = VAS_TX_WIN_OPEN_V2;
	attr.vas_id = -1;

	if (ioctl(fd, VAS_RX_WIN_OPEN, &attr) < 0) {
		perror("waiter: VAS_RX_WIN_OPEN");
		atomic_store(&armed, -1);
		close(fd);
		return NULL;
	}

	rx_fd = fd;
	atomic_store(&armed, 1);

	/*
	 * The loop is what the architecture asks for: wait may resume for
	 * reasons of its own, so the condition is what decides, and the time
	 * is what says which reason it was.
	 */
	while (!atomic_load(&poked))
		wait_for_notify();

	wake_us = now_us();

	close(fd);

	return NULL;
}

static int poke(void)
{
	struct vas_tx_win_open_attr attr;
	void *map, *paste;
	uint32_t cr;
	int fd;

	fd = open(DEVICE, O_RDWR);
	if (fd < 0) {
		perror("poker: " DEVICE);
		return -1;
	}

	memset(&attr, 0, sizeof(attr));
	attr.version = VAS_TX_WIN_OPEN_V2;
	attr.vas_id = -1;
	attr.flags = VAS_TX_WIN_FLAG_TARGET;
	attr.target_fd = rx_fd;

	if (ioctl(fd, VAS_TX_WIN_OPEN, &attr) < 0) {
		perror("poker: VAS_TX_WIN_OPEN");
		close(fd);
		return -1;
	}

	map = mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, 0ULL);
	if (map == MAP_FAILED) {
		perror("poker: mmap");
		close(fd);
		return -1;
	}
	paste = (char *)map + PASTE_REPORT_ENABLE;

	/*
	 * A wakeup carries no request, so what is pasted does not matter;
	 * that something was pasted is the whole of the message. The block
	 * still has to be one the copy instruction will take.
	 */
	static char block[128] __attribute__((aligned(128)));

	atomic_store(&poked, 1);

	asm volatile("sync" ::: "memory");
	asm volatile(".long 0x7c20060c" ::"b"(0), "b"(block) : "memory");
	asm volatile(".long 0x7c20070d ; mfocrf %0, 0x80"
		     : "=r"(cr) : "b"(0), "b"(paste) : "memory", "cr0");
	asm volatile("sync" ::: "memory");

	munmap(map, sysconf(_SC_PAGESIZE));
	close(fd);

	return ((cr >> 28) & 0x2) ? 0 : -1;
}

static int ftw_wake(void)
{
	pthread_t thread;
	double sent;
	int state;

	SKIP_IF(access(DEVICE, R_OK | W_OK) != 0);

	FAIL_IF(pthread_create(&thread, NULL, waiter, NULL));

	while (!(state = atomic_load(&armed)))
		sched_yield();
	if (state < 0) {
		pthread_join(thread, NULL);
		return 1;
	}

	/* Let the waiter reach its wait rather than racing it there. */
	usleep(10000);

	sent = now_us();
	FAIL_IF(poke());

	FAIL_IF(pthread_join(thread, NULL));

	printf("woken %.1f us after the paste\n", wake_us - sent);

	/*
	 * Slower than this and something other than the notify resumed the
	 * thread, which is the outcome worth failing on: it means the paste
	 * was accepted and delivered nothing.
	 */
	FAIL_IF(wake_us - sent > WAKE_LIMIT_US);

	return 0;
}

int main(void)
{
	return test_harness(ftw_wake, "ftw_wake");
}
