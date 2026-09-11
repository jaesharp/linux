// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Opening and bounding a VAS send window.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <asm/vas-api.h>

#include <vas/nx.h>
#include <vas/vas.h>
#include <vas/trace.h>

#include "internal.h"

struct cop_names {
	enum vas_cop cop;
	enum vas_node node;
	const char *name;
	const char *device;
	const char *class_dir;
};

/*
 * A node's device path is /dev/crypto/<name> and its class directory
 * /sys/class/<name>/<name>, as Documentation/ABI/testing/sysfs-class-vas
 * describes. Only GZIP has a legacy node; the high-priority coprocessor
 * types have no node of their own.
 */
static const struct cop_names cop_table[] = {
	{ VAS_COP_842, VAS_NODE_PLATFORM, "ibm-power9-nv-nx-842",
	  "/dev/crypto/ibm-power9-nv-nx-842",
	  "/sys/class/ibm-power9-nv-nx-842/ibm-power9-nv-nx-842" },
	{ VAS_COP_GZIP, VAS_NODE_PLATFORM, "ibm-power9-nv-nx-gzip",
	  "/dev/crypto/ibm-power9-nv-nx-gzip",
	  "/sys/class/ibm-power9-nv-nx-gzip/ibm-power9-nv-nx-gzip" },
	{ VAS_COP_GZIP, VAS_NODE_LEGACY, "nx-gzip",
	  "/dev/crypto/nx-gzip", "/sys/class/nx-gzip/nx-gzip" },
	{ VAS_COP_SYM, VAS_NODE_PLATFORM, "ibm-power9-nv-nx-sym",
	  "/dev/crypto/ibm-power9-nv-nx-sym",
	  "/sys/class/ibm-power9-nv-nx-sym/ibm-power9-nv-nx-sym" },

	{ VAS_COP_842_HIPRI, VAS_NODE_PLATFORM, "ibm-power9-nv-nx-842-hipri",
	  "/dev/crypto/ibm-power9-nv-nx-842-hipri",
	  "/sys/class/ibm-power9-nv-nx-842-hipri/ibm-power9-nv-nx-842-hipri" },
	{ VAS_COP_GZIP_HIPRI, VAS_NODE_PLATFORM, "ibm-power9-nv-nx-gzip-hipri",
	  "/dev/crypto/ibm-power9-nv-nx-gzip-hipri",
	  "/sys/class/ibm-power9-nv-nx-gzip-hipri/ibm-power9-nv-nx-gzip-hipri" },
	{ VAS_COP_SYM_HIPRI, VAS_NODE_PLATFORM, "ibm-power9-nv-nx-sym-hipri",
	  "/dev/crypto/ibm-power9-nv-nx-sym-hipri",
	  "/sys/class/ibm-power9-nv-nx-sym-hipri/ibm-power9-nv-nx-sym-hipri" },

	/*
	 * The switchboard's own node, under its own directory because there
	 * is no engine behind it to name.
	 */
	{ VAS_COP_FTW, VAS_NODE_PLATFORM, "ibm-power9-nv-vas-ftw",
	  "/dev/vas/ibm-power9-nv-vas-ftw",
	  "/sys/class/ibm-power9-nv-vas-ftw/ibm-power9-nv-vas-ftw" },
};

static const struct cop_names *cop_lookup(enum vas_cop cop, enum vas_node node)
{
	size_t i;

	for (i = 0; i < sizeof(cop_table) / sizeof(cop_table[0]); i++) {
		if (cop_table[i].cop == cop && cop_table[i].node == node)
			return &cop_table[i];
	}

	return NULL;
}

const char *vas_cop_name(enum vas_cop cop, enum vas_node node)
{
	const struct cop_names *entry = cop_lookup(cop, node);

	return entry ? entry->name : NULL;
}

const char *vas_cop_device(enum vas_cop cop, enum vas_node node)
{
	const struct cop_names *entry = cop_lookup(cop, node);

	return entry ? entry->device : NULL;
}

const char *vas_cop_class(enum vas_cop cop, enum vas_node node)
{
	const struct cop_names *entry = cop_lookup(cop, node);

	return entry ? entry->class_dir : NULL;
}

size_t vas_page_size(void)
{
	static size_t cached;

	if (!cached) {
		long size = sysconf(_SC_PAGESIZE);

		cached = (size > 0) ? (size_t)size : 0;
	}

	return cached;
}

