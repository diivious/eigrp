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
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_errors.h"
#include "eigrpd/eigrp_zebra.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_ROUTE_DESCRIPTOR, "EIGRP Route Entry");
DEFINE_MTYPE(EIGRPD, EIGRP_PREFIX_DESCRIPTOR,       "EIGRP Prefix Entry");

static int eigrp_route_descriptor_cmp(eigrp_route_descriptor_t *,
				      eigrp_route_descriptor_t *);

/**
 * Various fuctions for handling eigrp route descriptors
 */

/*
 * Returns new topology route
 */
eigrp_route_descriptor_t *eigrp_topology_route_create(eigrp_interface_t *intf)
{
	eigrp_route_descriptor_t *new;

	new = XCALLOC(MTYPE_EIGRP_ROUTE_DESCRIPTOR,
		      sizeof(eigrp_route_descriptor_t));
	new->reported_distance = EIGRP_MAX_METRIC;
	new->distance = EIGRP_MAX_METRIC;
	new->ei = intf;

	return new;
}

/*
 * Adding topology entry to topology node
 */
void eigrp_route_descriptor_add(eigrp_instance_t *eigrp,
				eigrp_prefix_descriptor_t *node,
				eigrp_route_descriptor_t *route)
{
	struct list *l = list_new();

	listnode_add(l, route);

	if (listnode_lookup(node->entries, route) == NULL) {
		listnode_add_sort(node->entries, route);
		route->prefix = node;

		eigrp_zebra_route_add(eigrp, node->destination, l,
				      node->fdistance);
	}

	list_delete(&l);
}


void eigrp_topology_prefix_free(eigrp_prefix_descriptor_t *pe)
{
	eigrp_route_descriptor_t *route;
	struct listnode *node, *nnode;

	if (!pe)
		return;

	if (pe->entries) {
		for (ALL_LIST_ELEMENTS(pe->entries, node, nnode, route))
			eigrp_topology_route_free(route);
		list_delete(&pe->entries);
	}

	if (pe->rij)
		list_delete(&pe->rij);

	if (pe->destination)
		prefix_free(&pe->destination);

	XFREE(MTYPE_EIGRP_PREFIX_DESCRIPTOR, pe);
}

/*
 * Topology entry comparison
 */
static int eigrp_route_descriptor_cmp(eigrp_route_descriptor_t *route1,
				      eigrp_route_descriptor_t *route2)
{
	if (route1->distance < route2->distance)
		return -1;
	if (route1->distance > route2->distance)
		return 1;

	return 0;
}

/*
 * Frees topology route
 */
void eigrp_topology_route_free(eigrp_route_descriptor_t *route)
{
	XFREE(MTYPE_EIGRP_ROUTE_DESCRIPTOR, route);
}

/**
 * Various fuctions for handling eigrp prefix descriptors
 */

/*
 * Returns new created toplogy node
 * cmp - assigned function for comparing topology entry
 */
eigrp_prefix_descriptor_t *eigrp_topology_prefix_create(void)
{
	eigrp_prefix_descriptor_t *new;
	new = XCALLOC(MTYPE_EIGRP_PREFIX_DESCRIPTOR,
		      sizeof(eigrp_prefix_descriptor_t));
	new->entries = list_new();
	new->rij = list_new();
	new->entries->cmp = (int (*)(void *, void *))eigrp_route_descriptor_cmp;
	new->distance = new->fdistance = new->rdistance = EIGRP_MAX_METRIC;
	new->destination = NULL;

	return new;
}

/*
 * Adding topology node to topology table
 */
void eigrp_prefix_descriptor_add(struct route_table *topology,
				 eigrp_prefix_descriptor_t *pe)
{
	struct route_node *rn;

	rn = route_node_get(topology, pe->destination);
	if (rn->info) {
		if (IS_DEBUG_EIGRP_EVENT) {
			zlog_debug("%s: %s Should we have found this prefix in the topo table?",
				   __func__,
				   eigrp_print_prefix(pe->destination));
		}
		route_unlock_node(rn);
	}

	rn->info = pe;
}

/*
 * Find topology node in topology table
 */
eigrp_route_descriptor_t *eigrp_prefix_descriptor_lookup(struct list *entries,
							 eigrp_neighbor_t *nbr)
{
	eigrp_route_descriptor_t *data;
	struct listnode *node, *nnode;
	for (ALL_LIST_ELEMENTS(entries, node, nnode, data)) {
		if (data->adv_router == nbr) {
			return data;
		}
	}

	return NULL;
}

