/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Opening a Virtual Accelerator Switchboard send window from userspace.
 *
 * A window is the handle a process holds on an accelerator: requests pasted
 * to it are translated in the address space that opened it. This header
 * covers the setup half -- finding an engine, opening a window, bounding what
 * it may translate -- and vas/nx.h covers the requests themselves.
 *
 * Documentation/arch/powerpc/vas.rst describes the model and
 * Documentation/arch/powerpc/vas-api.rst the interface this wraps.
 */

#ifndef _VAS_VAS_H
#define _VAS_VAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vas/field.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The coprocessor types a window may bind to, as
 * arch/powerpc/include/asm/vas.h numbers them. The number is also the minor
 * of the type's device node.
 */
enum vas_cop {
	VAS_COP_842 = 1,
	VAS_COP_842_HIPRI = 2,
	VAS_COP_GZIP = 3,
	VAS_COP_GZIP_HIPRI = 4,
	VAS_COP_SYM = 5,
	VAS_COP_SYM_HIPRI = 6,
};

/*
 * Each engine has two receive queues and the switchboard serves the high
 * priority one first. The kernel's own requests go there, so a window on a
 * high priority node competes with them, and such a node is granted
 * deliberately rather than being the one to reach for.
 */
static inline enum vas_cop vas_cop_hipri(enum vas_cop cop)
{
	switch (cop) {
	case VAS_COP_842:
		return VAS_COP_842_HIPRI;
	case VAS_COP_GZIP:
		return VAS_COP_GZIP_HIPRI;
	case VAS_COP_SYM:
		return VAS_COP_SYM_HIPRI;
	default:
		return cop;
	}
}

static inline bool vas_cop_is_hipri(enum vas_cop cop)
{
	return cop == VAS_COP_842_HIPRI || cop == VAS_COP_GZIP_HIPRI ||
	       cop == VAS_COP_SYM_HIPRI;
}

/*
 * An engine may offer more than one node. The platform's node carries
 * everything the running kernel offers. The legacy node carries the
 * interface userspace had before it and refuses anything later, so that a
 * program written against that interface cannot have it change underneath;
 * only GZIP has one, because only GZIP had userspace to keep.
 */
enum vas_node {
	VAS_NODE_PLATFORM = 0,
	VAS_NODE_LEGACY = 1,
};

/*
 * Which VAS instance to open on. There is one per chip, and a window on the
 * instance local to the running thread reaches its accelerator without
 * crossing the fabric.
 */
struct vas_instance_id {
	int32_t value;
};

static inline struct vas_instance_id vas_instance_any(void)
{
	struct vas_instance_id id = { .value = -1 };

	return id;
}

static inline struct vas_instance_id vas_instance(int32_t which)
{
	struct vas_instance_id id = { .value = which };

	return id;
}

/*
 * A protection-key mask, in the form the AMR holds: two bits per key, the
 * upper denying loads and the lower stores. A window may be opened under a
 * mask of its own, which may only withhold rights the opening thread has.
 */
struct vas_key_mask {
	uint64_t bits;
};

static inline struct vas_key_mask vas_key_mask(uint64_t bits)
{
	struct vas_key_mask mask = { .bits = bits };

	return mask;
}

/*
 * A range of the address space, for bounding what a confined window may
 * translate. The kernel widens it to whole segments.
 */
struct vas_region {
	const void *start;
	size_t len;
};

static inline struct vas_region vas_region(const void *start, size_t len)
{
	struct vas_region region = { .start = start, .len = len };

	return region;
}

/*
 * How a window is to be opened. Initialise with vas_window_attr_init() so
 * that fields added later keep their meaning; a zeroed structure asks for
 * nothing beyond a plain window.
 */
struct vas_window_attr {
	enum vas_cop cop;
	enum vas_node node;
	struct vas_instance_id instance;

	/* Draw against the caller's QoS credit rather than the default. */
	bool qos_credit;

	/*
	 * Translate only the regions handed to vas_window_domain_add(), and
	 * nothing at all until the first of them. Hashed page table kernels
	 * only.
	 */
	bool confined;

	/*
	 * Translate under this key mask instead of the opening thread's own.
	 * NULL leaves the window following the thread.
	 */
	const struct vas_key_mask *key_mask;
};

void vas_window_attr_init(struct vas_window_attr *attr, enum vas_cop cop);

/* An open window. Its paste mapping belongs to the address space that opened it. */
struct vas_window;

/*
 * Open a window on @attr->cop. Returns 0, or a negative errno; the interface
 * reports ENODEV when the engine is absent, EBUSY when no credit is free and
 * EOPNOTSUPP when the kernel does not carry a feature the attributes ask for.
 */
int vas_window_open(const struct vas_window_attr *attr, struct vas_window **window);

/* Close a window and clear the caller's pointer. Safe on NULL. */
void vas_window_close(struct vas_window **window);

/*
 * Widen or narrow what a confined window may translate. Adding the first
 * region is what lets a confined window translate anything at all; dropping
 * the last leaves it able to translate nothing.
 */
int vas_window_domain_add(struct vas_window *window, struct vas_region region);
int vas_window_domain_drop(struct vas_window *window, struct vas_region region);

/* The type the window is bound to, and the address requests are pasted to. */
enum vas_cop vas_window_cop(const struct vas_window *window);
void *vas_window_paste_target(const struct vas_window *window);

/* What an engine can do, read from its sysfs class. */
struct vas_engine_info {
	enum vas_cop cop;
	enum vas_node node;
	bool present;
	/* Bytes one request may process, or 0 when the engine sets no limit. */
	uint64_t req_max_processed_len;
};

int vas_engine_info(enum vas_cop cop, enum vas_node node,
		    struct vas_engine_info *info);

/* The name of a node, its device path, and its sysfs class directory. */
const char *vas_cop_name(enum vas_cop cop, enum vas_node node);
const char *vas_cop_device(enum vas_cop cop, enum vas_node node);
const char *vas_cop_class(enum vas_cop cop, enum vas_node node);

/* The page size this process runs under; the paste mapping is one page. */
size_t vas_page_size(void);

/*
 * The paste address, as VAS P9 workbook section 1.3.3.1 lays it out. The
 * kernel maps the page holding base + (window id << 16); NX additionally
 * requires the report-enable bit, which the library sets when it derives the
 * paste target from that mapping. Bit 53 of a 64-bit address is within the
 * first kilobyte of the page, so this holds at any page size.
 */
static inline struct vas_field vas_paste_window_id(void)
{
	struct vas_field f = { .msb = 32, .width = 16 };

	return f;
}

static inline struct vas_field vas_paste_report_enable(void)
{
	struct vas_field f = { .msb = 53, .width = 1 };

	return f;
}

#ifdef __cplusplus
}
#endif

#endif /* _VAS_VAS_H */
