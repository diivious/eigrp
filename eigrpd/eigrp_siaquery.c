// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Sending and Receiving EIGRP SIA-Query Packets.
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
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_packetizer.h"
#include "eigrpd/eigrp_dump.h"

/* EIGRP SIA-QUERY read function */
void eigrp_siaquery_receive(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
			    struct eigrp_header *eigrph, struct stream *pkt,
			    eigrp_interface_t *ei, int length)
{
	eigrp_debug_neighbor_sia(nbr, "SIA-QUERY received");
	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_SIA, eigrp,
				   nbr ? nbr->ei : NULL, nbr, "SIA-QUERY received");
	eigrp_fsm_action_message_t msg;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;

	/* increment statistics. */
	ei->stats.rcvd.siaQuery++;

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
			zlog_debug("EIGRP SIA-QUERY: Neighbor(%s) sent unknown prefix %s",
				   eigrp_print_addr(&nbr->src),
				   eigrp_print_prefix(&route->dest));
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

		msg.packet_type = EIGRP_OPC_SIAQUERY;
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
}

void eigrp_siaquery_send(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
			 eigrp_prefix_descriptor_t *prefix)
{
	eigrp_packetizer_work_t *work;

	eigrp_debug_neighbor_sia(nbr, "SIA-QUERY send");
	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_SIA, eigrp,
				   nbr ? nbr->ei : NULL, nbr, "queue SIA-QUERY");
	work = eigrp_packetizer_work_new(EIGRP_OPC_SIAQUERY);
	work->nbr = nbr;
	work->prefix = prefix;
	work->owner = prefix;
	eigrp_packetizer_enqueue(eigrp, work);
}

