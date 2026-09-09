// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * That what the library builds is what the accelerator reads.
 *
 * Every check here compares against a byte pattern worked out from the
 * structure definitions rather than against the library's own idea of them,
 * so a field that moves, a conversion that is dropped, or a host of the other
 * endianness all show up as a failure. No accelerator is needed: the point is
 * the encoding, not the engine.
 */

#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vas/nx842.h>
#include <vas/vas.h>

static unsigned int checks;
static unsigned int failures;

static void check(bool condition, const char *what)
{
	checks++;
	if (condition)
		return;

	printf("failed: %s\n", what);
	failures++;
}

static void check_u64(uint64_t got, uint64_t want, const char *what)
{
	checks++;
	if (got == want)
		return;

	printf("failed: %s\n  got  0x%" PRIx64 "\n  want 0x%" PRIx64 "\n", what, got, want);
	failures++;
}

/* Read a big-endian field out of the request as the hardware would. */
static uint32_t be32_at(const void *base, size_t offset)
{
	uint32_t raw;

	memcpy(&raw, (const char *)base + offset, sizeof(raw));

	return be32toh(raw);
}

static uint64_t be64_at(const void *base, size_t offset)
{
	uint64_t raw;

	memcpy(&raw, (const char *)base + offset, sizeof(raw));

	return be64toh(raw);
}

static void test_field_positions(void)
{
	struct vas_field whole = { .msb = 0, .width = 64 };
	struct vas_field byte_top = { .msb = 0, .width = 1 };
	struct vas_field byte_low = { .msb = 7, .width = 1 };

	/*
	 * The paste target's report-enable bit is what the library adds to
	 * the mapping the kernel returns. It is bit 53 of a 64-bit address,
	 * which is 0x400 and no other value.
	 */
	check_u64(vas_field_mask(vas_paste_report_enable(), 64), 0x400,
		  "report enable is bit 53, worth 0x400");
	check(vas_field_mask(vas_paste_report_enable(), 64) < 2048,
	      "report enable falls inside any page of 2 KiB or more");

	/*
	 * Bits 32:47 of a 64-bit word are worth 0xffff0000, which is the
	 * window id shifted left by 16 -- the shift the kernel reads from the
	 * device tree and applies to reach a window's paste address.
	 */
	check_u64(vas_field_mask(vas_paste_window_id(), 64), 0xffff0000,
		  "the window id occupies bits 32:47");
	check_u64(vas_field_shift(vas_paste_window_id(), 64), 16,
		  "which places a window id 16 bits up");
	check_u64(vas_field_put64(vas_paste_window_id(), 0, 4), 4 << 16,
		  "so window 4 sits at 0x40000, as the VAS workbook has it");

	check_u64(vas_field_get8(byte_top, 0x80), 1, "bit 0 of a byte is 0x80");
	check_u64(vas_field_get8(byte_low, 0x01), 1, "bit 7 of a byte is 0x01");
	check_u64(vas_field_put64(whole, 0, UINT64_MAX), UINT64_MAX,
		  "a 64-bit field carries every bit");

	/* The command word's function code, against the mask it replaces. */
	check_u64(vas_field_mask(nx_ccw_function(), 32), 0x00000007,
		  "the function code is the command word's low three bits");
	check_u64(vas_field_mask(nx_ccw_coprocessor_type(), 32), 0x00ff0000,
		  "the coprocessor type is bits 8:15");
	check_u64(vas_field_mask(nx_crb_csb_address(), 64), 0xfffffffffffffff0ULL,
		  "the status block address is bits 0:59");
}

static void test_structure_layout(void)
{
	check(sizeof(struct nx_csb) == NX_CSB_SIZE, "the status block is 16 bytes");
	check(sizeof(struct nx_dde) == NX_DDE_SIZE, "a descriptor entry is 16 bytes");
	check(sizeof(struct nx_crb) == NX_CRB_ALIGN,
	      "the request block is padded to its alignment");
	check(offsetof(struct nx_crb, source) == 16, "the source entry is at offset 16");
	check(offsetof(struct nx_crb, target) == 32, "the target entry is at offset 32");
	check(offsetof(struct nx_crb, stamp) == 64, "the fault stamp is at offset 64");
	check(offsetof(struct nx_crb, csb) == 112, "the status block is at offset 112");
}

