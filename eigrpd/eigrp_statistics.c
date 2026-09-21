// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP traffic and accounting state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <string.h>



#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_table.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_statistics.h"

static eigrp_result_t
eigrp_statistics_context_validate(const eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (!context->runtime) {
		if (context->config
		    && context->config->afi == EIGRP_ADDRESS_FAMILY_IPV6)
			return EIGRP_RESULT_NOT_IMPLEMENTED;
		return EIGRP_RESULT_NOT_FOUND;
	}
	if (!context->runtime->data_path_ready)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_statistics_neighbor_address(const eigrp_neighbor_t *nbr,
					      eigrp_address_t *address)
{
	memset(address, 0, sizeof(*address));
	if (nbr->src.afi == AF_INET6) {
		address->afi = EIGRP_ADDRESS_FAMILY_IPV6;
		memcpy(address->bytes, &nbr->src.ip.v6, 16);
		return;
	}
	address->afi = EIGRP_ADDRESS_FAMILY_IPV4;
	memcpy(address->bytes, &nbr->src.ip.v4, 4);
}

static uint32_t eigrp_statistics_prefix_count(eigrp_instance_t *runtime)
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

static uint32_t eigrp_statistics_neighbor_prefix_count(
	eigrp_instance_t *runtime, eigrp_neighbor_t *neighbor)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_table_node_t *route_node;
	eigrp_list_node_t *list_node;
	uint32_t count = 0;

	if (!runtime || !runtime->topology_table || !neighbor)
		return 0;

	for (route_node = eigrp_table_first(runtime->topology_table); route_node;
	     route_node = eigrp_table_next(route_node)) {
		prefix = route_node->info;
		if (!prefix)
			continue;
		for (EIGRP_LIST_ELEMENTS_RO(prefix->entries, list_node, route)) {
			if (route->adv_router == neighbor) {
				count++;
				break;
			}
		}
	}
	return count;
}

/*
 * Syntax:
 *   EXEC: `show eigrp address-family <ipv4|ipv6> ... accounting`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Exports EIGRP accounting state for the selected address-family.
 * FRR supplies rendering callbacks only.
 */
eigrp_result_t eigrp_statistics_accounting_show(
	const eigrp_instance_context_t *context, uint32_t *total_prefix_count,
	eigrp_statistics_accounting_cb callback, void *arg)
{
	eigrp_result_t result = eigrp_statistics_context_validate(context);
	eigrp_interface_t *interface;
	eigrp_neighbor_t *neighbor;
	eigrp_list_node_t *interface_node;
	eigrp_list_node_t *neighbor_node;

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (!total_prefix_count || !callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	*total_prefix_count = eigrp_statistics_prefix_count(context->runtime);
	for (EIGRP_LIST_ELEMENTS_RO(context->runtime->eiflist, interface_node,
				  interface)) {
		for (EIGRP_LIST_ELEMENTS_RO(interface->nbrs, neighbor_node, neighbor)) {
			eigrp_statistics_accounting_state_t state = {0};

			if (neighbor->state == EIGRP_NEIGHBOR_DOWN)
				continue;
			eigrp_statistics_neighbor_address(neighbor,
							  &state.neighbor_address);
			state.interface_name = eigrp_intf_name_string(interface);
			state.neighbor_state = eigrp_nbr_state_str(neighbor);
			state.prefix_count = eigrp_statistics_neighbor_prefix_count(
				context->runtime, neighbor);
			result = callback(&state, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
	}

	/* An empty adjacency table is still valid accounting output. */
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   EXEC: `show eigrp address-family <ipv4|ipv6> ... traffic`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Exports EIGRP packet and protocol traffic counters.
 * Counter ownership remains with the EIGRP runtime.
 */
eigrp_result_t eigrp_statistics_traffic_show(
	const eigrp_instance_context_t *context,
	eigrp_statistics_traffic_state_t *state)
{
	eigrp_result_t result = eigrp_statistics_context_validate(context);
	eigrp_interface_t *interface;
	eigrp_list_node_t *node;

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (!state)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	memset(state, 0, sizeof(*state));
	/* Validated packet I/O owns all IPv4 traffic counters. */
	state->sent_valid = EIGRP_STATISTICS_TRAFFIC_ACK
			    | EIGRP_STATISTICS_TRAFFIC_HELLO
			    | EIGRP_STATISTICS_TRAFFIC_UPDATE
			    | EIGRP_STATISTICS_TRAFFIC_QUERY
			    | EIGRP_STATISTICS_TRAFFIC_REPLY
			    | EIGRP_STATISTICS_TRAFFIC_SIA_QUERY
			    | EIGRP_STATISTICS_TRAFFIC_SIA_REPLY;
	state->received_valid = state->sent_valid;
	for (EIGRP_LIST_ELEMENTS_RO(context->runtime->eiflist, node, interface)) {
		state->sent_ack += interface->stats.sent.ack;
		state->sent_hello += interface->stats.sent.hello;
		state->sent_query += interface->stats.sent.query;
		state->sent_reply += interface->stats.sent.reply;
		state->sent_update += interface->stats.sent.update;
		state->sent_sia_query += interface->stats.sent.siaQuery;
		state->sent_sia_reply += interface->stats.sent.siaReply;
		state->received_ack += interface->stats.rcvd.ack;
		state->received_hello += interface->stats.rcvd.hello;
		state->received_query += interface->stats.rcvd.query;
		state->received_reply += interface->stats.rcvd.reply;
		state->received_update += interface->stats.rcvd.update;
		state->received_sia_query += interface->stats.rcvd.siaQuery;
		state->received_sia_reply += interface->stats.rcvd.siaReply;
	}
	return EIGRP_RESULT_SUCCESS;
}
