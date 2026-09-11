// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What the copy-paste facility does with each sequence it can be given.
 *
 * Power ISA 3.0B book II 4.4 lays down the rules: a copy followed by a
 * paste is a transfer; two copies with no paste between them, or a paste
 * with no copy before it, is a malformed sequence that the paste reports as
 * a failure; a paste of either kind leaves the facility clean; and cpabort
 * clears whatever is outstanding so that the next copy starts from nothing.
 * Each rule is exercised here through the library's own calls, on a window
 * this thread opens onto a queued destination of its own, so the queue says
 * what arrived and the return codes say what the hardware reported. The two
 * are held to each other: a paste that reports success must deliver exactly
 * the block that was copied, and one that reports failure must deliver
 * nothing at all.
 *
 * The interrupted sequence is what the abort exists for. A signal handler
 * that uses the facility has landed in the middle of someone else's
 * sequence, and the same section makes it responsible for clearing that
 * with cpabort before it uses the facility itself: its own copy would
 * otherwise be the second copy of a malformed sequence, and its own paste
 * refused. The things such a handler can do -- send straight off, abandon
 * and then send, stage and abandon, stage and simply return -- are each
 * tried between a stage and its commit, and what the handler's paste and the
 * interrupted commit then report and deliver is what the rule is about.
 *
 * Then the costs, since the split exists to move one: what a commit costs
 * with the block already loaded, against a send that loads it too, for a
 * block this thread just wrote and for one pushed out of every cache.
 *
 * Needs the switchboard. Exits 77 where there is none, as the examples do.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vas/vas.h>

/*
 * A block whose first word says which block it is and whose others are a
 * pattern that word seeds, so a delivery is checked whole and a block of
 * one kind cannot pass for another.
 */
struct block {
	uint64_t tag;
	uint64_t pattern[15];
} __attribute__((aligned(128)));

enum tag {
	TAG_NONE = 0,
	TAG_A = 0xa,
	TAG_B = 0xb,
	TAG_HANDLER = 0x4,
	TAG_TORN = 0xf,		/* something arrived that no block was */
};

static void block_fill(struct block *b, enum tag tag)
{
	int i;

	for (i = 0; i < 15; i++)
		b->pattern[i] = (uint64_t)tag * 1000003UL + (uint64_t)i;
	b->tag = tag;
}

static bool block_intact(const struct block *b)
{
	int i;

	for (i = 0; i < 15; i++)
		if (b->pattern[i] != b->tag * 1000003UL + (uint64_t)i)
			return false;

	return true;
}

static const char *tag_name(enum tag tag)
{
	switch (tag) {
	case TAG_NONE:
		return "nothing";
	case TAG_A:
		return "A";
	case TAG_B:
		return "B";
	case TAG_HANDLER:
		return "the handler's";
	case TAG_TORN:
		return "a torn entry";
	}

	return "?";
}

static struct vas_destination *dest;
static struct vas_window *window;
static struct block block_a, block_b, block_handler;

/*
 * How long an entry is given to appear before nothing is believed to have
 * been sent. Arrivals were measured in well under a microsecond; this is a
 * thousand of those.
 */
#define PATIENCE_NS 1000000

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* What arrives within the patience, released once read. */
static enum tag arrival(void)
{
	uint64_t until = now_ns() + PATIENCE_NS;

	do {
		const struct block *msg = vas_destination_next(dest);

		if (msg) {
			enum tag tag = block_intact(msg) ? (enum tag)msg->tag :
							   TAG_TORN;

			vas_destination_release(dest, msg);
			return tag;
		}
	} while (now_ns() < until);

	return TAG_NONE;
}

/* Leave nothing in the queue and nothing in the facility. */
static void clean(void)
{
	const void *msg;

	vas_send_abandon();
	while ((msg = vas_destination_next(dest)))
		vas_destination_release(dest, msg);
}

/* What one sequence came to: what the last paste said, and what arrived. */
struct outcome {
	int rc;
	enum tag arrived;
};

static const char *verdict_of(int rc)
{
	switch (rc) {
	case 0:
		return "accepted";
	case -EAGAIN:
		return "refused";
	default:
		return strerror(-rc);
	}
}

static unsigned int checks, failures;

static void expect(const char *sequence, struct outcome got, int want_rc,
		   enum tag want_arrived)
{
	bool ok = got.rc == want_rc && got.arrived == want_arrived;

	checks++;
	if (!ok)
		failures++;

	printf("  %-46s %-9s %-14s %s\n", sequence, verdict_of(got.rc),
	       tag_name(got.arrived), ok ? "as the ISA says" : "NOT AS THE ISA SAYS");
	if (!ok)
		printf("  %-46s %-9s %-14s expected\n", "", verdict_of(want_rc),
		       tag_name(want_arrived));
}

