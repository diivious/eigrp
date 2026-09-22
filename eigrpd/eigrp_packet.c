// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP General Sending and Receiving of EIGRP Packets.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_checksum.h"
#include "eigrpd/eigrp_auth.h"

#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_debug.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"
/* Packet Type String. */
const eigrp_message_t eigrp_packet_type_str[] = {
	{EIGRP_OPC_UPDATE, "Update"},
	{EIGRP_OPC_REQUEST, "Request"},
	{EIGRP_OPC_QUERY, "Query"},
	{EIGRP_OPC_REPLY, "Reply"},
	{EIGRP_OPC_HELLO, "Hello"},
	{EIGRP_OPC_IPXSAP, "IPX-SAP"},
	{EIGRP_OPC_PROBE, "Probe"},
	{EIGRP_OPC_ACK, "Ack"},
	{EIGRP_OPC_SIAQUERY, "SIAQuery"},
	{EIGRP_OPC_SIAREPLY, "SIAReply"},
	{0}};

/* Forward function reference*/
static int eigrp_verify_header(eigrp_interface_t *ei, eigrp_addr_t *source,
			       struct eigrp_header *header, uint16_t length);
static int eigrp_packet_auth_header_validate(eigrp_interface_t *ei,
					     struct eigrp_header *eigrph,
					     uint16_t length);
static int eigrp_packet_auth_digest_validate(eigrp_interface_t *ei,
					     eigrp_neighbor_t *nbr,
					     struct eigrp_header *eigrph,
					     uint16_t length);

#define EIGRP_PACKET_ADDR_TEXT_SIZE 64U

static bool eigrp_packet_header_is_ack(const struct eigrp_header *header)
{
	return header && header->opcode == EIGRP_OPC_HELLO
	       && ntohl(header->sequence) == 0 && ntohl(header->ack) != 0;
}

static void eigrp_packet_opcode_counter_increment(eigrp_intf_stats_t *stats,
						  bool sent,
						  const struct eigrp_header *header)
{
	if (!stats || !header)
		return;

	if (eigrp_packet_header_is_ack(header)) {
		if (sent)
			stats->sent.ack++;
		else
			stats->rcvd.ack++;
		return;
	}

	switch (header->opcode) {
	case EIGRP_OPC_HELLO:
		if (sent)
			stats->sent.hello++;
		else
			stats->rcvd.hello++;
		break;
	case EIGRP_OPC_UPDATE:
		if (sent)
			stats->sent.update++;
		else
			stats->rcvd.update++;
		break;
	case EIGRP_OPC_QUERY:
		if (sent)
			stats->sent.query++;
		else
			stats->rcvd.query++;
		break;
	case EIGRP_OPC_REPLY:
		if (sent)
			stats->sent.reply++;
		else
			stats->rcvd.reply++;
		break;
	case EIGRP_OPC_SIAQUERY:
		if (sent)
			stats->sent.siaQuery++;
		else
			stats->rcvd.siaQuery++;
		break;
	case EIGRP_OPC_SIAREPLY:
		if (sent)
			stats->sent.siaReply++;
		else
			stats->rcvd.siaReply++;
		break;
	default:
		break;
	}
}

static bool eigrp_packet_destination_is_ipv4_multicast(const eigrp_packet_t *packet)
{
	return packet && packet->dst.afi == AF_INET
	       && packet->dst.ip.v4.s_addr == htonl(EIGRP_MULTICAST_ADDRESS);
}

static void eigrp_packet_send_stats_record(eigrp_interface_t *ei,
					   const eigrp_packet_t *packet,
					   const struct eigrp_header *header)
{
	bool reliable;
	bool multicast;

	if (!ei || !packet || !header)
		return;

	eigrp_packet_opcode_counter_increment(&ei->stats, true, header);
	reliable = ntohl(header->sequence) != 0;
	multicast = eigrp_packet_destination_is_ipv4_multicast(packet);
	if (multicast) {
		if (reliable)
			ei->stats.reliable_multicast_sent++;
		else
			ei->stats.unreliable_multicast_sent++;
	} else {
		if (reliable)
			ei->stats.reliable_unicast_sent++;
		else
			ei->stats.unreliable_unicast_sent++;
	}
	if (ntohl(header->flags) & EIGRP_CR_FLAG)
		ei->stats.cr_packets_sent++;
	if (packet->retransmission) {
		ei->stats.retransmissions_sent++;
		if (packet->nbr)
			packet->nbr->retransmissions++;
	}
	if (packet->multicast_exception)
		ei->stats.multicast_exceptions++;
}

static void eigrp_packet_receive_stats_record(eigrp_interface_t *ei,
					      const struct eigrp_header *header)
{
	if (!ei || !header)
		return;
	eigrp_packet_opcode_counter_increment(&ei->stats, false, header);
}

static const char *eigrp_packet_addr_text(eigrp_instance_t *eigrp,
					 const eigrp_addr_t *address,
					 char *buf, size_t len)
{
	if (!buf || !len)
		return "";

	buf[0] = '\0';
	if (!eigrp || !address)
		snprintf(buf, len, "?");
	else if (eigrp->af_vectors.addr_snprintf(buf, len, address) < 0)
		snprintf(buf, len, "?");

	return buf;
}