/*
 * Deleting topology node from topology table
 */
void eigrp_prefix_descriptor_delete(eigrp_instance_t *eigrp,
				    struct route_table *table,
				    eigrp_prefix_descriptor_t *pe)
{
	eigrp_route_descriptor_t *ne;
	struct listnode *node, *nnode;
	struct route_node *rn;

	if (!eigrp)
		return;

	rn = route_node_lookup(table, pe->destination);
	if (!rn)
		return;

	/*
	 * Emergency removal of the node from this list.
	 * Whatever it is.
	 */
	listnode_delete(eigrp->topology_changes, pe);

	for (ALL_LIST_ELEMENTS(pe->entries, node, nnode, ne))
		eigrp_route_descriptor_delete(eigrp, pe, ne);
	list_delete(&pe->entries);
	list_delete(&pe->rij);
	eigrp_zebra_route_delete(eigrp, pe->destination);
	prefix_free(&pe->destination);

	rn->info = NULL;
	route_unlock_node(rn); // Lookup above
	route_unlock_node(rn); // Initial creation
	XFREE(MTYPE_EIGRP_PREFIX_DESCRIPTOR, pe);
}

/*
 * Deleting topology entry from topology node
 */
void eigrp_route_descriptor_delete(eigrp_instance_t *eigrp,
				   eigrp_prefix_descriptor_t *node,
				   eigrp_route_descriptor_t *route)
{
	if (listnode_lookup(node->entries, route) != NULL) {
		listnode_delete(node->entries, route);
		eigrp_zebra_route_delete(eigrp, node->destination);
		XFREE(MTYPE_EIGRP_ROUTE_DESCRIPTOR, route);
	}
}

/*
 * Returns linkedlist used as topology table
 * cmp - assigned function for comparing topology nodes
 * del - assigned function executed before deleting topology node by list
 * function
 */
struct route_table *eigrp_topology_table_create(void)
{
	return route_table_init();
}

/*
 * Deleting all nodes from topology table
 */
void eigrp_topology_delete_all(eigrp_instance_t *eigrp,
			       struct route_table *topology)
{
	struct route_node *rn;
	eigrp_prefix_descriptor_t *pe;

	for (rn = route_top(topology); rn; rn = route_next(rn)) {
		pe = rn->info;

		if (!pe)
			continue;

		eigrp_prefix_descriptor_delete(eigrp, topology, pe);
	}
}

/*
 * Freeing topology table list
 */
void eigrp_topology_table_delete(eigrp_instance_t *eigrp,
				 struct route_table *table)
{
	if (!table)
		return;
	eigrp_topology_delete_all(eigrp, table);
	route_table_finish(table);
}

eigrp_prefix_descriptor_t *
eigrp_topology_table_lookup_ipv4(struct route_table *table,
				 struct prefix *address)
{
	eigrp_prefix_descriptor_t *pe;
	struct route_node *rn;

	rn = route_node_lookup(table, address);
	if (!rn)
		return NULL;

	pe = rn->info;

	route_unlock_node(rn);

	return pe;
}

/*
 * For a future optimization, put the successor list into it's
 * own separate list from the full list?
 *
 * That way we can clean up all the list_new and list_delete's
 * that we are doing.  DBS
 */
struct list *eigrp_topology_get_successor(eigrp_prefix_descriptor_t *table_node)
{
	struct list *successors = list_new();
	eigrp_route_descriptor_t *data;
	struct listnode *node1, *node2;

	for (ALL_LIST_ELEMENTS(table_node->entries, node1, node2, data)) {
		if (data->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG) {
			listnode_add(successors, data);
		}
	}

	/*
	 * If we have no successors return NULL
	 */
	if (!successors->count) {
		list_delete(&successors);
		successors = NULL;
	}

	return successors;
}

struct list *
eigrp_topology_get_successor_max(eigrp_prefix_descriptor_t *table_node,
				 unsigned int maxpaths)
{
	struct list *successors = eigrp_topology_get_successor(table_node);

	if (successors && successors->count > maxpaths) {
		do {
			struct listnode *node = listtail(successors);

			list_delete_node(successors, node);

		} while (successors->count > maxpaths);
	}

	return successors;
}