static void test_request_encoding(void)
{
	static unsigned char source[4096] __attribute__((aligned(128)));
	static unsigned char target[4096] __attribute__((aligned(128)));
	struct nx_request *request;
	const struct nx_crb *crb;
	uint64_t csb_word;
	int rc;

	rc = nx_request_create(&request);
	if (rc) {
		printf("failed: creating a request: %s\n", strerror(-rc));
		failures++;
		return;
	}

	crb = nx_request_crb(request);
	check((((uintptr_t)crb) % NX_CRB_ALIGN) == 0,
	      "the request block is aligned for the copy instruction");

	nx_request_set_ccw(request, nx_842_ccw(NX_842_MOVE));
	rc = nx_request_set_source(request, nx_source(source, sizeof(source)));
	if (!rc)
		rc = nx_request_set_target(request, nx_target(target, sizeof(target)));
	check(rc == 0, "the buffers are accepted");

	/*
	 * The move function is code 4, and a request through a userspace
	 * window sets nothing else in the command word.
	 */
	check_u64(be32_at(crb, 0), NX_842_MOVE, "the command word holds only the function code");

	csb_word = be64_at(crb, 8);
	check_u64(csb_word, (uint64_t)(uintptr_t)&crb->csb,
		  "the request points at its own status block");
	check_u64(vas_field_get64(nx_crb_csb_address_type(), csb_word), 0,
		  "addresses are effective, not real");
	check_u64(vas_field_get64(nx_crb_csb_ccb_valid(), csb_word), 0,
		  "no completion block is claimed");

	/* Source entry: direct, so the count is zero and the address is the buffer. */
	check_u64(be32_at(crb, 16) >> 16, 0, "the source entry carries no flags");
	check_u64((be32_at(crb, 16) >> 8) & 0xff, 0, "the source entry is direct");
	check_u64(be32_at(crb, 20), sizeof(source), "the source length is in bytes 20:23");
	check_u64(be64_at(crb, 24), (uint64_t)(uintptr_t)source,
		  "the source address is in bytes 24:31");

	check_u64(be32_at(crb, 36), sizeof(target), "the target length is in bytes 36:39");
	check_u64(be64_at(crb, 40), (uint64_t)(uintptr_t)target,
		  "the target address is in bytes 40:47");

	nx_request_destroy(&request);
	check(request == NULL, "destroying a request clears the caller's pointer");
}

static void test_scatter_encoding(void)
{
	static unsigned char spans[4][1024] __attribute__((aligned(128)));
	struct nx_source_list *list;
	struct nx_request *request;
	const struct nx_crb *crb;
	unsigned int i;
	int rc;

	rc = nx_source_list_create(4, &list);
	if (rc) {
		printf("failed: creating a source list: %s\n", strerror(-rc));
		failures++;
		return;
	}

	for (i = 0; i < 4; i++) {
		rc = nx_source_list_add(list, nx_source(spans[i], sizeof(spans[i])));
		check(rc == 0, "a span is accepted");
	}

	rc = nx_source_list_add(list, nx_source(spans[0], sizeof(spans[0])));
	check(rc == -ENOSPC, "a fifth span into a list of four is refused");

	check(nx_source_list_count(list) == 4, "the list holds four spans");
	check_u64(nx_source_list_length(list), 4 * sizeof(spans[0]),
		  "the list totals its spans");

	rc = nx_request_create(&request);
	if (rc) {
		nx_source_list_destroy(&list);
		return;
	}
	crb = nx_request_crb(request);

	rc = nx_request_set_source_list(request, list);
	check(rc == 0, "the list is accepted as a source");

	/* An indirect entry names how many entries follow and their total. */
	check_u64((be32_at(crb, 16) >> 8) & 0xff, 4, "the source entry counts four");
	check_u64(be32_at(crb, 20), 4 * sizeof(spans[0]),
		  "the source entry totals the spans");
	check(be64_at(crb, 24) != (uint64_t)(uintptr_t)spans[0],
	      "the source entry points at the list, not at the first span");

	nx_request_destroy(&request);
	nx_source_list_destroy(&list);
	check(list == NULL, "destroying a list clears the caller's pointer");
}

