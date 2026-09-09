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
	/*
	 * The switchboard itself, with no engine behind it: a window on this
	 * type delivers to another window rather than to an accelerator, and
	 * the paste wakes the thread that opened the other one. See
	 * vas_destination_open() below.
	 */
	VAS_COP_FTW = 7,
};

/* Whether requests on this type are computed by an engine or only delivered. */
static inline bool vas_cop_has_engine(enum vas_cop cop)
{
	return cop != VAS_COP_FTW;
}

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

	/*
	 * Deliver to the destination this descriptor was opened on rather
	 * than to an engine, so that a paste wakes the thread that opened it.
	 * -1, which vas_window_attr_init() sets, for a window on an engine.
	 *
	 * Holding the descriptor is the whole of the right to wake that
	 * thread, so it is passed over a unix socket rather than named: see
	 * vas_destination_open().
	 */
	int wake_target;
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

/*
 * A destination: somewhere a sender can wake. The thread that opens one is
 * the thread that is woken and no other, so a process wanting several
 * destinations opens one per thread.
 *
 * There is no name to publish. The descriptor is the destination, and holding
 * it is the whole of the right to wake that thread: hand it to a sender over
 * a unix socket with SCM_RIGHTS, or let a child inherit it. A process that
 * was not given one cannot reach the thread, and cannot arrive at one by
 * guessing.
 */
struct vas_destination;

int vas_destination_open(struct vas_instance_id instance,
			 struct vas_destination **dest);

/* What one paste carries, and so what one entry of a queue holds. */
#define VAS_MESSAGE_BYTES 128

/*
 * A destination that keeps what is pasted to it rather than only being woken
 * by it. @bytes is how much queue to ask for, or 0 for the kernel's choice; it
 * is rounded up to whole pages.
 *
 * A paste carries its 128 bytes whether or not anyone keeps them, so a sender
 * with something to say can say it in the operation that wakes the reader
 * instead of leaving the reader to fetch it from memory -- which costs a miss
 * on a line the sender owns, and costs more as the group grows while the wake
 * does not.
 *
 * The queue is a ring and the switchboard does not wait for it: a reader that
 * falls behind is overwritten and neither end is told. That is the same
 * bargain as the wake itself, where a notify arriving before the thread waits
 * is simply not delivered.
 */
int vas_destination_open_queued(struct vas_instance_id instance, size_t bytes,
				struct vas_destination **dest);

/*
 * The next message to have arrived, or NULL if none has. Returns
 * VAS_MESSAGE_BYTES of whatever the sender pasted, owned by the destination
 * and valid until it is released.
 *
 * Nothing blocks here. Pair it with vas_wait(), which is what the same paste
 * resumes: wake, take what is waiting, wait again.
 */
const void *vas_destination_next(struct vas_destination *dest);

/*
 * Give an entry back, so the switchboard may write another there. A reader
 * that takes a copy and releases at once keeps the ring as empty as it can;
 * one that holds entries shortens the ring by that many.
 */
void vas_destination_release(struct vas_destination *dest, const void *message);

/*
 * Move on without freeing the entry, for a thread reading a queue it shares
 * with others: the slot is the group's, and freeing it is one thread's job,
 * but every one of them has its own place in the ring to keep.
 */
void vas_destination_advance(struct vas_destination *dest);

/*
 * Become a destination alongside the one @join_fd was opened on, so that one
 * paste wakes both. The switchboard addresses a destination by the partition,
 * process and thread running there, and joining gives this thread the same
 * identity as that one -- a notify carries an identity rather than a
 * recipient, so every thread answering to it is matched.
 *
 * Only threads of one process may share an identity: the process part of it
 * is the address space, which cannot be borrowed. A descriptor from another
 * process is refused with EPERM.
 *
 * The identity stops being unique to this thread, which is what an
 * accelerator otherwise uses to resume the thread that submitted to it. Join
 * threads that are waiting for the same thing; do not join a thread that has
 * work of its own outstanding.
 */
int vas_destination_join(int join_fd, struct vas_destination **dest);

/*
 * Join @owner's identity and read what arrives on it, rather than only being
 * woken by it.
 *
 * One paste writes one entry, into the queue of the window that owns the
 * identity, and wakes every thread sharing it. So the group does not each
 * get a copy: they each read the copy, from wherever the switchboard's write
 * left it, and the fabric answers the second reader from the same place it
 * answered the first. That is the whole of what a queue offers a group -- a
 * sender that pastes to one destination has said something to all of them.
 *
 * The queue is @owner's mapping, which this thread has already because it is
 * a thread of the same process; what it gains is a cursor of its own.
 * @owner must have been opened with vas_destination_open_queued() and must
 * outlive the result.
 *
 * The entry belongs to the group, so vas_destination_release() is the
 * group's decision: on any sharer it frees the slot for the switchboard to
 * write over, whatever the others have read. A group that cannot afford to
 * lose an entry releases it once, after the last reader has taken it.
 */
int vas_destination_join_queue(struct vas_destination *owner,
			       struct vas_destination **dest);

/* Close a destination and clear the caller's pointer. Safe on NULL. */
void vas_destination_close(struct vas_destination **dest);

/*
 * The descriptor to hand a sender. Owned by the destination, valid until it
 * is closed; the sender may close its copy once its window is open, because
 * the window holds a reference of its own.
 */
int vas_destination_fd(const struct vas_destination *dest);

/*
 * Wake the thread that opened the destination this window was pointed at.
 *
 * Nothing is delivered but the wake. The receive window has FIFO writes
 * disabled, so the 128 bytes a paste carries are discarded: any data must
 * travel through ordinary memory, stored before this call, which orders the
 * store against the wake.
 */
int vas_wake(struct vas_window *window);

/*
 * Wake, and deliver @block with it. The 128 bytes are copied and pasted as one
 * transfer, so they arrive in the destination's queue if it kept one and are
 * discarded if it did not -- a sender need not know which, beyond knowing that
 * a destination without a queue will only be woken.
 *
 * @block must be 128-byte aligned: the copy instruction takes an aligned block
 * and nothing else.
 *
 * Ordering is as vas_wake(): any store the woken thread is to see through
 * ordinary memory must be made before this call, which orders it against the
 * transfer. What travels in @block needs no such care, being part of the
 * transfer itself.
 */
int vas_send(struct vas_window *window, const void *block);

/*
 * Suspend this thread until something resumes it, and return. The caller's
 * loop is what decides whether to suspend again.
 *
 * "Something" is deliberately vague: a wake, but also any exception the
 * thread takes, so this returns for reasons that have nothing to do with a
 * sender. It is the primitive under vas_destination_wait(), exposed for a
 * caller whose condition is not a flag becoming non-zero -- a sequence number
 * reaching a value, say. Such a caller must load the condition with at least
 * acquire ordering, which is the part vas_destination_wait() otherwise does.
 */
void vas_wait(void);

/*
 * Suspend until woken, re-reading *@flag each time, and return once it reads
 * non-zero.
 *
 * The flag is not optional. A wake is a notify matched against the thread the
 * switchboard finds running: one that arrives before this call, or while the
 * thread is off a core, is neither delivered nor queued. The loop is what
 * makes the rendezvous correct and the notify is what makes it fast -- the
 * thread also resumes on any interrupt, so without the flag a lost wake is
 * indistinguishable from a slow one.
 *
 * The sender must set the flag before pasting; vas_wake() orders its own
 * side, but the store has to precede the call.
 */
void vas_destination_wait(const struct vas_destination *dest,
			  const volatile int *flag);

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
