// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Sending and Receiving EIGRP SIA-Query Packets.
 * Copyright (C) 2013-2014, 2026
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include "eigrp.h"
#include "eigrp_log.h"
#include "eigrp_structs.h"
#include "eigrp_neighbor.h"
#include "eigrp_packet.h"
#include "eigrp_auth.h"
#include "eigrp_network.h"
#include "eigrp_topology.h"
#include "eigrp_fsm.h"
#include "eigrp_packetizer.h"
#include "eigrp_prefix.h"
#include "eigrp_debug.h"
#include "eigrp_eventlog.h"

/* EIGRP SIA-QUERY read function */
void eigrp_siaquery_receive(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
			    struct eigrp_header *eigrph, eigrp_stream_t *pkt,
			    eigrp_intf_t *ei, int length)
{
	eigrp_debug_neighbor_sia(nbr, "SIA-QUERY received");
	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_SIA, eigrp,
				   nbr ? nbr->ei : NULL, nbr, "SIA-QUERY received");
	eigrp_fsm_action_message_t msg;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;


	/* get neighbor struct */
	nbr->recv_sequence_number = ntohl(eigrph->sequence);
	eigrp_hello_send_ack(nbr);

	// process all TLVs in the packet
	while (pkt->endp > pkt->getp) {
		route = (nbr->decoder)(eigrp, nbr, pkt, length);

		// should have got route off the packet, but one never knows
		if (!route)
			break;

		prefix = eigrp_topology_table_lookup(eigrp->topology_table,
						       &route->dest);
		(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_SIA_QUERY_RX,
			&route->dest, ei ? ei->ifindex : 0, route->distance,
			prefix ? prefix->state : EIGRP_FSM_STATE_PASSIVE, 0);
		if (!prefix) {
			char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";

			eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
					      &route->dest);
			eigrp_log(EIGRP_LOG_DEBUG, "EIGRP SIA-QUERY: Neighbor(%s) sent unknown prefix %s",
				   eigrp_print_addr(&nbr->src), prefix_buf);
			eigrp_topology_route_free(route);
			continue;
		}
		eigrp_route_descriptor_t *received_route = route;
		eigrp_route_descriptor_t *topology_route =
			eigrp_prefix_descriptor_lookup(prefix, nbr);
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

		/* An SIA-QUERY without an outstanding original QUERY is processed
		 * as a standard QUERY.  If this leaves the destination ACTIVE, the
		 * response is SIA-REPLY with the route ACTIVE bit set. */
		msg.packet_type = prefix->state == EIGRP_FSM_STATE_PASSIVE
				  ? EIGRP_OPC_QUERY : EIGRP_OPC_SIAQUERY;
		msg.eigrp = eigrp;
		msg.data_type = (received_route->type == EIGRP_TLV_IPv4_EXT
				 || received_route->type == EIGRP_TLV_IPv6_EXT
				 || received_route->type == EIGRP_TLV_MP_EXT)
					? EIGRP_EXT : EIGRP_INT;
		msg.adv_router = nbr;
		msg.route = route;
		msg.metrics = received_route->metric;
		msg.prefix = prefix;
		eigrp_fsm_event(&msg);
		if (prefix->state != EIGRP_FSM_STATE_PASSIVE)
			eigrp_siareply_send(eigrp, nbr, prefix, true);

		if (free_received_route)
			eigrp_topology_route_free(received_route);
	}

}

void eigrp_siaquery_send(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
			 eigrp_prefix_descriptor_t *prefix)
{
	eigrp_packetizer_work_t *work;

	eigrp_debug_neighbor_sia(nbr, "SIA-QUERY send");
	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_SIA, eigrp,
				   nbr ? nbr->ei : NULL, nbr, "queue SIA-QUERY");
	(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_SIA_QUERY_TX,
		&prefix->destination, nbr && nbr->ei ? nbr->ei->ifindex : 0,
		0, 0, 0);
	work = eigrp_packetizer_work_create(EIGRP_OPC_SIAQUERY);
	work->nbr = nbr;
	work->prefix = prefix;
	work->owner = prefix;
	eigrp_packetizer_enqueue(eigrp, work);
}
