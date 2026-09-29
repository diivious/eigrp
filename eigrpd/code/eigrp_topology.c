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
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "eigrpd.h"
#include "eigrp_table.h"

#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_packet.h"
#include "eigrp_packetizer.h"
#include "eigrp_prefix.h"
#include "eigrp_debug.h"
#include "eigrp_network.h"
#include "eigrp_topology.h"
#include "eigrp_eventlog.h"
#include "eigrp_fsm.h"
#include "eigrp_metric.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
static int eigrp_route_descriptor_cmp(eigrp_route_descriptor_t *,
				      eigrp_route_descriptor_t *);

static bool eigrp_topology_table_key(const eigrp_prefix_t *source,
                                     eigrp_prefix_t *destination)
{
	if (!source || !destination || !eigrp_prefix_valid(source))
		return false;

	*destination = *source;
	eigrp_prefix_normalize(destination);
	return true;
}

/* RFC 7868 section 6.2 external-protocol assignments. */
static uint8_t eigrp_topology_redistributed_protocol(
	const eigrp_redist_source_t *source)
{
	if (!source)
		return 0;

	switch (source->protocol) {
	case EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP:
		return 2;
	case EIGRP_REDISTRIBUTE_PROTOCOL_STATIC:
		return 3;
	case EIGRP_REDISTRIBUTE_PROTOCOL_RIP:
		return 4;
	case EIGRP_REDISTRIBUTE_PROTOCOL_OSPF:
		return 6;
	case EIGRP_REDISTRIBUTE_PROTOCOL_ISIS:
		return 7;
	case EIGRP_REDISTRIBUTE_PROTOCOL_BGP:
		return 9;
	case EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED:
		return 11;
	case EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED:
	default:
		return 0;
	}
}

static uint32_t eigrp_topology_redistributed_external_as(
	const eigrp_instance_t *eigrp, const eigrp_redist_source_t *source)
{
	if (!eigrp || !source)
		return 0;

	if (source->protocol == EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP)
		return ((uint32_t)eigrp->vrid << 16)
		       | ((uint32_t)source->route_instance & 0xffffU);

	/* RFC 7868 allows the field to carry protocol-specific identity when
	 * the external protocol has no AS (for example an OSPF process-id).
	 */
	return source->route_instance;
}

static void eigrp_topology_redistributed_extdata_build(
	const eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route,
	eigrp_extdata_t *extdata)
{
	memset(extdata, 0, sizeof(*extdata));
	extdata->orig = ntohl(eigrp->router_id.s_addr);
	extdata->as = eigrp_topology_redistributed_external_as(
		eigrp, &source_route->source);
	extdata->tag = source_route->tag;
	extdata->metric = source_route->metric > UINT32_MAX
			  ? UINT32_MAX
			  : (uint32_t)source_route->metric;
	extdata->protocol =
		eigrp_topology_redistributed_protocol(&source_route->source);
	/* reserved and flags are zero unless a later policy explicitly sets an
	 * RFC-defined external-route flag (for example candidate-default).
	 */
}

static void eigrp_topology_redistributed_nexthop_update(
	eigrp_operation_t operation, eigrp_route_descriptor_t *route,
	const eigrp_rib_source_route_t *source_route)
{
	if (operation != EIGRP_SET)
		return;
	memset(&route->nexthop, 0, sizeof(route->nexthop));
	if (!source_route->gateway_present)
		return;

	if (source_route->gateway.afi == EIGRP_AFI_IPV4) {
		route->nexthop.afi = AF_INET;
		memcpy(&route->nexthop.ip.v4, source_route->gateway.bytes,
		       sizeof(route->nexthop.ip.v4));
	} else if (source_route->gateway.afi == EIGRP_AFI_IPV6) {
		route->nexthop.afi = AF_INET6;
		memcpy(&route->nexthop.ip.v6, source_route->gateway.bytes,
		       sizeof(route->nexthop.ip.v6));
	}
}

static eigrp_metric_t eigrp_topology_redistributed_distance(
	eigrp_instance_t *eigrp, const eigrp_metrics_t *metric)
{
	eigrp_metrics_t calculation;
	eigrp_metric_t distance;

	if (!eigrp || !metric)
		return EIGRP_MAX_METRIC;
	if (metric->delay == EIGRP_MAX_METRIC)
		return EIGRP_MAX_METRIC;

	calculation = *metric;
	/* Locally selected redistribution seed delay is retained in the native
	 * route object in CLI/TLV 10-microsecond units.  The DUAL calculation
	 * consumes scaled delay, matching eigrp_topology_update_distance().
	 */
	calculation.delay = eigrp_delay_to_scaled(calculation.delay);
	distance = eigrp_metric_calculate(eigrp, calculation);
	return distance > EIGRP_MAX_METRIC ? EIGRP_MAX_METRIC : distance;
}

static eigrp_route_descriptor_t *eigrp_topology_redistributed_route_lookup(
	eigrp_instance_t *eigrp, eigrp_prefix_descriptor_t *prefix,
	const eigrp_rib_source_route_t *source_route)
{
	eigrp_extdata_t identity;
	eigrp_route_descriptor_t *route;
	eigrp_list_item_t *node;

	if (!eigrp || !prefix || !source_route)
		return NULL;

	eigrp_topology_redistributed_extdata_build(eigrp, source_route, &identity);
	for (EIGRP_LIST_ITERATE_RO(prefix->external_routes, node, route)) {
		if (route->adv_router != eigrp->neighbor_self)
			continue;
		if (route->type != EIGRP_EXT
		    && route->type != eigrp->af_vectors.classic_external_tlv_type
		    && route->type != EIGRP_TLV_MP_EXT)
			continue;
		if (route->extdata.protocol == identity.protocol
		    && route->extdata.as == identity.as)
			return route;
	}
	return NULL;
}

static bool eigrp_topology_southbound_nexthop(
	const eigrp_route_descriptor_t *route, eigrp_rib_nexthop_t *nexthop)
{
	if (!route || !route->ei || !nexthop)
		return false;

	memset(nexthop, 0, sizeof(*nexthop));
	nexthop->ifindex = route->ei->ifindex;

	if (route->adv_router && route->adv_router->src.afi == AF_INET
	    && route->adv_router->src.ip.v4.s_addr != INADDR_ANY) {
		nexthop->gateway_present = true;
		nexthop->gateway.afi = EIGRP_AFI_IPV4;
		memcpy(nexthop->gateway.bytes, &route->adv_router->src.ip.v4,
		       sizeof(route->adv_router->src.ip.v4));
	} else if (route->adv_router
		   && route->adv_router->src.afi == AF_INET6
		   && !IN6_IS_ADDR_UNSPECIFIED(&route->adv_router->src.ip.v6)) {
		nexthop->gateway_present = true;
		nexthop->gateway.afi = EIGRP_AFI_IPV6;
		memcpy(nexthop->gateway.bytes, &route->adv_router->src.ip.v6,
		       sizeof(route->adv_router->src.ip.v6));
	}

	return true;
}

static size_t eigrp_topology_southbound_nexthops(
	const eigrp_instance_t *eigrp, const eigrp_list_t *routes,
	eigrp_rib_nexthop_t *nexthops, size_t capacity)
{
	eigrp_route_descriptor_t *route;
	eigrp_list_item_t *node;
	uint64_t largest_metric = 0;
	size_t count = 0;

	if (!eigrp || !routes || !nexthops || capacity == 0)
		return 0;

	if (eigrp->traffic_share_balanced) {
		for (EIGRP_LIST_ITERATE_RO(routes, node, route)) {
			if (route->distance != EIGRP_MAX_METRIC
			    && route->distance > largest_metric)
				largest_metric = route->distance;
		}
	}

	for (EIGRP_LIST_ITERATE_RO(routes, node, route)) {
		if (count == capacity)
			break;
		if (!eigrp_topology_southbound_nexthop(route, &nexthops[count]))
			continue;
		if (eigrp->traffic_share_balanced && largest_metric
		    && route->distance && route->distance != EIGRP_MAX_METRIC) {
			nexthops[count].weight = largest_metric / route->distance;
			if (!nexthops[count].weight)
				nexthops[count].weight = 1;
		}
		count++;
	}

	return count;
}

