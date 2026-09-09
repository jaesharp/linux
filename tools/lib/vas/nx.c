// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Building, submitting and reading back accelerator requests.
 */

#include <endian.h>
#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vas/nx.h>

#include "internal.h"

/*
 * A data-descriptor list. Both directions use the same shape; they are
 * separate types to their callers so that a list built to be read cannot be
 * handed over to be written.
 */
struct nx_ddl {
	struct nx_dde *entries;
	unsigned int capacity;
	unsigned int count;
	uint64_t length;
};

struct nx_source_list {
	struct nx_ddl ddl;
};

struct nx_target_list {
	struct nx_ddl ddl;
};

struct nx_request {
	struct nx_crb crb;
	bool submitted;
};

/* A length the descriptor's 32-bit byte count can carry. */
static bool length_fits(size_t len)
{
	return len <= UINT32_MAX;
}

static void dde_set_direct(struct nx_dde *dde, const void *addr, size_t len)
{
	dde->flags = 0;
	dde->count = 0;
	dde->index = 0;
	dde->length = htobe32((uint32_t)len);
	dde->address = htobe64((uint64_t)(uintptr_t)addr);
}

static void dde_set_indirect(struct nx_dde *dde, const struct nx_ddl *ddl)
{
	dde->flags = 0;
	dde->count = (uint8_t)ddl->count;
	dde->index = 0;
	dde->length = htobe32((uint32_t)ddl->length);
	dde->address = htobe64((uint64_t)(uintptr_t)ddl->entries);
}

static int ddl_create(struct nx_ddl *ddl, unsigned int capacity)
{
	size_t bytes;

	if (!capacity || capacity > NX_SCATTER_SPANS_MAX)
		return -EINVAL;

	bytes = (size_t)capacity * sizeof(struct nx_dde);
	ddl->entries = aligned_alloc(NX_DDE_ALIGN, bytes);
	if (!ddl->entries)
		return -ENOMEM;

	memset(ddl->entries, 0, bytes);
	ddl->capacity = capacity;
	ddl->count = 0;
	ddl->length = 0;

	return 0;
}

static void ddl_destroy(struct nx_ddl *ddl)
{
	free(ddl->entries);
	ddl->entries = NULL;
	ddl->capacity = 0;
	ddl->count = 0;
	ddl->length = 0;
}

static void ddl_reset(struct nx_ddl *ddl)
{
	if (ddl->entries)
		memset(ddl->entries, 0, (size_t)ddl->capacity * sizeof(struct nx_dde));
	ddl->count = 0;
	ddl->length = 0;
}

static int ddl_add(struct nx_ddl *ddl, const void *addr, size_t len)
{
	if (!addr || !len)
		return -EINVAL;
	if (!length_fits(len))
		return -EOVERFLOW;
	if (ddl->count >= ddl->capacity)
		return -ENOSPC;
	if (!length_fits(ddl->length + len))
		return -EOVERFLOW;

	dde_set_direct(&ddl->entries[ddl->count], addr, len);
	ddl->count++;
	ddl->length += len;

	return 0;
}

int nx_source_list_create(unsigned int capacity, struct nx_source_list **list)
{
	struct nx_source_list *created;
	int rc;

	if (!list)
		return -EINVAL;
	*list = NULL;

	created = calloc(1, sizeof(*created));
	if (!created)
		return -ENOMEM;

	rc = ddl_create(&created->ddl, capacity);
	if (rc) {
		free(created);
		return rc;
	}

	*list = created;

	return 0;
}

void nx_source_list_destroy(struct nx_source_list **list)
{
	if (!list || !*list)
		return;

	ddl_destroy(&(*list)->ddl);
	free(*list);
	*list = NULL;
}

void nx_source_list_reset(struct nx_source_list *list)
{
	if (list)
		ddl_reset(&list->ddl);
}

int nx_source_list_add(struct nx_source_list *list, struct nx_source span)
{
	if (!list)
		return -EINVAL;

	return ddl_add(&list->ddl, span.addr, span.len);
}

unsigned int nx_source_list_count(const struct nx_source_list *list)
{
	return list ? list->ddl.count : 0;
}

uint64_t nx_source_list_length(const struct nx_source_list *list)
{
	return list ? list->ddl.length : 0;
}

int nx_target_list_create(unsigned int capacity, struct nx_target_list **list)
{
	struct nx_target_list *created;
	int rc;

	if (!list)
		return -EINVAL;
	*list = NULL;

	created = calloc(1, sizeof(*created));
	if (!created)
		return -ENOMEM;

	rc = ddl_create(&created->ddl, capacity);
	if (rc) {
		free(created);
		return rc;
	}

	*list = created;

	return 0;
}

void nx_target_list_destroy(struct nx_target_list **list)
{
	if (!list || !*list)
		return;

	ddl_destroy(&(*list)->ddl);
	free(*list);
	*list = NULL;
}

void nx_target_list_reset(struct nx_target_list *list)
{
	if (list)
		ddl_reset(&list->ddl);
}

