// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Move bytes through the accelerator: open a window, submit one request,
 * read the outcome, close. The shortest complete use of the library.
 *
 * The 842 engine's move function is a copy with no compression, so what
 * arrives is exactly what was sent and the check is a comparison. That keeps
 * the example about the interface rather than about a compression format.
 *
 *     move [kilobytes]
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vas/nx842.h>
#include <vas/vas.h>

#include "report.h"

enum { DEFAULT_KILOBYTES = 64 };

int main(int argc, char **argv)
{
	unsigned long kilobytes = DEFAULT_KILOBYTES;
	struct nx_completion completion;
	struct vas_window_attr attr;
	struct nx_request *request;
	struct vas_window *window;
	unsigned char *source;
	unsigned char *target;
	size_t len;
	int rc;

	if (argc > 1)
		kilobytes = strtoul(argv[1], NULL, 0);
	if (!kilobytes) {
		fprintf(stderr, "a size of zero has nothing to move\n");
		return 1;
	}
	len = kilobytes * 1024;

	source = alloc_engine_buffer(len);
	target = alloc_engine_buffer(len);
	if (!source || !target) {
		fprintf(stderr, "cannot allocate two buffers of %zu bytes\n", len);
		return 1;
	}
	fill_pattern(source, len);

	/* The engine's rules for the memory a request names, checked up front. */
	if (!nx_842_source_is_valid(nx_source(source, len)) ||
	    !nx_842_target_is_valid(nx_target(target, len))) {
		fprintf(stderr, "the buffers do not meet the engine's alignment "
				"or length rules\n");
		return 1;
	}

	vas_window_attr_init(&attr, VAS_COP_842);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("vas_window_open", rc);
		/* Absent engine is not a failure of this example. */
		return (rc == -ENODEV || rc == -ENOENT) ? 77 : 1;
	}

	rc = nx_request_create(&request);
	if (rc) {
		report_errno("nx_request_create", rc);
		vas_window_close(&window);
		return 1;
	}

	nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
	rc = nx_request_set_source(request, nx_source(source, len));
	if (!rc)
		rc = nx_request_set_target(request, nx_target(target, len));
	if (rc) {
		report_errno("describing the buffers", rc);
		goto out;
	}

	/*
	 * Nothing here pre-touches its buffers: nx_execute() resolves an
	 * address the accelerator cannot translate by making it present and
	 * submitting again. A caller that cares about the latency of the
	 * first request would call nx_touch_source() and nx_touch_target()
	 * instead and leave no faults to resolve.
	 */
	rc = nx_execute(window, request, NULL);
	if (rc) {
		report_errno("nx_execute", rc);
		goto out;
	}

	rc = nx_request_completion(request, &completion);
	if (rc) {
		report_errno("nx_request_completion", rc);
		goto out;
	}

	if (!report_completion("move", &completion)) {
		rc = -EIO;
		goto out;
	}

	if (completion.processed_bytes != len) {
		fprintf(stderr, "moved %u bytes, expected %zu\n",
			completion.processed_bytes, len);
		rc = -EIO;
		goto out;
	}
	if (memcmp(source, target, len) != 0) {
		fprintf(stderr, "the target does not match the source\n");
		rc = -EIO;
		goto out;
	}

	printf("\n%zu bytes moved through the accelerator, byte for byte\n", len);

out:
	nx_request_destroy(&request);
	vas_window_close(&window);
	free(source);
	free(target);

	return rc ? 1 : 0;
}
