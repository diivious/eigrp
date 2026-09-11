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

#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_result.h"

typedef enum eigrp_default_information_direction {
	EIGRP_DEFAULT_INFORMATION_IN = 0,
	EIGRP_DEFAULT_INFORMATION_OUT,
} eigrp_default_information_direction_t;

typedef struct eigrp_topology_prefix_state {
	eigrp_prefix_t destination;
	bool active;
	uint32_t feasible_distance;
	uint32_t successor_count;
	uint64_t serial_number;
} eigrp_topology_prefix_state_t;

typedef struct eigrp_topology_route_state {
	eigrp_address_t next_hop;
	const char *interface_name;
	bool connected;
	bool successor;
	bool feasible_successor;
	uint32_t distance;
	uint32_t reported_distance;
} eigrp_topology_route_state_t;

typedef eigrp_result_t (*eigrp_topology_prefix_state_cb)(
	const eigrp_topology_prefix_state_t *state, void *arg);
typedef eigrp_result_t (*eigrp_topology_route_state_cb)(
	const eigrp_topology_route_state_t *state, void *arg);

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
extern struct route_table *eigrp_topology_table_create(void);
extern void eigrp_topology_init(struct route_table *table);

extern eigrp_prefix_descriptor_t *eigrp_topology_prefix_create(void);
extern void eigrp_topology_prefix_free(eigrp_prefix_descriptor_t *);

extern void eigrp_topology_table_delete(eigrp_instance_t *eigrp,
					struct route_table *table);
extern void eigrp_prefix_descriptor_add(struct route_table *table,
					eigrp_prefix_descriptor_t *pe);
extern void eigrp_prefix_descriptor_delete(eigrp_instance_t *eigrp,
					   struct route_table *table,
					   eigrp_prefix_descriptor_t *pe);
extern void eigrp_topology_delete_all(eigrp_instance_t *eigrp,
				      struct route_table *table);
extern eigrp_prefix_descriptor_t *
eigrp_topology_table_lookup_ipv4(struct route_table *table, struct prefix *p);
extern struct list *eigrp_topology_get_successor(eigrp_prefix_descriptor_t *pe);
extern struct list *
eigrp_topology_get_successor_max(eigrp_prefix_descriptor_t *pe,
				 unsigned int maxpaths);
extern eigrp_route_descriptor_t *eigrp_prefix_descriptor_lookup(
    struct list *entries, eigrp_neighbor_t *neigh);
extern struct list *eigrp_neighbor_prefixes_lookup(eigrp_instance_t *eigrp,
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
					       struct route_table *table,
					       eigrp_prefix_descriptor_t *pe);

eigrp_result_t eigrp_topology_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const eigrp_prefix_t *destination, bool all_links,
	eigrp_topology_prefix_state_cb prefix_callback,
	eigrp_topology_route_state_cb route_callback, void *arg);

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
eigrp_result_t eigrp_topology_default_information_update(
	eigrp_instance_context_t *context,
	eigrp_default_information_direction_t direction, bool enabled,
	const char *access_list);
eigrp_result_t eigrp_topology_maximum_prefix_update(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_topology_maximum_prefix_delete(
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_topology_maximum_paths_update(
	eigrp_instance_context_t *context, uint8_t maximum_paths);
eigrp_result_t eigrp_topology_maximum_paths_delete(
	eigrp_instance_context_t *context);

#endif