void vas_window_attr_init(struct vas_window_attr *attr, enum vas_cop cop)
{
	if (!attr)
		return;

	memset(attr, 0, sizeof(*attr));
	attr->cop = cop;
	attr->instance = vas_instance_any();
	/* A window on an engine, until the caller names a destination. */
	attr->wake_target = -1;
}

/*
 * Version 1 carries only the QoS credit flag; anything else the attributes
 * ask for needs version 2, which also requires the reserved fields to be
 * zero.
 */
static uint32_t open_version(const struct vas_window_attr *attr)
{
	/*
	 * Every attribute added after version 1 has to be named here. Version
	 * 1 carries the flags it has always carried and ignores the rest, so
	 * asking for a later feature through it does not fail: the flag is
	 * masked off and the window opens without what was asked for.
	 */
	if (attr->key_mask || attr->confined || attr->wake_target >= 0)
		return VAS_TX_WIN_OPEN_V2;

	return VAS_TX_WIN_OPEN_V1;
}

static uint64_t open_flags(const struct vas_window_attr *attr)
{
	uint64_t flags = 0;

	if (attr->qos_credit)
		flags |= VAS_TX_WIN_FLAG_QOS_CREDIT;
	if (attr->key_mask)
		flags |= VAS_TX_WIN_FLAG_AMR;
	if (attr->confined)
		flags |= VAS_TX_WIN_FLAG_DOMAINS;
	if (attr->wake_target >= 0)
		flags |= VAS_TX_WIN_FLAG_TARGET;

	return flags;
}

/*
 * The kernel maps the page holding the window's paste address with the
 * report-enable bit clear. NX requires that bit, so the target within the
 * mapping is the mapping plus the bit's value. It is bit 53 of a 64-bit
 * address, so its value is under a kilobyte and falls inside any page.
 */
static int paste_target_offset(size_t *offset)
{
	uint64_t enable = vas_field_mask(vas_paste_report_enable(), 64);
	size_t page = vas_page_size();

	if (!page)
		return -ENOSYS;
	if (enable >= page)
		return -ERANGE;

	*offset = (size_t)enable;

	return 0;
}

int vas_window_open(const struct vas_window_attr *attr, struct vas_window **window)
{
	struct vas_tx_win_open_attr uattr;
	struct vas_window *win;
	const char *device;
	size_t offset;
	size_t page;
	void *map;
	int saved;
	int rc;

	if (!attr || !window)
		return -EINVAL;

	*window = NULL;

	device = vas_cop_device(attr->cop, attr->node);
	if (!device)
		return -ENODEV;

	rc = paste_target_offset(&offset);
	if (rc)
		return rc;
	page = vas_page_size();

	win = calloc(1, sizeof(*win));
	if (!win)
		return -ENOMEM;

	win->cop = attr->cop;
	win->fd = open(device, O_RDWR);
	if (win->fd < 0) {
		rc = -errno;
		free(win);
		return rc;
	}

	memset(&uattr, 0, sizeof(uattr));
	uattr.version = open_version(attr);
	uattr.vas_id = (int16_t)attr->instance.value;
	uattr.flags = open_flags(attr);
	if (attr->key_mask)
		uattr.amr = attr->key_mask->bits;
	if (attr->wake_target >= 0)
		uattr.target_fd = attr->wake_target;

	if (ioctl(win->fd, VAS_TX_WIN_OPEN, (unsigned long)&uattr) < 0) {
		rc = -errno;
		goto out_close;
	}

	map = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, win->fd, 0ULL);
	if (map == MAP_FAILED) {
		rc = -errno;
		goto out_close;
	}

	win->paste_map = map;
	win->map_len = page;
	win->paste_target = (char *)map + offset;

	vas_trace_window_open(attr->cop, attr->node, attr->instance.value,
			      uattr.flags);

	*window = win;

	return 0;

out_close:
	saved = rc;
	close(win->fd);
	free(win);

	return saved;
}

static int destination_open(struct vas_instance_id instance, int join_fd,
			    size_t queue_bytes, struct vas_destination **dest);

int vas_destination_open(struct vas_instance_id instance,
			 struct vas_destination **dest)
{
	return destination_open(instance, -1, 0, dest);
}

