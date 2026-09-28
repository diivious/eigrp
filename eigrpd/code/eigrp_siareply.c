// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Sending and Receiving EIGRP SIA-Reply Packets.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include "eigrpd.h"
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

/* EIGRP SIA-REPLY read function */
void eigrp_siareply_receive(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
			    struct eigrp_header *eigrph, eigrp_stream_t *pkt,
			    eigrp_intf_t *ei, int length)
{
	eigrp_debug_neighbor_sia(nbr, "SIA-REPLY received");
	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_SIA, eigrp,
				   nbr ? nbr->ei : NULL, nbr, "SIA-REPLY received");
	struct eigrp_fsm_action_message msg;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;

	nbr->recv_sequence_number = ntohl(eigrph->sequence);
	eigrp_hello_send_ack(nbr);

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
			eigrp_log(EIGRP_LOG_DEBUG, "EIGRP SIA-REPLY: Neighbor(%s) sent unknown prefix %s",
				   eigrp_print_addr(&nbr->src), prefix_buf);
			eigrp_topology_route_free(route);
			continue;
		}
		bool route_retained = false;

		if (prefix->state != EIGRP_FSM_STATE_PASSIVE
		    && eigrp_fsm_reply_status_pending(prefix, nbr)) {
			if (route->metric.flags & EIGRP_OPAQUE_ACTIVE)
				eigrp_fsm_sia_reply_received(prefix, nbr);
			else {
				/* A non-ACTIVE SIA-REPLY says convergence completed.
				 * Treat it as the outstanding REPLY through DUAL. */
				msg.packet_type = EIGRP_OPC_REPLY;
				msg.eigrp = eigrp;
				msg.data_type = (route->type == EIGRP_TLV_IPv4_EXT
						 || route->type == EIGRP_TLV_IPv6_EXT
						 || route->type == EIGRP_TLV_MP_EXT)
						? EIGRP_EXT : EIGRP_INT;
				msg.adv_router = nbr;
				msg.route = eigrp_prefix_descriptor_lookup(prefix, nbr);
				if (!msg.route) {
					route->adv_router = nbr;
					route->prefix = prefix;
					msg.route = route;
					route_retained = true;
				}
				msg.metrics = route->metric;
				msg.prefix = prefix;
				eigrp_fsm_event(&msg);
			}
		}
		if (!route_retained)
			eigrp_topology_route_free(route);
	}
}

void eigrp_siareply_send(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
			 eigrp_prefix_descriptor_t *prefix, bool active)
{
	eigrp_packetizer_work_t *work;

	eigrp_debug_neighbor_sia(nbr, "SIA-REPLY send");
	eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_SIA, eigrp,
				   nbr ? nbr->ei : NULL, nbr, "queue SIA-REPLY");
	work = eigrp_packetizer_work_create(EIGRP_OPC_SIAREPLY);
	work->nbr = nbr;
	work->prefix = prefix;
	work->owner = prefix;
	if (active)
		work->flags |= EIGRP_PACKETIZER_WORK_F_ROUTE_ACTIVE;
	eigrp_packetizer_enqueue(eigrp, work);
}
