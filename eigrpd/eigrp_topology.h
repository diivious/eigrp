// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Topology Table.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
 *
  */

#ifndef _ZEBRA_EIGRP_TOPOLOGY_H
#define _ZEBRA_EIGRP_TOPOLOGY_H

#include <string.h>

#include "eigrpd/eigrp_cli.h"
#include "eigrpd/eigrp_mgnt.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_rib.h"
#include "eigrpd/eigrp.h"

/* EIGRP Route Descriptor related functions. */
extern eigrp_route_descriptor_t *eigrp_topology_route_create(eigrp_interface_t *);
extern void eigrp_route_descriptor_add(eigrp_instance_t *,
				       eigrp_prefix_descriptor_t *,
				       eigrp_route_descriptor_t *);
extern void eigrp_route_descriptor_delete(eigrp_instance_t *,
					  eigrp_prefix_descriptor_t *,
					  eigrp_route_descriptor_t *);
void eigrp_topology_route_free(eigrp_route_descriptor_t *);

/* EIGRP Topology table storage related functions. */
extern eigrp_table_t *eigrp_topology_table_create(void);
extern void eigrp_topology_init(eigrp_table_t *table);

extern eigrp_prefix_descriptor_t *eigrp_topology_prefix_create(void);
extern void eigrp_topology_prefix_free(eigrp_prefix_descriptor_t *);

extern void eigrp_topology_table_delete(eigrp_instance_t *eigrp,
					eigrp_table_t *table);
extern void eigrp_prefix_descriptor_add(eigrp_table_t *table,
					eigrp_prefix_descriptor_t *pe);
extern void eigrp_prefix_descriptor_delete(eigrp_instance_t *eigrp,
					   eigrp_table_t *table,
					   eigrp_prefix_descriptor_t *pe);
extern void eigrp_topology_delete_all(eigrp_instance_t *eigrp,
				      eigrp_table_t *table);
extern eigrp_prefix_descriptor_t *
eigrp_topology_table_lookup(eigrp_table_t *table,
			    const eigrp_prefix_t *prefix);
extern eigrp_list_t *eigrp_topology_get_successor(eigrp_prefix_descriptor_t *pe);
extern eigrp_list_t *
eigrp_topology_get_successor_max(eigrp_prefix_descriptor_t *pe,
				 unsigned int maxpaths);
extern eigrp_route_descriptor_t *eigrp_prefix_descriptor_lookup(
    eigrp_prefix_descriptor_t *prefix, eigrp_neighbor_t *neigh);
extern eigrp_route_descriptor_t *
eigrp_topology_route_head(eigrp_prefix_descriptor_t *prefix);
extern eigrp_route_descriptor_t *
eigrp_topology_route_select(eigrp_prefix_descriptor_t *prefix);
extern eigrp_list_t *eigrp_neighbor_prefixes_lookup(eigrp_instance_t *eigrp,
						   eigrp_neighbor_t *n);
extern void eigrp_topology_update_all_node_flags(eigrp_instance_t *eigrp);
extern void eigrp_topology_update_node_flags(eigrp_instance_t *eigrp,
					     eigrp_prefix_descriptor_t *pe);
extern enum metric_change eigrp_topology_update_distance(eigrp_fsm_action_message_t *msg);
extern void eigrp_update_routing_table(eigrp_instance_t *eigrp,
				       eigrp_prefix_descriptor_t *pe);
extern void eigrp_topology_neighbor_down(eigrp_instance_t *eigrp,
					 eigrp_neighbor_t *neigh);
extern void eigrp_update_topology_table_prefix(eigrp_instance_t *eigrp,
					       eigrp_table_t *table,
					       eigrp_prefix_descriptor_t *pe);

/* Locally redistributed host-RIB candidates become EIGRP external paths. */
eigrp_result_t eigrp_topology_redistributed_route_update(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route,
	const eigrp_metrics_t *metric);
eigrp_result_t eigrp_topology_redistributed_route_remove(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route);

eigrp_result_t eigrp_topology_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const eigrp_prefix_t *destination, bool all_links,
	eigrp_topology_prefix_state_cb prefix_callback,
	eigrp_topology_route_state_cb route_callback, void *arg);
eigrp_result_t eigrp_topology_instance_walk(
	eigrp_address_family_t afi, eigrp_vrf_id_t vrf_id, uint16_t asn,
	eigrp_topology_instance_walk_cb callback, void *arg);
eigrp_result_t eigrp_topology_clear(
	eigrp_instance_context_t *context,
	const eigrp_topology_clear_request_t *request, size_t *affected_count);

typedef struct eigrp_topology_route_iterator {
	eigrp_prefix_descriptor_t *prefix;
	unsigned int queue;
	eigrp_list_node_t *node;
} eigrp_topology_route_iterator_t;

eigrp_route_descriptor_t *eigrp_topology_route_iterator_first(
	eigrp_prefix_descriptor_t *prefix, eigrp_topology_route_iterator_t *iterator);
eigrp_route_descriptor_t *eigrp_topology_route_iterator_next(
	eigrp_topology_route_iterator_t *iterator);

/* Route storage is split by EIGRP route class.  Both queues are CD sorted. */
static inline eigrp_list_t *
eigrp_topology_route_queue(eigrp_prefix_descriptor_t *prefix, unsigned int index)
{
	if (!prefix || index > 1)
		return NULL;
	return index == 0 ? prefix->internal_routes : prefix->external_routes;
}

/* Static inline functions */
/* IPv4/IPv6 prefix and address management functions
 * might move to eigrp_addr.h if this grows
 */
static inline
void eigrp_addr_copy (eigrp_addr_t *dst, eigrp_addr_t *src)
{
    memcpy(dst, src, sizeof(eigrp_addr_t));
}

static inline
int eigrp_addr_same (eigrp_addr_t *dst, eigrp_addr_t *src)
{
    if (memcmp(dst, src, sizeof(eigrp_addr_t)) == 0)
	return TRUE;
    return FALSE;
}

eigrp_result_t eigrp_topology_create(eigrp_instance_context_t *context);
eigrp_result_t eigrp_topology_delete(eigrp_instance_context_t *context);
eigrp_result_t eigrp_topology_default_information_set(
	eigrp_instance_context_t *context,
	eigrp_default_information_direction_t direction, const char *access_list);
eigrp_result_t eigrp_topology_default_information_reset(
	eigrp_instance_context_t *context,
	eigrp_default_information_direction_t direction, const char *access_list);
eigrp_result_t eigrp_topology_maximum_prefix_set(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_topology_maximum_prefix_reset(
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_topology_maximum_paths_set(
	eigrp_instance_context_t *context, uint8_t maximum_paths);
eigrp_result_t eigrp_topology_maximum_paths_reset(
	eigrp_instance_context_t *context);

#endif