/**
 * Various fuctions for handling eigrp route descriptors
 */

static bool eigrp_topology_route_external(const eigrp_route_descriptor_t *route)
{
	if (!route)
		return false;
	return route->type == EIGRP_EXT
	       || route->type == EIGRP_TLV_IPv4_EXT
	       || route->type == EIGRP_TLV_IPv6_EXT
	       || route->type == EIGRP_TLV_MP_EXT;
}

static eigrp_list_t *eigrp_topology_route_class_queue(
	eigrp_prefix_descriptor_t *prefix, const eigrp_route_descriptor_t *route)
{
	return eigrp_topology_route_external(route) ? prefix->external_routes
						     : prefix->internal_routes;
}

eigrp_route_descriptor_t *eigrp_topology_route_iterator_first(
	eigrp_prefix_descriptor_t *prefix, eigrp_topology_route_iterator_t *iterator)
{
	if (!prefix || !iterator)
		return NULL;
	iterator->prefix = prefix;
	iterator->queue = 0;
	iterator->node = prefix->internal_routes ? prefix->internal_routes->head : NULL;
	if (!iterator->node) {
		iterator->queue = 1;
		iterator->node = prefix->external_routes ? prefix->external_routes->head : NULL;
	}
	return iterator->node ? iterator->node->data : NULL;
}

eigrp_route_descriptor_t *eigrp_topology_route_iterator_next(
	eigrp_topology_route_iterator_t *iterator)
{
	if (!iterator || !iterator->prefix || !iterator->node)
		return NULL;
	iterator->node = iterator->node->next;
	if (!iterator->node && iterator->queue == 0) {
		iterator->queue = 1;
		iterator->node = iterator->prefix->external_routes
			? iterator->prefix->external_routes->head : NULL;
	}
	return iterator->node ? iterator->node->data : NULL;
}

eigrp_route_descriptor_t *
eigrp_topology_route_read(eigrp_prefix_descriptor_t *prefix)
{
	eigrp_route_descriptor_t *route;
	eigrp_route_descriptor_t *fallback = NULL;
	eigrp_list_item_t *node;
	unsigned int q;

	if (!prefix)
		return NULL;
	for (q = 0; q < 2; q++) {
		eigrp_list_t *routes = eigrp_topology_route_queue(prefix, q);
		for (EIGRP_LIST_ITERATE_RO(routes, node, route)) {
			if (!fallback)
				fallback = route;
			if (route->distance != EIGRP_MAX_METRIC)
				return route;
		}
	}
	return fallback;
}

eigrp_route_descriptor_t *
eigrp_topology_route_select(eigrp_prefix_descriptor_t *prefix)
{
	eigrp_route_descriptor_t *route;
	eigrp_list_item_t *node;
	unsigned int q;

	if (!prefix)
		return NULL;

	/* Internal routes are considered before external routes.  Each queue is
	 * CD sorted, so the first route satisfying FC is the best route in that
	 * class.  Inbound policy rejection is represented in the current topology
	 * state as an unreachable metric and therefore cannot satisfy this test.
	 */
	for (q = 0; q < 2; q++) {
		eigrp_list_t *routes = eigrp_topology_route_queue(prefix, q);
		for (EIGRP_LIST_ITERATE_RO(routes, node, route)) {
			if (route->distance == EIGRP_MAX_METRIC)
				continue;
			if (route->reported_distance >= prefix->fdistance)
				continue;
			return route;
		}
	}
	return NULL;
}

/*
 * Returns new topology route
 */
eigrp_route_descriptor_t *eigrp_topology_route_create(eigrp_intf_t *intf)
{
	eigrp_route_descriptor_t *new;

	new = calloc(1, sizeof(eigrp_route_descriptor_t));
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
	eigrp_rib_nexthop_t nexthop;

	eigrp_list_t *routes = eigrp_topology_route_class_queue(node, route);

	if (eigrp_list_lookup(routes, route) == NULL) {
		eigrp_list_insert(routes, route);
		route->prefix = node;

		if (eigrp_topology_route_read(node) == route
		    && eigrp_topology_southbound_nexthop(route, &nexthop)) {
			eigrp_rib_route_t rib_route = {
				.prefix = node->destination,
				.nexthops = &nexthop,
				.nexthop_count = 1,
				.metric = node->distance,
				.administrative_distance =
					eigrp_topology_route_external(route)
						? eigrp->distance_external
						: eigrp->distance_internal,
				.tag = route->extdata.tag,
				.type = eigrp_topology_route_external(route)
					? EIGRP_RIB_ROUTE_EXTERNAL
					: EIGRP_RIB_ROUTE_INTERNAL,
			};

			if (eigrp_rib_route_install(eigrp, &rib_route)
			    == EIGRP_RESULT_SUCCESS)
				route->flags |= EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG;
		}
	}
}


void eigrp_topology_prefix_free(eigrp_prefix_descriptor_t *pe)
{
	eigrp_route_descriptor_t *route;
	eigrp_list_item_t *node, *nnode;

	if (!pe)
		return;

	for (unsigned int q = 0; q < 2; q++) {
		eigrp_list_t *routes = eigrp_topology_route_queue(pe, q);
		if (!routes)
			continue;
		for (EIGRP_LIST_ITERATE(routes, node, nnode, route))
			eigrp_topology_route_free(route);
		eigrp_list_delete(q == 0 ? &pe->internal_routes
					 : &pe->external_routes);
	}

	eigrp_sys_event_cancel(&pe->t_active);
	pe->active_eigrp = NULL;
	if (pe->rij)
		eigrp_list_delete(&pe->rij);


	free(pe);
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
	free(route);
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
	new = calloc(1, sizeof(eigrp_prefix_descriptor_t));
	new->internal_routes = eigrp_list_create();
	new->external_routes = eigrp_list_create();
	new->rij = eigrp_list_create();
	new->rij->del = free;
	new->internal_routes->cmp = (int (*)(void *, void *))eigrp_route_descriptor_cmp;
	new->external_routes->cmp = (int (*)(void *, void *))eigrp_route_descriptor_cmp;
	new->distance = new->fdistance = new->rdistance = EIGRP_MAX_METRIC;

	return new;
}

/*
 * Adding topology node to topology table
 */
void eigrp_prefix_descriptor_add(eigrp_table_t *topology,
				 eigrp_prefix_descriptor_t *pe)
{
	char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";
	eigrp_prefix_t key;
	eigrp_table_node_t *rn;

	if (!topology || !pe
	    || !eigrp_topology_table_key(&pe->destination, &key))
		return;

	rn = eigrp_table_node_lookup_or_create(topology, &key);
	if (rn->info) {
		if (IS_DEBUG_EIGRP_EVENT) {
			eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
					      &pe->destination);
			eigrp_log(EIGRP_LOG_DEBUG, "%s: %s Should we have found this prefix in the topo table?",
				   __func__, prefix_buf);
		}
		eigrp_table_node_release(rn);
	}

	rn->info = pe;
	if (IS_DEBUG_EIGRP_EVENT) {
		eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
				      &pe->destination);
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP event: topology prefix add %s", prefix_buf);
		if (IS_DEBUG_EIGRP(0, DETAIL))
			eigrp_log(EIGRP_LOG_DEBUG, "EIGRP event detail: prefix state %u distance %u fd %u rd %u",
				   pe->state, pe->distance, pe->fdistance,
				   pe->rdistance);
	}
}

/*
 * Find topology node in topology table
 */
eigrp_route_descriptor_t *eigrp_prefix_descriptor_lookup(
	eigrp_prefix_descriptor_t *prefix, eigrp_nbr_t *nbr)
{
	eigrp_route_descriptor_t *data;
	eigrp_list_item_t *node;
	unsigned int q;

	if (!prefix)
		return NULL;
	for (q = 0; q < 2; q++) {
		eigrp_list_t *routes = eigrp_topology_route_queue(prefix, q);
		for (EIGRP_LIST_ITERATE_RO(routes, node, data))
			if (data->adv_router == nbr)
				return data;
	}
	return NULL;
}

/*
 * Deleting topology node from topology table
 */