int nx_target_list_add(struct nx_target_list *list, struct nx_target span)
{
	if (!list)
		return -EINVAL;

	return ddl_add(&list->ddl, span.addr, span.len);
}

unsigned int nx_target_list_count(const struct nx_target_list *list)
{
	return list ? list->ddl.count : 0;
}

uint64_t nx_target_list_length(const struct nx_target_list *list)
{
	return list ? list->ddl.length : 0;
}

/*
 * Point the request at its own status block. The block is 16-byte aligned
 * inside the request, so the low bits of the word are free to carry control:
 * all of them stay clear, which asks for effective addressing and no
 * completion block.
 */
static void request_set_csb_address(struct nx_request *request)
{
	uint64_t address = (uint64_t)(uintptr_t)&request->crb.csb;
	uint64_t word;

	word = vas_field_put64(nx_crb_csb_address(), 0, address >> 4);
	request->crb.csb_addr = htobe64(word);
}

void nx_request_reset(struct nx_request *request)
{
	if (!request)
		return;

	memset(&request->crb, 0, sizeof(request->crb));
	request_set_csb_address(request);
	request->submitted = false;
}

int nx_request_create(struct nx_request **request)
{
	struct nx_request *created;
	void *memory = NULL;

	if (!request)
		return -EINVAL;
	*request = NULL;

	if (posix_memalign(&memory, NX_CRB_ALIGN, sizeof(*created)) != 0)
		return -ENOMEM;

	created = memory;
	memset(created, 0, sizeof(*created));
	nx_request_reset(created);

	*request = created;

	return 0;
}

void nx_request_destroy(struct nx_request **request)
{
	if (!request || !*request)
		return;

	free(*request);
	*request = NULL;
}

void nx_request_set_ccw(struct nx_request *request, struct nx_ccw ccw)
{
	if (request)
		request->crb.ccw = htobe32(ccw.word);
}

struct nx_crb *nx_request_crb(struct nx_request *request)
{
	return request ? &request->crb : NULL;
}

int nx_request_set_source(struct nx_request *request, struct nx_source source)
{
	if (!request || !source.addr || !source.len)
		return -EINVAL;
	if (!length_fits(source.len))
		return -EOVERFLOW;

	dde_set_direct(&request->crb.source, source.addr, source.len);

	return 0;
}

int nx_request_set_target(struct nx_request *request, struct nx_target target)
{
	if (!request || !target.addr || !target.len)
		return -EINVAL;
	if (!length_fits(target.len))
		return -EOVERFLOW;

	dde_set_direct(&request->crb.target, target.addr, target.len);

	return 0;
}

int nx_request_set_source_list(struct nx_request *request, const struct nx_source_list *list)
{
	if (!request || !list || !list->ddl.count)
		return -EINVAL;

	dde_set_indirect(&request->crb.source, &list->ddl);

	return 0;
}

int nx_request_set_target_list(struct nx_request *request, const struct nx_target_list *list)
{
	if (!request || !list || !list->ddl.count)
		return -EINVAL;

	dde_set_indirect(&request->crb.target, &list->ddl);

	return 0;
}

/*
 * Defaults chosen so that a caller who does not think about retrying still
 * gets the behaviour the hardware expects: a paste is refused whenever the
 * window's queue is full, which is ordinary, and a translation the nest MMU
 * has not got is resolved by touching the address and asking again.
 */
enum {
	NX_PASTE_ATTEMPTS_DEFAULT = 5000,
	NX_FAULT_RETRIES_DEFAULT = 64,
	NX_COMPLETION_TIMEOUT_MS_DEFAULT = 5000,
};

void nx_retry_policy_init(struct nx_retry_policy *policy)
{
	if (!policy)
		return;

	policy->paste_attempts = NX_PASTE_ATTEMPTS_DEFAULT;
	policy->fault_retries = NX_FAULT_RETRIES_DEFAULT;
	policy->completion_timeout_ms = NX_COMPLETION_TIMEOUT_MS_DEFAULT;
}

int nx_submit(struct vas_window *window, struct nx_request *request)
{
	void *paste_target;
	uint32_t cr0;

	if (!window || !request)
		return -EINVAL;

	paste_target = vas_window_paste_target(window);
	if (!paste_target)
		return -EINVAL;

	/*
	 * The request and anything it points at must be visible to the
	 * accelerator before the copy reads it, and the paste must not be
	 * reordered ahead of the copy.
	 */
	vas_barrier();
	vas_copy_block(&request->crb);
	cr0 = vas_paste_block(paste_target);
	vas_barrier();

	if (!vas_paste_accepted(cr0))
		return -EBUSY;

	request->submitted = true;

	return 0;
}

/* A monotonic reading in milliseconds, for deadlines. */
static uint64_t now_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;

	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/*
 * The accelerator stores the status block from another device, so the read
 * must come from memory every time rather than from a register the compiler
 * decided to keep.
 */