static void test_completion_decoding(void)
{
	struct nx_completion completion;
	struct nx_request *request;
	struct nx_crb *crb;
	const void *fault_at = (const void *)0x3fff87fd1000ULL;
	int rc;

	rc = nx_request_create(&request);
	if (rc)
		return;
	crb = nx_request_crb(request);

	rc = nx_request_completion(request, &completion);
	check(rc == -EAGAIN, "a request that has not reported reads as not ready");

	/*
	 * Write the status block the accelerator would have written for an
	 * address it could not translate on a store, and read it back
	 * through the library.
	 */
	crb->csb.cc = NX_CC_FAULT_ADDRESS;
	crb->csb.count = htobe32(4096);
	crb->csb.flags = (uint8_t)vas_field_put8(nx_csb_valid(), 0, 1);
	crb->stamp.nx.fault_storage_addr = htobe64((uint64_t)(uintptr_t)fault_at);
	crb->stamp.nx.fault_status = NX_FAULT_NO_PTE;
	crb->stamp.nx.flags = (uint8_t)vas_field_put8(nx_fault_flag_write(), 0, 1);

	rc = nx_request_completion(request, &completion);
	check(rc == 0, "a reported request reads back");
	check(completion.cc == NX_CC_FAULT_ADDRESS, "the completion code survives");
	check(completion.processed_bytes == 4096, "the byte count is converted from big endian");
	check(completion.faulted, "the completion is marked faulted");
	check(completion.fault_address == fault_at,
	      "the fault address is converted from big endian");
	check(completion.fault_status == NX_FAULT_NO_PTE, "the fault status survives");
	check(completion.fault_on_write, "the faulting access is known to be a store");
	check(nx_cc_is_retryable(completion.cc), "this code is worth retrying");
	check(!nx_cc_is_retryable(NX_CC_NOSPC), "a full target is not worth retrying");

	nx_request_destroy(&request);
}

static void test_descriptions(void)
{
	check(strcmp(nx_cc_name(NX_CC_SUCCESS), "SUCCESS") == 0, "success is named");
	check(strcmp(nx_cc_name(NX_CC_WR_PROTECTION), "WR_PROTECTION") == 0,
	      "a refused store is named");
	check(strcmp(nx_cc_name((enum nx_cc)199), "UNRECOGNISED") == 0,
	      "a code the library does not know is said to be unknown");
	check(nx_cc_describe(NX_CC_NOSPC) != NULL, "every code has a description");
	check(nx_fault_status_name(NX_FAULT_KEY) != NULL, "a key refusal is described");
	check(strcmp(nx_842_function_name(NX_842_MOVE), "move") == 0,
	      "the move function is named");
	check(vas_cop_name(VAS_COP_842, VAS_NODE_PLATFORM) != NULL,
	      "the 842 node is named");
	check(strcmp(vas_cop_device(VAS_COP_GZIP, VAS_NODE_LEGACY),
		     "/dev/crypto/nx-gzip") == 0,
	      "the legacy gzip node keeps the path userspace had");
	check(strcmp(vas_cop_device(VAS_COP_GZIP, VAS_NODE_PLATFORM),
		     "/dev/crypto/ibm-power9-nv-nx-gzip") == 0,
	      "the platform gzip node carries the platform's name");
	check(vas_cop_device(VAS_COP_842, VAS_NODE_LEGACY) == NULL,
	      "842 has no legacy node, having had no userspace to keep");
}

int main(void)
{
	test_field_positions();
	test_structure_layout();
	test_request_encoding();
	test_scatter_encoding();
	test_completion_decoding();
	test_descriptions();

	printf("%u checks on a %s-endian host with %zu-byte pages: %u failed\n",
	       checks, (htobe32(1) == 1) ? "big" : "little", vas_page_size(),
	       failures);

	return failures ? 1 : 0;
}
