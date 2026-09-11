// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Saying what a completion code or a fault status means.
 *
 * The descriptions are of the accelerator's behaviour, not of the request
 * that provoked it, so that a caller can print one and know what to do next.
 * Codes are from the NX workbook by way of arch/powerpc/include/asm/icswx.h,
 * and the fault statuses from the same header, where they are recorded as
 * measured rather than documented.
 */

#include <stddef.h>

#include <vas/nx.h>
#include <vas/nx842.h>

struct cc_text {
	enum nx_cc cc;
	const char *name;
	const char *description;
};

static const struct cc_text cc_table[] = {
	{ NX_CC_SUCCESS, "SUCCESS",
	  "the request completed and the target holds its output" },
	{ NX_CC_INVALID_ALIGN, "INVALID_ALIGN",
	  "a buffer's address does not meet the engine's alignment rule" },
	{ NX_CC_OPERAND_OVERLAP, "OPERAND_OVERLAP",
	  "the source and the target name overlapping memory" },
	{ NX_CC_DATA_LENGTH, "DATA_LENGTH",
	  "a length is zero, or not a multiple the engine accepts" },
	{ NX_CC_TRANSLATION, "TRANSLATION",
	  "a source address could not be translated" },
	{ NX_CC_PROTECTION, "PROTECTION",
	  "a source address was refused: the page's protection or the window's key mask denies the read" },
	{ NX_CC_RD_EXTERNAL, "RD_EXTERNAL",
	  "a read took a machine check or other external error" },
	{ NX_CC_INVALID_OPERAND, "INVALID_OPERAND",
	  "the request names something the engine cannot act on" },
	{ NX_CC_PRIVILEGE, "PRIVILEGE",
	  "the request asks for something the window is not privileged to do" },
	{ NX_CC_INTERNAL, "INTERNAL",
	  "the engine reported an internal error" },
	{ NX_CC_WR_EXTERNAL, "WR_EXTERNAL",
	  "a write took a machine check or other external error" },
	{ NX_CC_NOSPC, "NOSPC",
	  "the target is too small for the output; nothing usable was written" },
	{ NX_CC_EXCESSIVE_DDE, "EXCESSIVE_DDE",
	  "a descriptor list is longer than the engine will walk" },
	{ NX_CC_WR_TRANSLATION, "WR_TRANSLATION",
	  "a target address could not be translated" },
	{ NX_CC_WR_PROTECTION, "WR_PROTECTION",
	  "a target address was refused: the page's protection or the window's key mask denies the write" },
	{ NX_CC_UNKNOWN_CODE, "UNKNOWN_CODE",
	  "the command word names a function this engine does not have" },
	{ NX_CC_ABORT, "ABORT",
	  "the request was aborted before it finished" },
	{ NX_CC_EXCEED_BYTE_COUNT, "EXCEED_BYTE_COUNT",
	  "the request would process more bytes than the engine permits in one job" },
	{ NX_CC_TRANSPORT, "TRANSPORT",
	  "the request failed on the way to or from the engine" },
	{ NX_CC_INVALID_CRB, "INVALID_CRB",
	  "the request block itself is malformed" },
	{ NX_CC_INVALID_DDE, "INVALID_DDE",
	  "a descriptor entry is malformed" },
	{ NX_CC_SEGMENTED_DDL, "SEGMENTED_DDL",
	  "a descriptor list crosses a boundary the engine will not follow" },
	{ NX_CC_PROGRESS_POINT, "PROGRESS_POINT",
	  "the request stopped at a progress point and may be resumed" },
	{ NX_CC_DDE_OVERFLOW, "DDE_OVERFLOW",
	  "the output ran past the end of the target's descriptor list" },
	{ NX_CC_SESSION, "SESSION",
	  "the engine's session state refuses the request" },
	{ NX_CC_PROVISION, "PROVISION",
	  "the engine is not provisioned for this request" },
	{ NX_CC_CHAIN, "CHAIN",
	  "a chained request failed" },
	{ NX_CC_SEQUENCE, "SEQUENCE",
	  "the request arrived out of the sequence the engine expects" },
	{ NX_CC_HW, "HW",
	  "the engine reported a hardware error" },
	{ NX_CC_FAULT_ADDRESS, "FAULT_ADDRESS",
	  "the accelerator could not translate an address and the kernel completed the request instead; the stamp names the address, and the request may be submitted again once it is present" },
};

static const struct cc_text *cc_lookup(enum nx_cc cc)
{
	size_t i;

	for (i = 0; i < sizeof(cc_table) / sizeof(cc_table[0]); i++) {
		if (cc_table[i].cc == cc)
			return &cc_table[i];
	}

	return NULL;
}

const char *nx_cc_name(enum nx_cc cc)
{
	const struct cc_text *text = cc_lookup(cc);

	return text ? text->name : "UNRECOGNISED";
}

const char *nx_cc_describe(enum nx_cc cc)
{
	const struct cc_text *text = cc_lookup(cc);

	return text ? text->description :
		      "the engine reported a completion code this library does not know";
}

const char *nx_fault_status_name(enum nx_fault_status status)
{
	switch (status) {
	case NX_FAULT_SEGMENT:
		return "no segment table entry for the address";
	case NX_FAULT_NO_PTE:
		return "no page table entry for the address";
	case NX_FAULT_PROTECTION:
		return "the page's protection refuses the access";
	case NX_FAULT_KEY:
		return "the window's key mask refuses the access";
	}

	return "the nest MMU refused the address for a reason this library does not know";
}

const char *nx_842_function_name(enum nx_842_function function)
{
	switch (function) {
	case NX_842_COMPRESS:
		return "compress";
	case NX_842_COMPRESS_CRC:
		return "compress with CRC";
	case NX_842_DECOMPRESS:
		return "decompress";
	case NX_842_DECOMPRESS_CRC:
		return "decompress with CRC";
	case NX_842_MOVE:
		return "move";
	}

	return "unrecognised function";
}
