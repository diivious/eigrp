// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Sending and Receiving EIGRP Update Packets.
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
#include <string.h>
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_table.h"

#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_packetizer.h"
#include "eigrpd/eigrp_prefix.h"

#include "eigrpd/eigrp_debug.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_metric.h"


/**
 * @fn remove_received_prefix_gr
 *
 * @param[in]		nbr_prefixes	List of neighbor prefixes
 * @param[in]		recv_prefix 	Prefix which needs to be removed from
 * list
 *
 * @return void
 *
 * @par
 * Function is used for removing received prefix
 * from list of neighbor prefixes
 */
static void remove_received_prefix_gr(eigrp_list_t *nbr_prefixes,
				      eigrp_prefix_descriptor_t *recv_prefix)
{
	eigrp_list_node_t *node1, *node11;
	eigrp_prefix_descriptor_t *prefix = NULL;

	/* iterate over all prefixes in list */
	for (EIGRP_LIST_ELEMENTS(nbr_prefixes, node1, node11, prefix)) {
		/* remove prefix from list if found */
		if (prefix == recv_prefix) {
			eigrp_list_delete_data(nbr_prefixes, prefix);
		}
	}
}

/**
 * @fn eigrp_update_receive_GR_ask
 *
 * @param[in]		eigrp			EIGRP process
 * @param[in]		nbr 			Neighbor update of who we
 * received
 * @param[in]		nbr_prefixes 	Prefixes which weren't advertised
 *
 * @return void
 *
 * @par
 * Function is used for notifying FSM about prefixes which
 * weren't advertised by neighbor:
 * We will send message to FSM with prefix delay set to infinity.
 */
static void eigrp_update_receive_GR_ask(eigrp_instance_t *eigrp,
					eigrp_neighbor_t *nbr,
					eigrp_list_t *nbr_prefixes)
{
	eigrp_list_node_t *node1;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_fsm_action_message_t fsm_msg;

	/* iterate over all prefixes which weren't advertised by neighbor */
	for (EIGRP_LIST_ELEMENTS_RO(nbr_prefixes, node1, prefix)) {
		char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";

		eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
				      &prefix->destination);
		eigrp_log(EIGRP_LOG_DEBUG, "GR receive: Neighbor not advertised %s", prefix_buf);

		fsm_msg.metrics = prefix->reported_metric;
		/* set delay to MAX */
		fsm_msg.metrics.delay = EIGRP_MAX_METRIC;

		eigrp_route_descriptor_t *route =
			eigrp_prefix_descriptor_lookup(prefix->entries, nbr);

		fsm_msg.packet_type = EIGRP_OPC_UPDATE;
		fsm_msg.eigrp = eigrp;
		fsm_msg.data_type = EIGRP_INT;
		fsm_msg.adv_router = nbr;
		fsm_msg.route = route;
		fsm_msg.prefix = prefix;

		/* send message to FSM */
		eigrp_fsm_event(&fsm_msg);
	}
}

/*
 * EIGRP UPDATE read function
 */