eigrp_route_descriptor_t *eigrp_packet_decoder_safe(eigrp_instance_t *eigrp,
						    eigrp_neighbor_t *nbr,
						    eigrp_stream_t *pkt,
						    uint16_t pktlen)
{
	(void)eigrp;
	(void)nbr;
	(void)pktlen;
	if (pkt)
		eigrp_stream_set_getp(pkt, eigrp_stream_get_endp(pkt));
	return NULL;
}

uint16_t eigrp_packet_encoder_safe(eigrp_instance_t *eigrp,
					  eigrp_interface_t *ei,
					  eigrp_neighbor_t *nbr,
					  eigrp_stream_t *pkt,
					  eigrp_route_descriptor_t *route)
{
	(void)eigrp;
	(void)ei;
	(void)nbr;
	(void)pkt;
	(void)route;
	return 0;
}

uint16_t eigrp_packet_encoder_both(eigrp_instance_t *eigrp,
					  eigrp_interface_t *ei,
					  eigrp_neighbor_t *nbr,
					  eigrp_stream_t *pkt,
					  eigrp_route_descriptor_t *route)
{
	size_t start;
	uint16_t len1;
	uint16_t len2;

	if (!eigrp || !pkt || !route)
		return 0;

	start = eigrp_stream_get_endp(pkt);
	len1 = eigrp->tlv1_codec.encoder(eigrp, ei, nbr, pkt, route);
	len2 = eigrp->tlv2_codec.encoder(eigrp, ei, nbr, pkt, route);

	if (!len1 || !len2) {
		eigrp_stream_set_endp(pkt, start);
		return 0;
	}

	return len1 + len2;
}

int eigrp_packet_route_encode_append(eigrp_instance_t *eigrp,
				     eigrp_interface_t *ei,
				     eigrp_neighbor_t *nbr,
				     eigrp_packet_encoder_t encoder,
				     eigrp_stream_t *pkt,
				     eigrp_route_descriptor_t *route,
				     uint16_t packet_limit)
{
	eigrp_stream_t *scratch;
	uint16_t encoded;

	if (!eigrp || !encoder || !pkt || !route || packet_limit == 0)
		return 0;

	/* Encode the complete route TLV set away from the destination packet.
	 * A TLV is appended only when the complete encoding fits the interface
	 * packet limit, so packet splitting can never cut through a TLV. */
	scratch = eigrp_stream_new(packet_limit);
	encoded = encoder(eigrp, ei, nbr, scratch, route);
	if (!encoded) {
		eigrp_stream_free(scratch);
		return 0;
	}

	if (eigrp_stream_get_endp(pkt) + encoded > packet_limit) {
		eigrp_stream_free(scratch);
		return -1;
	}

	eigrp_stream_put(pkt, eigrp_stream_data(scratch), encoded);
	eigrp_stream_free(scratch);
	return encoded;
}

static void eigrp_packet_retransmit_limit_exceeded(eigrp_neighbor_t *nbr)
{
	char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

	if (!nbr || !nbr->ei || !nbr->ei->eigrp)
		return;

	if (nbr->ei->eigrp->log_neighbor_changes)
		eigrp_log(EIGRP_LOG_INFO, "Neighbor %s (%s) is down: retry limit exceeded",
			  eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src, address,
					 sizeof(address)),
			  nbr->ei->name);
	eigrp_nbr_delete(nbr);
}

/*
 * New testing block of code for handling Acks
 */
static void eigrp_packet_ack(eigrp_instance_t *eigrp, struct eigrp_header *eigrph,
			     eigrp_neighbor_t *nbr)
{
	struct eigrp_packet *packet = NULL;
	uint32_t ack;

	if (!eigrp || !eigrph || !nbr || !nbr->retrans_queue)
		return;

	ack = ntohl(eigrph->ack);

	packet = eigrp_packet_queue_next(nbr->retrans_queue);
	if (packet && ack == packet->sequence_number) {
		eigrp_neighbor_srtt_update(nbr, packet);
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_ACK, eigrp, nbr->ei, nbr,
				   "ACK %u matched reliable sequence", ack);
		packet = eigrp_packet_dequeue(nbr->retrans_queue);
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_LINK, eigrp, nbr->ei, nbr,
				   "unlinked ACKed seq %u from reliable queue (depth %lu)",
				   ack, nbr->retrans_queue->count);
		eigrp_packet_free(packet);

		if ((nbr->state == EIGRP_NEIGHBOR_PENDING)
		    && ack == nbr->init_sequence_number) {
			eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_UP);
			{
				char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

				if (eigrp->log_neighbor_changes)
					eigrp_log(EIGRP_LOG_INFO, "Neighbor %s (%s) is up: new adjacency",
						  eigrp_packet_addr_text(eigrp, &nbr->src,
								 address, sizeof(address)),
						  nbr->ei->name);
			}
			nbr->init_sequence_number = 0;
			nbr->recv_sequence_number = ntohl(eigrph->sequence);
			eigrp_update_send_EOT(nbr);
		} else
			eigrp_packet_send_reliably(eigrp, nbr);
	}
}

