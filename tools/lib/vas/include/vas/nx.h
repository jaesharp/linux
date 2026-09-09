/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Requests to the Nest Accelerator, and what comes back.
 *
 * A request is a coprocessor request block: an operation, a description of
 * where to read from and where to write to, and the address of the status
 * block the engine reports through. It is submitted by copying it to the
 * processor's copy buffer and pasting that buffer to a window's paste
 * address, and it completes asynchronously when the engine stores a valid
 * status block.
 *
 * Every field the hardware reads is big-endian. The structures below are
 * declared in that form and the library converts at the boundary, so this
 * works unchanged on a big-endian or a little-endian host.
 *
 * Addresses in a request submitted through a userspace window are effective
 * addresses, translated by the nest MMU in the address space that opened the
 * window. A buffer therefore needs no physical contiguity and no splitting at
 * page boundaries, and nothing here depends on the page size.
 */

#ifndef _VAS_NX_H
#define _VAS_NX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vas/field.h>
#include <vas/vas.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware structures. Sizes and alignments are from the NX workbook by way
 * of arch/powerpc/include/asm/icswx.h, which these mirror field for field.
 */

enum {
	NX_CSB_SIZE = 0x10,
	NX_CSB_ALIGN = NX_CSB_SIZE,
	NX_CCB_SIZE = 0x10,
	NX_CCB_ALIGN = NX_CCB_SIZE,
	NX_DDE_SIZE = 0x10,
	NX_DDE_ALIGN = NX_DDE_SIZE,
	NX_FAULT_STAMP_ALIGN = 0x10,
	NX_CRB_SIZE = 0x80,
	/*
	 * The copy instruction takes a 128-byte aligned block, which is the
	 * alignment the kernel's own definition carries. The in-tree header
	 * tools/testing/selftests/powerpc/nx-gzip/include/crb.h raises it to
	 * 256 for an erratum it does not name; this takes the wider of the
	 * two, which satisfies both.
	 */
	NX_CRB_ALIGN = 0x100,
};

/* Coprocessor-status block: what the engine reports when a request ends. */
struct nx_csb {
	uint8_t flags;
	uint8_t cs;
	uint8_t cc;
	uint8_t ce;
	uint32_t count;		/* big-endian */
	uint64_t address;	/* big-endian */
} __attribute__((aligned(NX_CSB_ALIGN)));

/* Coprocessor-completion block. */
struct nx_ccb {
	uint64_t value;		/* big-endian */
	uint64_t address;	/* big-endian */
} __attribute__((aligned(NX_CCB_ALIGN)));

/*
 * Data-descriptor entry. With @count zero it describes one span of memory
 * directly; with @count non-zero it points at a list of that many direct
 * entries and @length is the total of their lengths.
 */
struct nx_dde {
	uint16_t flags;		/* big-endian */
	uint8_t count;
	uint8_t index;
	uint32_t length;	/* big-endian */
	uint64_t address;	/* big-endian */
} __attribute__((aligned(NX_DDE_ALIGN)));

/*
 * The nest MMU's reason for refusing a translation, as the accelerator
 * stamps it into a faulted request. Mirrors enum nx_fault_status in
 * arch/powerpc/include/asm/icswx.h.
 */
enum nx_fault_status {
	NX_FAULT_SEGMENT = 0x80,	/* no segment table entry */
	NX_FAULT_NO_PTE = 0x94,		/* no page table entry */
	NX_FAULT_PROTECTION = 0x95,	/* the page's protection refuses it */
	NX_FAULT_KEY = 0x97,		/* the window's key mask refuses it */
};

struct nx_fault_stamp {
	uint64_t fault_storage_addr;	/* big-endian */
	uint16_t reserved;
	uint8_t flags;
	uint8_t fault_status;
	uint32_t pswid;			/* big-endian */
} __attribute__((packed, aligned(NX_FAULT_STAMP_ALIGN)));

/* nx_fault_stamp.flags: the refused access was a store rather than a load. */
static inline struct vas_field nx_fault_flag_write(void)
{
	struct vas_field f = { .msb = 7, .width = 1 };

	return f;
}

/* Coprocessor-request block. */
struct nx_crb {
	uint32_t ccw;		/* big-endian */
	uint32_t flags;		/* big-endian */
	uint64_t csb_addr;	/* big-endian */

	struct nx_dde source;
	struct nx_dde target;

	struct nx_ccb ccb;

	union {
		struct nx_fault_stamp nx;
		uint8_t reserved[16];
	} stamp;

	uint8_t reserved[32];

	struct nx_csb csb;
} __attribute__((aligned(NX_CRB_ALIGN)));

/*
 * Fields of the coprocessor command word. Positions are from the NX P8
 * workbook section 4.3.1, figure 4-6, by way of drivers/crypto/nx/nx-842.h.
 *
 * A request pasted to a userspace window carries only its function code: the
 * window already names the engine, so the coprocessor type is not set here.
 */