void eigrp_update_receive(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
			  struct eigrp_header *eigrph, eigrp_stream_t *pkt,
			  eigrp_interface_t *ei, int length)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	uint32_t flags;
	uint8_t same;
	uint8_t graceful_restart;
	uint8_t graceful_restart_final;
	eigrp_list_t *nbr_prefixes = NULL;


	flags = ntohl(eigrph->flags);

	same = 0;
	graceful_restart = 0;
	graceful_restart_final = 0;
	if ((nbr->recv_sequence_number) == (ntohl(eigrph->sequence)))
		same = 1;

	nbr->recv_sequence_number = ntohl(eigrph->sequence);


	if ((flags == (EIGRP_INIT_FLAG + EIGRP_RS_FLAG + EIGRP_EOT_FLAG)) && (!same)) {
		/* Graceful restart Update received with all routes */
		eigrp_debug_nsf_event(eigrp, nbr, flags,
				      "peer graceful restart complete in one UPDATE");
		if (eigrp->log_neighbor_changes)
			eigrp_log(EIGRP_LOG_INFO, "Neighbor %s (%s) is resync: peer graceful-restart",
				  eigrp_print_addr(&nbr->src),
				  nbr->ei->name);

		/* get all prefixes from neighbor from topology table */
		nbr_prefixes = eigrp_neighbor_prefixes_lookup(eigrp, nbr);
		graceful_restart = 1;
		graceful_restart_final = 1;

	} else if ((flags == (EIGRP_INIT_FLAG + EIGRP_RS_FLAG)) && (!same)) {
		/* Graceful restart Update received, routes also in next packet
		 */
		eigrp_debug_nsf_event(eigrp, nbr, flags,
				      "peer graceful restart started");
		if (eigrp->log_neighbor_changes)
			eigrp_log(EIGRP_LOG_INFO, "Neighbor %s (%s) is resync: peer graceful-restart",
				  eigrp_print_addr(&nbr->src),
				  nbr->ei->name);

		/* get all prefixes from neighbor from topology table */
		nbr_prefixes = eigrp_neighbor_prefixes_lookup(eigrp, nbr);

		/* save prefixes to neighbor for later use */
		nbr->nbr_gr_prefixes = nbr_prefixes;
		graceful_restart = 1;
		graceful_restart_final = 0;

	} else if ((flags == (EIGRP_EOT_FLAG)) && (!same)) {
		/* If there was INIT+RS Update packet before,
		 *  consider this as GR EOT */
		if (nbr->nbr_gr_prefixes != NULL) {
			/* this is final packet of GR */
			eigrp_debug_nsf_event(eigrp, nbr, flags,
					      "peer graceful restart EOT received");
			nbr_prefixes = nbr->nbr_gr_prefixes;
			nbr->nbr_gr_prefixes = NULL;

			graceful_restart = 1;
			graceful_restart_final = 1;
		}

	} else if ((flags == (0)) && (!same)) {
		/* If there was INIT+RS Update packet before,
		 *  consider this as GR not final packet */
		if (nbr->nbr_gr_prefixes != NULL) {
			/* this is GR not final route packet */
			eigrp_debug_nsf_event(eigrp, nbr, flags,
					      "peer graceful restart continuation");
			nbr_prefixes = nbr->nbr_gr_prefixes;

			graceful_restart = 1;
			graceful_restart_final = 0;
		}

	} else if ((flags & EIGRP_INIT_FLAG) && (!same)) {
		/*
		 * When in pending state, send INIT update only if it wasn't
		 * already sent before (only if init_sequence is 0)
		 */
		if ((nbr->state == EIGRP_NEIGHBOR_PENDING)
		    && (nbr->init_sequence_number == 0))
			eigrp_update_send_init(eigrp, nbr);

		if (nbr->state == EIGRP_NEIGHBOR_UP) {
			eigrp_debug_nsf_event(eigrp, nbr, flags, "peer restarted");
			eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
			eigrp_topology_neighbor_down(nbr->ei->eigrp, nbr);
			nbr->recv_sequence_number = ntohl(eigrph->sequence);
			if (eigrp->log_neighbor_changes)
				eigrp_log(EIGRP_LOG_INFO, "Neighbor %s (%s) is down: peer restarted",
					  eigrp_print_addr(&nbr->src),
					  nbr->ei->name);
			eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_PENDING);
			eigrp_update_send_init(eigrp, nbr);
		}
	}

	/*If there is topology information*/
	while (pkt->endp > pkt->getp) {
		route = (nbr->decoder)(eigrp, nbr, pkt, length);

		// should have got route off the packet, but one never knows
		if (route) {
			prefix = eigrp_topology_table_lookup(eigrp->topology_table, &route->dest);
			/*if exists it comes to DUAL*/
			if (prefix != NULL) {
				/* remove received prefix from neighbor prefix
				 * list if in GR */
				if (graceful_restart)
					remove_received_prefix_gr(nbr_prefixes, prefix);

				struct eigrp_fsm_action_message msg;
				eigrp_route_descriptor_t *received_route = route;
				eigrp_route_descriptor_t *topology_route =
					eigrp_prefix_descriptor_lookup(prefix->entries, nbr);
				bool free_received_route = false;

				if (topology_route) {
					topology_route->type = received_route->type;
					topology_route->nexthop = received_route->nexthop;
					topology_route->extdata = received_route->extdata;
					route = topology_route;
					free_received_route = true;
				} else {
					received_route->adv_router = nbr;
					received_route->prefix = prefix;
				}

				msg.packet_type = EIGRP_OPC_UPDATE;
				msg.eigrp = eigrp;
				msg.data_type = (received_route->type == EIGRP_TLV_IPv4_EXT)
							? EIGRP_EXT
							: EIGRP_INT;
				msg.adv_router = nbr;
				msg.metrics = received_route->metric;
				msg.route = route;
				msg.prefix = prefix;
				eigrp_fsm_event(&msg);

				if (free_received_route)
					eigrp_topology_route_free(received_route);

			} else {
				/*Here comes topology information save*/
				prefix = eigrp_topology_prefix_create();
				prefix->serno = eigrp->serno;
				prefix->destination = route->dest;
				eigrp_prefix_normalize(&prefix->destination);
				prefix->state = EIGRP_FSM_STATE_PASSIVE;
				prefix->nt = (route->type == EIGRP_TLV_IPv4_EXT)
						     ? EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL
						     : EIGRP_TOPOLOGY_TYPE_REMOTE;

				route->adv_router = nbr;
				/*
				 * Seed the neighbor route descriptor from the metric decoded
				 * from the UPDATE TLV. The prefix object was just created
				 * and its reported metric is local topology state, not the
				 * neighbor's advertised RD.
				 */
				route->reported_metric = route->metric;
				route->reported_distance = eigrp_calculate_metrics(eigrp, route->reported_metric);

				/*
				 * Filtering
				 */
				if (eigrp_filter_prefix_apply(eigrp, ei, EIGRP_FILTER_IN, &route->dest))
					route->reported_metric.delay = EIGRP_MAX_METRIC;

				route->distance = eigrp_calculate_total_metrics(eigrp, route);
				prefix->fdistance = prefix->distance = prefix->rdistance = route->distance;
				route->prefix = prefix;
				route->flags = EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;

				eigrp_prefix_descriptor_add(eigrp->topology_table, prefix);
				eigrp_route_descriptor_add(eigrp, prefix, route);

				prefix->distance = route->distance;
				prefix->fdistance = route->distance;
				prefix->rdistance = route->distance;

				prefix->reported_metric = route->total_metric;
				eigrp_topology_update_node_flags(eigrp, prefix);

				prefix->req_action |= EIGRP_FSM_NEED_UPDATE;
				eigrp_list_add(eigrp->topology_changes, prefix);
			}
			break;
		}
	}

	/* ask about prefixes not present in GR update,
	 * if this is final GR packet */
	if (graceful_restart_final) {
		eigrp_update_receive_GR_ask(eigrp, nbr, nbr_prefixes);
	}

	/*
	 * We don't need to send separate Ack for INIT Update. INIT will be
	 * acked in EOT Update.
	 */
	if ((nbr->state == EIGRP_NEIGHBOR_UP) && !(flags == EIGRP_INIT_FLAG)) {
		eigrp_hello_send_ack(nbr);
	}

	eigrp_query_send_all(eigrp);
	eigrp_update_send_all(eigrp, ei);

	if (nbr_prefixes)
		eigrp_list_delete(&nbr_prefixes);
}