static void eigrp_packet_reliable_neighbor_send_record(eigrp_neighbor_t *nbr,
					       uint32_t sequence,
					       uint64_t now_msec)
{
	eigrp_packet_t *queued;

	if (!nbr || sequence == 0)
		return;

	queued = eigrp_packet_queue_next(nbr->retrans_queue);
	if (!queued || queued->sequence_number != sequence || queued->sent_msec != 0)
		return;

	queued->sent_msec = now_msec;
	/* The RTO starts at the successful wire send, not queue creation. */
	eigrp_packet_retransmit_timer_start(nbr);
}

static void eigrp_packet_reliable_send_record(eigrp_interface_t *ei,
				      eigrp_packet_t *packet)
{
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *node;
	uint64_t now_msec;

	if (!ei || !packet || packet->sequence_number == 0 || packet->retransmission)
		return;

	now_msec = eigrp_sys_monotime_msec();
	if (packet->nbr) {
		eigrp_packet_reliable_neighbor_send_record(packet->nbr,
						 packet->sequence_number, now_msec);
		return;
	}

	/* One reliable multicast wire send is an independent RTT start point for
	 * every neighbor that currently owns this sequence in its RTP queue. */
	for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr))
		eigrp_packet_reliable_neighbor_send_record(nbr, packet->sequence_number,
						 now_msec);
}

static void eigrp_packet_reliable_send_failure_record(eigrp_interface_t *ei,
					      eigrp_packet_t *packet)
{
	eigrp_neighbor_t *nbr;
	eigrp_packet_t *queued;
	eigrp_list_node_t *node;

	if (!ei || !packet || packet->sequence_number == 0 || packet->retransmission)
		return;

	if (packet->nbr) {
		queued = eigrp_packet_queue_next(packet->nbr->retrans_queue);
		if (queued && queued->sequence_number == packet->sequence_number)
			eigrp_packet_retransmit_timer_start(packet->nbr);
		return;
	}

	for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
		queued = eigrp_packet_queue_next(nbr->retrans_queue);
		if (queued && queued->sequence_number == packet->sequence_number)
			eigrp_packet_retransmit_timer_start(nbr);
	}
}

void eigrp_packet_write_schedule(eigrp_instance_t *eigrp)
{
	if (!eigrp || eigrp->t_write)
		return;
	eigrp_sys_write_add(&eigrp->t_write, eigrp,
			     eigrp_packet_write, eigrp);
}

void eigrp_packet_write(void *arg)
{
	eigrp_instance_t *eigrp = arg;
	struct eigrp_header *eigrph;
	eigrp_interface_t *ei;
	eigrp_packet_t *packet;
	uint32_t seqno, ack;
	int ret;
	eigrp_list_node_t *node;

	node = eigrp_list_head(eigrp->oi_write_q);
	assert(node);
	ei = eigrp_list_node_data(node);
	assert(ei);

	/* Get one packet from queue. */
	packet = eigrp_packet_queue_next(ei->obuf);
	if (!packet) {
		eigrp_log(EIGRP_LOG_ERROR,
			 "%s: Interface %s no packet on queue?", __func__,
			 ei->name);
		goto out;
	}
	if (packet->length < EIGRP_HEADER_LEN) {
		eigrp_log(EIGRP_LOG_ERROR,  "%s: Packet just has a header?",
			 __func__);
		eigrp_debug_header_dump((const eigrp_header_t *)packet->s->data);
		eigrp_packet_delete(ei);
		goto out;
	}

	eigrph = (struct eigrp_header *)eigrp_stream_data(packet->s);
	seqno = ntohl(eigrph->sequence);
	ack = ntohl(eigrph->ack);

	ret = eigrp->af_vectors.packet_send(eigrp, ei, packet);

	eigrp_debug_packet_send(ei, packet, ret);
	if (ret >= 0) {
		eigrp_packet_send_stats_record(ei, packet, eigrph);
		eigrp_packet_reliable_send_record(ei, packet);
	} else
		eigrp_packet_reliable_send_failure_record(ei, packet);

	if (IS_DEBUG_EIGRP_TRANSMIT(0, DETAIL)) {
		char destination[EIGRP_PACKET_ADDR_TEXT_SIZE];

		eigrph = (struct eigrp_header *)eigrp_stream_data(packet->s);
		eigrp_log(EIGRP_LOG_DEBUG,
			"Sending [%s][%d/%d] to [%s] via [%s] ret [%d].",
			eigrp_message_lookup(eigrp_packet_type_str, eigrph->opcode, NULL),
			seqno, ack,
			eigrp_packet_addr_text(eigrp, &packet->dst, destination,
					       sizeof(destination)),
			eigrp_intf_name_string(ei), ret);
	}

	/* Now delete packet from queue. */
	eigrp_packet_delete(ei);

out:
	if (eigrp_packet_queue_next(ei->obuf) == NULL) {
		ei->on_write_q = 0;
		eigrp_list_delete_node(eigrp->oi_write_q, node);
	}

	/* If packets still remain in queue, call write event. */
	if (!eigrp_list_isempty(eigrp->oi_write_q))
		eigrp_packet_write_schedule(eigrp);
}