/*
 * How much queue to ask for when the caller does not say. Enough entries that
 * a reader has room to fall behind, and asked for explicitly: letting each end
 * pick its own default had the kernel allocate 256 entries and the library map
 * 32, so seven pastes in eight landed where nothing was looking. Both ends
 * round to whole pages, so an explicit page multiple is a size they agree on.
 */
#define VAS_QUEUE_DEFAULT_BYTES (256 * VAS_MESSAGE_BYTES)

/*
 * A power of two bytes, at least a page and at least a kilobyte.
 *
 * The window context states the queue's size as log2 of its size in
 * kilobytes, so a size that is not a power of two kilobytes is not a size the
 * hardware can be told. The kernel rounds up to one; asking for exactly what
 * it will allocate is how this reader ends up walking the same ring the
 * switchboard writes. Asking for 60KB and mapping 480 entries, where the
 * switchboard had been told 32KB and wrapped at 256, lost every message from
 * the 257th on and reported nothing wrong.
 */
static size_t queue_size(size_t bytes)
{
	long page = sysconf(_SC_PAGESIZE);
	size_t want = bytes ? bytes : VAS_QUEUE_DEFAULT_BYTES;
	size_t size = 1024;

	if (page < 1)
		page = 4096;
	if (want < (size_t)page)
		want = (size_t)page;

	while (size < want)
		size <<= 1;

	return size;
}

int vas_destination_open_queued(struct vas_instance_id instance, size_t bytes,
				struct vas_destination **dest)
{
	return destination_open(instance, -1, queue_size(bytes), dest);
}

/*
 * Where the switchboard stamps the sending window into an entry. An entry that
 * has not been written reads 0xffffffff there, which is what the kernel leaves
 * every entry as and what a reader puts one back to.
 */
#define MESSAGE_UNUSED 0xffffffffu

static uint32_t *message_stamp(void *entry)
{
	return (uint32_t *)((char *)entry +
			    offsetof(struct nx_crb, stamp.nx.pswid));
}

const void *vas_destination_next(struct vas_destination *dest)
{
	void *entry;

	if (!dest || !dest->queue)
		return NULL;

	/*
	 * One entry looked at, not a search. The switchboard fills the ring in
	 * order, so the next message is always at the cursor and anywhere else
	 * is a slot this reader has already had. Searching the ring instead
	 * cost a read of every one of its lines each time a caller woke before
	 * the paste landed -- thirty-two kilobytes of cold misses to conclude
	 * nothing had arrived, which made a delivered message look several
	 * times slower than fetching it from the sender's memory.
	 *
	 * Acquire: the switchboard stamps the entry as part of the one store
	 * that writes it, so the stamp is not a flag published after the
	 * content and there is nothing to order on that side. The barrier is
	 * for this side. The stamp and the content are different addresses,
	 * and only accesses to the same address are ordered by themselves, so
	 * without it the caller's reads of the content may be satisfied from
	 * before the entry arrived.
	 */
	entry = (char *)dest->queue + (size_t)dest->cursor * VAS_MESSAGE_BYTES;
	if (__atomic_load_n(message_stamp(entry), __ATOMIC_ACQUIRE) ==
	    MESSAGE_UNUSED)
		return NULL;

	return entry;
}

void vas_destination_release(struct vas_destination *dest, const void *message)
{
	if (!dest || !dest->queue || !message)
		return;

	/*
	 * Release: everything the caller read from the entry must be done
	 * before the switchboard may write over it.
	 */
	__atomic_store_n(message_stamp((void *)message), MESSAGE_UNUSED,
			 __ATOMIC_RELEASE);

	/* Giving one back is what moves this reader on to the next. */
	dest->cursor = (dest->cursor + 1) % dest->slots;
}

void vas_destination_advance(struct vas_destination *dest)
{
	if (!dest || !dest->queue)
		return;

	dest->cursor = (dest->cursor + 1) % dest->slots;
}

int vas_destination_join(int join_fd, struct vas_destination **dest)
{
	if (join_fd < 0)
		return -EINVAL;

	/*
	 * The instance is not the caller's to choose: a joined destination
	 * must sit where the one it joins sits, and the kernel takes it from
	 * the descriptor.
	 */
	return destination_open(vas_instance_any(), join_fd, 0, dest);
}

int vas_destination_join_queue(struct vas_destination *owner,
			       struct vas_destination **dest)
{
	struct vas_destination *joined;
	int rc;

	if (!owner || !owner->queue || !dest)
		return -EINVAL;

	rc = vas_destination_join(owner->fd, &joined);
	if (rc)
		return rc;

