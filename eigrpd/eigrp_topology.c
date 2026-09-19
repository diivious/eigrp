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

#include "table.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_packetizer.h"
#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_errors.h"
#include "eigrpd/eigrp_southbound.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_ROUTE_DESCRIPTOR, "EIGRP Route Entry");
DEFINE_MTYPE(EIGRPD, EIGRP_PREFIX_DESCRIPTOR,       "EIGRP Prefix Entry");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_PREFIX_SNAPSHOT, "EIGRP Prefix Snapshot");

static int eigrp_route_descriptor_cmp(eigrp_route_descriptor_t *,
				      eigrp_route_descriptor_t *);

static bool eigrp_topology_table_key(const eigrp_prefix_t *source,
				     struct prefix *destination)
{
	eigrp_prefix_t normalized;

	if (!source || !destination || !eigrp_prefix_valid(source))
		return false;

	normalized = *source;
	eigrp_prefix_normalize(&normalized);
	memset(destination, 0, sizeof(*destination));
	destination->prefixlen = normalized.prefix_length;
	if (normalized.address.afi == EIGRP_ADDRESS_FAMILY_IPV4) {
		destination->family = AF_INET;
		memcpy(&destination->u.prefix4, normalized.address.bytes,
		       sizeof(destination->u.prefix4));
	} else {
		destination->family = AF_INET6;
		memcpy(&destination->u.prefix6, normalized.address.bytes,
		       sizeof(destination->u.prefix6));
	}
	return true;
}

static bool eigrp_topology_southbound_nexthop(
	const eigrp_route_descriptor_t *route, eigrp_southbound_nexthop_t *nexthop)
{
	if (!route || !route->ei || !nexthop)
		return false;

	memset(nexthop, 0, sizeof(*nexthop));
	nexthop->ifindex = route->ei->ifindex;

	if (route->adv_router && route->adv_router->src.afi == AF_INET
	    && route->adv_router->src.ip.v4.s_addr != INADDR_ANY) {
		nexthop->gateway_present = true;
		nexthop->gateway.afi = EIGRP_ADDRESS_FAMILY_IPV4;
		memcpy(nexthop->gateway.bytes, &route->adv_router->src.ip.v4,
		       sizeof(route->adv_router->src.ip.v4));
	}

	return true;
}

static size_t eigrp_topology_southbound_nexthops(
	const struct list *routes, eigrp_southbound_nexthop_t *nexthops,
	size_t capacity)
{
	eigrp_route_descriptor_t *route;
	struct listnode *node;
	size_t count = 0;

	if (!routes || !nexthops || capacity == 0)
		return 0;

	for (ALL_LIST_ELEMENTS_RO(routes, node, route)) {
		if (count == capacity)
			break;
		if (!eigrp_topology_southbound_nexthop(route, &nexthops[count]))
			continue;
		count++;
	}

	return count;
}

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
	eigrp_southbound_nexthop_t nexthop;

	if (listnode_lookup(node->entries, route) == NULL) {
		listnode_add_sort(node->entries, route);
		route->prefix = node;

		if (eigrp_topology_southbound_nexthop(route, &nexthop))
			(void)eigrp_southbound_route_install(
				eigrp, &node->destination, &nexthop, 1,
				node->fdistance);
	}
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

	return new;
}

/*
 * Adding topology node to topology table
 */