/* Starting point of packet process function. */
void eigrp_packet_read(void *arg)
{
	int ret;
	eigrp_stream_t *ibuf;
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei = NULL;
	struct eigrp_header *eigrph;
	eigrp_addr_t src;
	eigrp_addr_t dst;
	eigrp_neighbor_t *nbr;
	eigrp_packet_rx_meta_t meta;
	uint16_t opcode;
	uint16_t length;

	eigrp = arg;

	/* Prepare for the next packet before processing this one. */
	eigrp_sys_read_add(&eigrp->t_read, eigrp,
			    eigrp_packet_read, eigrp);

	eigrp_stream_reset(eigrp->ibuf);

	ibuf = eigrp->ibuf;
	if (!eigrp->af_vectors.packet_receive(eigrp, ibuf, &ei,
					      &src, &dst, &meta))
		return;

	if (!ei)
		return;

	eigrp_stream_forward_getp(ibuf, meta.network_header_length);
	eigrph = (struct eigrp_header *)eigrp_stream_pnt(ibuf);
	length = meta.eigrp_length;

	if (IS_DEBUG_EIGRP_TRANSMIT(0, DETAIL))
		eigrp_debug_header_dump(eigrph);

	/* If incoming interface is passive, ignore it. */
	if (eigrp_intf_is_passive(ei)) {
		if (IS_DEBUG_EIGRP_TRANSMIT(0, STRANGE)) {
			char destination[EIGRP_PACKET_ADDR_TEXT_SIZE];

			eigrp_log(EIGRP_LOG_DEBUG,
				"ignoring packet from router %u sent to %s, received on passive interface %s",
				ntohs(eigrph->vrid),
				eigrp_packet_addr_text(eigrp, &dst, destination,
						       sizeof(destination)),
				eigrp_intf_name_string(ei));
		}

		if (meta.destination_multicast)
			eigrp_intf_set_multicast(ei);
		return;
	}

	/* Verify common EIGRP header fields plus AF-specific source validity. */
	ret = eigrp_verify_header(ei, &src, eigrph, length);
	if (ret < 0) {
		if (IS_DEBUG_EIGRP_TRANSMIT(0, STRANGE)) {
			char source[EIGRP_PACKET_ADDR_TEXT_SIZE];

			eigrp_log(EIGRP_LOG_DEBUG, "eigrp_packet_read[%s]: Header check failed, dropping.",
				   eigrp_packet_addr_text(eigrp, &src, source,
							  sizeof(source)));
		}
		return;
	}

	eigrp_debug_packet_receive(ei, &src, &dst, eigrph, length);
	opcode = eigrph->opcode;

	if (IS_DEBUG_EIGRP_TRANSMIT(0, DETAIL)) {
		char source_text[64];
		char destination_text[64];

		eigrp_packet_addr_text(eigrp, &src, source_text, sizeof(source_text));
		eigrp_packet_addr_text(eigrp, &dst, destination_text,
				       sizeof(destination_text));
		eigrp_log(EIGRP_LOG_DEBUG,
			"Received [%s][%d/%d] length [%u] via [%s] src [%s] dst [%s]",
			eigrp_message_lookup(eigrp_packet_type_str, opcode, NULL),
			ntohl(eigrph->sequence), ntohl(eigrph->ack), length,
			eigrp_intf_name_string(ei), source_text, destination_text);
	}

	/* Move from the EIGRP header to the first TLV. */
	eigrp_stream_forward_getp(ibuf, EIGRP_HEADER_LEN);

	/*
	 * Handle Hello before the normal neighbor-required opcodes.  Check the
	 * authentication digest before allowing Hello processing to create or
	 * update neighbor state.
	 */
	nbr = eigrp_nbr_lookup(ei, eigrph, &src);
	if (opcode == EIGRP_OPC_HELLO) {
		if (eigrp_packet_auth_digest_validate(ei, nbr, eigrph, length) < 0)
			return;

		eigrp_packet_receive_stats_record(ei, eigrph);
		if (ntohl(eigrph->ack)) {
			/* An EIGRP ACK is a Hello opcode with sequence zero and a
			 * nonzero ACK field.  Consume it in RTP before Hello TLV
			 * processing; an ACK must never create a new adjacency. */
			if (nbr)
				eigrp_packet_ack(eigrp, eigrph, nbr);
			return;
		}
		eigrp_hello_receive(eigrp, eigrph, &src, ei, ibuf, length);
		return;
	}

	/* A neighbor must exist before accepting non-Hello packets. */
	if (!nbr)
		return;

	if (eigrp_packet_auth_digest_validate(ei, nbr, eigrph, length) < 0)
		return;

	if (!meta.destination_multicast && nbr->cr_mode
	    && nbr->cr_sequence == ntohl(eigrph->sequence)) {
		/* A CR multicast that was missed or intentionally ignored is
		 * recovered by the normal unicast reliable send.  Consuming that
		 * sequence also consumes the conditional-receive state. */
		nbr->cr_mode = false;
		nbr->cr_sequence = 0;
	}

	if (meta.destination_multicast && (ntohl(eigrph->flags) & EIGRP_CR_FLAG)) {
		uint32_t sequence = ntohl(eigrph->sequence);

		if (!nbr->cr_mode
		    || (nbr->cr_sequence != 0 && nbr->cr_sequence != sequence)) {
			eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_ACK, eigrp, ei, nbr,
					   "discard CR sequence %u while not eligible",
					   sequence);
			return;
		}

		nbr->cr_mode = false;
		nbr->cr_sequence = 0;
	}

	eigrp_packet_receive_stats_record(ei, eigrph);
	if (ntohl(eigrph->ack))
		eigrp_packet_ack(eigrp, eigrph, nbr);

	switch (opcode) {
	case EIGRP_OPC_PROBE:
		break;
	case EIGRP_OPC_QUERY:
		eigrp_query_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	case EIGRP_OPC_REPLY:
		eigrp_reply_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	case EIGRP_OPC_REQUEST:
		break;
	case EIGRP_OPC_SIAQUERY:
		eigrp_query_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	case EIGRP_OPC_SIAREPLY:
		eigrp_reply_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	case EIGRP_OPC_UPDATE:
		eigrp_update_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	default:
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP packet header type %d unsupported",
			  eigrp_intf_name_string(ei), opcode);
		break;
	}
}

