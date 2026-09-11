// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Waking a thread in another process, and what it costs.
 *
 * The switchboard can deliver a paste to a window instead of to an engine.
 * Nothing is carried: the receive window has FIFO writes disabled, so the 128
 * bytes a paste moves are discarded and the wake is the whole of the message.
 * Anything the woken thread is to read travels through ordinary memory, stored
 * before the paste.
 *
 * Two things about this are easy to get wrong, and both are demonstrated here
 * rather than described.
 *
 * The first is the handoff. A destination is named by its descriptor and by
 * nothing else, because being able to wake a thread is authority over that
 * thread. This example forks and passes the descriptor over a unix socket,
 * which is what unrelated processes would do; a parent that opened the
 * destination before forking could simply let the child inherit it.
 *
 * The second is the wait. A wake is a notify matched against the thread the
 * switchboard finds running. One that arrives before the thread waits, or
 * while it is off a core, is neither delivered nor queued -- so a flag in
 * memory is what makes the rendezvous correct, and the notify is only what
 * makes it fast. Waiting without the flag is a hang that looks like a slow
 * link, because the thread also resumes on every timer tick.
 *
 * That last point is why this example times the wake. A notify crosses the
 * fabric in well under a microsecond; a tick is a millisecond. If the figure
 * comes out tick-shaped, the notify was lost and the flag alone made progress,
 * and the run should be read as a failure however correct its output looks.
 *
 * Copyright 2026 J Lynn
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
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

/* Beyond this a notify cannot have been what resumed the thread. */
#define NOTIFY_LIMIT_US 500.0

/*
 * Shared between the two processes: the flag the waiter loops on, and the
 * moment the sender stored it, so the waiter can price its own wake.
 */
struct rendezvous {
	/*
	 * Released by the sender and acquired by vas_destination_wait(), so
	 * that seeing the flag means seeing sent_ns too.
	 *
	 * Released on the sender's failure paths as well, with woken left
	 * false. A waiter cannot tell "not yet" from "never" -- that is the
	 * nature of a wake that is not queued -- so a sender that gives up
	 * has to say so, or its peer waits for the rest of the run.
	 */
	int ready;
	bool woken;
	long long sent_ns;
};

static long long now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Hand @fd to the peer on @sock. */
static int send_fd(int sock, int fd)
{
	char control[CMSG_SPACE(sizeof(int))] = { 0 };
	char byte = 'd';
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof(control),
	};
	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

	return sendmsg(sock, &msg, 0) < 0 ? -errno : 0;
}

/* Receive the descriptor the peer sent, or a negative errno. */
static int recv_fd(int sock)
{
	char control[CMSG_SPACE(sizeof(int))] = { 0 };
	char byte = 0;
	struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
	struct msghdr msg = {
		.msg_iov = &iov,
		.msg_iovlen = 1,
		.msg_control = control,
		.msg_controllen = sizeof(control),
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

/*
 * The destination: open one for this thread, hand its descriptor to the peer,
 * and wait to be woken.
 */
static int destination(int sock, struct rendezvous *shared)
{
	struct vas_destination *dest = NULL;
	long long woke_ns;
	double us;
	int rc;

	rc = vas_destination_open(vas_instance_any(), &dest);
	if (rc) {
		report_errno("open a destination", rc);
		/*
		 * Closed rather than simply returned from: the sender is
		 * blocked reading a descriptor that is now never coming, and
		 * end of file is what tells it so. Without this the failure
		 * presents as a hang in the peer.
		 */
		close(sock);
		return 1;
	}

	rc = send_fd(sock, vas_destination_fd(dest));
	if (rc) {
		report_errno("hand the descriptor to the sender", rc);
		vas_destination_close(&dest);
		close(sock);
		return 1;
	}

	/*
	 * The loop, not the wait, is what decides. See the note at the top:
	 * the flag is what makes this correct and the notify is what makes it
	 * quick.
	 */
	vas_destination_wait(dest, &shared->ready);
	woke_ns = now_ns();

	vas_destination_close(&dest);

	us = (double)(woke_ns - shared->sent_ns) / 1000.0;

	if (!shared->woken) {
		printf("resumed in %.2f us with no wake sent\n", us);
		/*
		 * The control is only informative, never a failure: what it
		 * costs is what the notify has to beat.
		 */
		return 0;
	}

	printf("woken in %.2f us\n", us);
	if (us > NOTIFY_LIMIT_US) {
		printf("  too slow to have been a notify: the loop made this progress, not the wake\n");
		return 1;
	}

	return 0;
}

/*
 * Release the waiter without waking it. The peer is in a loop that only a
 * store can end, so a sender that cannot reach the hardware still has to
 * store something.
 */
static void give_up(struct rendezvous *shared)
{
	shared->sent_ns = now_ns();
	shared->woken = false;
	__atomic_store_n(&shared->ready, 1, __ATOMIC_RELEASE);
}

/* The sender: take the descriptor, open a window pointed at it, wake. */
static int sender(int sock, struct rendezvous *shared, bool paste)
{
	struct vas_window_attr attr;
	struct vas_window *window = NULL;
	int target;
	int rc;

	target = recv_fd(sock);
	if (target < 0) {
		give_up(shared);
		/*
		 * End of file here means the destination could not open one
		 * and said so by closing; it has already reported why.
		 */
		if (target != -EPROTO)
			report_errno("receive the destination's descriptor",
				     target);
		return 1;
	}

	vas_window_attr_init(&attr, VAS_COP_FTW);
	attr.wake_target = target;

	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("open a window onto the destination", rc);
		close(target);
		give_up(shared);
		return 1;
	}

	/*
	 * The window holds a reference of its own, so the sender's copy of
	 * the descriptor has done its work.
	 */
	close(target);

	/*
	 * Stamp, then release the flag, then wake. The release is what makes
	 * the stamp visible to a thread that has seen the flag; the wake only
	 * decides how soon it looks.
	 *
	 * Without the wake this is the control the figure needs. The waiter is
	 * in wait, which resumes on any exception, so the flag alone is found
	 * on the next timer tick. If the two cases time the same, the notify
	 * did nothing and the waiter was never suspended to begin with.
	 */
	shared->sent_ns = now_ns();
	shared->woken = paste;
	__atomic_store_n(&shared->ready, 1, __ATOMIC_RELEASE);

	rc = paste ? vas_wake(window) : 0;
	if (rc)
		report_errno("wake the destination", rc);

	vas_window_close(&window);

	return rc ? 1 : 0;
}

/*
 * With --no-wake the sender releases the flag and pastes nothing, which is
 * the control: it measures what the waiter costs when only the flag ends its
 * loop, and the wake has to be quicker than that to be doing anything.
 */
int main(int argc, char **argv)
{
	bool paste = !(argc > 1 && !strcmp(argv[1], "--no-wake"));
	struct rendezvous *shared;
	int sv[2];
	int waited;
	int rc;
	pid_t pid;

	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		report_errno("map the shared flag", -errno);
		return 1;
	}
	shared->ready = 0;
	shared->woken = false;
	shared->sent_ns = 0;

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
		_exit(sender(sv[1], shared, paste));
	}

	close(sv[1]);
	rc = destination(sv[0], shared);

	/*
	 * Kept apart from rc: waitpid writes the child's wait status, which
	 * is not an exit code and would bury what the destination reported.
	 */
	if (waitpid(pid, &waited, 0) > 0 &&
	    (!WIFEXITED(waited) || WEXITSTATUS(waited) != 0))
		rc = 1;

	return rc;
}