void eigrp_prefix_descriptor_add(struct route_table *topology,
				 eigrp_prefix_descriptor_t *pe)
{
	char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";
	struct prefix key;
	struct route_node *rn;

	if (!topology || !pe
	    || !eigrp_topology_table_key(&pe->destination, &key))
		return;

	rn = route_node_get(topology, &key);
	if (rn->info) {
		if (IS_DEBUG_EIGRP_EVENT) {
			eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
					      &pe->destination);
			zlog_debug("%s: %s Should we have found this prefix in the topo table?",
				   __func__, prefix_buf);
		}
		route_unlock_node(rn);
	}

	rn->info = pe;
	if (IS_DEBUG_EIGRP_EVENT) {
		eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
				      &pe->destination);
		zlog_debug("EIGRP event: topology prefix add %s", prefix_buf);
		if (IS_DEBUG_EIGRP(0, DETAIL))
			zlog_debug("EIGRP event detail: prefix state %u distance %u fd %u rd %u",
				   pe->state, pe->distance, pe->fdistance,
				   pe->rdistance);
	}
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
	char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";
	eigrp_route_descriptor_t *ne;
	struct listnode *node, *nnode;
	struct prefix key;
	struct route_node *rn;

	if (!eigrp || !table || !pe
	    || !eigrp_topology_table_key(&pe->destination, &key))
		return;

	rn = route_node_lookup(table, &key);
	if (!rn)
		return;

	if (IS_DEBUG_EIGRP_EVENT) {
		eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
				      &pe->destination);
		zlog_debug("EIGRP event: topology prefix delete %s", prefix_buf);
		if (IS_DEBUG_EIGRP(0, DETAIL))
			zlog_debug("EIGRP event detail: AS %u prefix state %u distance %u fd %u rd %u",
				   eigrp->AS, pe->state, pe->distance,
				   pe->fdistance, pe->rdistance);
	}

	listnode_delete(eigrp->topology_changes, pe);

	for (ALL_LIST_ELEMENTS(pe->entries, node, nnode, ne))
		eigrp_route_descriptor_delete(eigrp, pe, ne);
	list_delete(&pe->entries);
	list_delete(&pe->rij);
	(void)eigrp_southbound_route_remove(eigrp, &pe->destination);

	rn->info = NULL;
	route_unlock_node(rn); /* lookup reference */
	route_unlock_node(rn); /* initial creation reference */
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
		(void)eigrp_southbound_route_remove(eigrp, &node->destination);
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
eigrp_topology_table_lookup(struct route_table *table,
			    const eigrp_prefix_t *address)
{
	eigrp_prefix_descriptor_t *pe;
	struct prefix key;
	struct route_node *rn;

	if (!table || !eigrp_topology_table_key(address, &key))
		return NULL;

	rn = route_node_lookup(table, &key);
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
		uint32_t old_flags = route->flags;

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

		if (IS_DEBUG_EIGRP(0, FAST_REROUTE)
		    && ((old_flags ^ route->flags)
			& (EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG
			   | EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG))) {
			char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";

			eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
					      &dest->destination);
			zlog_debug(
				"EIGRP FRR AS %u prefix %s via %s: successor %s feasible-successor %s RD %u FD %u distance %u",
				eigrp->AS, prefix_buf,
				route->adv_router
					? eigrp_print_addr(&route->adv_router->src)
					: "connected",
				CHECK_FLAG(route->flags,
					   EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)
					? "yes"
					: "no",
				CHECK_FLAG(route->flags,
					   EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG)
					? "yes"
					: "no",
				route->reported_distance, dest->fdistance,
				route->distance);
		}
	}
}

