// SPDX-License-Identifier: GPL-2.0-or-later
/* Public opaque interface lookup target for host configuration adapters.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <string.h>
#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_list.h"

/* Resolve only runtime-owned objects. Configuration objects are read through
 * their own public API and do not use a Linux or OpenWrt interface identity. */
eigrp_intf_t *eigrp_intf_runtime_lookup(eigrp_instance_t *runtime,
	const char *interface_name)
{
	eigrp_intf_t *interface;
	eigrp_list_item_t *node;

	if (!runtime || !interface_name || !interface_name[0] || !runtime->eiflist)
		return NULL;
	for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, node, interface)) {
		if (interface->name && strcmp(interface->name, interface_name) == 0)
			return interface;
	}
	return NULL;
}
