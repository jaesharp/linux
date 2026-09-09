// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * One move whose input is gathered from several buffers and whose output is
 * scattered across several others, with a different number of pieces on each
 * side. The engine walks each list in order, so only the totals must agree.
 *
 * A request submitted through a userspace window carries effective
 * addresses, so a list is needed only where the data really is in pieces. A
 * single virtually contiguous buffer is one span however many pages it
 * covers, whatever the page size.
 *
 *     move_scatter [fragments] [kilobytes-per-fragment]
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vas/nx842.h>
#include <vas/vas.h>

#include "report.h"

enum {
	DEFAULT_FRAGMENTS = 8,
	DEFAULT_KILOBYTES = 8,
	/* The output is cut into this many times more pieces than the input. */
	TARGET_SPLIT = 2,
};

struct fragments {
	unsigned char **piece;
	unsigned int count;
	size_t each;
};

static void fragments_free(struct fragments *set)
{
	unsigned int i;

	if (!set->piece)
		return;

	for (i = 0; i < set->count; i++)
		free(set->piece[i]);
	free(set->piece);
	set->piece = NULL;
}

static int fragments_alloc(struct fragments *set, unsigned int count, size_t each)
{
	unsigned int i;

	set->piece = calloc(count, sizeof(*set->piece));
	if (!set->piece)
		return -ENOMEM;

	set->count = count;
	set->each = each;

	for (i = 0; i < count; i++) {
		set->piece[i] = alloc_engine_buffer(each);
		if (!set->piece[i]) {
			fragments_free(set);
			return -ENOMEM;
		}
	}

	return 0;
}

int main(int argc, char **argv)
{
	unsigned long kilobytes = DEFAULT_KILOBYTES;
	unsigned long count = DEFAULT_FRAGMENTS;
	struct nx_target_list *targets = NULL;
	struct nx_source_list *sources = NULL;
	struct fragments input = { 0 };
	struct fragments output = { 0 };
	struct nx_completion completion;
	struct vas_window_attr attr;
	struct nx_request *request = NULL;
	struct vas_window *window = NULL;
	unsigned char *flat = NULL;
	unsigned int i;
	size_t target_each;
	size_t total;
	size_t each;
	int rc;

	if (argc > 1)
		count = strtoul(argv[1], NULL, 0);
	if (argc > 2)
		kilobytes = strtoul(argv[2], NULL, 0);
	if (!count || !kilobytes || count * TARGET_SPLIT > NX_SCATTER_SPANS_MAX) {
		fprintf(stderr, "up to %d source fragments of a non-zero size\n",
			NX_SCATTER_SPANS_MAX / TARGET_SPLIT);
		return 1;
	}

	/*
	 * Every span but the last of a list must be a multiple of the
	 * engine's larger length rule. Whole kilobytes satisfy it on both
	 * sides, and alloc_engine_buffer() gives each span its alignment.
	 */
	each = kilobytes * 1024;
	target_each = each / TARGET_SPLIT;
	total = each * count;

	rc = fragments_alloc(&input, count, each);
	if (!rc)
		rc = fragments_alloc(&output, count * TARGET_SPLIT, target_each);
	if (rc) {
		report_errno("allocating fragments", rc);
		return 1;
	}

	flat = alloc_engine_buffer(total);
	if (!flat) {
		fprintf(stderr, "cannot allocate the reference buffer\n");
		return 1;
	}
	fill_pattern(flat, total);
	for (i = 0; i < input.count; i++)
		memcpy(input.piece[i], flat + (size_t)i * each, each);

	rc = nx_source_list_create(input.count, &sources);
	if (!rc)
		rc = nx_target_list_create(output.count, &targets);
	if (rc) {
		report_errno("creating the lists", rc);
		return 1;
	}

	for (i = 0; i < input.count && !rc; i++)
		rc = nx_source_list_add(sources, nx_source(input.piece[i], each));
	for (i = 0; i < output.count && !rc; i++)
		rc = nx_target_list_add(targets, nx_target(output.piece[i], target_each));
	if (rc) {
		report_errno("filling the lists", rc);
		return 1;
	}

	printf("gathering %u spans of %zu bytes into %u spans of %zu bytes\n",
	       nx_source_list_count(sources), each, nx_target_list_count(targets),
	       target_each);
	printf("  %llu bytes in, %llu bytes of room out\n",
	       (unsigned long long)nx_source_list_length(sources),
	       (unsigned long long)nx_target_list_length(targets));

	vas_window_attr_init(&attr, VAS_COP_842);
	rc = vas_window_open(&attr, &window);
	if (rc) {
		report_errno("vas_window_open", rc);
		return (rc == -ENODEV || rc == -ENOENT) ? 77 : 1;
	}

	rc = nx_request_create(&request);
	if (rc) {
		report_errno("nx_request_create", rc);
		goto out;
	}

	nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
	rc = nx_request_set_source_list(request, sources);
	if (!rc)
		rc = nx_request_set_target_list(request, targets);
	if (!rc)
		rc = nx_execute(window, request, NULL);
	if (rc) {
		report_errno("moving", rc);
		goto out;
	}

	rc = nx_request_completion(request, &completion);
	if (rc) {
		report_errno("nx_request_completion", rc);
		goto out;
	}
	if (!report_completion("gathered and scattered move", &completion)) {
		rc = -EIO;
		goto out;
	}

	if (completion.processed_bytes != total) {
		fprintf(stderr, "moved %u bytes, expected %zu\n",
			completion.processed_bytes, total);
		rc = -EIO;
		goto out;
	}

	/* The engine fills the target spans in order, so this reassembles. */
	for (i = 0; i < output.count; i++) {
		const unsigned char *expected = flat + (size_t)i * target_each;

		if (memcmp(output.piece[i], expected, target_each) == 0)
			continue;

		fprintf(stderr, "target span %u does not match the input\n", i);
		rc = -EIO;
		goto out;
	}

	printf("\n%zu bytes moved from %u spans into %u, byte for byte\n", total,
	       input.count, output.count);

out:
	nx_request_destroy(&request);
	vas_window_close(&window);
	nx_source_list_destroy(&sources);
	nx_target_list_destroy(&targets);
	fragments_free(&input);
	fragments_free(&output);
	free(flat);

	return rc ? 1 : 0;
}