/* Build one adjacency-specific UPDATE wire image. */
static eigrp_packet_t *eigrp_update_neighbor_packet_new(eigrp_neighbor_t *nbr,
                                                        uint32_t flags)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	eigrp_packet_t *packet;
	uint32_t sequence;

	if (!nbr || !nbr->ei)
		return NULL;

	ei = nbr->ei;
	eigrp = ei->eigrp;
	sequence = eigrp_packet_sequence_reserve(eigrp);
	packet = eigrp_packet_new(eigrp_packet_payload_limit(ei->curr_mtu), nbr);
	if (!packet)
		return NULL;

	eigrp_packet_header_init(EIGRP_OPC_UPDATE, eigrp, packet->s, flags,
				 sequence, nbr->recv_sequence_number);
	if (ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
	    && ei->params.auth_keychain != NULL)
		eigrp_add_authTLV_MD5_encode(packet->s, ei);

	packet->sequence_number = sequence;
	eigrp_addr_copy(&packet->dst, &nbr->src);
	return packet;
}

static void eigrp_update_neighbor_packet_queue(eigrp_neighbor_t *nbr,
                                                eigrp_packet_t *packet)
{
	eigrp_interface_t *ei;
	eigrp_instance_t *eigrp;
	struct eigrp_header *header;
	uint8_t auth_flags;
	uint16_t length;
	bool queue_was_empty;

	if (!nbr || !packet)
		return;

	ei = nbr->ei;
	eigrp = ei->eigrp;
	header = (struct eigrp_header *)eigrp_stream_data(packet->s);
	auth_flags = (ntohl(header->flags) & EIGRP_INIT_FLAG)
			     ? EIGRP_AUTH_UPDATE_INIT_FLAG
			     : EIGRP_AUTH_UPDATE_FLAG;
	length = (uint16_t)eigrp_stream_get_endp(packet->s);
	if (ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
	    && ei->params.auth_keychain != NULL)
		eigrp_make_md5_digest(ei, packet->s, auth_flags);

	eigrp_packet_checksum(ei, packet->s, length);
	packet->length = length;
	queue_was_empty = nbr->retrans_queue->count == 0;
	eigrp_packet_enqueue(nbr->retrans_queue, packet);
	eigrp_debug_transmit_event(
		EIGRP_DEBUG_TRANSMIT_LINK, eigrp, ei, nbr,
		"linked UPDATE seq %u to reliable queue (depth %lu)",
		packet->sequence_number, nbr->retrans_queue->count);
	if (queue_was_empty)
		eigrp_packet_send_reliably(eigrp, nbr);
}

