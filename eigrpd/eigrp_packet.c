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
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_auth.h"

#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_errors.h"
#include "eigrpd/eigrp_southbound.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_PACKET,          "EIGRP Packet");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_PACKET_QUEUE,    "EIGRP Packet Queue");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_SEQ_TLV,         "EIGRP SEQ TLV");

/* Packet Type String. */
const struct message eigrp_packet_type_str[] = {
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
	if (!eigrp || !address || !eigrp->af_vectors.addr_snprintf
	    || eigrp->af_vectors.addr_snprintf(buf, len, address) < 0)
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
		stream_set_getp(pkt, stream_get_endp(pkt));
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

	start = stream_get_endp(pkt);
	len1 = eigrp->tlv1_codec.encoder(eigrp, ei, nbr, pkt, route);
	len2 = eigrp->tlv2_codec.encoder(eigrp, ei, nbr, pkt, route);

	if (!len1 || !len2) {
		stream_set_endp(pkt, start);
		return 0;
	}

	return len1 + len2;
}

static void eigrp_packet_retransmit_limit_exceeded(eigrp_neighbor_t *nbr)
{
	char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

	if (!nbr || !nbr->ei || !nbr->ei->eigrp)
		return;

	if (nbr->ei->eigrp->log_neighbor_changes)
		zlog_info("Neighbor %s (%s) is down: retry limit exceeded",
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

	packet = eigrp_packet_queue_next(nbr->retrans_queue);
	if ((packet) && (ntohl(eigrph->ack) == packet->sequence_number)) {
		eigrp_neighbor_srtt_update(nbr, packet);
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_ACK, eigrp, nbr->ei, nbr,
				   "ACK %u matched reliable sequence", ntohl(eigrph->ack));
		packet = eigrp_packet_dequeue(nbr->retrans_queue);
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_LINK, eigrp, nbr->ei, nbr,
				   "unlinked ACKed seq %u from reliable queue (depth %lu)",
				   ntohl(eigrph->ack), nbr->retrans_queue->count);
		eigrp_packet_free(packet);

		if ((nbr->state == EIGRP_NEIGHBOR_PENDING)
		    && (ntohl(eigrph->ack) == nbr->init_sequence_number)) {
			eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_UP);
			{
				char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

				if (eigrp->log_neighbor_changes)
					zlog_info("Neighbor %s (%s) is up: new adjacency",
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

	packet = eigrp_packet_queue_next(nbr->multicast_queue);
	if (packet) {
		if (ntohl(eigrph->ack) == packet->sequence_number) {
			eigrp_neighbor_srtt_update(nbr, packet);
			eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_ACK, eigrp, nbr->ei, nbr,
					   "ACK %u matched multicast sequence", ntohl(eigrph->ack));
			packet = eigrp_packet_dequeue(nbr->multicast_queue);
			eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_LINK, eigrp, nbr->ei, nbr,
					   "unlinked ACKed seq %u from multicast queue (depth %lu)",
					   ntohl(eigrph->ack), nbr->multicast_queue->count);
			eigrp_packet_free(packet);
			if (nbr->multicast_queue->count > 0) {
				eigrp_packet_send_reliably(eigrp, nbr);
			}
		}
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
	if ((!queued || queued->sequence_number != sequence)
	    && nbr->multicast_queue)
		queued = eigrp_packet_queue_next(nbr->multicast_queue);
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
	struct listnode *node;
	uint64_t now_msec;

	if (!ei || !packet || packet->sequence_number == 0 || packet->retransmission)
		return;

	now_msec = eigrp_southbound_monotime_msec();
	if (packet->nbr) {
		eigrp_packet_reliable_neighbor_send_record(packet->nbr,
						 packet->sequence_number, now_msec);
		return;
	}

	/* One reliable multicast wire send is an independent RTT start point for
	 * every neighbor that currently owns this sequence in its RTP queue. */
	for (ALL_LIST_ELEMENTS_RO(ei->nbrs, node, nbr))
		eigrp_packet_reliable_neighbor_send_record(nbr, packet->sequence_number,
						 now_msec);
}

void eigrp_packet_write_schedule(eigrp_instance_t *eigrp)
{
	if (!eigrp || eigrp->t_write)
		return;
	eigrp_southbound_write_add(&eigrp->t_write, eigrp->fd,
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
	struct listnode *node;

	node = listhead(eigrp->oi_write_q);
	assert(node);
	ei = listgetdata(node);
	assert(ei);

	/* Get one packet from queue. */
	packet = eigrp_packet_queue_next(ei->obuf);
	if (!packet) {
		flog_err(EC_LIB_DEVELOPMENT,
			 "%s: Interface %s no packet on queue?", __func__,
			 ei->name);
		goto out;
	}
	if (packet->length < EIGRP_HEADER_LEN) {
		flog_err(EC_EIGRP_PACKET, "%s: Packet just has a header?",
			 __func__);
		eigrp_header_dump((struct eigrp_header *)packet->s->data);
		eigrp_packet_delete(ei);
		goto out;
	}

	/*
	 * We build and schedule packets to go out in the future.  In the mean
	 * time we may process some update packets from the neighbor, thus making
	 * it necessary to update the ACK used by this outgoing packet.
	 */
	eigrph = (struct eigrp_header *)STREAM_DATA(packet->s);
	seqno = ntohl(eigrph->sequence);
	ack = ntohl(eigrph->ack);
	if (packet->nbr && ack != packet->nbr->recv_sequence_number) {
		eigrph->ack = htonl(packet->nbr->recv_sequence_number);
		ack = packet->nbr->recv_sequence_number;
		eigrph->checksum = 0;
		eigrp_packet_checksum(ei, packet->s, packet->length);
	}

	if (!eigrp->af_vectors.packet_send) {
		zlog_warn("%s: no address-family packet sender is bound", __func__);
		ret = -1;
	} else {
		ret = eigrp->af_vectors.packet_send(eigrp, ei, packet);
	}

	eigrp_debug_packet_send(ei, packet, ret);
	if (ret >= 0) {
		eigrp_packet_send_stats_record(ei, packet, eigrph);
		eigrp_packet_reliable_send_record(ei, packet);
	}

	if (IS_DEBUG_EIGRP_TRANSMIT(0, DETAIL)) {
		char destination[EIGRP_PACKET_ADDR_TEXT_SIZE];

		eigrph = (struct eigrp_header *)STREAM_DATA(packet->s);
		zlog_debug(
			"Sending [%s][%d/%d] to [%s] via [%s] ret [%d].",
			lookup_msg(eigrp_packet_type_str, eigrph->opcode, NULL),
			seqno, ack,
			eigrp_packet_addr_text(eigrp, &packet->dst, destination,
					       sizeof(destination)),
			EIGRP_INTF_NAME(ei), ret);
	}

	/* Now delete packet from queue. */
	eigrp_packet_delete(ei);

out:
	if (eigrp_packet_queue_next(ei->obuf) == NULL) {
		ei->on_write_q = 0;
		list_delete_node(eigrp->oi_write_q, node);
	}

	/* If packets still remain in queue, call write event. */
	if (!list_isempty(eigrp->oi_write_q))
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
	eigrp_southbound_read_add(&eigrp->t_read, eigrp->fd,
			    eigrp_packet_read, eigrp);

	stream_reset(eigrp->ibuf);
	if (!eigrp->af_vectors.packet_receive) {
		zlog_warn("%s: no address-family packet receiver is bound", __func__);
		return;
	}

	ibuf = eigrp->ibuf;
	if (!eigrp->af_vectors.packet_receive(eigrp, eigrp->fd, ibuf, &ei,
					      &src, &dst, &meta))
		return;

	if (!ei)
		return;

	stream_forward_getp(ibuf, meta.network_header_length);
	eigrph = (struct eigrp_header *)stream_pnt(ibuf);
	length = meta.eigrp_length;

	if (IS_DEBUG_EIGRP_TRANSMIT(0, DETAIL))
		eigrp_header_dump(eigrph);

	/* If incoming interface is passive, ignore it. */
	if (eigrp_intf_is_passive(ei)) {
		if (IS_DEBUG_EIGRP_TRANSMIT(0, STRANGE)) {
			char destination[EIGRP_PACKET_ADDR_TEXT_SIZE];

			zlog_debug(
				"ignoring packet from router %u sent to %s, received on passive interface %s",
				ntohs(eigrph->vrid),
				eigrp_packet_addr_text(eigrp, &dst, destination,
						       sizeof(destination)),
				EIGRP_INTF_NAME(ei));
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

			zlog_debug("eigrp_packet_read[%s]: Header check failed, dropping.",
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
		zlog_debug(
			"Received [%s][%d/%d] length [%u] via [%s] src [%s] dst [%s]",
			lookup_msg(eigrp_packet_type_str, opcode, NULL),
			ntohl(eigrph->sequence), ntohl(eigrph->ack), length,
			EIGRP_INTF_NAME(ei), source_text, destination_text);
	}

	/* Move from the EIGRP header to the first TLV. */
	stream_forward_getp(ibuf, EIGRP_HEADER_LEN);

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
		eigrp_hello_receive(eigrp, eigrph, &src, ei, ibuf, length);
		return;
	}

	/* A neighbor must exist before accepting non-Hello packets. */
	if (!nbr)
		return;

	if (eigrp_packet_auth_digest_validate(ei, nbr, eigrph, length) < 0)
		return;

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
		zlog_warn("interface %s: EIGRP packet header type %d unsupported",
			  EIGRP_INTF_NAME(ei), opcode);
		break;
	}
}

eigrp_packet_queue_t *eigrp_packet_queue_new(void)
{
	eigrp_packet_queue_t *new;

	new = XCALLOC(MTYPE_EIGRP_PACKET_QUEUE, sizeof(eigrp_packet_queue_t));
	return new;
}

/* Free eigrp packet queue. */
void eigrp_packet_queue_free(eigrp_packet_queue_t *queue)
{
	eigrp_packet_t *packet;
	eigrp_packet_t *next;

	for (packet = queue->head; packet; packet = next) {
		next = packet->next;
		eigrp_packet_free(packet);
	}
	queue->head = queue->tail = NULL;
	queue->count = 0;

	XFREE(MTYPE_EIGRP_PACKET_QUEUE, queue);
}

/* Free eigrp queue entries without destroying queue itself*/
void eigrp_packet_queue_reset(eigrp_packet_queue_t *queue)
{
	eigrp_packet_t *packet;
	eigrp_packet_t *next;

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

	new = XCALLOC(MTYPE_EIGRP_PACKET, sizeof(eigrp_packet_t));
	new->s = stream_new(size);
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
		listnode_add(eigrp->oi_write_q, ei);
		ei->on_write_q = 1;
	}
	eigrp_packet_write_schedule(eigrp);
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

		zlog_debug("EIGRP: start retransmit timer nbr %s seq %u interval %u ms",
			   eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src,
						  address, sizeof(address)),
			   packet->sequence_number, rto_msec);
	}
	eigrp_southbound_timer_msec_add(&packet->t_retrans_timer,
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
		eigrp_packet_output_enqueue(eigrp, nbr->ei, duplicate);
		eigrp_packet_retransmit_timer_start(nbr);

		if (!packet->sequence_reserved) {
			nbr->ei->eigrp->sequence_number++;
			if (nbr->ei->eigrp->sequence_number == 0)
				nbr->ei->eigrp->sequence_number = 1;
			packet->sequence_reserved = true;
		}
	}
}