/* Lookup all prefixes from specified neighbor */
struct list *eigrp_neighbor_prefixes_lookup(eigrp_instance_t *eigrp,
					    eigrp_neighbor_t *nbr)
{
	struct listnode *node2, *node22;
	eigrp_route_descriptor_t *route;
	eigrp_prefix_descriptor_t *pe;
	struct route_node *rn;

	/* create new empty list for prefixes storage */
	struct list *prefixes = list_new();

	/* iterate over all prefixes in topology table */
	for (rn = route_top(eigrp->topology_table); rn; rn = route_next(rn)) {
		if (!rn->info)
			continue;
		pe = rn->info;
		/* iterate over all neighbor route in prefix */
		for (ALL_LIST_ELEMENTS(pe->entries, node2, node22, route)) {
			/* if route is from specified neighbor, add to list */
			if (route->adv_router == nbr) {
				listnode_add(prefixes, pe);
			}
		}
	}

	/* return list of prefixes from specified neighbor */
	return prefixes;
}

enum metric_change
eigrp_topology_update_distance(eigrp_fsm_action_message_t *msg)
{
	eigrp_instance_t *eigrp = msg->eigrp;
	eigrp_prefix_descriptor_t *prefix = msg->prefix;
	eigrp_route_descriptor_t *route = msg->route;
	enum metric_change change = METRIC_SAME;
	uint32_t new_reported_distance;

	assert(route);

	if (!route->adv_router)
		route->adv_router = msg->adv_router;
	if (!route->prefix)
		route->prefix = prefix;
	if (!route->ei && msg->adv_router)
		route->ei = msg->adv_router->ei;

	switch (msg->data_type) {
	case EIGRP_CONNECTED:
		if (prefix->nt == EIGRP_TOPOLOGY_TYPE_CONNECTED)
			return change;

		change = METRIC_DECREASE;
		break;
	case EIGRP_INT:
		if (eigrp_metrics_is_same(msg->metrics,
					  route->reported_metric)) {
			return change; // No change
		}

		new_reported_distance =
			eigrp_calculate_metrics(eigrp, msg->metrics);

		if (route->reported_distance < new_reported_distance)
			change = METRIC_INCREASE;
		else
			change = METRIC_DECREASE;

		route->metric = msg->metrics;
		route->reported_metric = msg->metrics;
		route->reported_distance = new_reported_distance;
		route->distance = eigrp_calculate_total_metrics(eigrp, route);
		break;
	case EIGRP_EXT:
		if (prefix->nt == EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL
		    && eigrp_metrics_is_same(msg->metrics,
					     route->reported_metric))
			return change;

		new_reported_distance =
			eigrp_calculate_metrics(eigrp, msg->metrics);

		if (route->reported_distance < new_reported_distance)
			change = METRIC_INCREASE;
		else
			change = METRIC_DECREASE;

		route->metric = msg->metrics;
		route->reported_metric = msg->metrics;
		route->reported_distance = new_reported_distance;
		route->distance = eigrp_calculate_total_metrics(eigrp, route);
		break;
	default:
		flog_err(EC_LIB_DEVELOPMENT, "%s: Please implement handler",
			 __func__);
		break;
	}

	/*
	 * Move to correct position in list according to new distance
	 */
	listnode_delete(prefix->entries, route);
	listnode_add_sort(prefix->entries, route);

	return change;
}

void eigrp_topology_update_all_node_flags(eigrp_instance_t *eigrp)
{
	eigrp_prefix_descriptor_t *pe;
	struct route_node *rn;

	if (!eigrp)
		return;

	for (rn = route_top(eigrp->topology_table); rn; rn = route_next(rn)) {
		pe = rn->info;

		if (!pe)
			continue;

		eigrp_topology_update_node_flags(eigrp, pe);
	}
}

void eigrp_topology_update_node_flags(eigrp_instance_t *eigrp,
				      eigrp_prefix_descriptor_t *dest)
{
	struct listnode *node;
	eigrp_route_descriptor_t *route;

	for (ALL_LIST_ELEMENTS_RO(dest->entries, node, route)) {
		if (route->reported_distance < dest->fdistance) {
			// is feasible successor, can be successor
			if (((uint64_t)route->distance
			     <= (uint64_t)dest->distance
					* (uint64_t)eigrp->variance)
			    && route->distance != EIGRP_MAX_METRIC) {
				// is successor
				route->flags |=
					EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
				route->flags &=
					~EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG;
			} else {
				// is feasible successor only
				route->flags |=
					EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG;
				route->flags &=
					~EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
			}
		} else {
			route->flags &= ~EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG;
			route->flags &= ~EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
		}
	}
}

