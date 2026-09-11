// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What each kind of failure looks like, provoked on purpose.
 *
 * Every case here is a mistake a real caller can make, and the point is to
 * show what the accelerator says about it and how to tell the classes apart:
 * a request the engine refuses outright, a request that ran out of room, and
 * a request that could not be translated and is worth submitting again.
 *
 *     error_handling
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <vas/nx842.h>
#include <vas/vas.h>

#include "report.h"

enum {
	SOURCE_BYTES = 64 * 1024,
	/* Far too small to hold what the request moves. */
	CRAMPED_TARGET_BYTES = 128,
	/* A function code the 842 engine does not define. */
	UNDEFINED_FUNCTION = 7,
};

static int execute(struct vas_window *window, struct nx_ccw ccw,
		   struct nx_source source, struct nx_target target,
		   const struct nx_retry_policy *policy,
		   struct nx_completion *completion)
{
	struct nx_request *request;
	int rc;

	rc = nx_request_create(&request);
	if (rc)
		return rc;

	nx_request_set_ccw(request, ccw);
	rc = nx_request_set_source(request, source);
	if (!rc)
		rc = nx_request_set_target(request, target);
	if (!rc)
		rc = nx_execute(window, request, policy);
	if (!rc)
		rc = nx_request_completion(request, completion);

	nx_request_destroy(&request);

	return rc;
}

/* A request the engine will refuse: its target cannot hold what is moved. */
static void case_no_room(struct vas_window *window, const unsigned char *source)
{
	struct nx_completion completion;
	unsigned char *cramped;
	int rc;

	printf("\n-- a target with no room for the output\n");

	cramped = alloc_engine_buffer(CRAMPED_TARGET_BYTES);
	if (!cramped)
		return;

	rc = execute(window, nx_842_ccw(NX_842_MOVE),
		     nx_source(source, SOURCE_BYTES),
		     nx_target(cramped, CRAMPED_TARGET_BYTES), NULL, &completion);
	if (rc)
		report_errno("execute", rc);
	else
		report_completion("move 64 KiB into 128 bytes", &completion);

	free(cramped);
}

/* A request naming a function the engine does not have. */
static void case_unknown_function(struct vas_window *window,
				  const unsigned char *source, unsigned char *target)
{
	struct nx_completion completion;
	struct nx_ccw ccw = { .word = 0 };
	int rc;

	printf("\n-- a function code the engine does not define\n");

	/*
	 * Built through the field rather than through nx_842_ccw(), which
	 * only accepts codes the engine has.
	 */
	ccw.word = vas_field_put32(nx_ccw_function(), ccw.word, UNDEFINED_FUNCTION);

	rc = execute(window, ccw, nx_source(source, SOURCE_BYTES),
		     nx_target(target, SOURCE_BYTES), NULL, &completion);
	if (rc)
		report_errno("execute", rc);
	else
		report_completion("an undefined function", &completion);
}

/*
 * A source the accelerator cannot translate. The memory is mapped but has
 * never been touched, so it has no page table entry until something asks for
 * one. With retries withheld the fault is visible; with them allowed the
 * library resolves it and the request succeeds.
 */
static void case_untranslatable(struct vas_window *window, unsigned char *target)
{
	struct nx_retry_policy no_retries;
	struct nx_completion completion;
	unsigned char *untouched;
	int rc;

	printf("\n-- a source that has never been touched\n");

	untouched = mmap(NULL, SOURCE_BYTES, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (untouched == MAP_FAILED) {
		report_errno("mmap", -errno);
		return;
	}

	nx_retry_policy_init(&no_retries);
	no_retries.fault_retries = 0;

	rc = execute(window, nx_842_ccw(NX_842_MOVE),
		     nx_source(untouched, SOURCE_BYTES),
		     nx_target(target, SOURCE_BYTES), &no_retries, &completion);
	if (rc) {
		report_errno("execute", rc);
	} else {
		report_completion("with retries withheld", &completion);
		printf("    nx_cc_is_retryable() says %s\n",
		       nx_cc_is_retryable(completion.cc) ? "retry it" : "do not retry");
	}

	/* Now the same request with the library's default allowance. */
	rc = execute(window, nx_842_ccw(NX_842_MOVE),
		     nx_source(untouched, SOURCE_BYTES),
		     nx_target(target, SOURCE_BYTES), NULL, &completion);
	if (rc)
		report_errno("execute", rc);
	else if (report_completion("with retries allowed", &completion))
		printf("    resolved by touching the address and asking again\n");

	munmap(untouched, SOURCE_BYTES);
}

/* A buffer the engine's alignment rule refuses, caught before submitting. */
static void case_misaligned(const unsigned char *source)
{
	struct nx_source misaligned;

	printf("\n-- a source the engine's alignment rule refuses\n");

	misaligned = nx_source(source + 1, SOURCE_BYTES - 1);

	printf("    %p is %s the engine's %d-byte alignment\n", misaligned.addr,
	       nx_842_source_is_valid(misaligned) ? "within" : "outside",
	       NX_842_BUFFER_ALIGN);
	printf("    checking before submitting turns a completion code into a "
	       "caller's own error\n");
}

/* A list longer than it has room for, caught by the list itself. */
static void case_list_full(const unsigned char *source)
{
	struct nx_source_list *list;
	int rc;

	printf("\n-- more spans than the list was created to hold\n");

	rc = nx_source_list_create(2, &list);
	if (rc) {
		report_errno("nx_source_list_create", rc);
		return;
	}

	rc = nx_source_list_add(list, nx_source(source, 1024));
	if (!rc)
		rc = nx_source_list_add(list, nx_source(source + 1024, 1024));
	if (rc)
		report_errno("filling a list of two", rc);

	rc = nx_source_list_add(list, nx_source(source + 2048, 1024));
	printf("    a third span into a list of two: %s\n", strerror(-rc));

	nx_source_list_destroy(&list);
}

int main(void)
{
	struct vas_window_attr attr;
	struct vas_window *window;
	unsigned char *source;
	unsigned char *target;
	int rc;

	source = alloc_engine_buffer(SOURCE_BYTES);
	target = alloc_engine_buffer(SOURCE_BYTES);
	if (!source || !target) {
		fprintf(stderr, "cannot allocate the buffers\n");
		return 1;
	}
	fill_pattern(source, SOURCE_BYTES);

	/* Two cases need no engine at all. */
	case_misaligned(source);
	case_list_full(source);

	vas_window_attr_init(&attr, VAS_COP_842);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("\nvas_window_open", rc);
		fprintf(stderr, "the cases that need an engine were not run\n");
		return (rc == -ENODEV) ? 77 : 1;
	}

	case_no_room(window, source);
	case_unknown_function(window, source, target);
	case_untranslatable(window, target);

	vas_window_close(&window);
	free(source);
	free(target);

	return 0;
}