void eigrp_prefix_descriptor_delete(eigrp_instance_t *eigrp,
				    eigrp_table_t *table,
				    eigrp_prefix_descriptor_t *pe)
{
	char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";
	eigrp_route_descriptor_t *ne;
	eigrp_list_item_t *node, *nnode;
	eigrp_prefix_t key;
	eigrp_table_node_t *rn;

	if (!eigrp || !table || !pe
	    || !eigrp_topology_table_key(&pe->destination, &key))
		return;

	rn = eigrp_table_node_lookup(table, &key);
	if (!rn)
		return;

	if (IS_DEBUG_EIGRP_EVENT) {
		eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
				      &pe->destination);
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP event: topology prefix delete %s", prefix_buf);
		if (IS_DEBUG_EIGRP(0, DETAIL))
			eigrp_log(EIGRP_LOG_DEBUG, "EIGRP event detail: AS %u prefix state %u distance %u fd %u rd %u",
				   eigrp->AS, pe->state, pe->distance,
				   pe->fdistance, pe->rdistance);
	}

	eigrp_list_delete_data(eigrp->topology_changes, pe);

	for (unsigned int q = 0; q < 2; q++) {
		eigrp_list_t *routes = eigrp_topology_route_queue(pe, q);
		for (EIGRP_LIST_ITERATE(routes, node, nnode, ne))
			eigrp_route_descriptor_delete(eigrp, pe, ne);
	}
	eigrp_list_delete(&pe->internal_routes);
	eigrp_list_delete(&pe->external_routes);
	eigrp_sys_event_cancel(&pe->t_active);
	pe->active_eigrp = NULL;
	eigrp_list_delete(&pe->rij);
	(void)eigrp_rib_route_remove(eigrp, &pe->destination);

	rn->info = NULL;
	eigrp_table_node_release(rn); /* lookup reference */
	eigrp_table_node_release(rn); /* initial creation reference */
	free(pe);
}

/*
 * Deleting topology entry from topology node
 */
void eigrp_route_descriptor_delete(eigrp_instance_t *eigrp,
				   eigrp_prefix_descriptor_t *node,
				   eigrp_route_descriptor_t *route)
{
	eigrp_list_t *routes = eigrp_topology_route_class_queue(node, route);

	if (eigrp_list_lookup(routes, route) != NULL) {
		bool installed = (route->flags & EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG) != 0;

		eigrp_list_delete_data(routes, route);
		if (installed)
			(void)eigrp_rib_route_remove(eigrp, &node->destination);
		free(route);
	}
}

/*
 * Returns linkedlist used as topology table
 * cmp - assigned function for comparing topology nodes
 * del - assigned function executed before deleting topology node by list
 * function
 */
eigrp_table_t *eigrp_topology_table_create(void)
{
	return eigrp_table_create();
}

/*
 * Deleting all nodes from topology table
 */
void eigrp_topology_delete_all(eigrp_instance_t *eigrp,
			       eigrp_table_t *topology)
{
	eigrp_table_node_t *rn;
	eigrp_prefix_descriptor_t *pe;

	for (rn = eigrp_table_first(topology); rn; rn = eigrp_table_next(rn)) {
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
				 eigrp_table_t *table)
{
	if (!table)
		return;
	eigrp_topology_delete_all(eigrp, table);
	eigrp_table_free(table);
}

eigrp_prefix_descriptor_t *
eigrp_topology_table_lookup(eigrp_table_t *table,
			    const eigrp_prefix_t *address)
{
	eigrp_prefix_descriptor_t *pe;
	eigrp_prefix_t key;
	eigrp_table_node_t *rn;

	if (!table || !eigrp_topology_table_key(address, &key))
		return NULL;

	rn = eigrp_table_node_lookup(table, &key);
	if (!rn)
		return NULL;

	pe = rn->info;
	eigrp_table_node_release(rn);
	return pe;
}

static void eigrp_topology_redistributed_update_mark(
	eigrp_instance_t *eigrp, eigrp_prefix_descriptor_t *prefix)
{
	if (!eigrp || !prefix || prefix->state != EIGRP_FSM_STATE_PASSIVE)
		return;

	prefix->req_action |= EIGRP_FSM_NEED_UPDATE;
	if (!eigrp_list_lookup(eigrp->topology_changes, prefix))
		eigrp_list_add(eigrp->topology_changes, prefix);
}

/* Host redistribution ingress has no packet-receive tail to drain DUAL's
 * topology-change actions.  Use the same query/update packetizer entry points
 * as normal protocol processing after DUAL has consumed the change. */
static void eigrp_topology_changes_send(eigrp_instance_t *eigrp)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_list_item_t *node;
	bool query = false;
	bool update = false;

	if (!eigrp || !eigrp->topology_changes)
		return;
	for (EIGRP_LIST_ITERATE_RO(eigrp->topology_changes, node, prefix)) {
		query |= (prefix->req_action & EIGRP_FSM_NEED_QUERY) != 0;
		update |= (prefix->req_action & EIGRP_FSM_NEED_UPDATE) != 0;
	}
	if (query)
		eigrp_query_send_all(eigrp);
	if (update)
		eigrp_update_send_all(eigrp, NULL);
}