void eigrp_update_routing_table(eigrp_instance_t *eigrp,
				eigrp_prefix_descriptor_t *prefix)
{
	eigrp_southbound_nexthop_t nexthops[EIGRP_MAX_PATHS_MAX];
	struct list *successors;
	struct listnode *node;
	eigrp_route_descriptor_t *route;
	size_t nexthop_count;

	successors = eigrp_topology_get_successor_max(prefix, eigrp->max_paths);

	if (successors) {
		nexthop_count = eigrp_topology_southbound_nexthops(
			successors, nexthops, EIGRP_MAX_PATHS_MAX);
		if (nexthop_count != 0)
			(void)eigrp_southbound_route_install(
				eigrp, &prefix->destination, nexthops,
				nexthop_count, prefix->fdistance);
		for (ALL_LIST_ELEMENTS_RO(successors, node, route))
			route->flags |= EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG;

		list_delete(&successors);
	} else {
		(void)eigrp_southbound_route_remove(eigrp, &prefix->destination);
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

static struct list *eigrp_topology_connected_route_snapshot(
	eigrp_instance_t *eigrp, const eigrp_prefix_descriptor_t *prefix)
{
	struct list *routes;
	eigrp_route_descriptor_t *route;
	eigrp_route_descriptor_t *copy;
	struct listnode *node;

	routes = list_new();
	for (ALL_LIST_ELEMENTS_RO(prefix->entries, node, route)) {
		if (route->adv_router != eigrp->neighbor_self)
			continue;
		copy = eigrp_topology_route_create(route->ei);
		if (!copy)
			continue;
		*copy = *route;
		copy->prefix = NULL;
		listnode_add(routes, copy);
	}
	return routes;
}

static void eigrp_topology_route_snapshot_free(struct list *routes)
{
	eigrp_route_descriptor_t *route;
	struct listnode *node, *nnode;

	if (!routes)
		return;
	for (ALL_LIST_ELEMENTS(routes, node, nnode, route)) {
		listnode_delete(routes, route);
		eigrp_topology_route_free(route);
	}
	list_delete(&routes);
}


static eigrp_route_descriptor_t *eigrp_topology_query_route_snapshot(
	const eigrp_prefix_descriptor_t *prefix)
{
	eigrp_route_descriptor_t *route;
	eigrp_route_descriptor_t *copy;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(prefix->entries, node, route)) {
		if (route->adv_router == NULL)
			continue;
		copy = eigrp_topology_route_create(NULL);
		if (!copy)
			return NULL;
		*copy = *route;
		copy->prefix = NULL;
		copy->adv_router = NULL;
		copy->ei = NULL;
		return copy;
	}
	return NULL;
}

static bool eigrp_topology_prefix_detach(eigrp_instance_t *eigrp,
					  eigrp_prefix_descriptor_t *prefix)
{
	struct prefix key;
	struct route_node *rn;

	if (!eigrp_topology_table_key(&prefix->destination, &key))
		return false;
	rn = route_node_lookup(eigrp->topology_table, &key);
	if (!rn)
		return false;

	listnode_delete(eigrp->topology_changes, prefix);
	(void)eigrp_southbound_route_remove(eigrp, &prefix->destination);
	rn->info = NULL;
	route_unlock_node(rn); /* lookup reference */
	route_unlock_node(rn); /* initial creation reference */
	return true;
}

static eigrp_prefix_descriptor_t *eigrp_topology_prefix_recreate(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *destination, uint8_t type,
	const eigrp_metrics_t *reported_metric, uint8_t state)
{
	eigrp_prefix_descriptor_t *prefix;

	prefix = eigrp_topology_prefix_create();
	if (!prefix)
		return NULL;
	prefix->destination = *destination;
	eigrp_prefix_normalize(&prefix->destination);
	prefix->nt = type;
	prefix->state = state;
	prefix->serno = eigrp->serno;
	if (reported_metric)
		prefix->reported_metric = *reported_metric;
	eigrp_prefix_descriptor_add(eigrp->topology_table, prefix);
	return prefix;
}

static eigrp_result_t eigrp_topology_connected_recreate(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *destination, uint8_t type,
	const eigrp_metrics_t *reported_metric, uint32_t distance,
	uint32_t feasible_distance, uint32_t reported_distance,
	struct list *routes)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	struct listnode *node, *nnode;

	if (!routes || routes->count == 0)
		return EIGRP_RESULT_SUCCESS;

	prefix = eigrp_topology_prefix_recreate(
		eigrp, destination, type, reported_metric, EIGRP_FSM_STATE_PASSIVE);
	if (!prefix)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	prefix->distance = distance;
	prefix->fdistance = feasible_distance;
	prefix->rdistance = reported_distance;

	for (ALL_LIST_ELEMENTS(routes, node, nnode, route)) {
		listnode_delete(routes, route);
		route->prefix = prefix;
		eigrp_route_descriptor_add(eigrp, prefix, route);
	}
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_topology_remote_requery(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *destination, uint8_t type,
	const eigrp_metrics_t *reported_metric, eigrp_route_descriptor_t *query_route)
{
	eigrp_prefix_descriptor_t *prefix;

	if (eigrp_nbr_count_get(eigrp) == 0) {
		if (query_route)
			eigrp_topology_route_free(query_route);
		return EIGRP_RESULT_SUCCESS;
	}

	prefix = eigrp_topology_prefix_recreate(
		eigrp, destination, type, reported_metric, EIGRP_FSM_STATE_ACTIVE_1);
	if (!prefix) {
		if (query_route)
			eigrp_topology_route_free(query_route);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	prefix->distance = EIGRP_MAX_METRIC;
	prefix->fdistance = EIGRP_MAX_METRIC;
	prefix->rdistance = EIGRP_MAX_METRIC;

	/*
	 * The old NDB/RDB objects are detached from the topology table.  This
	 * route is not installed beneath the new NDB; it exists only to encode
	 * the locally-originated QUERY.  Replies repopulate fresh RDBs beneath
	 * the ACTIVE NDB.
	 */
	if (!query_route) {
		query_route = eigrp_topology_route_create(NULL);
		if (!query_route) {
			eigrp_prefix_descriptor_delete(eigrp, eigrp->topology_table,
						       prefix);
			return EIGRP_RESULT_INTERNAL_FAILURE;
		}
		query_route->type = type == EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL
					    ? EIGRP_TLV_IPv4_EXT
					    : EIGRP_TLV_IPv4_INT;
		query_route->metric = *reported_metric;
	}
	query_route->prefix = prefix;
	query_route->dest = *destination;
	query_route->adv_router = NULL;
	query_route->ei = NULL;
	query_route->metric.delay = EIGRP_MAX_METRIC;
	query_route->metric.flags = 0;
	eigrp_query_send_route(eigrp, prefix, query_route,
			       EIGRP_PACKETIZER_WORK_F_OWN_ROUTE);
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_topology_clear_prefix(
	eigrp_instance_t *eigrp, eigrp_prefix_descriptor_t *prefix)
{
	eigrp_prefix_t destination;
	struct list *connected_routes = NULL;
	eigrp_route_descriptor_t *query_route = NULL;
	eigrp_metrics_t reported_metric;
	uint32_t distance;
	uint32_t feasible_distance;
	uint32_t reported_distance;
	uint8_t type;
	eigrp_result_t result;

	if (!eigrp || !prefix || !eigrp_prefix_valid(&prefix->destination))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	destination = prefix->destination;
	type = prefix->nt;
	reported_metric = prefix->reported_metric;
	distance = prefix->distance;
	feasible_distance = prefix->fdistance;
	reported_distance = prefix->rdistance;
	if (type == EIGRP_TOPOLOGY_TYPE_CONNECTED)
		connected_routes = eigrp_topology_connected_route_snapshot(eigrp,
								       prefix);
	else
		query_route = eigrp_topology_query_route_snapshot(prefix);

	/*
	 * Remove the old NDB from the live topology and remove its RIB route.
	 * Packetizer work queued before the EXEC command may still reference the
	 * old NDB/RDBs, so their storage is released only by a tail work item.
	 */
	if (!eigrp_topology_prefix_detach(eigrp, prefix)) {
		eigrp_topology_route_snapshot_free(connected_routes);
		if (query_route)
			eigrp_topology_route_free(query_route);
		return EIGRP_RESULT_NOT_FOUND;
	}
	eigrp_packetizer_prefix_defer_free(eigrp, prefix);

	if (type == EIGRP_TOPOLOGY_TYPE_CONNECTED) {
		result = eigrp_topology_connected_recreate(
			eigrp, &destination, type, &reported_metric, distance,
			feasible_distance, reported_distance, connected_routes);
		eigrp_topology_route_snapshot_free(connected_routes);
		return result;
	}

	return eigrp_topology_remote_requery(eigrp, &destination, type,
					     &reported_metric, query_route);
}

static eigrp_result_t eigrp_topology_clear_all(eigrp_instance_t *eigrp,
					       size_t *affected_count)
{
	struct list *destinations;
	struct route_node *rn;
	eigrp_prefix_t *destination;
	struct listnode *node, *nnode;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_result_t result = EIGRP_RESULT_SUCCESS;
	size_t affected = 0;

	destinations = list_new();
	for (rn = route_top(eigrp->topology_table); rn; rn = route_next(rn)) {
		prefix = rn->info;
		if (!prefix)
			continue;
		destination = XCALLOC(MTYPE_EIGRP_PREFIX_SNAPSHOT,
				      sizeof(*destination));
		*destination = prefix->destination;
		listnode_add(destinations, destination);
	}

	for (ALL_LIST_ELEMENTS(destinations, node, nnode, destination)) {
		listnode_delete(destinations, destination);
		if (result == EIGRP_RESULT_SUCCESS) {
			prefix = eigrp_topology_table_lookup(eigrp->topology_table,
						       destination);
			if (prefix) {
				result = eigrp_topology_clear_prefix(eigrp, prefix);
				if (result == EIGRP_RESULT_SUCCESS)
					affected++;
			}
		}
		XFREE(MTYPE_EIGRP_PREFIX_SNAPSHOT, destination);
	}
	list_delete(&destinations);
	if (affected_count)
		*affected_count = affected;
	return result;
}

eigrp_result_t eigrp_topology_clear(
	eigrp_instance_context_t *context,
	const eigrp_topology_clear_request_t *request, size_t *affected_count)
{
	eigrp_prefix_t destination;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_result_t result;

	if (affected_count)
		*affected_count = 0;
	if (!context || !request)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (context->config
	    && context->config->afi == EIGRP_ADDRESS_FAMILY_IPV6)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	if (!context->runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config
	    && context->config->afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return EIGRP_RESULT_UNSUPPORTED;

	if (!request->destination)
		return eigrp_topology_clear_all(context->runtime, affected_count);

	if (request->destination->address.afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return request->destination->address.afi == EIGRP_ADDRESS_FAMILY_IPV6
			       ? EIGRP_RESULT_NOT_IMPLEMENTED
			       : EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_prefix_valid(request->destination))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	destination = *request->destination;
	eigrp_prefix_normalize(&destination);
	prefix = eigrp_topology_table_lookup(context->runtime->topology_table,
					     &destination);
	if (!prefix)
		return EIGRP_RESULT_NOT_FOUND;

	result = eigrp_topology_clear_prefix(context->runtime, prefix);
	if (result == EIGRP_RESULT_SUCCESS && affected_count)
		*affected_count = 1;
	return result;
}

static uint32_t eigrp_topology_successor_count(eigrp_prefix_descriptor_t *prefix)
{
	eigrp_route_descriptor_t *route;
	struct listnode *node;
	uint32_t count = 0;

	for (ALL_LIST_ELEMENTS_RO(prefix->entries, node, route))
		if (route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)
			count++;
	return count;
}

static void eigrp_topology_route_next_hop_export(
	const eigrp_route_descriptor_t *route, eigrp_topology_route_state_t *state)
{
	memset(&state->next_hop, 0, sizeof(state->next_hop));
	if (!route->adv_router)
		return;
	if (route->adv_router->src.afi == AF_INET6) {
		state->next_hop.afi = EIGRP_ADDRESS_FAMILY_IPV6;
		memcpy(state->next_hop.bytes, &route->adv_router->src.ip.v6, 16);
		return;
	}
	state->next_hop.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	memcpy(state->next_hop.bytes, &route->adv_router->src.ip.v4, 4);
}

static eigrp_result_t eigrp_topology_state_emit_prefix(
	eigrp_instance_t *runtime, eigrp_prefix_descriptor_t *prefix, bool all_links,
	eigrp_topology_prefix_state_cb prefix_callback,
	eigrp_topology_route_state_cb route_callback, void *arg)
{
	eigrp_topology_prefix_state_t prefix_state = {0};
	eigrp_topology_route_state_t route_state;
	eigrp_route_descriptor_t *route;
	struct listnode *node;
	eigrp_result_t result;

	prefix_state.destination = prefix->destination;
	prefix_state.active = prefix->state != 0;
	prefix_state.feasible_distance = prefix->fdistance;
	prefix_state.successor_count = eigrp_topology_successor_count(prefix);
	prefix_state.serial_number = prefix->serno;

	result = prefix_callback(&prefix_state, arg);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	for (ALL_LIST_ELEMENTS_RO(prefix->entries, node, route)) {
		bool successor =
			(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG) != 0;
		bool feasible =
			(route->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG) != 0;

		if (route->reported_distance == EIGRP_MAX_METRIC)
			continue;
		if (!all_links && !successor && !feasible)
			continue;

		memset(&route_state, 0, sizeof(route_state));
		route_state.connected = route->adv_router == runtime->neighbor_self;
		route_state.successor = successor;
		route_state.feasible_successor = feasible;
		route_state.distance = route->distance;
		route_state.reported_distance = route->reported_distance;
		route_state.interface_name = route->ei ? eigrp_intf_name_string(route->ei)
						       : NULL;
		if (!route_state.connected)
			eigrp_topology_route_next_hop_export(route, &route_state);

		result = route_callback(&route_state, arg);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
	}

	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_topology_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const eigrp_prefix_t *destination, bool all_links,
	eigrp_topology_prefix_state_cb prefix_callback,
	eigrp_topology_route_state_cb route_callback, void *arg)
{
	eigrp_prefix_descriptor_t *prefix;
	struct route_node *node;
	struct prefix lookup;
	eigrp_result_t result;
	bool matched = false;

	if (!prefix_callback || !route_callback || (!config && !runtime))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!runtime) {
		if (config && config->afi == EIGRP_ADDRESS_FAMILY_IPV6)
			return EIGRP_RESULT_UNSUPPORTED;
		return EIGRP_RESULT_NOT_FOUND;
	}
	if (!runtime->topology_table)
		return EIGRP_RESULT_NOT_FOUND;

	if (destination) {
		if (!eigrp_topology_table_key(destination, &lookup))
			return EIGRP_RESULT_INVALID_ARGUMENT;
		node = route_node_match(runtime->topology_table, &lookup);
		if (!node)
			return EIGRP_RESULT_NOT_FOUND;
		prefix = node->info;
		if (!prefix) {
			route_unlock_node(node);
			return EIGRP_RESULT_NOT_FOUND;
		}
		result = eigrp_topology_state_emit_prefix(runtime, prefix, all_links,
							 prefix_callback, route_callback,
							 arg);
		route_unlock_node(node);
		return result;
	}

	for (node = route_top(runtime->topology_table); node;
	     node = route_next(node)) {
		prefix = node->info;
		if (!prefix)
			continue;
		result = eigrp_topology_state_emit_prefix(runtime, prefix, all_links,
							 prefix_callback, route_callback,
							 arg);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
		matched = true;
	}

	return matched ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;
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
	if (!maximum_paths || maximum_paths > EIGRP_MAX_PATHS_MAX)
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