eigrp_packet_queue_t *eigrp_packet_queue_new(void)
{
	eigrp_packet_queue_t *new;

	new = calloc(1, sizeof(eigrp_packet_queue_t));
	return new;
}

/* Free eigrp packet queue. */
void eigrp_packet_queue_free(eigrp_packet_queue_t *queue)
{
	eigrp_packet_t *packet;
	eigrp_packet_t *next;

	if (!queue)
		return;

	for (packet = queue->head; packet; packet = next) {
		next = packet->next;
		eigrp_packet_free(packet);
	}
	queue->head = queue->tail = NULL;
	queue->count = 0;

	free(queue);
}

/* Free eigrp queue entries without destroying queue itself*/
void eigrp_packet_queue_reset(eigrp_packet_queue_t *queue)
{
	eigrp_packet_t *packet;
	eigrp_packet_t *next;

	if (!queue)
		return;

	for (packet = queue->head; packet; packet = next) {
		next = packet->next;
		eigrp_packet_free(packet);
	}
	queue->head = queue->tail = NULL;
	queue->count = 0;
}

eigrp_packet_t *eigrp_packet_new(size_t size, eigrp_neighbor_t *nbr)
{
	eigrp_packet_t *new;

	new = calloc(1, sizeof(eigrp_packet_t));
	new->s = eigrp_stream_new(size);
	new->retrans_counter = 0;
	new->nbr = nbr;

	return new;
}

void eigrp_packet_output_enqueue(eigrp_instance_t *eigrp, eigrp_interface_t *ei,
				      eigrp_packet_t *packet)
{
	if (!eigrp || !ei || !packet)
		return;

	eigrp_packet_enqueue(ei->obuf, packet);

	if (ei->on_write_q == 0) {
		eigrp_list_add(eigrp->oi_write_q, ei);
		ei->on_write_q = 1;
	}
	eigrp_packet_write_schedule(eigrp);
}

bool eigrp_packet_multicast_reliable_enqueue(eigrp_instance_t *eigrp,
					      eigrp_interface_t *ei,
					      eigrp_packet_t *packet)
{
	eigrp_neighbor_t *nbr;
	struct eigrp_header *header;
	eigrp_list_node_t *node;
	unsigned int receivers = 0;
	unsigned int ready = 0;
	unsigned int busy = 0;

	if (!eigrp || !ei || !packet || packet->sequence_number == 0)
		return false;

	for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
		if (nbr->state != EIGRP_NEIGHBOR_UP || !nbr->retrans_queue)
			continue;
		receivers++;
		if (nbr->retrans_queue->count == 0)
			ready++;
		else
			busy++;
	}

	if (!receivers) {
		eigrp_packet_free(packet);
		return false;
	}

	/* When only some peers can accept the multicast, advertise the peers
	 * that must ignore it and finalize CR in the packet image before any
	 * neighbor queue receives a copy. */
	if (ready && busy) {
		header = (struct eigrp_header *)eigrp_stream_data(packet->s);
		header->flags = htonl(ntohl(header->flags) | EIGRP_CR_FLAG);
		header->checksum = 0;
		if (ei->params.auth_type == EIGRP_AUTH_TYPE_MD5
		    && ei->params.auth_keychain != NULL)
			eigrp_make_md5_digest(ei, packet->s,
					      EIGRP_AUTH_UPDATE_FLAG);
		header->checksum = 0;
		eigrp_packet_checksum(ei, packet->s, packet->length);

		/* This Hello must be queued before the reliable multicast.  At this
		 * point only the pre-existing busy queues are visible to the Sequence
		 * TLV encoder. */
		eigrp_hello_send_sequence(ei, packet->sequence_number);
	}

	packet->dst.afi = AF_INET;
	packet->dst.ip.v4.s_addr = htonl(EIGRP_MULTICAST_ADDRESS);
	for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
		eigrp_packet_t *duplicate;

		if (nbr->state != EIGRP_NEIGHBOR_UP || !nbr->retrans_queue)
			continue;

		duplicate = eigrp_packet_duplicate(packet, nbr);
		eigrp_packet_enqueue(nbr->retrans_queue, duplicate);
		eigrp_debug_transmit_event(
			EIGRP_DEBUG_TRANSMIT_LINK, eigrp, ei, nbr,
			"linked multicast seq %u to reliable queue (depth %lu)",
			packet->sequence_number, nbr->retrans_queue->count);
	}

	/* If every peer already had an outstanding reliable packet, there is no
	 * useful multicast receiver.  Each queued copy will advance as an
	 * ordinary reliable unicast when that neighbor ACKs its current head. */
	if (!ready) {
		eigrp_packet_free(packet);
		return true;
	}

	eigrp_packet_output_enqueue(eigrp, ei, packet);
	return true;
}