static bool completion_reported(const struct nx_request *request)
{
	const volatile uint8_t *flags = &request->crb.csb.flags;
	uint8_t value = *flags;

	return vas_field_get8(nx_csb_valid(), value) != 0;
}

int nx_wait(struct nx_request *request, unsigned int timeout_ms)
{
	uint64_t deadline;

	if (!request)
		return -EINVAL;
	if (!request->submitted)
		return -EINVAL;

	deadline = now_ms() + timeout_ms;

	for (;;) {
		if (completion_reported(request)) {
			/* Order the status block's other fields after its valid bit. */
			vas_barrier();
			return 0;
		}

		if (now_ms() >= deadline)
			return -ETIMEDOUT;

		/*
		 * Yield rather than spin: a request the engine is still
		 * working on will not finish sooner for being watched.
		 */
		sched_yield();
	}
}

int nx_request_completion(const struct nx_request *request, struct nx_completion *completion)
{
	const struct nx_fault_stamp *stamp;
	uint8_t ce;

	if (!request || !completion)
		return -EINVAL;

	if (!completion_reported(request))
		return -EAGAIN;

	memset(completion, 0, sizeof(*completion));
	completion->cc = (enum nx_cc)request->crb.csb.cc;
	completion->processed_bytes = be32toh(request->crb.csb.count);

	ce = request->crb.csb.ce;
	completion->incomplete = vas_field_get8(nx_csb_ce_incomplete(), ce) != 0;
	completion->terminated = vas_field_get8(nx_csb_ce_termination(), ce) != 0;

	if (completion->cc != NX_CC_FAULT_ADDRESS)
		return 0;

	stamp = &request->crb.stamp.nx;
	completion->faulted = true;
	completion->fault_address = (const void *)(uintptr_t)be64toh(stamp->fault_storage_addr);
	completion->fault_status = (enum nx_fault_status)stamp->fault_status;
	completion->fault_on_write =
		vas_field_get8(nx_fault_flag_write(), stamp->flags) != 0;

	return 0;
}

bool nx_cc_is_retryable(enum nx_cc cc)
{
	/*
	 * Only an address the kernel could not translate on the
	 * accelerator's behalf. Every other non-zero code describes the
	 * request itself, which a retry would repeat.
	 */
	return cc == NX_CC_FAULT_ADDRESS;
}

/*
 * Make the page holding @address present, and writable if the access that
 * faulted was a store. Reading suffices for a load, and a read-modify-write
 * of a read-only page would take a signal where the accelerator only wanted
 * the translation.
 */
static void touch_address(const void *address, bool for_write)
{
	volatile uint8_t *byte = (volatile uint8_t *)(uintptr_t)address;
	uint8_t value;

	if (!address)
		return;

	value = *byte;
	if (for_write)
		*byte = value;
}

int nx_execute(struct vas_window *window, struct nx_request *request,
	       const struct nx_retry_policy *policy)
{
	struct nx_retry_policy defaults;
	unsigned int fault_retries;
	unsigned int attempt;
	int rc;

	if (!window || !request)
		return -EINVAL;

	if (!policy) {
		nx_retry_policy_init(&defaults);
		policy = &defaults;
	}

	fault_retries = policy->fault_retries;

	for (;;) {
		struct nx_completion completion;

		for (attempt = 0; attempt < policy->paste_attempts; attempt++) {
			rc = nx_submit(window, request);
			if (rc != -EBUSY)
				break;
			sched_yield();
		}
		if (rc)
			return rc;

		rc = nx_wait(request, policy->completion_timeout_ms);
		if (rc)
			return rc;

		rc = nx_request_completion(request, &completion);
		if (rc)
			return rc;

		if (!nx_cc_is_retryable(completion.cc))
			return 0;

		if (!fault_retries)
			return 0;
		fault_retries--;

		touch_address(completion.fault_address, completion.fault_on_write);

		/*
		 * The status block must read invalid again, or the next wait
		 * would accept the block this attempt left behind.
		 */
		request->crb.csb.flags = 0;
		request->crb.csb.cc = 0;
		request->crb.csb.ce = 0;
		request->crb.csb.count = 0;
		memset(&request->crb.stamp, 0, sizeof(request->crb.stamp));
	}
}

int nx_touch_source(struct nx_source source)
{
	size_t page = vas_page_size();
	const uint8_t *base = source.addr;
	size_t offset;

	if (!base || !source.len)
		return -EINVAL;
	if (!page)
		return -ENOSYS;

	for (offset = 0; offset < source.len; offset += page)
		touch_address(base + offset, false);

	/* The last byte may sit in a page no stride landed on. */
	touch_address(base + source.len - 1, false);

	return 0;
}

int nx_touch_target(struct nx_target target)
{
	size_t page = vas_page_size();
	uint8_t *base = target.addr;
	size_t offset;

	if (!base || !target.len)
		return -EINVAL;
	if (!page)
		return -ENOSYS;

	for (offset = 0; offset < target.len; offset += page)
		touch_address(base + offset, true);

	touch_address(base + target.len - 1, true);

	return 0;
}