eigrp_result_t eigrp_topology_redistributed_route_update(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route,
	const eigrp_metrics_t *metric)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_extdata_t old_extdata;
	eigrp_addr_t old_nexthop;
	eigrp_prefix_t destination;
	eigrp_metric_t distance;
	bool existing_route;
	bool metadata_changed;

	if (!eigrp || !source_route || !metric || !eigrp->topology_table
	    || !eigrp->neighbor_self
	    || !eigrp_topology_table_key(&source_route->prefix, &destination)
	    || !eigrp_topology_redistributed_protocol(&source_route->source))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	prefix = eigrp_topology_table_lookup(eigrp->topology_table, &destination);
	if (!prefix) {
		prefix = eigrp_topology_prefix_create();
		route = eigrp_topology_route_create(NULL);
		if (!prefix || !route) {
			eigrp_topology_prefix_free(prefix);
			eigrp_topology_route_free(route);
			return EIGRP_RESULT_INTERNAL_FAILURE;
		}

		distance = eigrp_topology_redistributed_distance(eigrp, metric);
		prefix->serno = eigrp->serno;
		prefix->destination = destination;
		prefix->nt = EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL;
		prefix->state = EIGRP_FSM_STATE_PASSIVE;
		prefix->fdistance = prefix->distance = prefix->rdistance = distance;
		prefix->reported_metric = *metric;

		route->type = eigrp->af_vectors.classic_external_tlv_type;
		route->dest = destination;
		route->adv_router = eigrp->neighbor_self;
		route->metric = route->reported_metric = route->total_metric = *metric;
		route->reported_distance = 0;
		route->distance = distance;
		route->flags = EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
		eigrp_topology_redistributed_extdata_build(eigrp, source_route,
						   &route->extdata);
		eigrp_topology_redistributed_nexthop_update(EIGRP_SET, route, source_route);

		eigrp_prefix_descriptor_add(eigrp->topology_table, prefix);
		eigrp_route_descriptor_add(eigrp, prefix, route);
		eigrp_topology_redistributed_update_mark(eigrp, prefix);
		eigrp_topology_changes_send(eigrp);
		return EIGRP_RESULT_SUCCESS;
	}

	route = eigrp_topology_redistributed_route_lookup(eigrp, prefix,
							 source_route);
	existing_route = route != NULL;
	if (!route) {
		route = eigrp_topology_route_create(NULL);
		if (!route)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		route->type = eigrp->af_vectors.classic_external_tlv_type;
		route->dest = destination;
		route->adv_router = eigrp->neighbor_self;
		route->prefix = prefix;
		eigrp_route_descriptor_add(eigrp, prefix, route);
	}

	old_extdata = route->extdata;
	old_nexthop = route->nexthop;
	route->type = eigrp->af_vectors.classic_external_tlv_type;
	route->dest = destination;
	route->adv_router = eigrp->neighbor_self;
	route->ei = NULL;
	eigrp_topology_redistributed_extdata_build(eigrp, source_route,
						   &route->extdata);
	eigrp_topology_redistributed_nexthop_update(EIGRP_SET, route, source_route);
	metadata_changed = !existing_route
			   || memcmp(&old_extdata, &route->extdata,
				     sizeof(old_extdata)) != 0
			   || memcmp(&old_nexthop, &route->nexthop,
				     sizeof(old_nexthop)) != 0;

	{
		eigrp_fsm_action_message_t msg = {
			.packet_type = EIGRP_OPC_UPDATE,
			.eigrp = eigrp,
			.adv_router = eigrp->neighbor_self,
			.route = route,
			.prefix = prefix,
			.data_type = EIGRP_EXT,
			.metrics = *metric,
		};
		eigrp_fsm_event(&msg);
	}

	/* Metric-only changes are queued by DUAL.  Tag/external-metadata/next-hop
	 * changes carry no metric delta, so explicitly advertise them once the
	 * destination is Passive.  While Active, path state is updated but the
	 * frozen destination-level state and advertisement decision are left to
	 * the DUAL transition back to Passive.
	 */
	if (metadata_changed)
		eigrp_topology_redistributed_update_mark(eigrp, prefix);

	eigrp_topology_changes_send(eigrp);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_topology_redistributed_route_remove(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_prefix_t destination;
	eigrp_metrics_t poison;

	if (!eigrp || !source_route || !eigrp->topology_table
	    || !eigrp_topology_table_key(&source_route->prefix, &destination)
	    || !eigrp_topology_redistributed_protocol(&source_route->source))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	prefix = eigrp_topology_table_lookup(eigrp->topology_table, &destination);
	if (!prefix)
		return EIGRP_RESULT_NOT_FOUND;
	route = eigrp_topology_redistributed_route_lookup(eigrp, prefix,
							 source_route);
	if (!route)
		return EIGRP_RESULT_NOT_FOUND;

	poison = route->metric;
	poison.delay = EIGRP_MAX_METRIC;
	{
		eigrp_fsm_action_message_t msg = {
			.packet_type = EIGRP_OPC_UPDATE,
			.eigrp = eigrp,
			.adv_router = eigrp->neighbor_self,
			.route = route,
			.prefix = prefix,
			.data_type = EIGRP_EXT,
			.metrics = poison,
		};
		eigrp_fsm_event(&msg);
	}

	eigrp_topology_changes_send(eigrp);

	/* Passive unreachable state remains topology-owned until packetizer has
	 * advertised it.  Active state remains owned by DUAL until convergence. */

	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_topology_redistributed_source_remove(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source)
{
	eigrp_rib_source_route_t source_route;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_table_node_t *rn;
	eigrp_list_item_t *node;
	uint8_t protocol;
	uint32_t external_as;
	bool found = false;

	if (!eigrp || !source || !eigrp->topology_table)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	protocol = eigrp_topology_redistributed_protocol(source);
	if (!protocol)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	external_as = eigrp_topology_redistributed_external_as(eigrp, source);

	memset(&source_route, 0, sizeof(source_route));
	source_route.source = *source;
	for (rn = eigrp_table_first(eigrp->topology_table); rn;
	     rn = eigrp_table_next(rn)) {
		prefix = rn->info;
		if (!prefix || !prefix->external_routes)
			continue;
		for (EIGRP_LIST_ITERATE_RO(prefix->external_routes, node, route)) {
			if (!route || route->adv_router != eigrp->neighbor_self
			    || route->extdata.protocol != protocol
			    || route->extdata.as != external_as)
				continue;
			source_route.prefix = prefix->destination;
			if (eigrp_topology_redistributed_route_remove(
				    eigrp, &source_route) == EIGRP_RESULT_SUCCESS)
				found = true;
			break;
		}
	}
	return found ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;
}


/*
 * For a future optimization, put the successor list into it's
 * own separate list from the full list?
 *
 * That way we can clean up all the list_new and list_delete's
 * that we are doing.  DBS
 */
eigrp_list_t *eigrp_topology_successors_read(eigrp_prefix_descriptor_t *table_node)
{
	eigrp_list_t *successors = eigrp_list_create();
	eigrp_route_descriptor_t *data;
	eigrp_topology_route_iterator_t iterator;

	for (data = eigrp_topology_route_iterator_first(table_node, &iterator); data;
	     data = eigrp_topology_route_iterator_next(&iterator)) {
		if (data->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG) {
			eigrp_list_add(successors, data);
		}
	}

	/*
	 * If we have no successors return NULL
	 */
	if (!successors->count) {
		eigrp_list_delete(&successors);
		successors = NULL;
	}

	return successors;
}

eigrp_list_t *
eigrp_topology_successors_max_read(eigrp_prefix_descriptor_t *table_node,
				 unsigned int maxpaths)
{
	eigrp_list_t *successors = eigrp_topology_successors_read(table_node);

	if (successors && successors->count > maxpaths) {
		do {
			eigrp_list_item_t *node = eigrp_list_last(successors);

			eigrp_list_remove(successors, node);

		} while (successors->count > maxpaths);
	}

	return successors;
}

/* Lookup all prefixes from specified neighbor */
eigrp_list_t *eigrp_nbr_prefixes_lookup(eigrp_instance_t *eigrp,
					    eigrp_nbr_t *nbr)
{
	eigrp_prefix_descriptor_t *pe;
	eigrp_table_node_t *rn;

	/* create new empty list for prefixes storage */
	eigrp_list_t *prefixes = eigrp_list_create();

	/* iterate over all prefixes in topology table */
	for (rn = eigrp_table_first(eigrp->topology_table); rn; rn = eigrp_table_next(rn)) {
		if (!rn->info)
			continue;
		pe = rn->info;
		if (eigrp_prefix_descriptor_lookup(pe, nbr))
			eigrp_list_add(prefixes, pe);
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
	bool class_changed;

	assert(route);

	if (!route->adv_router)
		route->adv_router = msg->adv_router;
	if (!route->prefix)
		route->prefix = prefix;
	if (!route->ei && msg->adv_router)
		route->ei = msg->adv_router->ei;

	class_changed =
		(eigrp_list_lookup(prefix->external_routes, route) != NULL)
		!= eigrp_topology_route_external(route);

	switch (msg->data_type) {
	case EIGRP_CONNECTED:
		if (prefix->nt == EIGRP_TOPOLOGY_TYPE_CONNECTED)
			return change;

		change = METRIC_DECREASE;
		break;
	case EIGRP_INT:
		if (!class_changed
		    && eigrp_metrics_match(msg->metrics, route->reported_metric))
			return change; // No change

		new_reported_distance =
			eigrp_metric_calculate(eigrp, msg->metrics);

		if (route->reported_distance < new_reported_distance)
			change = METRIC_INCREASE;
		else
			change = METRIC_DECREASE;

		route->metric = msg->metrics;
		route->reported_metric = msg->metrics;
		route->reported_distance = new_reported_distance;
		route->distance = eigrp_metric_total_calculate(eigrp, route);
		break;
	case EIGRP_EXT:
		if (!class_changed
		    && eigrp_metrics_match(msg->metrics, route->reported_metric))
			return change;

		if (route->adv_router == eigrp->neighbor_self && !route->ei) {
			eigrp_metric_t old_distance = route->distance;
			eigrp_metric_t new_distance =
				eigrp_topology_redistributed_distance(eigrp, &msg->metrics);

			if (old_distance < new_distance)
				change = METRIC_INCREASE;
			else if (old_distance > new_distance)
				change = METRIC_DECREASE;

			/* A locally originated external route has no advertising
			 * EIGRP neighbor, so its DUAL reported distance is zero just
			 * like another locally originated route.  The selected seed
			 * vector is the complete local distance and the metric carried
			 * in the external route advertisement.
			 */
			route->metric = msg->metrics;
			route->reported_metric = msg->metrics;
			route->reported_distance = 0;
			route->total_metric = msg->metrics;
			route->distance = new_distance;
		} else {
			new_reported_distance =
				eigrp_metric_calculate(eigrp, msg->metrics);

			if (route->reported_distance < new_reported_distance)
				change = METRIC_INCREASE;
			else if (route->reported_distance > new_reported_distance)
				change = METRIC_DECREASE;

			route->metric = msg->metrics;
			route->reported_metric = msg->metrics;
			route->reported_distance = new_reported_distance;
			route->distance = eigrp_metric_total_calculate(eigrp, route);
		}
		break;
	default:
		eigrp_log(EIGRP_LOG_ERROR,  "%s: Please implement handler",
			 __func__);
		break;
	}

	/*
	 * Move to correct position in list according to new distance
	 */
	if (eigrp_list_lookup(prefix->internal_routes, route))
		eigrp_list_delete_data(prefix->internal_routes, route);
	if (eigrp_list_lookup(prefix->external_routes, route))
		eigrp_list_delete_data(prefix->external_routes, route);
	eigrp_list_insert(eigrp_topology_route_class_queue(prefix, route), route);

	return change;
}

void eigrp_topology_update_all_node_flags(eigrp_instance_t *eigrp)
{
	eigrp_prefix_descriptor_t *pe;
	eigrp_table_node_t *rn;

	if (!eigrp)
		return;

	for (rn = eigrp_table_first(eigrp->topology_table); rn; rn = eigrp_table_next(rn)) {
		pe = rn->info;

		if (!pe)
			continue;

		eigrp_topology_update_node_flags(eigrp, pe);
	}
}

void eigrp_topology_update_node_flags(eigrp_instance_t *eigrp,
				      eigrp_prefix_descriptor_t *dest)
{
	eigrp_route_descriptor_t *route;
	eigrp_route_descriptor_t *best;
	eigrp_topology_route_iterator_t iterator;
	eigrp_list_item_t *node;
	eigrp_list_t *eligible = NULL;
	unsigned int successor_count = 0;
	unsigned int max_paths;

	if (!eigrp || !dest)
		return;

	/* The current least-cost reachable route chooses the route class.  DUAL
	 * may only call this while PASSIVE after that least-cost route has passed
	 * the Feasibility Condition.  Do not skip an infeasible least-cost
	 * internal path merely to mark a higher-cost or external path.
	 */
	best = eigrp_topology_route_read(dest);
	if (best && best->distance != EIGRP_MAX_METRIC)
		eligible = eigrp_topology_route_class_queue(dest, best);
	max_paths = eigrp->max_paths ? eigrp->max_paths : EIGRP_MAX_PATHS_DEFAULT;

	/* Only the selected I/E class participates in successor/FS marking. */
	for (route = eigrp_topology_route_iterator_first(dest, &iterator); route;
	     route = eigrp_topology_route_iterator_next(&iterator))
		route->flags &= ~(EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG
				 | EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG);

	for (EIGRP_LIST_ITERATE_RO(eligible, node, route)) {
		uint32_t old_flags = route->flags;
		bool feasible = route->reported_distance < dest->fdistance;
		bool within_variance =
			route->distance != EIGRP_MAX_METRIC
			&& (uint64_t)route->distance
				<= (uint64_t)dest->distance * (uint64_t)eigrp->variance;

		if (feasible) {
			if (within_variance && successor_count < max_paths) {
				route->flags |= EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
				successor_count++;
			} else {
				route->flags |= EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG;
			}
		}

		if (IS_DEBUG_EIGRP(0, FAST_REROUTE)
		    && ((old_flags ^ route->flags)
			& (EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG
			   | EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG))) {
			char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";

			eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
					      &dest->destination);
			eigrp_log(EIGRP_LOG_DEBUG,
				"EIGRP AS %u prefix %s via %s: successor %s feasible-successor %s RD %u FD %u distance %u",
				eigrp->AS, prefix_buf,
				route->adv_router ? eigrp_print_addr(&route->adv_router->src)
						  : "connected",
				(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG) ? "yes" : "no",
				(route->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG) ? "yes" : "no",
				route->reported_distance, dest->fdistance, route->distance);
		}
	}
}

void eigrp_topology_multipath_update(eigrp_instance_t *eigrp)
{
	eigrp_table_node_t *rn;
	eigrp_prefix_descriptor_t *prefix;

	if (!eigrp || !eigrp->topology_table)
		return;
	for (rn = eigrp_table_first(eigrp->topology_table); rn;
	     rn = eigrp_table_next(rn)) {
		prefix = rn->info;
		if (!prefix || prefix->state != EIGRP_FSM_STATE_PASSIVE)
			continue;
		eigrp_topology_update_node_flags(eigrp, prefix);
		eigrp_update_routing_table(eigrp, prefix);
	}
}

void eigrp_topology_traffic_share_update(eigrp_instance_t *eigrp)
{
	eigrp_table_node_t *rn;
	eigrp_prefix_descriptor_t *prefix;

	if (!eigrp || !eigrp->topology_table)
		return;
	for (rn = eigrp_table_first(eigrp->topology_table); rn;
	     rn = eigrp_table_next(rn)) {
		prefix = rn->info;
		/* A forwarding-hint change must not alter or reinstall frozen Active
		 * destination state. Passive destinations retain their existing
		 * successor set; only the RIB snapshot weights are refreshed.
		 */
		if (!prefix || prefix->state != EIGRP_FSM_STATE_PASSIVE)
			continue;
		eigrp_update_routing_table(eigrp, prefix);
	}
}

static void eigrp_topology_route_queue_resort(eigrp_list_t *routes)
{
	eigrp_list_item_t *a, *b;
	bool changed;

	if (!routes)
		return;
	do {
		changed = false;
		for (a = routes->head; a && a->next; a = a->next) {
			b = a->next;
			if (((eigrp_route_descriptor_t *)a->data)->distance
			    <= ((eigrp_route_descriptor_t *)b->data)->distance)
				continue;
			void *tmp = a->data;
			a->data = b->data;
			b->data = tmp;
			changed = true;
		}
	} while (changed);
}

void eigrp_topology_metric_update(eigrp_instance_t *eigrp)
{
	eigrp_table_node_t *rn;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route, *best;
	eigrp_topology_route_iterator_t iterator;

	if (!eigrp || !eigrp->topology_table)
		return;
	for (rn = eigrp_table_first(eigrp->topology_table); rn;
	     rn = eigrp_table_next(rn)) {
		prefix = rn->info;
		if (!prefix || prefix->state != EIGRP_FSM_STATE_PASSIVE)
			continue;
		for (route = eigrp_topology_route_iterator_first(prefix, &iterator); route;
		     route = eigrp_topology_route_iterator_next(&iterator)) {
			route->reported_distance = eigrp_metric_calculate(eigrp, route->reported_metric);
			if (route->ei)
				route->distance = eigrp_metric_total_calculate(eigrp, route);
			else
				route->distance = eigrp_metric_calculate(eigrp, route->total_metric);
		}
		eigrp_topology_route_queue_resort(prefix->internal_routes);
		eigrp_topology_route_queue_resort(prefix->external_routes);
		best = eigrp_topology_route_read(prefix);
		if (best) {
			prefix->distance = best->distance;
			prefix->fdistance = best->distance;
			prefix->rdistance = best->reported_distance;
			prefix->reported_metric = best->reported_metric;
		}
		eigrp_topology_update_node_flags(eigrp, prefix);
		eigrp_update_routing_table(eigrp, prefix);
		prefix->req_action |= EIGRP_FSM_NEED_UPDATE;
		if (!eigrp_list_lookup(eigrp->topology_changes, prefix))
			eigrp_list_add(eigrp->topology_changes, prefix);
	}
	eigrp_update_send_all(eigrp, NULL);
}

void eigrp_update_routing_table(eigrp_instance_t *eigrp,
				eigrp_prefix_descriptor_t *prefix)
{
	eigrp_rib_nexthop_t nexthops[EIGRP_MAX_PATHS_MAX];
	eigrp_list_t *successors;
	eigrp_list_item_t *node;
	eigrp_route_descriptor_t *route;
	size_t nexthop_count;

	/* INTABLE describes the current southbound successor set, not historical
	 * ownership. Clear stale flags before replacing the RIB snapshot so a
	 * retired former successor cannot later remove the replacement route. */
	{
		eigrp_topology_route_iterator_t iterator;

		for (route = eigrp_topology_route_iterator_first(prefix, &iterator); route;
		     route = eigrp_topology_route_iterator_next(&iterator))
			route->flags &= ~EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG;
	}

	successors = eigrp_topology_successors_max_read(prefix, eigrp->max_paths);

	if (successors) {
		nexthop_count = eigrp_topology_southbound_nexthops(
			eigrp, successors, nexthops, EIGRP_MAX_PATHS_MAX);
		if (nexthop_count != 0) {
			eigrp_rib_route_t rib_route = {
				.prefix = prefix->destination,
				.nexthops = nexthops,
				.nexthop_count = nexthop_count,
				.metric = prefix->distance,
				.administrative_distance = eigrp->distance_internal,
				.type = EIGRP_RIB_ROUTE_INTERNAL,
			};

			for (EIGRP_LIST_ITERATE_RO(successors, node, route)) {
				if (eigrp_topology_route_external(route)) {
					rib_route.type = EIGRP_RIB_ROUTE_EXTERNAL;
					rib_route.administrative_distance =
						eigrp->distance_external;
					rib_route.tag = route->extdata.tag;
				}
				break;
			}
			(void)eigrp_rib_route_install(eigrp, &rib_route);
		}
		for (EIGRP_LIST_ITERATE_RO(successors, node, route))
			route->flags |= EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG;

		eigrp_list_delete(&successors);
	} else {
		(void)eigrp_rib_route_remove(eigrp, &prefix->destination);
		eigrp_topology_route_iterator_t iterator;
		for (route = eigrp_topology_route_iterator_first(prefix, &iterator); route;
		     route = eigrp_topology_route_iterator_next(&iterator))
			route->flags &= ~EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG;
	}
}




void eigrp_topology_connected_interface_down(eigrp_instance_t *eigrp,
					     eigrp_intf_t *ei)
{
	eigrp_table_node_t *rn;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_route_descriptor_t *best;
	eigrp_topology_route_iterator_t iterator;
	eigrp_list_t *owned;
	eigrp_list_item_t *node, *nnode;

	if (!eigrp || !ei)
		return;

	for (rn = eigrp_table_first(eigrp->topology_table); rn;
	     rn = eigrp_table_next(rn)) {
		prefix = rn->info;
		if (!prefix)
			continue;

		/* The interface owns its locally originated connected RDB. Neighbor
		 * teardown handles neighbor-owned RDBs separately. Retire only this
		 * interface's descriptors so interface-up cannot duplicate them. */
		owned = eigrp_list_create();
		for (route = eigrp_topology_route_iterator_first(prefix, &iterator);
		     route; route = eigrp_topology_route_iterator_next(&iterator)) {
			if (route->adv_router == eigrp->neighbor_self && route->ei == ei)
				eigrp_list_add(owned, route);
		}
		if (owned->count == 0) {
			eigrp_list_delete(&owned);
			continue;
		}
		for (EIGRP_LIST_ITERATE(owned, node, nnode, route)) {
			eigrp_list_delete_data(owned, route);
			eigrp_route_descriptor_delete(eigrp, prefix, route);
		}
		eigrp_list_delete(&owned);

		best = eigrp_topology_route_read(prefix);
		if (best && best->distance != EIGRP_MAX_METRIC) {
			eigrp_fsm_action_message_t msg = {0};

			msg.packet_type = EIGRP_OPC_UPDATE;
			msg.eigrp = eigrp;
			msg.data_type = eigrp_topology_route_external(best)
						? EIGRP_EXT : EIGRP_INT;
			msg.metrics = best->reported_metric;
			msg.adv_router = best->adv_router;
			msg.route = best;
			msg.prefix = prefix;
			eigrp_fsm_event(&msg);
			continue;
		}

		{
			uint8_t old_state = prefix->state;

			prefix->state = EIGRP_FSM_STATE_PASSIVE;
			if (old_state != prefix->state)
				(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_DUAL_STATE_CHANGE,
					&prefix->destination, old_state, prefix->state, 0, 0);
		}
		prefix->distance = EIGRP_MAX_METRIC;
		prefix->fdistance = EIGRP_MAX_METRIC;
		prefix->rdistance = EIGRP_MAX_METRIC;
		prefix->reported_metric.delay = EIGRP_MAX_METRIC;
		prefix->req_action |= EIGRP_FSM_NEED_UPDATE;
		if (!eigrp_list_lookup(eigrp->topology_changes, prefix))
			eigrp_list_add(eigrp->topology_changes, prefix);
		eigrp_topology_update_node_flags(eigrp, prefix);
		eigrp_update_routing_table(eigrp, prefix);
	}
}

void eigrp_topology_neighbor_down(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr)
{
	eigrp_prefix_descriptor_t *pe;
	eigrp_route_descriptor_t *route;
	eigrp_table_node_t *rn;

	for (rn = eigrp_table_first(eigrp->topology_table); rn; rn = eigrp_table_next(rn)) {
		pe = rn->info;

		if (!pe)
			continue;

		/* A neighbor that originated a deferred QUERY no longer needs a
		 * REPLY after it goes down. Clear the DUAL origin before processing
		 * its poisoned path so the prefix never retains a freed neighbor. */
		if (pe->query_origin == nbr)
			pe->query_origin = NULL;

		route = eigrp_prefix_descriptor_lookup(pe, nbr);
		if (route) {
			eigrp_fsm_action_message_t msg = {0};

			msg.metrics.delay = EIGRP_MAX_METRIC;
			msg.packet_type = pe->state == EIGRP_FSM_STATE_PASSIVE
					  ? EIGRP_OPC_UPDATE : EIGRP_OPC_REPLY;
			msg.eigrp = eigrp;
			msg.data_type = eigrp_topology_route_external(route) ? EIGRP_EXT : EIGRP_INT;
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
					eigrp_table_t *table,
					eigrp_prefix_descriptor_t *prefix)
{
	eigrp_list_item_t *node1, *node2;

	eigrp_route_descriptor_t *route;
	/* Keep poisoned topology state intact until packetizer has encoded the
	 * pending withdrawal.  This preserves route type and external metadata
	 * and avoids replacing a real external path with a synthetic internal
	 * poison TLV.  Packetizer performs the normal unreachable cleanup after
	 * every eligible interface has inspected the change set. */
	if (prefix->req_action & EIGRP_FSM_NEED_UPDATE)
		return;

	for (unsigned int q = 0; q < 2; q++) {
		eigrp_list_t *routes = eigrp_topology_route_queue(prefix, q);
		for (EIGRP_LIST_ITERATE(routes, node1, node2, route)) {
			if (route->distance == EIGRP_MAX_METRIC)
				eigrp_route_descriptor_delete(eigrp, prefix, route);
		}
	}
	/* Keep the PASSIVE destination descriptor after its unreachable RDBs
	 * have been retired. A later UPDATE can repopulate the same DUAL
	 * destination without manufacturing stale route ownership. */
}

static eigrp_list_t *eigrp_topology_connected_route_snapshot(
	eigrp_instance_t *eigrp, const eigrp_prefix_descriptor_t *prefix)
{
	eigrp_list_t *routes;
	eigrp_route_descriptor_t *route;
	eigrp_route_descriptor_t *copy;
	eigrp_list_item_t *node;

	routes = eigrp_list_create();
	for (EIGRP_LIST_ITERATE_RO(prefix->internal_routes, node, route)) {
		if (route->adv_router != eigrp->neighbor_self)
			continue;
		copy = eigrp_topology_route_create(route->ei);
		if (!copy)
			continue;
		*copy = *route;
		copy->prefix = NULL;
		eigrp_list_add(routes, copy);
	}
	return routes;
}

static void eigrp_topology_route_snapshot_free(eigrp_list_t *routes)
{
	eigrp_route_descriptor_t *route;
	eigrp_list_item_t *node, *nnode;

	if (!routes)
		return;
	for (EIGRP_LIST_ITERATE(routes, node, nnode, route)) {
		eigrp_list_delete_data(routes, route);
		eigrp_topology_route_free(route);
	}
	eigrp_list_delete(&routes);
}


static eigrp_route_descriptor_t *eigrp_topology_query_route_snapshot(
	const eigrp_prefix_descriptor_t *prefix)
{
	eigrp_route_descriptor_t *route;
	eigrp_route_descriptor_t *copy;

	eigrp_topology_route_iterator_t iterator;
	for (route = eigrp_topology_route_iterator_first((eigrp_prefix_descriptor_t *)prefix, &iterator); route;
	     route = eigrp_topology_route_iterator_next(&iterator)) {
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
	eigrp_prefix_t key;
	eigrp_table_node_t *rn;

	if (!eigrp_topology_table_key(&prefix->destination, &key))
		return false;
	rn = eigrp_table_node_lookup(eigrp->topology_table, &key);
	if (!rn)
		return false;

	eigrp_list_delete_data(eigrp->topology_changes, prefix);
	(void)eigrp_rib_route_remove(eigrp, &prefix->destination);
	rn->info = NULL;
	eigrp_table_node_release(rn); /* lookup reference */
	eigrp_table_node_release(rn); /* initial creation reference */
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
	eigrp_list_t *routes)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_list_item_t *node, *nnode;

	if (!routes || routes->count == 0)
		return EIGRP_RESULT_SUCCESS;

	prefix = eigrp_topology_prefix_recreate(
		eigrp, destination, type, reported_metric, EIGRP_FSM_STATE_PASSIVE);
	if (!prefix)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	prefix->distance = distance;
	prefix->fdistance = feasible_distance;
	prefix->rdistance = reported_distance;

	for (EIGRP_LIST_ITERATE(routes, node, nnode, route)) {
		eigrp_list_delete_data(routes, route);
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

	if (eigrp_nbr_count(eigrp) == 0) {
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
	eigrp_list_t *connected_routes = NULL;
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
	eigrp_list_t *destinations;
	eigrp_table_node_t *rn;
	eigrp_prefix_t *destination;
	eigrp_list_item_t *node, *nnode;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_result_t result = EIGRP_RESULT_SUCCESS;
	size_t affected = 0;

	destinations = eigrp_list_create();
	for (rn = eigrp_table_first(eigrp->topology_table); rn; rn = eigrp_table_next(rn)) {
		prefix = rn->info;
		if (!prefix)
			continue;
		destination = calloc(1, sizeof(*destination));
		*destination = prefix->destination;
		eigrp_list_add(destinations, destination);
	}

	for (EIGRP_LIST_ITERATE(destinations, node, nnode, destination)) {
		eigrp_list_delete_data(destinations, destination);
		if (result == EIGRP_RESULT_SUCCESS) {
			prefix = eigrp_topology_table_lookup(eigrp->topology_table,
						       destination);
			if (prefix) {
				result = eigrp_topology_clear_prefix(eigrp, prefix);
				if (result == EIGRP_RESULT_SUCCESS)
					affected++;
			}
		}
		free(destination);
	}
	eigrp_list_delete(&destinations);
	if (affected_count)
		*affected_count = affected;
	return result;
}

/*
 * Syntax:
 *   EXEC: `clear eigrp [AS] [vrf ...] <ipv4|ipv6> topology [PREFIX]`
 * Supported: EXEC
 * Placement:
 *   Privileged operational
 * Description:
 * Clears selected EIGRP topology runtime state without changing retained configuration.
 * The operation is owned by the topology module, not by FRR VTY code.
 */
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

	if (!context->runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config
	    && context->config->afi != EIGRP_AFI_IPV4
	    && context->config->afi != EIGRP_AFI_IPV6)
		return EIGRP_RESULT_UNSUPPORTED;

	if (!request->destination)
		return eigrp_topology_clear_all(context->runtime, affected_count);

	if (request->destination->address.afi
	    != eigrp_instance_afi(context->runtime))
		return EIGRP_RESULT_INVALID_ARGUMENT;
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
	uint32_t count = 0;

	eigrp_topology_route_iterator_t iterator;
	for (route = eigrp_topology_route_iterator_first(prefix, &iterator); route;
	     route = eigrp_topology_route_iterator_next(&iterator))
		if (route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG)
			count++;
	return count;
}

static void eigrp_route_nexthop_export(
	const eigrp_route_descriptor_t *route, eigrp_topology_route_state_t *state)
{
	memset(&state->next_hop, 0, sizeof(state->next_hop));
	if (!route->adv_router)
		return;
	if (route->adv_router->src.afi == AF_INET6) {
		state->next_hop.afi = EIGRP_AFI_IPV6;
		memcpy(state->next_hop.bytes, &route->adv_router->src.ip.v6, 16);
		return;
	}
	state->next_hop.afi = EIGRP_AFI_IPV4;
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
	eigrp_result_t result;

	prefix_state.destination = prefix->destination;
	prefix_state.active = prefix->state != 0;
	prefix_state.feasible_distance = prefix->fdistance;
	prefix_state.successor_count = eigrp_topology_successor_count(prefix);
	prefix_state.serial_number = prefix->serno;

	result = prefix_callback(&prefix_state, arg);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	eigrp_topology_route_iterator_t iterator;
	for (route = eigrp_topology_route_iterator_first(prefix, &iterator); route;
	     route = eigrp_topology_route_iterator_next(&iterator)) {
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
			eigrp_route_nexthop_export(route, &route_state);

		result = route_callback(&route_state, arg);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
	}

	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   EXEC: `show eigrp address-family <ipv4|ipv6> ... topology [TARGET] [all-links]`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Walks EIGRP topology descriptors for show commands.
 * Filtering and presentation are supplied by the caller while topology ownership remains in common EIGRP code.
 */
eigrp_result_t eigrp_topology_state_iterate(
	eigrp_af_instance_t *config, eigrp_instance_t *runtime,
	const eigrp_prefix_t *destination, bool all_links,
	eigrp_topology_prefix_state_cb prefix_callback,
	eigrp_topology_route_state_cb route_callback, void *arg)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_table_node_t *node;
	eigrp_prefix_t lookup;
	eigrp_result_t result;
	bool matched = false;

	if (!prefix_callback || !route_callback || (!config && !runtime))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (!runtime->topology_table)
		return EIGRP_RESULT_NOT_FOUND;

	if (destination) {
		if (!eigrp_topology_table_key(destination, &lookup))
			return EIGRP_RESULT_INVALID_ARGUMENT;
		node = eigrp_table_node_match(runtime->topology_table, &lookup);
		if (!node)
			return EIGRP_RESULT_NOT_FOUND;
		prefix = node->info;
		if (!prefix) {
			eigrp_table_node_release(node);
			return EIGRP_RESULT_NOT_FOUND;
		}
		result = eigrp_topology_state_emit_prefix(runtime, prefix, all_links,
							 prefix_callback, route_callback,
							 arg);
		eigrp_table_node_release(node);
		return result;
	}

	for (node = eigrp_table_first(runtime->topology_table); node;
	     node = eigrp_table_next(node)) {
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

/*
 * Syntax:
 *   EXEC: topology show commands with an optional autonomous-system filter
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Walks every EIGRP runtime matching {AF, VRF, optional AS}.  Management
 * adapters resolve host VRF names to the portable VRF identifier before
 * calling this target; instance selection remains common EIGRP behavior.
 */
eigrp_result_t eigrp_topology_instance_iterate(
	eigrp_afi_t afi, eigrp_vrf_id_t vrf_id, uint16_t asn,
	eigrp_topology_instance_iterate_cb callback, void *arg)
{
	eigrp_instance_t *runtime;
	eigrp_list_item_t *node;
	eigrp_result_t result;
	bool matched = false;

	if (!callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (afi != EIGRP_AFI_IPV4
	    && afi != EIGRP_AFI_IPV6)
		return EIGRP_RESULT_UNSUPPORTED;
	if (!eigrp_om || !eigrp_om->eigrp)
		return EIGRP_RESULT_NOT_FOUND;

	for (EIGRP_LIST_ITERATE_RO(eigrp_om->eigrp, node, runtime)) {
		if (runtime->af_vectors.afi != afi || runtime->vrf_id != vrf_id)
			continue;
		if (asn && runtime->AS != asn)
			continue;

		matched = true;
		result = callback(runtime, arg);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
	}

	return matched ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;
}

static eigrp_result_t
eigrp_topology_context_validate(const eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;


	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `topology base` / removal with address-family cleanup
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Creates or removes the named-mode base-topology configuration object.
 * Topology behavior remains in EIGRP-owned state rather than in the FRR parser.
 */
eigrp_result_t eigrp_topology_create(eigrp_instance_context_t *context)
{
	eigrp_result_t result;

	/*
	 * Topology IDs are a portable EIGRP protocol identity.  Host integrations
	 * may expose only the base topology, but the common API must not reject a
	 * valid non-base TID merely because a particular CLI does not expose it.
	 */
	result = eigrp_topology_context_validate(context);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (context->config)
		context->config->topology_base_configured = true;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `topology base` / removal with address-family cleanup
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Creates or removes the named-mode base-topology configuration object.
 * Topology behavior remains in EIGRP-owned state rather than in the FRR parser.
 */
eigrp_result_t eigrp_topology_delete(eigrp_instance_context_t *context)
{
	eigrp_result_t result;

	result = eigrp_topology_context_validate(context);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (context->config)
		context->config->topology_base_configured = false;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `default-information <in|out> [POLICY]` / `no default-information <in|out> [POLICY]`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Controls default-route acceptance/advertisement for the named topology.
 * A configured access list is evaluated by the host policy adapter.  Policy
 * lookup/evaluation failure is fail-closed for the default route.
 */
static bool eigrp_topology_default_prefix(const eigrp_prefix_t *prefix)
{
	return prefix && prefix->prefix_length == 0
	       && (prefix->address.afi == EIGRP_AFI_IPV4
		   || prefix->address.afi == EIGRP_AFI_IPV6);
}

bool eigrp_topology_default_information_denies(
	eigrp_instance_t *eigrp, eigrp_default_information_direction_t direction,
	const eigrp_prefix_t *prefix)
{
	eigrp_af_instance_t *af;
	eigrp_filter_decision_t decision = EIGRP_FILTER_DECISION_DENY;
	eigrp_result_t result;
	const char *access_list;

	if (!eigrp_topology_default_prefix(prefix))
		return false;
	if (!eigrp || (direction != EIGRP_DEFAULT_INFORMATION_IN
		       && direction != EIGRP_DEFAULT_INFORMATION_OUT))
		return true;

	af = eigrp_instance_runtime_config(eigrp);
	if (!af || !af->default_information_enabled[direction])
		return true;

	access_list = af->default_information_access_list[direction];
	if (!access_list)
		return false;

	result = eigrp_sys_filter_evaluate(eigrp, EIGRP_DISTRIBUTE_ACCESS_LIST,
					   access_list, prefix, &decision);
	return result != EIGRP_RESULT_SUCCESS
	       || decision != EIGRP_FILTER_DECISION_PERMIT;
}

static void eigrp_topology_default_information_refresh(
	eigrp_instance_t *eigrp, eigrp_default_information_direction_t direction)
{
	eigrp_nbr_clear_request_t request;

	if (!eigrp)
		return;

	if (direction == EIGRP_DEFAULT_INFORMATION_OUT) {
		eigrp_update_send_process_GR(eigrp, EIGRP_GR_FILTER);
		return;
	}

	memset(&request, 0, sizeof(request));
	request.soft = true;
	(void)eigrp_nbr_clear(eigrp, &request, NULL, NULL, NULL);
}

eigrp_result_t eigrp_topology_default_information_update(
	eigrp_operation_t operation, eigrp_instance_context_t *context,
	eigrp_default_information_direction_t direction, const char *access_list)
{
	eigrp_af_instance_t *af;
	char *replacement = NULL;
	bool enabled;

	if (operation != EIGRP_SET && operation != EIGRP_RESET)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (direction != EIGRP_DEFAULT_INFORMATION_IN
	    && direction != EIGRP_DEFAULT_INFORMATION_OUT)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (access_list && !access_list[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	af = context->config;
	if (!af && context->runtime)
		af = eigrp_instance_runtime_config(context->runtime);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;

	enabled = operation == EIGRP_SET;
	if (enabled && access_list) {
		replacement = strdup(access_list);
		if (!replacement)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	free(af->default_information_access_list[direction]);
	af->default_information_access_list[direction] = replacement;
	af->default_information_enabled[direction] = enabled;

	if (context->runtime)
		eigrp_topology_default_information_refresh(context->runtime, direction);
	return EIGRP_RESULT_SUCCESS;
}






/*
 * Syntax:
 *   Named: `maximum-prefix LIMIT [...]` / `no maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or removes the topology prefix-limit policy.
 * Configuration is retained and supported runtime limit semantics are enforced.
 */
static uint32_t eigrp_topology_prefix_count(eigrp_instance_t *runtime)
{
	eigrp_table_node_t *node;
	uint32_t count = 0;

	if (!runtime || !runtime->topology_table)
		return 0;
	for (node = eigrp_table_first(runtime->topology_table); node;
	     node = eigrp_table_next(node))
		if (node->info)
			count++;
	return count;
}

bool eigrp_topology_prefix_admit(eigrp_instance_t *runtime,
	const eigrp_prefix_t *prefix)
{
	eigrp_af_instance_t *af;
	const eigrp_prefix_limit_t *limit;

	if (!runtime || !prefix)
		return false;
	if (eigrp_topology_table_lookup(runtime->topology_table, prefix))
		return true;
	af = eigrp_instance_runtime_config(runtime);
	if (!af || !af->topology_maximum_prefix_configured)
		return true;
	limit = &af->topology_maximum_prefix;
	{
		uint32_t count = eigrp_topology_prefix_count(runtime);
		if (eigrp_prefix_limit_threshold_crossed(limit, count))
			eigrp_log(EIGRP_LOG_WARNING,
				  "EIGRP topology maximum-prefix threshold reached (%u/%u)",
				  count + 1U, limit->maximum);
		return eigrp_prefix_limit_allows(limit, count, false);
	}
}

eigrp_result_t eigrp_topology_max_prefix_update(eigrp_operation_t operation, eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (operation == EIGRP_RESET) {
		if (!context->config || !context->config->topology_maximum_prefix_configured)
			return EIGRP_RESULT_NOT_FOUND;
		context->config->topology_maximum_prefix_configured = false;
		memset(&context->config->topology_maximum_prefix, 0,
		       sizeof(context->config->topology_maximum_prefix));
		return EIGRP_RESULT_SUCCESS;
	}
	if (operation != EIGRP_SET || !limit || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (context->config) {
		context->config->topology_maximum_prefix = *limit;
		context->config->topology_maximum_prefix_configured = true;
	}
	return context->runtime && (limit->dampened || limit->reset_time_minutes
		|| limit->restart_minutes || limit->restart_count)
		       ? EIGRP_RESULT_UNSUPPORTED : EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `maximum-paths PATHS` / `no maximum-paths [PATHS]`
 *   Named: `maximum-paths PATHS` / `no maximum-paths`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: topology base mode
 * Description:
 * Sets or resets the number of EIGRP successor paths eligible for installation.
 * The named parser reaches the same EIGRP-owned topology limit instead of a separate named implementation.
 */
eigrp_result_t eigrp_topology_maximum_paths_update(eigrp_operation_t operation, eigrp_instance_context_t *context, uint8_t maximum_paths)
{
	if (operation == EIGRP_RESET) {
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime) {
		context->runtime->max_paths = EIGRP_MAX_PATHS_DEFAULT;
		eigrp_topology_multipath_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (!maximum_paths || maximum_paths > EIGRP_MAX_PATHS_MAX)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime) {
		context->runtime->max_paths = maximum_paths;
		eigrp_topology_multipath_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `maximum-paths PATHS` / `no maximum-paths [PATHS]`
 *   Named: `maximum-paths PATHS` / `no maximum-paths`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: topology base mode
 * Description:
 * Sets or resets the number of EIGRP successor paths eligible for installation.
 * The named parser reaches the same EIGRP-owned topology limit instead of a separate named implementation.
 */


/*
 * Syntax:
 *   Named: `maximum-prefix LIMIT [...]` / `no maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or removes the topology prefix-limit policy.
 * Configuration is retained and supported runtime limit semantics are enforced.
 */

