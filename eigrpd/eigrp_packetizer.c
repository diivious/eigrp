// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP packetizer work queue.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdlib.h>
#include <string.h>
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_packetizer.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_debug.h"
#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_filter.h"
typedef struct eigrp_packetizer_builder {
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	eigrp_neighbor_t *nbr;
	eigrp_packet_encoder_t encoder;
	eigrp_packet_t *packet;
	uint8_t opcode;
	uint32_t flags;
	uint16_t packet_limit;
	uint32_t route_count;
} eigrp_packetizer_builder_t;

static bool eigrp_packetizer_opcode_valid(uint8_t opcode)
{
	switch (opcode) {
	case EIGRP_OPC_UPDATE:
	case EIGRP_OPC_QUERY:
	case EIGRP_OPC_REPLY:
	case EIGRP_OPC_SIAQUERY:
	case EIGRP_OPC_SIAREPLY:
		return true;
	default:
		return false;
	}
}

static bool eigrp_packetizer_interface_has_up_neighbors(eigrp_interface_t *ei)
{
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *node;

	if (!ei || !ei->nbrs)
		return false;

	for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
		if (nbr->state == EIGRP_NEIGHBOR_UP)
			return true;
	}
	return false;
}

static eigrp_route_descriptor_t *
eigrp_packetizer_poison_route_create(eigrp_prefix_descriptor_t *prefix)
{
	eigrp_route_descriptor_t *route;

	if (!prefix || !eigrp_prefix_valid(&prefix->destination))
		return NULL;

	route = eigrp_topology_route_create(NULL);
	if (!route)
		return NULL;

	route->prefix = prefix;
	route->dest = prefix->destination;
	route->type = (prefix->nt == EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL)
			      ? EIGRP_TLV_IPv4_EXT
			      : EIGRP_TLV_IPv4_INT;
	route->metric = prefix->reported_metric;
	route->metric.delay = EIGRP_MAX_METRIC;
	route->metric.flags = 0;

	return route;
}

static eigrp_route_descriptor_t *eigrp_packetizer_route_select(
	eigrp_prefix_descriptor_t *prefix, eigrp_route_descriptor_t *route_hint,
	uint8_t opcode, bool *owned)
{
	eigrp_route_descriptor_t *route = route_hint;
	eigrp_list_t *successors = NULL;

	*owned = false;
	if (route)
		return route;

	if (opcode == EIGRP_OPC_QUERY) {
		successors = eigrp_topology_get_successor(prefix);
		if (successors)
			route = eigrp_list_node_data(eigrp_list_head(successors));
		if (successors)
			eigrp_list_delete(&successors);
	} else if (prefix && prefix->entries) {
		route = eigrp_list_node_data(eigrp_list_head(prefix->entries));
	}

	if (!route) {
		route = eigrp_packetizer_poison_route_create(prefix);
		*owned = route != NULL;
	}
	return route;
}

static bool eigrp_packetizer_builder_start(eigrp_packetizer_builder_t *builder)
{
	uint32_t sequence;

	if (!builder || !builder->eigrp || !builder->ei || !builder->encoder)
		return false;

	sequence = eigrp_packet_sequence_reserve(builder->eigrp);
	builder->packet = eigrp_packet_new(builder->packet_limit, builder->nbr);
	if (!builder->packet)
		return false;

	eigrp_packet_header_init(builder->opcode, builder->eigrp,
				 builder->packet->s, builder->flags, sequence, 0);
	if (builder->ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
	    && builder->ei->params.auth_keychain != NULL)
		eigrp_add_authTLV_MD5_encode(builder->packet->s, builder->ei);

	builder->packet->sequence_number = sequence;
	builder->route_count = 0;
	return true;
}