/* Calculate EIGRP checksum */
void eigrp_packet_checksum(eigrp_interface_t *ei, struct stream *s,
			   uint16_t length)
{
	struct eigrp_header *eigrph;

	eigrph = (struct eigrp_header *)STREAM_DATA(s);

	/* Calculate checksum. */
	eigrph->checksum = in_cksum(eigrph, length);
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
void eigrp_packet_header_init(int type, eigrp_instance_t *eigrp, struct stream *s,
			      uint32_t flags, uint32_t sequence, uint32_t ack)
{
	struct eigrp_header *eigrph;

	stream_reset(s);
	eigrph = (struct eigrp_header *)STREAM_DATA(s);

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
		zlog_debug("Packet Header Init Seq [%u] Ack [%u]",
			   htonl(eigrph->sequence), htonl(eigrph->ack));

	stream_forward_endp(s, EIGRP_HEADER_LEN);
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
		stream_free(packet->s);

	eigrp_southbound_event_cancel(&packet->t_retrans_timer);

	XFREE(MTYPE_EIGRP_PACKET, packet);
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
		zlog_warn("interface %s: EIGRP authentication type mismatch: received %u expected %u",
			  EIGRP_INTF_NAME(ei), auth_type, ei->params.auth_type);
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
		zlog_warn("interface %s: unsupported EIGRP authentication type %u",
			  EIGRP_INTF_NAME(ei), auth_type);
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
		zlog_warn("interface %s: malformed EIGRP TLV framing",
			  EIGRP_INTF_NAME(ei));
		return -1;
	}

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_NONE) {
		if (auth_tlv) {
			zlog_warn("interface %s: EIGRP authentication TLV received on unauthenticated interface",
				  EIGRP_INTF_NAME(ei));
			return -1;
		}
		return 0;
	}

	if (!ei->params.auth_keychain) {
		zlog_warn("interface %s: EIGRP authentication configured without keychain",
			  EIGRP_INTF_NAME(ei));
		return -1;
	}

	if (!auth_tlv) {
		zlog_warn("interface %s: EIGRP authenticated interface received packet without auth TLV",
			  EIGRP_INTF_NAME(ei));
		return -1;
	}

	if (!auth_first) {
		zlog_warn("interface %s: EIGRP authentication TLV is not first TLV",
			  EIGRP_INTF_NAME(ei));
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
	struct stream *auth_stream;
	eigrp_neighbor_t tmp_nbr;
	bool auth_first;
	int ret;

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_NONE)
		return 0;

	ret = eigrp_packet_auth_tlv_find(eigrph, length, &auth_tlv, &auth_first);
	if (ret < 0 || !auth_tlv || !auth_first)
		return -1;

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_SHA256) {
		zlog_warn("interface %s: EIGRP SHA256 authentication receive validation is not implemented",
			  EIGRP_INTF_NAME(ei));
		return -1;
	}

	if (ei->params.auth_type != EIGRP_AUTH_TYPE_MD5)
		return -1;

	memset(&tmp_nbr, 0, sizeof(tmp_nbr));
	if (!nbr) {
		tmp_nbr.ei = ei;
		nbr = &tmp_nbr;
	}

	auth_stream = stream_new(length);
	stream_put(auth_stream, eigrph, length);

	ret = eigrp_check_md5_digest(
		auth_stream,
		(struct TLV_MD5_Authentication_Type *)(STREAM_DATA(auth_stream)
							   + EIGRP_HEADER_LEN),
		nbr, eigrp_packet_auth_flags_get(eigrph));

	stream_free(auth_stream);
	if (!ret) {
		zlog_warn("interface %s: EIGRP MD5 authentication failed",
			  EIGRP_INTF_NAME(ei));
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
		zlog_warn("interface %s: EIGRP packet too short: %u",
			  EIGRP_INTF_NAME(ei), length);
		return -1;
	}

	if (eigrph->version != EIGRP_HEADER_VERSION) {
		zlog_warn("interface %s: unsupported EIGRP header version %u",
			  EIGRP_INTF_NAME(ei), eigrph->version);
		return -1;
	}

	if (ntohs(eigrph->ASNumber) != ei->eigrp->AS) {
		zlog_warn("interface %s: EIGRP AS mismatch: received %u expected %u",
			  EIGRP_INTF_NAME(ei), ntohs(eigrph->ASNumber),
			  ei->eigrp->AS);
		return -1;
	}

	if (ntohs(eigrph->vrid) != ei->eigrp->vrid) {
		zlog_warn("interface %s: EIGRP VRID mismatch: received %u expected %u",
			  EIGRP_INTF_NAME(ei), ntohs(eigrph->vrid),
			  ei->eigrp->vrid);
		return -1;
	}

	checksum = in_cksum(eigrph, length);
	if (checksum != 0) {
		zlog_warn("interface %s: EIGRP checksum failed from %s",
			  EIGRP_INTF_NAME(ei),
			  eigrp_packet_addr_text(ei->eigrp, source, source_text,
					 sizeof(source_text)));
		return -1;
	}

	if (eigrp_packet_auth_header_validate(ei, eigrph, length) < 0)
		return -1;

	/* Raw sockets can receive protocol-matched packets from other links. */
	if (!ei->eigrp->af_vectors.packet_source_on_link
	    || !ei->eigrp->af_vectors.packet_source_on_link(ei, source)) {
		zlog_warn("interface %s: eigrp_packet_read source is not on-link [%s]",
			  EIGRP_INTF_NAME(ei),
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

		zlog_debug("EIGRP: retransmit timer expired nbr %s seq %u retry %u",
			   eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src, address,
						  sizeof(address)),
			   packet->sequence_number, packet->retrans_counter + 1);
	}
	eigrp_debug_packet_retry(nbr, packet, packet->retrans_counter + 1);
	duplicate = eigrp_packet_duplicate(packet, nbr);
	duplicate->retransmission = true;
	eigrp_addr_copy(&duplicate->dst, &nbr->src);
	eigrp_packet_output_enqueue(nbr->ei->eigrp, nbr->ei, duplicate);

	packet->retrans_counter++;
	eigrp_southbound_timer_msec_add(&packet->t_retrans_timer,
				 eigrp_packet_unack_retrans, nbr,
				 eigrp_neighbor_rto_get(nbr));
}