/*
 * What a handler does with the facility while a sequence is outstanding.
 * The handler runs on this thread, between the stage and the commit.
 */
enum handler_action {
	HANDLER_SENDS,			/* a copy and paste of its own, straight off */
	HANDLER_ABANDONS_THEN_SENDS,	/* cpabort first, then its own pair */
	HANDLER_STAGES_AND_ABANDONS,	/* a copy, then cpabort */
	HANDLER_STAGES_ONLY,		/* a copy, and returns */
};

static volatile enum handler_action handler_does;
static volatile int handler_rc;

static void on_signal(int sig)
{
	(void)sig;

	switch (handler_does) {
	case HANDLER_SENDS:
		handler_rc = vas_send(window, &block_handler);
		break;
	case HANDLER_ABANDONS_THEN_SENDS:
		vas_send_abandon();
		handler_rc = vas_send(window, &block_handler);
		break;
	case HANDLER_STAGES_AND_ABANDONS:
		handler_rc = vas_send_stage(&block_handler);
		vas_send_abandon();
		break;
	case HANDLER_STAGES_ONLY:
		handler_rc = vas_send_stage(&block_handler);
		break;
	}
}

static struct outcome interrupted(enum handler_action action)
{
	struct outcome o;

	clean();
	handler_does = action;
	handler_rc = 1;
	o.rc = vas_send_stage(&block_a);
	if (o.rc)
		return o;
	raise(SIGUSR1);
	o.rc = vas_send_commit(window);
	o.arrived = arrival();

	return o;
}

static void test_sequences(void)
{
	struct outcome o;

	printf("  %-46s %-9s %-14s\n", "sequence", "paste", "arrived");

	clean();
	o.rc = vas_send(window, &block_a);
	o.arrived = arrival();
	expect("copy A, paste", o, 0, TAG_A);

	clean();
	o.rc = vas_send_commit(window);
	o.arrived = arrival();
	expect("paste alone", o, -EAGAIN, TAG_NONE);

	clean();
	vas_send_stage(&block_a);
	vas_send_abandon();
	o.rc = vas_send_commit(window);
	o.arrived = arrival();
	expect("copy A, cpabort, paste", o, -EAGAIN, TAG_NONE);

	clean();
	vas_send_stage(&block_a);
	vas_send_stage(&block_b);
	o.rc = vas_send_commit(window);
	o.arrived = arrival();
	expect("copy A, copy B, paste", o, -EAGAIN, TAG_NONE);

	clean();
	o.rc = vas_send(window, &block_a);
	o.arrived = arrival();
	expect("copy A, paste", o, 0, TAG_A);
	o.rc = vas_send_commit(window);
	o.arrived = arrival();
	expect("  then paste again", o, -EAGAIN, TAG_NONE);

	clean();
	vas_send_stage(&block_a);
	vas_send_abandon();
	vas_send_stage(&block_b);
	o.rc = vas_send_commit(window);
	o.arrived = arrival();
	expect("copy A, cpabort, copy B, paste", o, 0, TAG_B);

	clean();
	vas_send_abandon();
	o.rc = vas_send(window, &block_a);
	o.arrived = arrival();
	expect("cpabort with nothing staged, copy A, paste", o, 0, TAG_A);

	clean();
	vas_send_stage(&block_a);
	o.rc = vas_send_commit(window);
	o.arrived = arrival();
	expect("stage A, commit", o, 0, TAG_A);

	printf("\n  a signal handler runs between stage A and its commit, and:\n");

	/*
	 * The handler's copy lands on top of the staged one, so its own
	 * paste is the paste of a malformed sequence and is refused; that
	 * paste resets the facility, and the commit it interrupted then
	 * finds nothing staged. Nothing goes anywhere.
	 */
	o = interrupted(HANDLER_SENDS);
	expect("  sends a block of its own, straight off", o, -EAGAIN, TAG_NONE);
	checks++;
	if (handler_rc != -EAGAIN) {
		failures++;
		printf("  the handler's own send was %s, not refused\n",
		       verdict_of(handler_rc));
	}

	/*
	 * Which is what the abort is for: the handler clears what it
	 * interrupted, its own pair is then a whole sequence and its block
	 * goes, and the interrupted commit finds nothing staged.
	 */
	o = interrupted(HANDLER_ABANDONS_THEN_SENDS);
	expect("  abandons what it interrupted, then sends", o, -EAGAIN,
	       TAG_HANDLER);
	checks++;
	if (handler_rc) {
		failures++;
		printf("  the handler's own send was refused: %s\n",
		       strerror(-handler_rc));
	}
	checks++;
	if (arrival() != TAG_NONE) {
		failures++;
		printf("  and something else arrived after it\n");
	}

	o = interrupted(HANDLER_STAGES_AND_ABANDONS);
	expect("  stages a block and abandons it", o, -EAGAIN, TAG_NONE);

	/*
	 * A copy on top of a copy: the sequence is malformed and the paste
	 * must say so. A hardware that delivered the handler's block here
	 * would be delivering it to a window the handler never named.
	 */
	o = interrupted(HANDLER_STAGES_ONLY);
	expect("  stages a block and returns", o, -EAGAIN, TAG_NONE);
	clean();
}

