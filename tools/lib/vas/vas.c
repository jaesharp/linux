// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Opening and bounding a VAS send window.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <asm/vas-api.h>

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
}

/*
 * Version 1 carries only the QoS credit flag; anything else the attributes
 * ask for needs version 2, which also requires the reserved fields to be
 * zero.
 */
static uint32_t open_version(const struct vas_window_attr *attr)
{
	if (attr->key_mask || attr->confined)
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