	/*
	 * The queue, not a queue of its own: a paste writes one entry, into
	 * the window that owns the identity, and every thread sharing that
	 * identity is woken to read that one entry. Only the cursor is this
	 * thread's, so each keeps its own place in the ring.
	 *
	 * Started where the owner is rather than at nothing, so a thread that
	 * joins a destination already in use looks where the switchboard is
	 * about to write and not at entries the group has finished with.
	 */
	joined->queue = owner->queue;
	joined->queue_bytes = owner->queue_bytes;
	joined->slots = owner->slots;
	joined->cursor = owner->cursor;
	joined->borrowed_queue = true;

	*dest = joined;
	return 0;
}

static int destination_open(struct vas_instance_id instance, int join_fd,
			    size_t queue_bytes, struct vas_destination **dest)
{
	struct vas_rx_win_open_attr uattr;
	struct vas_destination *d;
	const char *device;
	int rc;

	if (!dest)
		return -EINVAL;

	*dest = NULL;

	device = vas_cop_device(VAS_COP_FTW, VAS_NODE_PLATFORM);
	if (!device)
		return -ENODEV;

	d = calloc(1, sizeof(*d));
	if (!d)
		return -ENOMEM;

	d->fd = open(device, O_RDWR);
	if (d->fd < 0) {
		rc = -errno;
		free(d);
		return rc;
	}

	memset(&uattr, 0, sizeof(uattr));
	uattr.version = VAS_TX_WIN_OPEN_V2;
	uattr.vas_id = (int16_t)instance.value;
	if (join_fd >= 0) {
		uattr.flags = VAS_RX_WIN_FLAG_JOIN;
		uattr.join_fd = join_fd;
	}
	if (queue_bytes) {
		/*
		 * Always an explicit size, so the kernel allocates exactly what
		 * will be mapped. Leaving it to the kernel's default meant the
		 * two disagreed and most of the queue was never looked at.
		 */
		uattr.flags |= VAS_RX_WIN_FLAG_FIFO;
		uattr.fifo_size = (uint32_t)queue_bytes;
	}

	if (ioctl(d->fd, VAS_RX_WIN_OPEN, (unsigned long)&uattr) < 0) {
		rc = -errno;
		close(d->fd);
		free(d);
		return rc;
	}

	if (queue_bytes) {
		size_t want = queue_bytes;

		d->queue = mmap(NULL, want, PROT_READ | PROT_WRITE, MAP_SHARED,
				d->fd, VAS_RX_FIFO_OFFSET);
		if (d->queue == MAP_FAILED) {
			rc = -errno;
			d->queue = NULL;
			close(d->fd);
			free(d);
			return rc;
		}
		d->queue_bytes = want;
		d->slots = (unsigned int)(want / VAS_MESSAGE_BYTES);
	}

	*dest = d;

	return 0;
}

void vas_destination_close(struct vas_destination **dest)
{
	if (!dest || !*dest)
		return;

	/*
	 * Unmapped before the descriptor closes. The mapping holds the window
	 * open by itself -- a VMA keeps a reference to the file it came from
	 * -- so the other order would leave the queue mapped over memory
	 * nothing owns any more.
	 */
	if ((*dest)->queue && !(*dest)->borrowed_queue)
		munmap((*dest)->queue, (*dest)->queue_bytes);
	close((*dest)->fd);
	free(*dest);
	*dest = NULL;
}

int vas_destination_fd(const struct vas_destination *dest)
{
	return dest ? dest->fd : -1;
}

int vas_wake(struct vas_window *window)
{
	/*
	 * Discarded by the receive window, which has FIFO writes disabled,
	 * but copy still needs a 128-byte aligned block to load.
	 */
	static _Alignas(128) char block[128];
	bool taken;
	void *target;

	if (!window)
		return -EINVAL;

	/*
	 * Loaded before the copy, not between it and the paste. Power ISA
	 * 3.0B section 4.4: "It is always best to avoid unnecessary
	 * instructions between the copy and the paste." The copy buffer is
	 * state the architecture may discard at any interruption, and every
	 * instruction in between is another chance to take one.
	 */
	target = window->paste_target;

	/*
	 * Any store the woken thread is to see must be visible before the
	 * transfer. Section 1.7.2 requires hwsync for this and nothing
	 * weaker: between a storage access and a data transfer, "the
	 * sequential execution model and coherence-required ordering
	 * relationships do not apply", so a release store does not order
	 * itself against what follows here.
	 */
	vas_barrier();
	taken = vas_copy_paste_block(block, target);
	vas_barrier();

	return taken ? 0 : -EAGAIN;
}