static inline struct vas_field nx_ccw_priority(void)
{
	struct vas_field f = { .msb = 0, .width = 8 };

	return f;
}

static inline struct vas_field nx_ccw_coprocessor_type(void)
{
	struct vas_field f = { .msb = 8, .width = 8 };

	return f;
}

static inline struct vas_field nx_ccw_instance(void)
{
	struct vas_field f = { .msb = 18, .width = 11 };

	return f;
}

static inline struct vas_field nx_ccw_function(void)
{
	struct vas_field f = { .msb = 29, .width = 3 };

	return f;
}

/*
 * Fields of the status block. V marks the block written, so a request is
 * complete when it is set and not before.
 */
static inline struct vas_field nx_csb_valid(void)
{
	struct vas_field f = { .msb = 0, .width = 1 };

	return f;
}

static inline struct vas_field nx_csb_format(void)
{
	struct vas_field f = { .msb = 5, .width = 1 };

	return f;
}

static inline struct vas_field nx_csb_chain(void)
{
	struct vas_field f = { .msb = 6, .width = 2 };

	return f;
}

static inline struct vas_field nx_csb_ce_incomplete(void)
{
	struct vas_field f = { .msb = 0, .width = 1 };

	return f;
}

static inline struct vas_field nx_csb_ce_termination(void)
{
	struct vas_field f = { .msb = 1, .width = 1 };

	return f;
}

static inline struct vas_field nx_csb_ce_tpbc(void)
{
	struct vas_field f = { .msb = 2, .width = 1 };

	return f;
}

/*
 * Fields of the request's status-block address word. The address occupies
 * the leading bits because the block is 16-byte aligned, leaving the low
 * bits to carry control. AT selects between effective and real addressing
 * for the whole request and stays clear for a userspace window.
 */
static inline struct vas_field nx_crb_csb_address(void)
{
	struct vas_field f = { .msb = 0, .width = 60 };

	return f;
}

static inline struct vas_field nx_crb_csb_ccb_valid(void)
{
	struct vas_field f = { .msb = 60, .width = 1 };

	return f;
}

static inline struct vas_field nx_crb_csb_address_type(void)
{
	struct vas_field f = { .msb = 62, .width = 1 };

	return f;
}

static inline struct vas_field nx_crb_csb_perf_monitor(void)
{
	struct vas_field f = { .msb = 63, .width = 1 };

	return f;
}

/* Fields of a data-descriptor entry's flag word. */
static inline struct vas_field nx_dde_present(void)
{
	struct vas_field f = { .msb = 0, .width = 1 };

	return f;
}

/*
 * Completion codes, from arch/powerpc/include/asm/icswx.h. Zero is success
 * and everything else describes why the request did not finish.
 */
enum nx_cc {
	NX_CC_SUCCESS = 0,
	NX_CC_INVALID_ALIGN = 1,
	NX_CC_OPERAND_OVERLAP = 2,
	NX_CC_DATA_LENGTH = 3,
	NX_CC_TRANSLATION = 5,
	NX_CC_PROTECTION = 6,
	NX_CC_RD_EXTERNAL = 7,
	NX_CC_INVALID_OPERAND = 8,
	NX_CC_PRIVILEGE = 9,
	NX_CC_INTERNAL = 10,
	NX_CC_WR_EXTERNAL = 12,
	NX_CC_NOSPC = 13,
	NX_CC_EXCESSIVE_DDE = 14,
	NX_CC_WR_TRANSLATION = 15,
	NX_CC_WR_PROTECTION = 16,
	NX_CC_UNKNOWN_CODE = 17,
	NX_CC_ABORT = 18,
	NX_CC_EXCEED_BYTE_COUNT = 19,
	NX_CC_TRANSPORT = 20,
	NX_CC_INVALID_CRB = 21,
	NX_CC_INVALID_DDE = 30,
	NX_CC_SEGMENTED_DDL = 31,
	NX_CC_PROGRESS_POINT = 32,
	NX_CC_DDE_OVERFLOW = 33,
	NX_CC_SESSION = 34,
	NX_CC_PROVISION = 36,
	NX_CC_CHAIN = 37,
	NX_CC_SEQUENCE = 38,
	NX_CC_HW = 39,
	/*
	 * The kernel completed a request the accelerator could not translate.
	 * The stamp names the address; the request is to be resubmitted once
	 * that address is present.
	 */
	NX_CC_FAULT_ADDRESS = 250,
};

/*
 * A span of memory to read from, and one to write to. They are separate
 * types because passing one where the other belongs is a mistake the
 * compiler should catch rather than the accelerator.
 */
struct nx_source {
	const void *addr;
	size_t len;
};

struct nx_target {
	void *addr;
	size_t len;
};

static inline struct nx_source nx_source(const void *addr, size_t len)
{
	struct nx_source source = { .addr = addr, .len = len };

	return source;
}