void eigrp_packet_unack_multicast_retrans(void *arg)
{
	eigrp_neighbor_t *nbr = arg;
	eigrp_packet_t *packet;
	eigrp_packet_t *duplicate;

	packet = eigrp_packet_queue_next(nbr->multicast_queue);
	if (!packet)
		return;

	if (packet->retrans_counter >= EIGRP_TRANSPORT_RETRANS_MAX) {
		eigrp_packet_retransmit_limit_exceeded(nbr);
		return;
	}

	eigrp_neighbor_rto_backoff(nbr);
	if (IS_DEBUG_EIGRP(0, TIMERS)) {
		char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

		zlog_debug("EIGRP: retransmit timer expired nbr %s seq %u retry %u",
			   eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src, address,
						  sizeof(address)),
			   packet->sequence_number, packet->retrans_counter + 1);
	}
	eigrp_debug_packet_retry(nbr, packet, packet->retrans_counter + 1);
	duplicate = eigrp_packet_duplicate(packet, nbr);
	duplicate->retransmission = true;
	duplicate->multicast_exception = true;
	eigrp_addr_copy(&duplicate->dst, &nbr->src);
	eigrp_packet_output_enqueue(nbr->ei->eigrp, nbr->ei, duplicate);

	packet->retrans_counter++;
	eigrp_southbound_timer_msec_add(&packet->t_retrans_timer,
				 eigrp_packet_unack_multicast_retrans, nbr,
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

	new = eigrp_packet_new(EIGRP_PACKET_MTU(nbr->ei->curr_mtu), nbr);
	new->length = old->length;
	new->retrans_counter = old->retrans_counter;
	new->dst = old->dst;
	new->sequence_number = old->sequence_number;
	new->sequence_reserved = old->sequence_reserved;
	new->retransmission = false;
	new->multicast_exception = false;
	stream_copy(new->s, old->s);

	return new;
}

struct TLV_Sequence_Type *eigrp_SequenceTLV_new(void)
{
	struct TLV_Sequence_Type *new;

	new = XCALLOC(MTYPE_EIGRP_SEQ_TLV, sizeof(struct TLV_Sequence_Type));

	return new;
}