/* send EIGRP INIT Update packet */
void eigrp_update_send_init(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr)
{
	eigrp_packet_t *packet;

	if (!eigrp || !nbr || !nbr->ei)
		return;

	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_STARTUP, eigrp, nbr->ei,
				   nbr, "build INIT UPDATE");
	packet = eigrp_update_neighbor_packet_new(nbr, EIGRP_INIT_FLAG);
	if (!packet)
		return;

	nbr->init_sequence_number = packet->sequence_number;
	eigrp_update_neighbor_packet_queue(nbr, packet);
}

void eigrp_update_send_EOT(eigrp_neighbor_t *nbr)
{
	eigrp_packet_t *packet;
	eigrp_route_descriptor_t *route;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_list_node_t *node, *nnode;
	eigrp_interface_t *ei;
	eigrp_instance_t *eigrp;
	eigrp_table_node_t *rn;
	uint16_t packet_limit;
	uint32_t route_count = 0;

	if (!nbr || !nbr->ei)
		return;

	ei = nbr->ei;
	eigrp = ei->eigrp;
	packet_limit = eigrp_packet_payload_limit(ei->curr_mtu);
	packet = eigrp_update_neighbor_packet_new(nbr, 0);
	if (!packet)
		return;

	for (rn = eigrp_table_first(eigrp->topology_table); rn; rn = eigrp_table_next(rn)) {
		if (!rn->info)
			continue;

		prefix = rn->info;
		for (EIGRP_LIST_ELEMENTS(prefix->entries, node, nnode, route)) {
			int encoded;

			if (eigrp_nbr_split_horizon_check(route, ei)
			    || eigrp_filter_prefix_apply(eigrp, ei, EIGRP_FILTER_OUT,
							 &prefix->destination))
				continue;

			encoded = eigrp_packet_route_encode_append(
				eigrp, ei, nbr, nbr->encoder, packet->s, route,
				packet_limit);
			if (encoded < 0 && route_count) {
				eigrp_update_neighbor_packet_queue(nbr, packet);
				packet = eigrp_update_neighbor_packet_new(nbr, 0);
				if (!packet)
					return;
				route_count = 0;
				encoded = eigrp_packet_route_encode_append(
					eigrp, ei, nbr, nbr->encoder, packet->s, route,
					packet_limit);
			}
			if (encoded > 0)
				route_count++;
			else if (encoded < 0)
				eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP route TLV exceeds packet limit %u",
					  ei->name, packet_limit);
		}
	}

	/* EOT belongs only on the final packet in the initial table walk. */
	((struct eigrp_header *)eigrp_stream_data(packet->s))->flags =
		htonl(EIGRP_EOT_FLAG);
	eigrp_update_neighbor_packet_queue(nbr, packet);
}

void eigrp_update_send_all(eigrp_instance_t *eigrp, eigrp_interface_t *exception)
{
	eigrp_packetizer_work_t *work;

	work = eigrp_packetizer_work_new(EIGRP_OPC_UPDATE);
	work->exception = exception;
	eigrp_packetizer_enqueue(eigrp, work);
}

/**
 * @fn eigrp_update_send_GR_part
 *
 * @param[in]		nbr		contains neighbor who would receive
 * Graceful
 * restart
 *
 * @return void
 *
 * @par
 * Function used for sending Graceful restart Update packet
 * and if there are multiple chunks, send only one of them.
 * It is called from event. Do not call it directly.
 *
 * Uses nbr_gr_packet_type from neighbor.
 */