void eigrp_packet_retransmit_timer_start(eigrp_neighbor_t *nbr)
{
	eigrp_packet_t *packet;
	uint32_t rto_msec;

	if (!nbr || !nbr->retrans_queue)
		return;

	packet = eigrp_packet_queue_next(nbr->retrans_queue);
	if (!packet)
		return;

	rto_msec = eigrp_neighbor_rto_get(nbr);
	if (IS_DEBUG_EIGRP(0, TIMERS)) {
		char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: start retransmit timer nbr %s seq %u interval %u ms",
			   eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src,
						  address, sizeof(address)),
			   packet->sequence_number, rto_msec);
	}
	eigrp_sys_timer_add(&packet->t_retrans_timer,
				 eigrp_packet_unack_retrans, nbr, rto_msec);
}

void eigrp_packet_send_reliably(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr)
{
	eigrp_packet_t *packet;

	packet = eigrp_packet_queue_next(nbr->retrans_queue);

	if (packet) {
		eigrp_packet_t *duplicate;
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_LINK, eigrp, nbr->ei, nbr,
				   "send reliable queue head seq %u (depth %lu)",
				   packet->sequence_number, nbr->retrans_queue->count);
		duplicate = eigrp_packet_duplicate(packet, nbr);
		if (eigrp_packet_destination_is_ipv4_multicast(packet))
			duplicate->multicast_exception = true;
		eigrp_addr_copy(&duplicate->dst, &nbr->src);
		eigrp_packet_output_enqueue(eigrp, nbr->ei, duplicate);
	}
}

/* Calculate EIGRP checksum */
void eigrp_packet_checksum(eigrp_interface_t *ei, eigrp_stream_t *s,
			   uint16_t length)
{
	struct eigrp_header *eigrph;

	eigrph = (struct eigrp_header *)eigrp_stream_data(s);

	/* Calculate checksum. */
	eigrph->checksum = eigrp_checksum(eigrph, length);
}


uint32_t eigrp_packet_sequence_reserve(eigrp_instance_t *eigrp)
{
	uint32_t sequence;

	if (!eigrp)
		return 0;

	if (eigrp->sequence_number == 0)
		eigrp->sequence_number = 1;

	sequence = eigrp->sequence_number++;
	if (eigrp->sequence_number == 0)
		eigrp->sequence_number = 1;

	return sequence;
}

/* Make EIGRP header. */
void eigrp_packet_header_init(int type, eigrp_instance_t *eigrp, eigrp_stream_t *s,
			      uint32_t flags, uint32_t sequence, uint32_t ack)
{
	struct eigrp_header *eigrph;

	eigrp_stream_reset(s);
	eigrph = (struct eigrp_header *)eigrp_stream_data(s);

	eigrph->version = (uint8_t)EIGRP_HEADER_VERSION;
	eigrph->opcode = (uint8_t)type;
	eigrph->checksum = 0;

	eigrph->vrid = htons(eigrp->vrid);
	eigrph->ASNumber = htons(eigrp->AS);
	eigrph->ack = htonl(ack);
	eigrph->sequence = htonl(sequence);
	//  if(flags == EIGRP_INIT_FLAG)
	//    eigrph->sequence = htonl(3);
	eigrph->flags = htonl(flags);

	if (IS_DEBUG_EIGRP_TRANSMIT(0, BUILD))
		eigrp_log(EIGRP_LOG_DEBUG, "Packet Header Init Seq [%u] Ack [%u]",
			   htonl(eigrph->sequence), htonl(eigrph->ack));

	eigrp_stream_forward_endp(s, EIGRP_HEADER_LEN);
}

/* Add new packet to head of queue. */
void eigrp_packet_enqueue(eigrp_packet_queue_t *queue, eigrp_packet_t *packet)
{
	packet->next = queue->head;
	packet->previous = NULL;

	if (queue->tail == NULL)
		queue->tail = packet;

	if (queue->count != 0)
		queue->head->previous = packet;

	queue->head = packet;

	queue->count++;
}

/* Return last queue entry. */
eigrp_packet_t *eigrp_packet_queue_next(eigrp_packet_queue_t *queue)
{
	return queue->tail;
}

void eigrp_packet_delete(eigrp_interface_t *ei)
{
	eigrp_packet_t *packet;

	packet = eigrp_packet_dequeue(ei->obuf);

	if (packet)
		eigrp_packet_free(packet);
}

void eigrp_packet_free(eigrp_packet_t *packet)
{
	if (packet->s)
		eigrp_stream_free(packet->s);

	eigrp_sys_event_cancel(&packet->t_retrans_timer);

	free(packet);
}