int vas_send(struct vas_window *window, const void *block)
{
	bool taken;
	void *target;

	if (!window || !block)
		return -EINVAL;

	/* copy takes a 128-byte aligned block and refuses anything else. */
	if ((uintptr_t)block % VAS_MESSAGE_BYTES)
		return -EINVAL;

	/* Loaded before the copy; see vas_wake() for why nothing sits between. */
	target = window->paste_target;

	vas_barrier();
	taken = vas_copy_paste_block(block, target);
	vas_barrier();

	return taken ? 0 : -EAGAIN;
}

enum vas_isa vas_isa_built_with(void)
{
	return (enum vas_isa)VAS_ISA;
}

const char *vas_isa_name(enum vas_isa isa)
{
	switch (isa) {
	case VAS_ISA_BY_VALUE:
		return "direct";
	case VAS_ISA_BY_BUILTIN:
		return "builtin";
	}

	return "unknown";
}

void vas_wait(void)
{
	vas_wait_for_notify();
}

void vas_destination_wait(const struct vas_destination *dest,
			  const volatile int *flag)
{
	(void)dest;

	if (!flag)
		return;

	/*
	 * Acquired, not merely re-read: the sender stores whatever the woken
	 * thread is to look at before it stores the flag, and this is the
	 * load that has to see those stores once it sees the flag. volatile
	 * would repeat the load without ordering anything after it.
	 */
	while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE))
		vas_wait_for_notify();
}

void vas_window_close(struct vas_window **window)
{
	struct vas_window *win;

	if (!window || !*window)
		return;

	win = *window;
	vas_trace_window_close(win->cop, 0);

	if (win->paste_map)
		munmap(win->paste_map, win->map_len);
	if (win->fd >= 0)
		close(win->fd);

	free(win);
	*window = NULL;
}

enum vas_cop vas_window_cop(const struct vas_window *window)
{
	return window->cop;
}

void *vas_window_paste_target(const struct vas_window *window)
{
	return window ? window->paste_target : NULL;
}

static int window_domain(struct vas_window *window, struct vas_region region,
			 unsigned long request)
{
	struct vas_win_domain domain;

	if (!window)
		return -EINVAL;

	memset(&domain, 0, sizeof(domain));
	domain.start = (uint64_t)(uintptr_t)region.start;
	domain.len = region.len;

	if (ioctl(window->fd, request, (unsigned long)&domain) < 0)
		return -errno;

	return 0;
}

int vas_window_domain_add(struct vas_window *window, struct vas_region region)
{
	return window_domain(window, region, VAS_WIN_DOMAIN_ADD);
}

int vas_window_domain_drop(struct vas_window *window, struct vas_region region)
{
	return window_domain(window, region, VAS_WIN_DOMAIN_DROP);
}

/* Read one decimal attribute of an engine's sysfs class directory. */
static int read_class_u64(const char *dir, const char *name, uint64_t *value)
{
	char path[512];
	FILE *file;
	int rc;

	if (snprintf(path, sizeof(path), "%s/%s", dir, name) >= (int)sizeof(path))
		return -ENAMETOOLONG;

	file = fopen(path, "re");
	if (!file)
		return -errno;

	rc = (fscanf(file, "%" SCNu64, value) == 1) ? 0 : -EIO;
	fclose(file);

	return rc;
}

int vas_engine_info(enum vas_cop cop, enum vas_node node,
		    struct vas_engine_info *info)
{
	const char *dir = vas_cop_class(cop, node);
	uint64_t value;
	int rc;

	if (!info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));
	info->cop = cop;
	info->node = node;

	if (!dir)
		return -ENODEV;

	rc = read_class_u64(dir, "cop_type", &value);
	if (rc)
		return (rc == -ENOENT) ? 0 : rc;

	/*
	 * A class whose cop_type does not name the type it was looked up
	 * under is not the engine it claims to be.
	 */
	if (value != (uint64_t)cop)
		return -EPROTO;

	info->present = true;

	rc = read_class_u64(dir, "req_max_processed_len", &value);
	if (rc)
		return rc;

	info->req_max_processed_len = value;

	return 0;
}