static inline struct nx_target nx_target(void *addr, size_t len)
{
	struct nx_target target = { .addr = addr, .len = len };

	return target;
}

/*
 * Lists of discontiguous spans, one type per direction for the same reason.
 * A list holds the descriptor entries the accelerator reads, so it lives
 * until the request that names it has completed.
 *
 * The entry count is carried in an 8-bit field, so a list holds at most 255
 * spans; an engine may accept fewer and answers NX_CC_EXCESSIVE_DDE when a
 * list is longer than it will walk.
 */
enum { NX_SCATTER_SPANS_MAX = 255 };

struct nx_source_list;
struct nx_target_list;

int nx_source_list_create(unsigned int capacity, struct nx_source_list **list);
void nx_source_list_destroy(struct nx_source_list **list);
void nx_source_list_reset(struct nx_source_list *list);
int nx_source_list_add(struct nx_source_list *list, struct nx_source span);
unsigned int nx_source_list_count(const struct nx_source_list *list);
uint64_t nx_source_list_length(const struct nx_source_list *list);

int nx_target_list_create(unsigned int capacity, struct nx_target_list **list);
void nx_target_list_destroy(struct nx_target_list **list);
void nx_target_list_reset(struct nx_target_list *list);
int nx_target_list_add(struct nx_target_list *list, struct nx_target span);
unsigned int nx_target_list_count(const struct nx_target_list *list);
uint64_t nx_target_list_length(const struct nx_target_list *list);

/*
 * The command word of a request. Engines build it from their own function
 * codes; see vas/nx842.h for one that does.
 */
struct nx_ccw {
	uint32_t word;
};

/* A request, with the block the accelerator reads and the one it reports in. */
struct nx_request;

int nx_request_create(struct nx_request **request);
void nx_request_destroy(struct nx_request **request);

/* Return a request to the state it had when created. */
void nx_request_reset(struct nx_request *request);

void nx_request_set_ccw(struct nx_request *request, struct nx_ccw ccw);

int nx_request_set_source(struct nx_request *request, struct nx_source source);
int nx_request_set_target(struct nx_request *request, struct nx_target target);
int nx_request_set_source_list(struct nx_request *request, const struct nx_source_list *list);
int nx_request_set_target_list(struct nx_request *request, const struct nx_target_list *list);

/*
 * The block the engine reads. An engine whose request carries a parameter
 * block of its own places it after the request; this exposes the request so
 * such an engine can be built on top.
 */
struct nx_crb *nx_request_crb(struct nx_request *request);

/*
 * How hard to try. A paste is refused while the window's queue is full, and
 * a request may complete asking for an address to be made present; neither
 * is an error until the allowance runs out.
 */
struct nx_retry_policy {
	unsigned int paste_attempts;
	unsigned int fault_retries;
	unsigned int completion_timeout_ms;
};

void nx_retry_policy_init(struct nx_retry_policy *policy);

/*
 * Submit a request once. Returns 0 when the accelerator accepted it, -EBUSY
 * when the window would not take it and the caller should try again, or
 * another negative errno.
 */
int nx_submit(struct vas_window *window, struct nx_request *request);

/*
 * Wait for a submitted request to report. Returns 0 once the status block is
 * valid, -ETIMEDOUT if it is not within @timeout_ms.
 */
int nx_wait(struct nx_request *request, unsigned int timeout_ms);

/*
 * Submit, wait, and resolve what can be resolved: retry a refused paste, and
 * make present any address the accelerator reports it could not translate.
 * Returns 0 when the request completed, whatever its completion code, so the
 * caller reads the outcome from nx_request_completion(). A negative errno
 * means the request never completed.
 */
int nx_execute(struct vas_window *window, struct nx_request *request,
	       const struct nx_retry_policy *policy);

/* What a completed request reports. */
struct nx_completion {
	enum nx_cc cc;
	/* Bytes the engine wrote to the target. */
	uint32_t processed_bytes;
	bool incomplete;
	bool terminated;

	/* Set when cc is NX_CC_FAULT_ADDRESS. */
	bool faulted;
	const void *fault_address;
	enum nx_fault_status fault_status;
	bool fault_on_write;
};

/*
 * Read the outcome of a request. Returns 0, or -EAGAIN if the accelerator has
 * not reported yet.
 */
int nx_request_completion(const struct nx_request *request, struct nx_completion *completion);

/* Whether a completion code describes something a retry can resolve. */
bool nx_cc_is_retryable(enum nx_cc cc);

/* The name of a code, and a sentence saying what it means. */
const char *nx_cc_name(enum nx_cc cc);
const char *nx_cc_describe(enum nx_cc cc);
const char *nx_fault_status_name(enum nx_fault_status status);

/*
 * Make a span present and, for a target, writable, so that a request does not
 * have to fault its way through it. Walks at the running page size.
 */
int nx_touch_source(struct nx_source source);
int nx_touch_target(struct nx_target target);

#ifdef __cplusplus
}
#endif

#endif /* _VAS_NX_H */