static void eigrp_update_send_GR_part(eigrp_neighbor_t *nbr)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	eigrp_packet_t *packet;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_list_node_t *node, *nnode;
	eigrp_list_t *successors;
	eigrp_list_t *prefixes;
	uint32_t flags;
	uint16_t packet_limit;
	uint32_t route_count = 0;
	bool first;

	if (!nbr || !nbr->ei || !nbr->nbr_gr_prefixes_send)
		return;
	if (nbr->nbr_gr_packet_type == EIGRP_PACKET_PART_LAST)
		return;

	ei = nbr->ei;
	eigrp = ei->eigrp;
	prefixes = nbr->nbr_gr_prefixes_send;
	packet_limit = eigrp_packet_payload_limit(ei->curr_mtu);
	first = nbr->nbr_gr_packet_type == EIGRP_PACKET_PART_FIRST;
	flags = first ? (EIGRP_INIT_FLAG | EIGRP_RS_FLAG) : 0;
	packet = eigrp_update_neighbor_packet_new(nbr, flags);
	if (!packet)
		return;

	for (EIGRP_LIST_ELEMENTS(prefixes, node, nnode, prefix)) {
		int encoded = 0;

		if (eigrp_filter_prefix_apply(eigrp, ei, EIGRP_FILTER_OUT,
					      &prefix->destination)) {
			eigrp_list_delete_data(prefixes, prefix);
			continue;
		}

		successors = eigrp_topology_get_successor(prefix);
		route = successors ? eigrp_list_node_data(eigrp_list_head(successors)) : NULL;
		if (!route || eigrp_nbr_split_horizon_check(route, ei)) {
			if (successors)
				eigrp_list_delete(&successors);
			eigrp_list_delete_data(prefixes, prefix);
			continue;
		}

		encoded = eigrp_packet_route_encode_append(
			eigrp, ei, nbr, nbr->encoder, packet->s, route, packet_limit);
		if (successors)
			eigrp_list_delete(&successors);

		if (encoded < 0 && route_count)
			break;
		if (encoded < 0) {
			eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP route TLV exceeds packet limit %u",
				  ei->name, packet_limit);
			eigrp_list_delete_data(prefixes, prefix);
			continue;
		}
		if (encoded > 0)
			route_count++;

		/* Preserve the existing filter-change behavior while the prefix is
		 * still valid, then consume this resync work item. */
		if (eigrp_filter_prefix_apply(eigrp, ei, EIGRP_FILTER_IN,
					      &prefix->destination)) {
			eigrp_fsm_action_message_t fsm_msg;
			eigrp_route_descriptor_t *fsm_route =
				eigrp_prefix_descriptor_lookup(prefix->entries, nbr);

			memset(&fsm_msg, 0, sizeof(fsm_msg));
			fsm_msg.packet_type = EIGRP_OPC_UPDATE;
			fsm_msg.eigrp = eigrp;
			fsm_msg.data_type = EIGRP_INT;
			fsm_msg.adv_router = nbr;
			fsm_msg.metrics = prefix->reported_metric;
			fsm_msg.metrics.delay = EIGRP_MAX_METRIC;
			fsm_msg.route = fsm_route;
			fsm_msg.prefix = prefix;
			eigrp_fsm_event(&fsm_msg);
		}
		eigrp_list_delete_data(prefixes, prefix);
	}

	if (prefixes->count == 0) {
		flags |= EIGRP_EOT_FLAG;
		nbr->nbr_gr_packet_type = EIGRP_PACKET_PART_LAST;
		eigrp_list_delete(&nbr->nbr_gr_prefixes_send);
	} else {
		nbr->nbr_gr_packet_type = EIGRP_PACKET_PART_NA;
	}
	((struct eigrp_header *)eigrp_stream_data(packet->s))->flags = htonl(flags);
	eigrp_update_neighbor_packet_queue(nbr, packet);
}

/**
 * @fn eigrp_update_send_GR_event
 *
 * @param[in]		event		contains neighbor who would receive
 * Graceful restart
 *
 * @return void
 *
 * @par
 * Function used for sending Graceful restart Update packet
 * in event, it is prepared for multiple chunks of packet.
 *
 * Uses nbr_gr_packet_type and t_nbr_send_gr from neighbor.
 */
