// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What accelerators this machine offers, and whether this process may use
 * them. Needs no window, so it is the first thing to run when a request is
 * failing and it is not yet clear whether the engine is even there.
 *
 *     discover
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <vas/vas.h>

struct node_ref {
	enum vas_cop cop;
	enum vas_node node;
};

static const struct node_ref nodes[] = {
	{ VAS_COP_842, VAS_NODE_PLATFORM },
	{ VAS_COP_GZIP, VAS_NODE_PLATFORM },
	{ VAS_COP_GZIP, VAS_NODE_LEGACY },
	{ VAS_COP_SYM, VAS_NODE_PLATFORM },
};

static void report(enum vas_cop cop, enum vas_node node)
{
	struct vas_engine_info info;
	const char *device;
	int rc;

	rc = vas_engine_info(cop, node, &info);
	if (rc) {
		printf("%-22s  unreadable: %s\n", vas_cop_name(cop, node), strerror(-rc));
		return;
	}

	if (!info.present) {
		printf("%-22s  absent\n", vas_cop_name(cop, node));
		return;
	}

	device = vas_cop_device(cop, node);

	printf("%-22s  present", vas_cop_name(cop, node));
	if (info.req_max_processed_len)
		printf(", up to %llu bytes a request",
		       (unsigned long long)info.req_max_processed_len);
	else
		printf(", no request length limit");

	if (access(device, R_OK | W_OK) == 0)
		printf(", %s is usable\n", device);
	else
		printf(", %s is not usable by this process (%s)\n", device,
		       strerror(errno));
}

int main(void)
{
	size_t i;

	printf("page size %zu bytes\n\n", vas_page_size());

	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
		report(nodes[i].cop, nodes[i].node);

	return 0;
}
