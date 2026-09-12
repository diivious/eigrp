// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP packetizer work queue.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_packetizer.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_dump.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_PACKETIZER_WORK,
		    "EIGRP packetizer work");

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


static eigrp_route_descriptor_t *
eigrp_packetizer_poison_route_create(eigrp_prefix_descriptor_t *prefix)
{
	eigrp_route_descriptor_t *route;

	if (!prefix || !prefix->destination)
		return NULL;

	route = eigrp_topology_route_create(NULL);
	if (!route)
		return NULL;

	route->prefix = prefix;
	route->dest = *prefix->destination;
	route->type = (prefix->nt == EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL)
			      ? EIGRP_TLV_IPv4_EXT
			      : EIGRP_TLV_IPv4_INT;
	route->metric = prefix->reported_metric;
	route->metric.delay = EIGRP_MAX_METRIC;
	route->metric.flags = 0;

	return route;
}

static void eigrp_packetizer_neighbor_stat(eigrp_neighbor_t *nbr,
					   uint8_t opcode)
{
	if (!nbr || !nbr->ei)
		return;

	switch (opcode) {
	case EIGRP_OPC_QUERY:
		nbr->ei->stats.sent.query++;
		break;
	case EIGRP_OPC_REPLY:
		nbr->ei->stats.sent.reply++;
		break;
	case EIGRP_OPC_SIAQUERY:
		nbr->ei->stats.sent.siaQuery++;
		break;
	case EIGRP_OPC_SIAREPLY:
		nbr->ei->stats.sent.siaReply++;
		break;
	default:
		break;
	}
}

static void eigrp_packetizer_neighbor_route_send(eigrp_instance_t *eigrp,
						 eigrp_packetizer_work_t *work)
{
	eigrp_neighbor_t *nbr = work->nbr;
	eigrp_prefix_descriptor_t *prefix = work->prefix;
	eigrp_interface_t *ei;
	eigrp_packet_t *packet;
	eigrp_route_descriptor_t *route;
	struct list *successors = NULL;
	bool free_route = false;
	uint32_t sequence;
	uint16_t tlv_length;
	uint16_t length = EIGRP_HEADER_LEN;

	if (!eigrp || !nbr || !nbr->ei || !prefix)
		return;

	if (nbr->state != EIGRP_NEIGHBOR_UP)
		return;

	ei = nbr->ei;
	sequence = eigrp_packet_sequence_reserve(eigrp);
	packet = eigrp_packet_new(EIGRP_PACKET_MTU(ei->ifp->mtu), nbr);
	eigrp_packet_header_init(work->opcode, eigrp, packet->s, 0, sequence, 0);

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
	    && ei->params.auth_keychain != NULL)
		length += eigrp_add_authTLV_MD5_encode(packet->s, ei);

	route = work->route;
	if (!route) {
		successors = eigrp_topology_get_successor(prefix);
		if (successors)
			route = listnode_head(successors);

		if (!route) {
			route = eigrp_packetizer_poison_route_create(prefix);
			free_route = true;
		}

		if (!route) {
			if (successors)
				list_delete(&successors);
			eigrp_packet_free(packet);
			return;
		}
	}

	tlv_length = nbr->encoder(eigrp, ei, nbr, packet->s, route);
	if (successors)
		list_delete(&successors);
	if (free_route)
		eigrp_topology_route_free(route);
	if (!tlv_length) {
		eigrp_packet_free(packet);
		return;
	}
	length += tlv_length;

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
	    && ei->params.auth_keychain != NULL)
		eigrp_make_md5_digest(ei, packet->s, EIGRP_AUTH_UPDATE_FLAG);

	eigrp_packet_checksum(ei, packet->s, length);
	packet->length = length;
	eigrp_addr_copy(&packet->dst, &nbr->src);
	packet->sequence_number = sequence;
	packet->sequence_reserved = true;

	eigrp_packet_enqueue(nbr->retrans_queue, packet);
	eigrp_packetizer_neighbor_stat(nbr, work->opcode);

	if (nbr->retrans_queue->count == 1)
		eigrp_packet_send_reliably(eigrp, nbr);
}


static eigrp_neighbor_t *eigrp_packetizer_query_encoder_neighbor(
	eigrp_interface_t *ei)
{
	eigrp_neighbor_t *nbr;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
		if (nbr->state == EIGRP_NEIGHBOR_UP)
			return nbr;
	}

	return NULL;
}