/* Return authentication TLV when present and validate TLV framing. */
static int eigrp_packet_auth_tlv_find(struct eigrp_header *eigrph,
				      uint16_t length,
				      struct eigrp_tlv_hdr_type **auth_tlv,
				      bool *auth_first)
{
	uint16_t offset;

	*auth_tlv = NULL;
	*auth_first = false;

	if (length < EIGRP_HEADER_LEN)
		return -1;

	offset = EIGRP_HEADER_LEN;
	while (offset < length) {
		struct eigrp_tlv_hdr_type *tlv;
		uint16_t tlv_type;
		uint16_t tlv_length;

		if ((length - offset) < EIGRP_TLV_HDR_SIZE)
			return -1;

		tlv = (struct eigrp_tlv_hdr_type *)((uint8_t *)eigrph + offset);
		tlv_type = ntohs(tlv->type);
		tlv_length = ntohs(tlv->length);

		if (tlv_length < EIGRP_TLV_HDR_SIZE || tlv_length > (length - offset))
			return -1;

		if (tlv_type == EIGRP_TLV_AUTH) {
			*auth_tlv = tlv;
			*auth_first = (offset == EIGRP_HEADER_LEN);
		}

		offset += tlv_length;
	}

	return 0;
}

static int eigrp_packet_auth_tlv_validate(eigrp_interface_t *ei,
					  struct eigrp_tlv_hdr_type *auth_tlv,
					  uint16_t length)
{
	uint16_t auth_type;
	uint16_t auth_length;

	if (length < EIGRP_AUTH_MD5_TLV_SIZE)
		return -1;

	auth_type = ntohs(((struct TLV_MD5_Authentication_Type *)auth_tlv)->auth_type);
	auth_length = ntohs(((struct TLV_MD5_Authentication_Type *)auth_tlv)->auth_length);

	if (auth_type != ei->params.auth_type) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication type mismatch: received %u expected %u",
			  eigrp_intf_name_string(ei), auth_type, ei->params.auth_type);
		return -1;
	}

	switch (auth_type) {
	case EIGRP_AUTH_TYPE_MD5:
		if (length != EIGRP_AUTH_MD5_TLV_SIZE
		    || auth_length != EIGRP_AUTH_TYPE_MD5_LEN)
			return -1;
		return 0;
	case EIGRP_AUTH_TYPE_SHA256:
		if (length != EIGRP_AUTH_SHA256_TLV_SIZE
		    || auth_length != EIGRP_AUTH_TYPE_SHA256_LEN)
			return -1;
		return 0;
	default:
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: unsupported EIGRP authentication type %u",
			  eigrp_intf_name_string(ei), auth_type);
		return -1;
	}
}

static int eigrp_packet_auth_header_validate(eigrp_interface_t *ei,
					     struct eigrp_header *eigrph,
					     uint16_t length)
{
	struct eigrp_tlv_hdr_type *auth_tlv;
	bool auth_first;
	int ret;

	ret = eigrp_packet_auth_tlv_find(eigrph, length, &auth_tlv, &auth_first);
	if (ret < 0) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: malformed EIGRP TLV framing",
			  eigrp_intf_name_string(ei));
		return -1;
	}

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_NONE) {
		if (auth_tlv) {
			eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication TLV received on unauthenticated interface",
				  eigrp_intf_name_string(ei));
			return -1;
		}
		return 0;
	}

	if (!ei->params.auth_keychain) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication configured without keychain",
			  eigrp_intf_name_string(ei));
		return -1;
	}

	if (!auth_tlv) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authenticated interface received packet without auth TLV",
			  eigrp_intf_name_string(ei));
		return -1;
	}

	if (!auth_first) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication TLV is not first TLV",
			  eigrp_intf_name_string(ei));
		return -1;
	}

	return eigrp_packet_auth_tlv_validate(ei, auth_tlv, ntohs(auth_tlv->length));
}

static uint8_t eigrp_packet_auth_flags_get(struct eigrp_header *eigrph)
{
	if (eigrph->opcode == EIGRP_OPC_HELLO)
		return EIGRP_AUTH_BASIC_HELLO_FLAG;

	if (eigrph->opcode == EIGRP_OPC_UPDATE
	    && (ntohl(eigrph->flags) & EIGRP_INIT_FLAG))
		return EIGRP_AUTH_UPDATE_INIT_FLAG;

	return EIGRP_AUTH_UPDATE_FLAG;
}

static int eigrp_packet_auth_digest_validate(eigrp_interface_t *ei,
					     eigrp_neighbor_t *nbr,
					     struct eigrp_header *eigrph,
					     uint16_t length)
{
	struct eigrp_tlv_hdr_type *auth_tlv;
	eigrp_stream_t *auth_stream;
	eigrp_neighbor_t tmp_nbr;
	bool auth_first;
	int ret;

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_NONE)
		return 0;

	ret = eigrp_packet_auth_tlv_find(eigrph, length, &auth_tlv, &auth_first);
	if (ret < 0 || !auth_tlv || !auth_first)
		return -1;

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_SHA256) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP SHA256 authentication receive validation is not implemented",
			  eigrp_intf_name_string(ei));
		return -1;
	}

	if (ei->params.auth_type != EIGRP_AUTH_TYPE_MD5)
		return -1;

	memset(&tmp_nbr, 0, sizeof(tmp_nbr));
	if (!nbr) {
		tmp_nbr.ei = ei;
		nbr = &tmp_nbr;
	}

	auth_stream = eigrp_stream_new(length);
	eigrp_stream_put(auth_stream, eigrph, length);

	ret = eigrp_check_md5_digest(
		auth_stream,
		(struct TLV_MD5_Authentication_Type *)(eigrp_stream_data(auth_stream)
							   + EIGRP_HEADER_LEN),
		nbr, eigrp_packet_auth_flags_get(eigrph));

	eigrp_stream_free(auth_stream);
	if (!ret) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP MD5 authentication failed",
			  eigrp_intf_name_string(ei));
		return -1;
	}

	return 0;
}