/* Timebase ticks to nanoseconds, from the device tree. */
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

static inline uint64_t now_tb(void)
{
	uint64_t tb;

	asm volatile("mfspr %0, 268" : "=r"(tb));

	return tb;
}

static int by_value(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

#define SAMPLES 1000

/* Push the block out of every cache, so the next copy reads memory. */
static void evict(const struct block *b)
{
	asm volatile("sync" ::: "memory");
	asm volatile("dcbf 0,%0" :: "r"(b) : "memory");
	asm volatile("sync" ::: "memory");
}

enum cost {
	COST_SEND_HOT,		/* copy and paste, block just written */
	COST_COMMIT_HOT,	/* the paste alone, block staged before */
	COST_SEND_COLD,		/* copy and paste, block evicted first */
	COST_COMMIT_COLD,	/* the paste alone, evicted then staged */
	COST_ABANDON,		/* cpabort of a staged block */
	COST_COUNT,
};

static const char *cost_name(enum cost c)
{
	switch (c) {
	case COST_SEND_HOT:
		return "send, block just written";
	case COST_COMMIT_HOT:
		return "commit, block staged already";
	case COST_SEND_COLD:
		return "send, block pushed out of every cache";
	case COST_COMMIT_COLD:
		return "commit, block evicted then staged";
	case COST_ABANDON:
		return "abandon a staged block";
	case COST_COUNT:
		break;
	}

	return "?";
}

static uint64_t one_cost(enum cost c, uint64_t i)
{
	uint64_t t0, t1;

	block_a.pattern[0] = i;
	switch (c) {
	case COST_SEND_HOT:
		t0 = now_tb();
		vas_send(window, &block_a);
		t1 = now_tb();
		break;
	case COST_COMMIT_HOT:
		vas_send_stage(&block_a);
		t0 = now_tb();
		vas_send_commit(window);
		t1 = now_tb();
		break;
	case COST_SEND_COLD:
		evict(&block_a);
		t0 = now_tb();
		vas_send(window, &block_a);
		t1 = now_tb();
		break;
	case COST_COMMIT_COLD:
		evict(&block_a);
		vas_send_stage(&block_a);
		t0 = now_tb();
		vas_send_commit(window);
		t1 = now_tb();
		break;
	case COST_ABANDON:
		vas_send_stage(&block_a);
		t0 = now_tb();
		vas_send_abandon();
		t1 = now_tb();
		break;
	default:
		return 0;
	}

	return t1 - t0;
}

static void measure_costs(void)
{
	static uint64_t samples[SAMPLES];
	double ns = tb_ns();
	enum cost c;

	printf("\n  %-42s %10s %10s\n", "cost, over 1000 samples", "median ns",
	       "90th ns");
	for (c = 0; c < COST_COUNT; c++) {
		uint64_t i;

		clean();
		for (i = 0; i < SAMPLES; i++) {
			samples[i] = one_cost(c, i);
			clean();
		}
		qsort(samples, SAMPLES, sizeof(samples[0]), by_value);
		printf("  %-42s %10.0f %10.0f\n", cost_name(c),
		       (double)samples[SAMPLES / 2] * ns,
		       (double)samples[SAMPLES * 9 / 10] * ns);
	}
}

int main(void)
{
	struct vas_window_attr attr;
	struct sigaction sa;
	int rc;

	block_fill(&block_a, TAG_A);
	block_fill(&block_b, TAG_B);
	block_fill(&block_handler, TAG_HANDLER);

	rc = vas_destination_open_queued(vas_instance_any(), 0, &dest);
	if (rc) {
		fprintf(stderr, "open a queued destination: %s\n", strerror(-rc));
		/* No switchboard, or a kernel without the node for it. */
		return rc == -ENODEV || rc == -ENOENT ? 77 : 1;
	}

	vas_window_attr_init(&attr, VAS_COP_FTW);
	attr.wake_target = vas_destination_fd(dest);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		fprintf(stderr, "open a window onto it: %s\n", strerror(-rc));
		vas_destination_close(&dest);
		return rc == -ENODEV || rc == -ENOENT ? 77 : 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGUSR1, &sa, NULL);

	printf("copy-paste sequences through a window onto this thread's own queue,"
	       " library built %s\n\n", vas_isa_name(vas_isa_built_with()));

	test_sequences();
	measure_costs();

	vas_window_close(&window);
	vas_destination_close(&dest);

	printf("\n%u checks: %u failed\n", checks, failures);

	return failures ? 1 : 0;
}
