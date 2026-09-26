// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Eigrp Sending and Receiving EIGRP Reply Packets.
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
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_packetizer.h"
#include "eigrpd/eigrp_prefix.h"

void eigrp_reply_send_route(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
			   eigrp_prefix_descriptor_t *prefix,
			   eigrp_route_descriptor_t *route, uint32_t flags)
{
	eigrp_packetizer_work_t *work;

	if (!eigrp || !nbr || !prefix)
		return;

	work = eigrp_packetizer_work_new(EIGRP_OPC_REPLY);
	work->nbr = nbr;
	work->prefix = prefix;
	work->route = route;
	work->owner = prefix;
	work->flags = flags;
	eigrp_packetizer_enqueue(eigrp, work);
}

void eigrp_reply_send(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
		      eigrp_prefix_descriptor_t *prefix)
{
	eigrp_reply_send_route(eigrp, nbr, prefix, NULL, 0);
}

/*EIGRP REPLY read function*/
void eigrp_reply_receive(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
			 struct eigrp_header *eigrph, eigrp_stream_t *pkt,
			 eigrp_interface_t *ei, int length)
{
	struct eigrp_fsm_action_message msg;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;


	// record neighbor seq were processing
	nbr->recv_sequence_number = ntohl(eigrph->sequence);

	while (pkt->endp > pkt->getp) {
		route = (nbr->decoder)(eigrp, nbr, pkt, length);
		if (!route)
			break;

		prefix = eigrp_topology_table_lookup(eigrp->topology_table,
						       &route->dest);
		if (!prefix) {
			char prefix_buf[EIGRP_PREFIX_STRLEN] = "invalid";

			eigrp_prefix_snprintf(prefix_buf, sizeof(prefix_buf),
					      &route->dest);
			eigrp_log(EIGRP_LOG_DEBUG, "EIGRP REPLY: Neighbor(%s) sent unknown prefix %s",
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

		// should have got route off the packet, but one never knows
		msg.packet_type = EIGRP_OPC_REPLY;
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