/* EIGRP Header verification. */
static int eigrp_verify_header(eigrp_interface_t *ei, eigrp_addr_t *source,
			       struct eigrp_header *eigrph, uint16_t length)
{
	char source_text[EIGRP_PACKET_ADDR_TEXT_SIZE];
	uint16_t checksum;

	if (length < EIGRP_HEADER_LEN) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP packet too short: %u",
			  eigrp_intf_name_string(ei), length);
		return -1;
	}

	if (eigrph->version != EIGRP_HEADER_VERSION) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: unsupported EIGRP header version %u",
			  eigrp_intf_name_string(ei), eigrph->version);
		return -1;
	}

	if (ntohs(eigrph->ASNumber) != ei->eigrp->AS) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP AS mismatch: received %u expected %u",
			  eigrp_intf_name_string(ei), ntohs(eigrph->ASNumber),
			  ei->eigrp->AS);
		return -1;
	}

	if (ntohs(eigrph->vrid) != ei->eigrp->vrid) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP VRID mismatch: received %u expected %u",
			  eigrp_intf_name_string(ei), ntohs(eigrph->vrid),
			  ei->eigrp->vrid);
		return -1;
	}

	checksum = eigrp_checksum(eigrph, length);
	if (checksum != 0) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP checksum failed from %s",
			  eigrp_intf_name_string(ei),
			  eigrp_packet_addr_text(ei->eigrp, source, source_text,
					 sizeof(source_text)));
		return -1;
	}

	if (eigrp_packet_auth_header_validate(ei, eigrph, length) < 0)
		return -1;

	/* Raw sockets can receive protocol-matched packets from other links. */
	if (!ei->eigrp->af_vectors.packet_source_on_link(ei, source)) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: eigrp_packet_read source is not on-link [%s]",
			  eigrp_intf_name_string(ei),
			  eigrp_packet_addr_text(ei->eigrp, source, source_text,
					 sizeof(source_text)));
		return -1;
	}

	return 0;
}

void eigrp_packet_unack_retrans(void *arg)
{
	eigrp_neighbor_t *nbr = arg;
	eigrp_packet_t *packet;
	eigrp_packet_t *duplicate;

	packet = eigrp_packet_queue_next(nbr->retrans_queue);
	if (!packet)
		return;

	/* Give the peer all 16 retries.  Tear it down only after retry 16
	 * has itself gone unacknowledged. */
	if (packet->retrans_counter >= EIGRP_TRANSPORT_RETRANS_MAX) {
		eigrp_packet_retransmit_limit_exceeded(nbr);
		return;
	}

	eigrp_neighbor_rto_backoff(nbr);
	if (IS_DEBUG_EIGRP(0, TIMERS)) {
		char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: retransmit timer expired nbr %s seq %u retry %u",
			   eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src, address,
						  sizeof(address)),
			   packet->sequence_number, packet->retrans_counter + 1);
	}
	eigrp_debug_packet_retry(nbr, packet, packet->retrans_counter + 1);
	duplicate = eigrp_packet_duplicate(packet, nbr);
	duplicate->retransmission = true;
	if (eigrp_packet_destination_is_ipv4_multicast(packet))
		duplicate->multicast_exception = true;
	eigrp_addr_copy(&duplicate->dst, &nbr->src);
	eigrp_packet_output_enqueue(nbr->ei->eigrp, nbr->ei, duplicate);

	packet->retrans_counter++;
	eigrp_sys_timer_add(&packet->t_retrans_timer,
				 eigrp_packet_unack_retrans, nbr,
				 eigrp_neighbor_rto_get(nbr));
}

/* Get packet from tail of queue. */
eigrp_packet_t *eigrp_packet_dequeue(eigrp_packet_queue_t *queue)
{
	eigrp_packet_t *packet = NULL;

	packet = queue->tail;

	if (packet) {
		queue->tail = packet->previous;

		if (queue->tail == NULL)
			queue->head = NULL;
		else
			queue->tail->next = NULL;

		queue->count--;
	}

	return packet;
}

eigrp_packet_t *eigrp_packet_duplicate(eigrp_packet_t *old,
				       eigrp_neighbor_t *nbr)
{
	eigrp_packet_t *new;

	if (!old || !old->s)
		return NULL;

	new = eigrp_packet_new(old->length, nbr);
	new->length = old->length;
	new->retrans_counter = old->retrans_counter;
	new->dst = old->dst;
	new->sequence_number = old->sequence_number;
	new->retransmission = false;
	new->multicast_exception = false;
	eigrp_stream_copy(new->s, old->s);

	return new;
}

struct TLV_Sequence_Type *eigrp_SequenceTLV_new(void)
{
	struct TLV_Sequence_Type *new;

	new = calloc(1, sizeof(struct TLV_Sequence_Type));

	return new;
}
