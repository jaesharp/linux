/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Printing what happened, in one shape across the examples.
 */

#ifndef _VAS_EXAMPLE_REPORT_H
#define _VAS_EXAMPLE_REPORT_H

#include <stdbool.h>
#include <stddef.h>

#include <vas/nx.h>

/*
 * Say why a request did not succeed, and nothing at all when it did: the
 * caller already knows what it asked for. Returns whether it succeeded.
 */
bool report_completion(const char *what, const struct nx_completion *completion);

/* Print a call that failed before the accelerator saw anything. */
void report_errno(const char *what, int rc);

/* Allocate a buffer the 842 engine will accept, or return NULL. */
void *alloc_engine_buffer(size_t len);

/* Fill with a pattern a comparison can check the move against. */
void fill_pattern(void *buffer, size_t len);

#endif /* _VAS_EXAMPLE_REPORT_H */