static void eigrp_packetizer_builder_flush(eigrp_packetizer_builder_t *builder)
{
	eigrp_packet_t *packet;
	uint16_t length;

	if (!builder || !builder->packet)
		return;

	packet = builder->packet;
	builder->packet = NULL;
	if (!builder->route_count) {
		eigrp_packet_free(packet);
		return;
	}

	length = (uint16_t)eigrp_stream_get_endp(packet->s);
	if (builder->ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
	    && builder->ei->params.auth_keychain != NULL)
		eigrp_make_md5_digest(builder->ei, packet->s,
				      EIGRP_AUTH_UPDATE_FLAG);

	eigrp_packet_checksum(builder->ei, packet->s, length);
	packet->length = length;

	if (builder->nbr) {
		bool queue_was_empty = builder->nbr->retrans_queue->count == 0;

		eigrp_addr_copy(&packet->dst, &builder->nbr->src);
		eigrp_packet_enqueue(builder->nbr->retrans_queue, packet);
		eigrp_debug_transmit_event(
			EIGRP_DEBUG_TRANSMIT_LINK, builder->eigrp, builder->ei,
			builder->nbr,
			"linked opcode %u seq %u to reliable queue (depth %lu)",
			builder->opcode, packet->sequence_number,
			builder->nbr->retrans_queue->count);
		if (queue_was_empty)
			eigrp_packet_send_reliably(builder->eigrp, builder->nbr);
		return;
	}

	eigrp_packet_multicast_reliable_enqueue(builder->eigrp, builder->ei,
						 packet);
}

static int eigrp_packetizer_builder_route_add(
	eigrp_packetizer_builder_t *builder, eigrp_route_descriptor_t *route)
{
	int encoded;

	if (!builder || !route)
		return 0;
	if (!builder->packet && !eigrp_packetizer_builder_start(builder))
		return 0;

	encoded = eigrp_packet_route_encode_append(
		builder->eigrp, builder->ei, builder->nbr, builder->encoder,
		builder->packet->s, route, builder->packet_limit);
	if (encoded >= 0) {
		if (encoded > 0)
			builder->route_count++;
		return encoded;
	}

	/* The next complete TLV does not fit.  Finish the current immutable
	 * packet and retry that destination in a fresh packet. */
	if (!builder->route_count)
		return 0;

	eigrp_packetizer_builder_flush(builder);
	if (!eigrp_packetizer_builder_start(builder))
		return 0;

	encoded = eigrp_packet_route_encode_append(
		builder->eigrp, builder->ei, builder->nbr, builder->encoder,
		builder->packet->s, route, builder->packet_limit);
	if (encoded > 0)
		builder->route_count++;
	else if (encoded < 0)
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP route TLV exceeds packet limit %u",
			  builder->ei->name, builder->packet_limit);
	return encoded > 0 ? encoded : 0;
}

static void eigrp_packetizer_builder_init(eigrp_packetizer_builder_t *builder,
					   eigrp_instance_t *eigrp,
					   eigrp_interface_t *ei,
					   eigrp_neighbor_t *nbr,
					   uint8_t opcode, uint32_t flags)
{
	memset(builder, 0, sizeof(*builder));
	builder->eigrp = eigrp;
	builder->ei = ei;
	builder->nbr = nbr;
	builder->opcode = opcode;
	builder->flags = flags;
	builder->packet_limit = eigrp_packet_payload_limit(ei->curr_mtu);
	builder->encoder = nbr ? nbr->encoder : ei->encoder;
}

static bool eigrp_packetizer_prefix_allowed(eigrp_instance_t *eigrp,
					     eigrp_interface_t *ei,
					     eigrp_prefix_descriptor_t *prefix,
					     eigrp_route_descriptor_t *route,
					     uint8_t opcode)
{
	if (!eigrp || !ei || !prefix || !route)
		return false;

	if (eigrp_filter_prefix_apply(eigrp, ei, EIGRP_FILTER_OUT,
				      &prefix->destination))
		return false;

	if ((opcode == EIGRP_OPC_UPDATE || opcode == EIGRP_OPC_QUERY)
	    && eigrp_nbr_split_horizon_check(route, ei))
		return false;

	return true;
}

static void eigrp_packetizer_query_rij_add(eigrp_prefix_descriptor_t *prefix,
					    eigrp_interface_t *ei)
{
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *node;

	if (!prefix || !prefix->rij || !ei)
		return;

	for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
		if (nbr->state != EIGRP_NEIGHBOR_UP)
			continue;
		if (!eigrp_list_lookup(prefix->rij, nbr))
			eigrp_list_add(prefix->rij, nbr);
	}
}