static void eigrp_packetizer_query_interface_send(eigrp_instance_t *eigrp,
						  eigrp_interface_t *ei,
						  eigrp_packetizer_work_t *work)
{
	eigrp_neighbor_t *nbr;
	eigrp_prefix_descriptor_t *prefix = work->prefix;
	eigrp_route_descriptor_t *route;
	eigrp_packet_t *packet;
	struct listnode *node, *nnode;
	struct list *successors;
	uint32_t sequence;
	uint16_t tlv_length;
	uint16_t length = EIGRP_HEADER_LEN;
	uint16_t eigrp_mtu;

	if (!eigrp || !ei || !prefix)
		return;

	if (work->exception == ei)
		return;

	nbr = eigrp_packetizer_query_encoder_neighbor(ei);
	if (!nbr)
		return;

	successors = eigrp_topology_get_successor(prefix);
	if (!successors)
		return;

	route = listnode_head(successors);
	if (!route) {
		list_delete(&successors);
		return;
	}

	eigrp_mtu = EIGRP_PACKET_MTU(ei->ifp->mtu);
	sequence = eigrp_packet_sequence_reserve(eigrp);
	packet = eigrp_packet_new(eigrp_mtu, NULL);
	eigrp_packet_header_init(work->opcode, eigrp, packet->s, 0, sequence, 0);

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
	    && ei->params.auth_keychain != NULL)
		length += eigrp_add_authTLV_MD5_encode(packet->s, ei);

	tlv_length = ei->encoder(eigrp, ei, NULL, packet->s, route);
	list_delete(&successors);
	if (!tlv_length) {
		eigrp_packet_free(packet);
		return;
	}
	length += tlv_length;

	for (ALL_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
		if (nbr->state == EIGRP_NEIGHBOR_UP)
			listnode_add(prefix->rij, nbr);
	}

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
	    && ei->params.auth_keychain != NULL)
		eigrp_make_md5_digest(ei, packet->s, EIGRP_AUTH_UPDATE_FLAG);

	eigrp_packet_checksum(ei, packet->s, length);
	packet->length = length;
	packet->dst.ip.v4.s_addr = htonl(EIGRP_MULTICAST_ADDRESS);
	packet->sequence_number = sequence;
	packet->sequence_reserved = true;

	{
		bool initial_multicast_send = false;

		for (ALL_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
			eigrp_packet_t *dup;
			bool queue_was_empty;

			if (nbr->state != EIGRP_NEIGHBOR_UP)
				continue;

			queue_was_empty = (nbr->retrans_queue->count == 0);
			dup = eigrp_packet_duplicate(packet, nbr);
			eigrp_packet_enqueue(nbr->retrans_queue, dup);

			if (queue_was_empty) {
				eigrp_packet_retransmit_timer_start(nbr);
				initial_multicast_send = true;
			}
		}

		if (initial_multicast_send)
			eigrp_packet_output_enqueue(eigrp, ei, packet);
		else
			eigrp_packet_free(packet);
	}
	if (work->opcode == EIGRP_OPC_SIAQUERY)
		ei->stats.sent.siaQuery++;
	else
		ei->stats.sent.query++;
}

static void eigrp_packetizer_query_send(eigrp_instance_t *eigrp,
					 eigrp_packetizer_work_t *work)
{
	eigrp_interface_t *ei;
	struct listnode *node;

	if (!eigrp || !work || !work->prefix)
		return;

	if (work->nbr) {
		eigrp_packetizer_neighbor_route_send(eigrp, work);
		return;
	}

	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei))
		eigrp_packetizer_query_interface_send(eigrp, ei, work);
}

static void eigrp_packetizer_work_process(eigrp_instance_t *eigrp,
					  eigrp_packetizer_work_t *work)
{
	if (!eigrp || !work)
		return;

	switch (work->opcode) {
	case EIGRP_OPC_UPDATE:
		eigrp_update_packetize_all(eigrp, work->exception);
		break;
	case EIGRP_OPC_QUERY:
	case EIGRP_OPC_SIAQUERY:
		eigrp_packetizer_query_send(eigrp, work);
		break;
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

	work = XCALLOC(MTYPE_EIGRP_PACKETIZER_WORK, sizeof(*work));
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

	XFREE(MTYPE_EIGRP_PACKETIZER_WORK, work);
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

	eigrp_work_queue_enqueue(eigrp->packetizer_queue, work);
}
