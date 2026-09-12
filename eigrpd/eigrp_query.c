// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Sending and Receiving EIGRP Query Packets.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_packetizer.h"


static void eigrp_query_unknown_reply_send(eigrp_instance_t *eigrp,
					eigrp_neighbor_t *nbr,
					eigrp_route_descriptor_t *query_route)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *reply_route;

	if (!eigrp || !nbr || !query_route)
		return;

	prefix = eigrp_topology_prefix_create();
	if (!prefix)
		return;

	prefix->destination = (struct prefix *)prefix_ipv4_new();
	if (!prefix->destination) {
		eigrp_topology_prefix_free(prefix);
		return;
	}
	prefix_copy(prefix->destination, &query_route->dest);

	reply_route = eigrp_topology_route_create(nbr->ei);
	if (!reply_route) {
		eigrp_topology_prefix_free(prefix);
		return;
	}

	reply_route->type = query_route->type;
	reply_route->dest = query_route->dest;
	reply_route->prefix = prefix;
	reply_route->adv_router = nbr;
	reply_route->metric = query_route->metric;
	reply_route->metric.delay = EIGRP_MAX_METRIC;
	reply_route->metric.flags = 0;

	eigrp_reply_send_route(eigrp, nbr, prefix, reply_route,
			      EIGRP_PACKETIZER_WORK_F_OWN_PREFIX |
			      EIGRP_PACKETIZER_WORK_F_OWN_ROUTE);
}

uint32_t eigrp_query_send_all(eigrp_instance_t *eigrp)
{
	eigrp_packetizer_work_t *work;
	eigrp_prefix_descriptor_t *prefix;
	struct listnode *node, *nnode;
	uint32_t counter = 0;

	if (!eigrp)
		return 0;

	for (ALL_LIST_ELEMENTS(eigrp->topology_changes, node, nnode, prefix)) {
		if (!(prefix->req_action & EIGRP_FSM_NEED_QUERY))
			continue;

		work = eigrp_packetizer_work_new(EIGRP_OPC_QUERY);
		work->prefix = prefix;
		work->owner = prefix;
		eigrp_packetizer_enqueue(eigrp, work);

		prefix->req_action &= ~EIGRP_FSM_NEED_QUERY;
		if (!prefix->req_action)
			listnode_delete(eigrp->topology_changes, prefix);

		counter++;
	}

	return counter;
}

/*EIGRP QUERY read function*/
void eigrp_query_receive(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
			 struct eigrp_header *eigrph, struct stream *pkt,
			 eigrp_interface_t *ei, int length)
{
	eigrp_fsm_action_message_t msg;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;

	/* increment statistics. */
	ei->stats.rcvd.query++;

	/* get neighbor struct */
	nbr->recv_sequence_number = ntohl(eigrph->sequence);

	// process all TLVs in the packet
	while (pkt->endp > pkt->getp) {
		route = (nbr->decoder)(eigrp, nbr, pkt, length);

		// should have got route off the packet, but one never knows
		if (!route)
			break;

		prefix = eigrp_topology_table_lookup_ipv4(eigrp->topology_table,
						       &route->dest);
		if (!prefix) {
			zlog_debug("EIGRP QUERY: Neighbor(%s) sent unknown prefix %s",
				   eigrp_print_addr(&nbr->src),
				   eigrp_print_prefix(&route->dest));
			eigrp_query_unknown_reply_send(eigrp, nbr, route);
			eigrp_topology_route_free(route);
			continue;
		}
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

		msg.packet_type = EIGRP_OPC_QUERY;
		msg.eigrp = eigrp;
		msg.data_type = (received_route->type == EIGRP_TLV_IPv4_EXT)
					? EIGRP_EXT
					: EIGRP_INT;
		msg.adv_router = nbr;
		msg.route = route;
		msg.metrics = received_route->metric;
		msg.prefix = prefix;
		eigrp_fsm_event(&msg);

		if (free_received_route)
			eigrp_topology_route_free(received_route);
	}

	eigrp_hello_send_ack(nbr);
	eigrp_query_send_all(eigrp);
	eigrp_update_send_all(eigrp, nbr->ei);
}
