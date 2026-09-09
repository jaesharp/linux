// SPDX-License-Identifier: GPL-2.0-or-later

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vas/nx842.h>

#include "report.h"

bool report_completion(const char *what, const struct nx_completion *completion)
{
	if (completion->cc == NX_CC_SUCCESS)
		return true;

	printf("%s: %s (%d)\n", what, nx_cc_name(completion->cc), (int)completion->cc);
	printf("    %s\n", nx_cc_describe(completion->cc));

	if (completion->faulted) {
		printf("    at %p, on a %s\n", completion->fault_address,
		       completion->fault_on_write ? "store" : "load");
		printf("    %s\n", nx_fault_status_name(completion->fault_status));
	}

	if (completion->incomplete)
		printf("    the engine stopped partway; %u bytes are written\n",
		       completion->processed_bytes);
	if (completion->terminated)
		printf("    the request was terminated\n");

	return false;
}

void report_errno(const char *what, int rc)
{
	fprintf(stderr, "%s: %s\n", what, strerror(-rc));
}

void *alloc_engine_buffer(size_t len)
{
	void *buffer = NULL;

	if (posix_memalign(&buffer, NX_842_BUFFER_ALIGN, len) != 0)
		return NULL;

	memset(buffer, 0, len);

	return buffer;
}

void fill_pattern(void *buffer, size_t len)
{
	unsigned char *bytes = buffer;
	size_t i;

	/*
	 * Varied enough that a target left untouched, or filled from the
	 * wrong span, does not compare equal by accident.
	 */
	for (i = 0; i < len; i++)
		bytes[i] = (unsigned char)((i / 64) % 7);
}