void eigrp_update_routing_table(eigrp_instance_t *eigrp,
				eigrp_prefix_descriptor_t *prefix)
{
	struct list *successors;
	struct listnode *node;
	eigrp_route_descriptor_t *route;

	successors = eigrp_topology_get_successor_max(prefix, eigrp->max_paths);

	if (successors) {
		eigrp_zebra_route_add(eigrp, prefix->destination, successors,
				      prefix->fdistance);
		for (ALL_LIST_ELEMENTS_RO(successors, node, route))
			route->flags |= EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG;

		list_delete(&successors);
	} else {
		eigrp_zebra_route_delete(eigrp, prefix->destination);
		for (ALL_LIST_ELEMENTS_RO(prefix->entries, node, route))
			route->flags &= ~EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG;
	}
}

void eigrp_topology_neighbor_down(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr)
{
	eigrp_prefix_descriptor_t *pe;
	eigrp_route_descriptor_t *route;
	struct listnode *node2, *node22;
	struct route_node *rn;

	for (rn = route_top(eigrp->topology_table); rn; rn = route_next(rn)) {
		pe = rn->info;

		if (!pe)
			continue;

		for (ALL_LIST_ELEMENTS(pe->entries, node2, node22, route)) {
			eigrp_fsm_action_message_t msg;

			if (route->adv_router != nbr)
				continue;

			msg.metrics.delay = EIGRP_MAX_METRIC;
			msg.packet_type = EIGRP_OPC_UPDATE;
			msg.eigrp = eigrp;
			msg.data_type = EIGRP_INT;
			msg.adv_router = nbr;
			msg.route = route;
			msg.prefix = pe;
			eigrp_fsm_event(&msg);
		}
	}

	eigrp_query_send_all(eigrp);
	eigrp_update_send_all(eigrp, nbr->ei);
}

void eigrp_update_topology_table_prefix(eigrp_instance_t *eigrp,
					struct route_table *table,
					eigrp_prefix_descriptor_t *prefix)
{
	struct listnode *node1, *node2;

	eigrp_route_descriptor_t *route;
	for (ALL_LIST_ELEMENTS(prefix->entries, node1, node2, route)) {
		if (route->distance == EIGRP_MAX_METRIC) {
			eigrp_route_descriptor_delete(eigrp, prefix, route);
		}
	}
	if (prefix->distance == EIGRP_MAX_METRIC
	    && prefix->nt != EIGRP_TOPOLOGY_TYPE_CONNECTED) {
		eigrp_prefix_descriptor_delete(eigrp, table, prefix);
	}
}

static eigrp_result_t
eigrp_topology_context_validate(const eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	if (context->topology_id != EIGRP_TOPOLOGY_ID_BASE)
		return EIGRP_RESULT_NOT_IMPLEMENTED;

	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_topology_create(eigrp_instance_context_t *context)
{
	eigrp_result_t result;

	/*
	 * Step 1 implements only TID 0.  Keep the public lifecycle generic so a
	 * future non-zero TID does not require a second portable API family.
	 * Runtime support for additional topologies is not part of this cleanup.
	 */
	result = eigrp_topology_context_validate(context);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_topology_delete(eigrp_instance_context_t *context)
{
	eigrp_result_t result;

	result = eigrp_topology_context_validate(context);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_topology_default_information_update(
	eigrp_instance_context_t *context,
	eigrp_default_information_direction_t direction, bool enabled,
	const char *access_list)
{
	(void)enabled;
	if (direction != EIGRP_DEFAULT_INFORMATION_IN
	    && direction != EIGRP_DEFAULT_INFORMATION_OUT)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (access_list && !access_list[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_topology_maximum_prefix_update(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit)
{
	if (!limit || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_topology_maximum_paths_update(
	eigrp_instance_context_t *context, uint8_t maximum_paths)
{
	if (!maximum_paths || maximum_paths > 32)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime)
		context->runtime->max_paths = maximum_paths;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_topology_maximum_paths_delete(
	eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime)
		context->runtime->max_paths = EIGRP_MAX_PATHS_DEFAULT;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_topology_maximum_prefix_delete(
	eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