static void eigrp_packetizer_interface_prefix_send(
	eigrp_instance_t *eigrp, eigrp_interface_t *ei,
	eigrp_packetizer_work_t *work)
{
	eigrp_packetizer_builder_t builder;
	eigrp_route_descriptor_t *route;
	bool owned;

	if (!eigrp || !ei || !work || !work->prefix
	    || !eigrp_packetizer_interface_has_up_neighbors(ei)
	    || work->exception == ei)
		return;

	route = eigrp_packetizer_route_select(work->prefix, work->route,
					      work->opcode, &owned);
	if (!route)
		return;

	eigrp_packetizer_builder_init(&builder, eigrp, ei, NULL, work->opcode, 0);
	if (eigrp_packetizer_prefix_allowed(eigrp, ei, work->prefix, route,
					    work->opcode)
	    && eigrp_packetizer_builder_route_add(&builder, route) > 0
	    && work->opcode == EIGRP_OPC_QUERY)
		eigrp_packetizer_query_rij_add(work->prefix, ei);
	eigrp_packetizer_builder_flush(&builder);

	if (owned)
		eigrp_topology_route_free(route);
}

static void eigrp_packetizer_neighbor_route_send(eigrp_instance_t *eigrp,
						 eigrp_packetizer_work_t *work)
{
	eigrp_packetizer_builder_t builder;
	eigrp_neighbor_t *nbr;
	eigrp_route_descriptor_t *route;
	bool owned;

	if (!eigrp || !work || !work->nbr || !work->prefix)
		return;

	nbr = work->nbr;
	if (!nbr->ei || nbr->state != EIGRP_NEIGHBOR_UP || !nbr->retrans_queue)
		return;

	route = eigrp_packetizer_route_select(work->prefix, work->route,
					      work->opcode, &owned);
	if (!route)
		return;

	eigrp_packetizer_builder_init(&builder, eigrp, nbr->ei, nbr,
				      work->opcode, 0);
	eigrp_packetizer_builder_route_add(&builder, route);
	eigrp_packetizer_builder_flush(&builder);

	if (owned)
		eigrp_topology_route_free(route);
}

static void eigrp_packetizer_changes_send(eigrp_instance_t *eigrp,
					   eigrp_packetizer_work_t *work,
					   uint32_t action)
{
	eigrp_interface_t *ei;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_list_node_t *inode, *pnode, *pnnode;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, inode, ei)) {
		eigrp_packetizer_builder_t builder;

		if (work->exception == ei
		    || !eigrp_packetizer_interface_has_up_neighbors(ei))
			continue;

		eigrp_packetizer_builder_init(&builder, eigrp, ei, NULL,
					      work->opcode, 0);
		for (EIGRP_LIST_ELEMENTS_RO(eigrp->topology_changes, pnode, prefix)) {
			eigrp_route_descriptor_t *route;
			bool owned;

			if (!(prefix->req_action & action))
				continue;

			route = eigrp_packetizer_route_select(prefix, NULL,
							      work->opcode, &owned);
			if (!route)
				continue;

			if (eigrp_packetizer_prefix_allowed(eigrp, ei, prefix, route,
							    work->opcode)
			    && eigrp_packetizer_builder_route_add(&builder, route) > 0
			    && work->opcode == EIGRP_OPC_QUERY)
				eigrp_packetizer_query_rij_add(prefix, ei);

			if (owned)
				eigrp_topology_route_free(route);
		}
		eigrp_packetizer_builder_flush(&builder);
	}

	/* The work bead represents the complete current topology-change set.  Do
	 * not clear actions until every eligible interface has inspected it. */
	for (EIGRP_LIST_ELEMENTS(eigrp->topology_changes, pnode, pnnode, prefix)) {
		if (!(prefix->req_action & action))
			continue;
		prefix->req_action &= ~action;
		if (!prefix->req_action)
			eigrp_list_delete_data(eigrp->topology_changes, prefix);
	}
}

static void eigrp_packetizer_query_send(eigrp_instance_t *eigrp,
					 eigrp_packetizer_work_t *work)
{
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (work->nbr) {
		eigrp_packetizer_neighbor_route_send(eigrp, work);
		return;
	}

	if (!work->prefix) {
		eigrp_packetizer_changes_send(eigrp, work, EIGRP_FSM_NEED_QUERY);
		return;
	}

	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei))
		eigrp_packetizer_interface_prefix_send(eigrp, ei, work);
}