void eigrp_update_send_GR_event(void *arg)
{
	eigrp_neighbor_t *nbr = arg;

	/* if there is packet waiting in queue,
	 * schedule this event again with small delay */
	if (nbr->retrans_queue->count > 0) {
		eigrp_southbound_timer_add(&nbr->t_nbr_send_gr,
				       eigrp_update_send_GR_event, nbr, 10);
		return;
	}

	/* send GR EIGRP packet chunk */
	eigrp_update_send_GR_part(nbr);

	/* if it wasn't last chunk, schedule this event again */
	if (nbr->nbr_gr_packet_type != EIGRP_PACKET_PART_LAST)
		eigrp_southbound_event_add(&nbr->t_nbr_send_gr,
				       eigrp_update_send_GR_event, nbr);

	return;
}

/**
 * @fn eigrp_update_send_GR
 *
 * @param[in]		nbr			Neighbor who would receive
 * Graceful
 * restart
 * @param[in]		gr_type 	Who executed Graceful restart
 *
 * @return void
 *
 * @par
 * Function used for sending Graceful restart Update packet:
 * Creates Update packet with INIT, RS, EOT flags and include
 * all route except those filtered
 */
void eigrp_update_send_GR(eigrp_neighbor_t *nbr, enum GR_type gr_type)
{
	eigrp_prefix_descriptor_t *prefix2;
	eigrp_list_t *prefixes;
	eigrp_table_node_t *rn;
	eigrp_interface_t *ei = nbr->ei;
	eigrp_instance_t *eigrp = ei->eigrp;

	if (gr_type == EIGRP_GR_FILTER) {
		/* function was called after applying filtration */
		if (eigrp->log_neighbor_changes)
			eigrp_log(EIGRP_LOG_INFO,
				"Neighbor %s (%s) is resync: route configuration changed",
				eigrp_print_addr(&nbr->src),
				ei->name);
	} else if (gr_type == EIGRP_GR_MANUAL) {
		/* Graceful restart was called manually */
		if (eigrp->log_neighbor_changes)
			eigrp_log(EIGRP_LOG_INFO, "Neighbor %s (%s) is resync: manually cleared",
				  eigrp_print_addr(&nbr->src),
				  ei->name);

	}

	prefixes = eigrp_list_new();
	if (nbr->nbr_gr_prefixes_send)
		eigrp_list_delete(&nbr->nbr_gr_prefixes_send);
	/* add all prefixes from topology table to list */
	for (rn = eigrp_table_first(eigrp->topology_table); rn; rn = eigrp_table_next(rn)) {
		if (!rn->info)
			continue;

		prefix2 = rn->info;
		eigrp_list_add(prefixes, prefix2);
	}

	/* save prefixes to neighbor */
	nbr->nbr_gr_prefixes_send = prefixes;

	/* indicate, that this is first GR Update packet chunk */
	nbr->nbr_gr_packet_type = EIGRP_PACKET_PART_FIRST;

	/* Start packet sending through the host event abstraction. */
	eigrp_southbound_event_add(&nbr->t_nbr_send_gr,
			       eigrp_update_send_GR_event, nbr);
}

/**
 * @fn eigrp_update_send_interface_GR
 *
 * @param[in]		ei		Interface to neighbors of which
 * the
 * GR
 * is sent
 * @param[in]		gr_type 	Who executed Graceful restart
 *
 * @return void
 *
 * @par
 * Function used for sending Graceful restart Update packet
 * to all neighbors on specified interface.
 */
void eigrp_update_send_interface_GR(eigrp_interface_t *ei, enum GR_type gr_type)
{
	eigrp_list_node_t *node;
	eigrp_neighbor_t *nbr;

	/* iterate over all neighbors on eigrp interface */
	for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
		/* send GR to neighbor */
		eigrp_update_send_GR(nbr, gr_type);
	}
}

/**
 * @fn eigrp_update_send_process_GR
 *
 * @param[in]		eigrp		EIGRP process
 * @param[in]		gr_type 	Who executed Graceful restart
 *
 * @return void
 *
 * @par
 * Function used for sending Graceful restart Update packet
 * to all neighbors in eigrp process.
 */
void eigrp_update_send_process_GR(eigrp_instance_t *eigrp, enum GR_type gr_type)
{
	eigrp_list_node_t *node;
	eigrp_interface_t *ei;

	/* iterate over all eigrp interfaces */
	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		/* send GR to all neighbors on interface */
		eigrp_update_send_interface_GR(ei, gr_type);
	}
}
