// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Portable EIGRP host-RIB lifecycle helpers.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#include "eigrpd.h"
#include "eigrp_structs.h"
#include "eigrp_rib.h"
#include "eigrp_topology.h"
#include "eigrp_table.h"
#include "eigrp_list.h"

static bool eigrp_rib_prefix_installed(const eigrp_prefix_descriptor_t *prefix)
{
	eigrp_route_descriptor_t *route;
	eigrp_list_item_t *node;

	if (!prefix)
		return false;
	for (EIGRP_LIST_ITERATE_RO(prefix->internal_routes, node, route))
		if (route->flags & EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG)
			return true;
	for (EIGRP_LIST_ITERATE_RO(prefix->external_routes, node, route))
		if (route->flags & EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG)
			return true;
	return false;
}

eigrp_result_t eigrp_rib_routes_replay_instance(eigrp_instance_t *eigrp)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_table_node_t *rn;

	if (!eigrp)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp->topology_table)
		return EIGRP_RESULT_SUCCESS;

	for (rn = eigrp_table_first(eigrp->topology_table); rn;
	     rn = eigrp_table_next(rn)) {
		prefix = rn->info;
		if (eigrp_rib_prefix_installed(prefix))
			eigrp_update_routing_table(eigrp, prefix);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_routes_replay(void)
{
	eigrp_instance_t *eigrp;
	eigrp_list_item_t *node;

	if (!eigrp_om || !eigrp_om->eigrp)
		return EIGRP_RESULT_SUCCESS;

	for (EIGRP_LIST_ITERATE_RO(eigrp_om->eigrp, node, eigrp))
		(void)eigrp_rib_routes_replay_instance(eigrp);
	return EIGRP_RESULT_SUCCESS;
}