static void eigrp_packetizer_work_process(eigrp_instance_t *eigrp,
					  eigrp_packetizer_work_t *work)
{
	if (!eigrp || !work)
		return;

	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_PACKETIZE, eigrp,
				   work->nbr ? work->nbr->ei : work->exception,
				   work->nbr, "process opcode %u%s", work->opcode,
				   work->prefix ? " with route work" : "");

	if (work->flags & EIGRP_PACKETIZER_WORK_F_DEFER_FREE)
		return;

	switch (work->opcode) {
	case EIGRP_OPC_UPDATE:
		if (work->nbr)
			eigrp_packetizer_neighbor_route_send(eigrp, work);
		else if (work->prefix)
			eigrp_packetizer_query_send(eigrp, work);
		else
			eigrp_packetizer_changes_send(eigrp, work,
						      EIGRP_FSM_NEED_UPDATE);
		break;
	case EIGRP_OPC_QUERY:
		eigrp_packetizer_query_send(eigrp, work);
		break;
	case EIGRP_OPC_SIAQUERY:
	case EIGRP_OPC_REPLY:
	case EIGRP_OPC_SIAREPLY:
		eigrp_packetizer_neighbor_route_send(eigrp, work);
		break;
	default:
		break;
	}
}

static eigrp_work_queue_result_t eigrp_packetizer_work_queue_run(
	eigrp_work_queue_t *queue, void *data)
{
	eigrp_packetizer_work_t *work = data;
	eigrp_instance_t *eigrp = eigrp_work_queue_eigrp(queue);

	eigrp_packetizer_work_process(eigrp, work);
	return EIGRP_WORK_QUEUE_SUCCESS;
}

static void eigrp_packetizer_work_queue_delete(eigrp_work_queue_t *queue,
					       void *data)
{
	(void)queue;
	eigrp_packetizer_work_free(data);
}

void eigrp_packetizer_init(eigrp_instance_t *eigrp)
{
	if (!eigrp || eigrp->packetizer_queue)
		return;

	eigrp->packetizer_queue = eigrp_work_queue_new(
		eigrp, "eigrp packetizer", eigrp_packetizer_work_queue_run,
		eigrp_packetizer_work_queue_delete);
}

void eigrp_packetizer_finish(eigrp_instance_t *eigrp)
{
	if (!eigrp)
		return;

	eigrp_work_queue_free(eigrp->packetizer_queue);
	eigrp->packetizer_queue = NULL;
}

eigrp_packetizer_work_t *eigrp_packetizer_work_new(uint8_t opcode)
{
	eigrp_packetizer_work_t *work;

	work = calloc(1, sizeof(*work));
	work->opcode = opcode;
	return work;
}

void eigrp_packetizer_work_free(eigrp_packetizer_work_t *work)
{
	if (!work)
		return;

	if ((work->flags & EIGRP_PACKETIZER_WORK_F_OWN_ROUTE) && work->route)
		eigrp_topology_route_free(work->route);

	if ((work->flags & EIGRP_PACKETIZER_WORK_F_OWN_PREFIX) && work->prefix)
		eigrp_topology_prefix_free(work->prefix);

	free(work);
}

void eigrp_packetizer_enqueue(eigrp_instance_t *eigrp,
			      eigrp_packetizer_work_t *work)
{
	if (!eigrp || !work)
		return;

	if (!eigrp_packetizer_opcode_valid(work->opcode)) {
		eigrp_packetizer_work_free(work);
		return;
	}

	if (!eigrp->packetizer_queue)
		eigrp_packetizer_init(eigrp);

	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_PACKETIZE, eigrp,
				   work->nbr ? work->nbr->ei : work->exception,
				   work->nbr, "enqueue opcode %u%s", work->opcode,
				   work->prefix ? " route work" : "");
	eigrp_work_queue_enqueue(eigrp->packetizer_queue, work);
}

void eigrp_packetizer_prefix_defer_free(eigrp_instance_t *eigrp,
				       eigrp_prefix_descriptor_t *prefix)
{
	eigrp_packetizer_work_t *work;

	if (!eigrp || !prefix)
		return;
	work = eigrp_packetizer_work_new(EIGRP_OPC_UPDATE);
	work->prefix = prefix;
	work->owner = prefix;
	work->flags = EIGRP_PACKETIZER_WORK_F_OWN_PREFIX
		      | EIGRP_PACKETIZER_WORK_F_DEFER_FREE;
	eigrp_packetizer_enqueue(eigrp, work);
}
