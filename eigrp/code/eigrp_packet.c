// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP General Sending and Receiving of EIGRP Packets.
 * Copyright (C) 2013-2014, 2026
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eigrp.h"
#include "eigrp_log.h"
#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_instance.h"
#include "eigrp_neighbor.h"
#include "eigrp_packet.h"
#include "eigrp_checksum.h"
#include "eigrp_auth.h"

#include "eigrp_topology.h"
#include "eigrp_debug.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
#include "eigrp_eventlog.h"
#include "eigrp_timer.h"
#include "eigrp_packetizer.h"
#include "eigrp_pfxlimits.h"
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
static int eigrp_packet_header_validate(eigrp_intf_t *ei, eigrp_addr_t *source,
			       struct eigrp_header *header, uint16_t length);
static int eigrp_packet_auth_header_validate(eigrp_intf_t *ei,
					     const eigrp_addr_t *source,
					     struct eigrp_header *eigrph,
					     uint16_t length);
static int eigrp_packet_auth_digest_validate(eigrp_intf_t *ei,
					     eigrp_nbr_t *nbr,
					     const eigrp_addr_t *source,
					     struct eigrp_header *eigrph,
					     uint16_t length);

#define EIGRP_PACKET_ADDR_TEXT_SIZE 64U
#define EIGRP_AF_PACKET_INPUT_QUEUE_MAX 1024U
#define EIGRP_AF_PACKET_DISCOVERY_QUEUE_MAX 256U

/* Validate receive metadata against the bytes actually present in the stream.
 * Metadata is supplied by the host adapter and must never enlarge the portable
 * parser's memory boundary. */
static bool eigrp_packet_input_bounds_validate(size_t endp,
					size_t network_header_length,
					size_t declared_eigrp_length,
					size_t *offset, uint16_t *length)
{
	size_t remaining;

	if (!offset || !length || network_header_length > endp)
		return false;

	remaining = endp - network_header_length;
	if (declared_eigrp_length < EIGRP_HEADER_LEN
	    || declared_eigrp_length > remaining
	    || declared_eigrp_length > UINT16_MAX)
		return false;

	*offset = network_header_length;
	*length = (uint16_t)declared_eigrp_length;
	return true;
}

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

static bool eigrp_packet_destination_is_multicast(const eigrp_packet_t *packet)
{
	if (!packet)
		return false;
	if (packet->dst.afi == AF_INET)
		return packet->dst.ip.v4.s_addr == htonl(EIGRP_MULTICAST_ADDRESS);
	if (packet->dst.afi == AF_INET6)
		return IN6_IS_ADDR_MULTICAST(&packet->dst.ip.v6);
	return false;
}

static bool eigrp_packet_eventlog_opcode(uint8_t opcode)
{
	return opcode == EIGRP_OPC_UPDATE || opcode == EIGRP_OPC_QUERY
	       || opcode == EIGRP_OPC_REPLY || opcode == EIGRP_OPC_SIAQUERY
	       || opcode == EIGRP_OPC_SIAREPLY;
}

static void eigrp_packet_send_stats_update(eigrp_intf_t *ei,
					   const eigrp_packet_t *packet,
					   const struct eigrp_header *header)
{
	bool reliable;
	bool multicast;

	if (!ei || !packet || !header)
		return;

	eigrp_packet_opcode_counter_increment(&ei->stats, true, header);
	reliable = ntohl(header->sequence) != 0;
	multicast = eigrp_packet_destination_is_multicast(packet);
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

static void eigrp_packet_receive_stats_update(eigrp_intf_t *ei,
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

static void eigrp_packet_reject_event(eigrp_intf_t *ei,
				      const eigrp_addr_t *source, uint8_t opcode,
				      eigrp_eventlog_packet_reject_reason_t reason,
				      eventmsg_arg_t detail1, eventmsg_arg_t detail2)
{
	eigrp_prefix_t peer_addr;

	if (!ei || !ei->eigrp || !source)
		return;
	eigrp_eventlog_addr_from_legacy(&peer_addr, source);
	(void)eigrp_eventlog_msg_add(ei->eigrp, EIGRP_EVENTLOG_OPCODE_PACKET_REJECT,
		&peer_addr, opcode, reason, detail1, detail2);
}

eigrp_route_descriptor_t *eigrp_packet_decoder_safe(eigrp_instance_t *eigrp,
						    eigrp_nbr_t *nbr,
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
					  eigrp_intf_t *ei,
					  eigrp_nbr_t *nbr,
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
					  eigrp_intf_t *ei,
					  eigrp_nbr_t *nbr,
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
				     eigrp_intf_t *ei,
				     eigrp_nbr_t *nbr,
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
	scratch = eigrp_stream_create(packet_limit);
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

static void eigrp_packet_retransmit_limit_exceeded(eigrp_nbr_t *nbr)
{
	char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

	if (!nbr || !nbr->ei || !nbr->ei->eigrp)
		return;

	if (nbr->ei->eigrp->log_neighbor_changes)
		eigrp_log(EIGRP_LOG_INFO, "Neighbor %s (%s) is down: retry limit exceeded",
			  eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src, address,
					 sizeof(address)),
			  nbr->ei->name);
	eigrp_nbr_state_update_reason(EIGRP_SET, nbr, EIGRP_NEIGHBOR_DOWN,
		EIGRP_EVENTLOG_NBR_RTP_RETRY_LIMIT);
	eigrp_nbr_delete(nbr);
}

/* Paced output retains neighbor pointers until send.  Never allow a
 * deleted peer or already-ACKed retry to remain in an interface output
 * queue; they may wait there for many milliseconds on constrained links. */
static void eigrp_packet_neighbor_output_drop(eigrp_nbr_t *nbr,
                                              uint32_t sequence, bool retries_only)
{
	eigrp_packet_queue_t *queue;
	eigrp_packet_t *packet, *next;
	eigrp_intf_t *ei;

	if (!nbr || !(ei = nbr->ei) || !(queue = ei->obuf))
		return;
	for (packet = queue->tail; packet; packet = next) {
		next = packet->previous;
		if (packet->nbr != nbr || (retries_only &&
		    (!packet->retransmission || packet->sequence_number != sequence)))
			continue;
		if (packet->previous)
			packet->previous->next = packet->next;
		else
			queue->head = packet->next;
		if (packet->next)
			packet->next->previous = packet->previous;
		else
			queue->tail = packet->previous;
		queue->count--;
		eigrp_packet_free(packet);
	}
	if (!queue->count && ei->on_write_q) {
		eigrp_list_delete_data(ei->eigrp->oi_write_q, ei);
		ei->on_write_q = 0;
	}
}

void eigrp_packet_neighbor_output_clear(eigrp_nbr_t *nbr)
{
	eigrp_packet_neighbor_output_drop(nbr, 0, false);
}

/* Interface DOWN invalidates the entire outbound wire queue, including
 * control Hellos/ACKs and CR SEQUENCE Hellos from the old adjacency lifetime.
 * Drop queued packets only after any deliberate goodbye has been transmitted. */
void eigrp_packet_interface_output_clear(eigrp_intf_t *ei)
{
	if (!ei)
		return;
	if (ei->on_write_q && ei->eigrp && ei->eigrp->oi_write_q)
		eigrp_list_delete_data(ei->eigrp->oi_write_q, ei);
	ei->on_write_q = 0;
	eigrp_packet_queue_clear(ei->obuf);
}

static void eigrp_packet_neighbor_retry_cancel(eigrp_nbr_t *nbr,
                                                uint32_t sequence)
{
	eigrp_packet_neighbor_output_drop(nbr, sequence, true);
}

/*
 * New testing block of code for handling Acks
 */
static void eigrp_packet_ack(eigrp_instance_t *eigrp, struct eigrp_header *eigrph,
			     eigrp_nbr_t *nbr)
{
	struct eigrp_packet *packet = NULL;
	uint32_t ack;

	if (!eigrp || !eigrph || !nbr || !nbr->retrans_queue)
		return;

	ack = ntohl(eigrph->ack);

	packet = eigrp_packet_queue_next(nbr->retrans_queue);
	if (packet && ack == packet->sequence_number) {
		eigrp_nbr_srtt_update(nbr, packet);
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_ACK, eigrp, nbr->ei, nbr,
				   "ACK %u matched reliable sequence", ack);
		{
			eigrp_prefix_t peer_addr;

			eigrp_eventlog_addr_from_legacy(&peer_addr, &nbr->src);
			(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_RTP_ACK,
				&peer_addr, ack, nbr->retrans_queue->count, 0, 0);
		}
		/* A pacing-delayed retry must not go out after its ACK. */
		if (packet->retry_queued)
			eigrp_packet_neighbor_retry_cancel(nbr, ack);
		packet = eigrp_packet_dequeue(nbr->retrans_queue);
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_LINK, eigrp, nbr->ei, nbr,
				   "unlinked ACKed seq %u from reliable queue (depth %lu)",
				   ack, nbr->retrans_queue->count);
		eigrp_packet_free(packet);
		if (nbr->ei)
			eigrp_packet_multicast_resume(nbr->ei);

		if ((nbr->state == EIGRP_NEIGHBOR_PENDING)
		    && ack == nbr->init_sequence_number) {
			eigrp_nbr_state_update(EIGRP_SET, nbr, EIGRP_NEIGHBOR_UP);
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
		} else {
			/* Preserve normal RTP head advancement before replenishing
			 * this peer's bounded initial synchronization queue. */
			eigrp_packet_send_reliably(eigrp, nbr);
			eigrp_update_sync_continue(nbr);
		}
	} else {
		eigrp_prefix_t peer_addr;

		eigrp_eventlog_addr_from_legacy(&peer_addr, &nbr->src);
		(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_RTP_ACK_UNEXPECTED,
			&peer_addr, ack, packet ? packet->sequence_number : 0,
			nbr->retrans_queue->count, 0);
	}
}

static void eigrp_packet_reliable_neighbor_send_update(eigrp_nbr_t *nbr,
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

static void eigrp_packet_reliable_first_send_update(eigrp_intf_t *ei,
                                      eigrp_packet_t *packet)
{
	eigrp_nbr_t *nbr;
	eigrp_list_item_t *node;
	uint64_t now_msec;

	/* Karn's rule: retransmits never start a new first-transmission RTT. */
	if (!ei || !packet || packet->sequence_number == 0 || packet->retransmission)
		return;

	now_msec = eigrp_sys_monotime_msec();
	if (packet->nbr) {
		eigrp_packet_reliable_neighbor_send_update(packet->nbr,
						 packet->sequence_number, now_msec);
		return;
	}

	/* One reliable multicast wire send is an independent RTT start point for
	 * every neighbor that currently owns this sequence in its RTP queue. */
	for (EIGRP_LIST_ITERATE_RO(ei->nbrs, node, nbr))
		eigrp_packet_reliable_neighbor_send_update(nbr, packet->sequence_number,
						 now_msec);
}

static void eigrp_packet_reliable_send_update(eigrp_intf_t *ei,
                                      eigrp_packet_t *packet)
{
	eigrp_packet_t *head;

	if (!ei || !packet || packet->sequence_number == 0)
		return;
	if (!packet->retransmission) {
		eigrp_packet_reliable_first_send_update(ei, packet);
		return;
	}
	if (!packet->nbr || !packet->nbr->retrans_queue)
		return;
	head = eigrp_packet_queue_next(packet->nbr->retrans_queue);
	if (!head || head->sequence_number != packet->sequence_number)
		return;
	head->retry_queued = false;
	head->retrans_counter++; /* counted only after actual wire send */
	eigrp_packet_retransmit_timer_start(packet->nbr);
}

static void eigrp_packet_reliable_send_failure_update(eigrp_intf_t *ei,
					      eigrp_packet_t *packet)
{
	eigrp_nbr_t *nbr;
	eigrp_packet_t *queued;
	eigrp_list_item_t *node;

	if (!ei || !packet || packet->sequence_number == 0)
		return;

	if (packet->retransmission) {
		if (packet->nbr && packet->nbr->retrans_queue) {
			queued = eigrp_packet_queue_next(packet->nbr->retrans_queue);
			if (queued && queued->sequence_number == packet->sequence_number) {
				queued->retry_queued = false;
				eigrp_packet_retransmit_timer_start(packet->nbr);
			}
		}
		return;
	}

	if (packet->nbr) {
		queued = eigrp_packet_queue_next(packet->nbr->retrans_queue);
		if (queued && queued->sequence_number == packet->sequence_number)
			eigrp_packet_retransmit_timer_start(packet->nbr);
		return;
	}

	for (EIGRP_LIST_ITERATE_RO(ei->nbrs, node, nbr)) {
		queued = eigrp_packet_queue_next(nbr->retrans_queue);
		if (queued && queued->sequence_number == packet->sequence_number)
			eigrp_packet_retransmit_timer_start(nbr);
	}
}

/* RFC 7868 section 5.2.1: configured interface bandwidth is in kbps.
 * The millisecond rate, bandwidth_kbps * percent, and packet charge,
 * packet_bytes * 800, share the same exact fixed-point unit.  No floating
 * point or premature integer rounding is involved (including 1-kbps links).
 *
 * Limit accumulated idle credit to a single millisecond of bandwidth, or
 * one complete packet on a slow link.  Long idle periods cannot accumulate
 * enough credit to transmit an unbounded burst when redistribution starts.
 * The AF clock is millisecond resolution, so fast interfaces send a bounded
 * sub-millisecond batch, then yield until the next clock tick. */
static uint64_t eigrp_packet_pacing_rate(const eigrp_intf_t *ei)
{
	uint32_t bandwidth = ei->params.bandwidth;
	uint32_t percent = ei->bandwidth_percent;

	/* A missing metric bandwidth is not permission to disable pacing. */
	if (!bandwidth)
		bandwidth = ei->curr_bandwidth ? ei->curr_bandwidth
					       : EIGRP_BANDWIDTH_DEFAULT;
	if (!percent)
		percent = 50;
	return (uint64_t)bandwidth * percent;
}

uint32_t eigrp_packet_pacing_delay_msec(eigrp_intf_t *ei,
	uint16_t packet_length, uint64_t now_msec)
{
	uint64_t rate, charge, capacity, elapsed, needed;

	if (!ei || !packet_length)
		return 0;
	rate = eigrp_packet_pacing_rate(ei);
	charge = (uint64_t)packet_length * 800U;
	capacity = rate > charge ? rate : charge;

	if (!ei->pacing_initialized || ei->pacing_rate != rate
	    || now_msec < ei->pacing_last_msec) {
		ei->pacing_initialized = true;
		ei->pacing_last_msec = now_msec;
		ei->pacing_rate = rate;
		ei->pacing_credit = capacity;
	} else {
		elapsed = now_msec - ei->pacing_last_msec;
		ei->pacing_last_msec = now_msec;
		/* Saturated refill avoids multiplication overflow on long idle. */
		if (elapsed >= (capacity + rate - 1U) / rate)
			ei->pacing_credit = capacity;
		else {
			uint64_t credit = ei->pacing_credit + elapsed * rate;
			ei->pacing_credit = credit < capacity ? credit : capacity;
		}
		if (ei->pacing_credit > capacity)
			ei->pacing_credit = capacity;
	}
	if (ei->pacing_credit >= charge)
		return 0;
	needed = charge - ei->pacing_credit;
	/* rate is always nonzero (configuration minimum is 1 kbps/1%). */
	elapsed = (needed + rate - 1U) / rate;
	return (uint32_t)(elapsed > UINT32_MAX ? UINT32_MAX : elapsed);
}

void eigrp_packet_pacing_charge(eigrp_intf_t *ei, uint16_t packet_length)
{
	uint64_t charge = (uint64_t)packet_length * 800U;

	if (!ei || !ei->pacing_initialized)
		return;
	ei->pacing_credit = ei->pacing_credit >= charge
				    ? ei->pacing_credit - charge : 0;
}

static void eigrp_packet_pacing_wake(void *arg)
{
	eigrp_packet_write_schedule(arg);
}

void eigrp_packet_write_schedule(eigrp_instance_t *eigrp)
{
	if (!eigrp || eigrp->t_write)
		return;
	eigrp_sys_write_add(&eigrp->t_write, eigrp,
			     eigrp_packet_write_ready, eigrp);
}

void eigrp_packet_write_ready(void *arg)
{
	eigrp_instance_t *eigrp = arg;

	if (!eigrp)
		return;
	pthread_mutex_lock(&eigrp->work_lock);
	if (!eigrp->shutdown && eigrp->thread_running) {
		eigrp->write_ready = true;
		pthread_cond_signal(&eigrp->work_cond);
	}
	pthread_mutex_unlock(&eigrp->work_lock);
}

static bool eigrp_packet_multicast_admit(eigrp_intf_t *ei);

/* Both normal paced output and the exceptional final goodbye use the
 * same late authentication, host send, statistics and budget accounting. */
static void eigrp_packet_wire_send(eigrp_intf_t *ei, eigrp_packet_t *packet)
{
	eigrp_instance_t *eigrp = ei->eigrp;
	struct eigrp_header *eigrph;
	uint32_t seqno, ack;
	int ret;

	eigrph = (struct eigrp_header *)eigrp_stream_data(packet->s);
	seqno = ntohl(eigrph->sequence);
	ack = ntohl(eigrph->ack);

	/* Authoritative transmit envelope: pacing order, including retransmissions,
	 * determines cryptographic sequence order, never packet build order. */
	if (!eigrp_auth_transmit_finalize(ei, packet->s, packet->length)) {
		eigrp_log(EIGRP_LOG_WARNING,
			  "interface %s: packet authentication could not be finalized",
			  eigrp_intf_name_string(ei));
		ret = -1;
	} else
		ret = eigrp->af_vectors.packet_send(eigrp, ei, packet);

	eigrp_debug_packet_send(ei, packet, ret);
	if (ret >= 0) {
		ei->pacing_tx_bytes += packet->length;
		eigrp_packet_send_stats_update(ei, packet, eigrph);
		if (eigrp_packet_eventlog_opcode(eigrph->opcode)) {
			eigrp_prefix_t peer_addr;

			eigrp_eventlog_addr_from_legacy(&peer_addr, &packet->dst);
			(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_PACKET_TX,
				&peer_addr, eigrph->opcode, ntohl(eigrph->sequence),
				ntohl(eigrph->ack), packet->length);
		}
		eigrp_packet_reliable_send_update(ei, packet);
	} else {
		eigrp_prefix_t peer_addr;

		eigrp_eventlog_addr_from_legacy(&peer_addr, &packet->dst);
		(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_PACKET_TX_FAILURE,
			&peer_addr, eigrph->opcode, ntohl(eigrph->sequence), packet->length,
			(eventmsg_arg_t)(intptr_t)ret);
		eigrp_packet_reliable_send_failure_update(ei, packet);
	}

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

	/* Account for the attempted wire send.  Charging errors too prevents
	 * a persistent host send failure from spinning on a full output queue. */
	eigrp_packet_pacing_charge(ei, packet->length);
}

/* Final multicast goodbye must reach the wire while the interface is still
 * operative.  It cannot wait for the AF write-ready event: the caller is
 * about to tear down this interface and invalidate its output queue.
 * Ownership of packet is transferred to this action in every case. */
void eigrp_packet_goodbye_send(eigrp_intf_t *ei, eigrp_packet_t *packet)
{
	const struct eigrp_header *header;

	if (!packet)
		return;
	if (ei && ei->eigrp && packet->s
	    && packet->length >= EIGRP_HEADER_LEN) {
		header = (const struct eigrp_header *)eigrp_stream_data(packet->s);
		if (header->opcode == EIGRP_OPC_HELLO
		    && ntohl(header->sequence) == 0)
			eigrp_packet_wire_send(ei, packet);
	}
	eigrp_packet_free(packet);
}

void eigrp_packet_write(void *arg)
{
	eigrp_instance_t *eigrp = arg;
	eigrp_intf_t *ei;
	eigrp_packet_t *packet;
	eigrp_list_item_t *node;

	/* Skip paced interfaces instead of head-of-line blocking every interface
	 * in this address family.  Every output, including ACKs and retransmits,
	 * shares its interface's byte budget. */
	{
		eigrp_list_item_t *candidate;
		eigrp_intf_t *earliest_ei = NULL;
		uint32_t earliest = UINT32_MAX;
		uint64_t now = eigrp_sys_monotime_msec();

		node = NULL;
		for (candidate = eigrp_list_first(eigrp->oi_write_q); candidate;
		     candidate = candidate->next) {
			eigrp_intf_t *pending_ei = eigrp_list_item_data(candidate);
			eigrp_packet_t *pending;
			uint32_t wait;

			if (!pending_ei || !pending_ei->obuf)
				continue;
			pending = eigrp_packet_queue_next(pending_ei->obuf);
			if (!pending && pending_ei->multicast_pending.count
			    && !pending_ei->multicast_backoff
			    && !pending_ei->multicast_waiting
			    && !pending_ei->t_multicast_retry) {
				(void)eigrp_packet_multicast_admit(pending_ei);
				pending = eigrp_packet_queue_next(pending_ei->obuf);
			}
			if (!pending)
				continue;
			/* Hellos, including RTP ACKs, must make progress regardless
			 * of a depleted data pacing budget.  Their bytes still consume
			 * credit below so reliable traffic gives the budget back. */
			if (pending->s && pending->length >= EIGRP_HEADER_LEN
			    && ((const struct eigrp_header *)
				eigrp_stream_data(pending->s))->opcode == EIGRP_OPC_HELLO)
				wait = 0;
			else
				wait = eigrp_packet_pacing_delay_msec(pending_ei,
							pending->length, now);
			if (!wait) {
				node = candidate;
				break;
			}
			if (wait < earliest) {
				earliest = wait;
				earliest_ei = pending_ei;
			}
		}
		if (!node) {
			if (earliest != UINT32_MAX) {
				/* Only count new pacing sleeps, not incoming output
				 * events that rediscover the same pending timer. */
				if (!eigrp->t_pacing && earliest_ei) {
					earliest_ei->pacing_defer_count++;
					earliest_ei->pacing_defer_msec += earliest;
				}
				eigrp_timer_add(eigrp, &eigrp->t_pacing,
					eigrp_packet_pacing_wake, eigrp, earliest);
			}
			return;
		}
		if (eigrp->t_pacing)
			eigrp_timer_cancel(eigrp, &eigrp->t_pacing);
	}
	ei = eigrp_list_item_data(node);
	packet = eigrp_packet_queue_next(ei->obuf);
	if (packet->length < EIGRP_HEADER_LEN) {
		eigrp_log(EIGRP_LOG_ERROR,  "%s: Packet just has a header?",
			 __func__);
		eigrp_debug_header_dump((const eigrp_header_t *)packet->s->data);
		eigrp_packet_delete(ei);
		goto out;
	}

	eigrp_packet_wire_send(ei, packet);

	/* Now delete packet from queue. */
	eigrp_packet_delete(ei);

out:
	if (eigrp_packet_queue_next(ei->obuf) == NULL
	    && !ei->multicast_pending.count) {
		ei->on_write_q = 0;
		eigrp_list_remove(eigrp->oi_write_q, node);
	}

	/* Round-robin among interfaces that have pending output.  A hot fast
	 * interface must not monopolize the AF writer while another is ready. */
	if (ei->on_write_q && node != eigrp->oi_write_q->tail) {
		if (node->prev)
			node->prev->next = node->next;
		else
			eigrp->oi_write_q->head = node->next;
		if (node->next)
			node->next->prev = node->prev;
		node->prev = eigrp->oi_write_q->tail;
		node->next = NULL;
		eigrp->oi_write_q->tail->next = node;
		eigrp->oi_write_q->tail = node;
	}
	/* A blocked multicast does not self-reschedule.  An ACK or neighbor
	 * teardown will wake it; Hellos still schedule themselves independently. */
	for (eigrp_list_item_t *item = eigrp_list_first(eigrp->oi_write_q);
	     item; item = item->next) {
		eigrp_intf_t *candidate = eigrp_list_item_data(item);
		if (candidate && ((candidate->obuf && candidate->obuf->count)
		    || (candidate->multicast_pending.count
			&& !candidate->multicast_backoff
			&& !candidate->multicast_waiting
			&& !candidate->t_multicast_retry))) {
			eigrp_packet_write_schedule(eigrp);
			break;
		}
	}
}

/* Starting point of packet process function. */
/* Release one receive-side packet handoff object. */
static void eigrp_packet_input_free(eigrp_packet_input_t *input)
{
	if (!input)
		return;
	if (input->stream)
		eigrp_stream_free(input->stream);
	free(input);
}

/* Process one packet after the process receive thread has validated and
 * demultiplexed it to this AF.  This is the old eigrp_packet_read protocol
 * body with socket receive/demux removed; protocol behavior remains here. */
static void eigrp_packet_input_process(eigrp_instance_t *eigrp,
				       eigrp_packet_input_t *input)
{
	int ret;
	eigrp_stream_t *ibuf;
	eigrp_intf_t *ei;
	struct eigrp_header *eigrph;
	eigrp_addr_t src;
	eigrp_addr_t dst;
	eigrp_nbr_t *nbr;
	eigrp_packet_rx_meta_t meta;
	size_t endp;
	size_t offset;
	uint16_t opcode;
	uint16_t length;

	if (!eigrp || !input || !input->stream)
		return;

	ibuf = input->stream;
	src = input->source;
	dst = input->destination;
	meta = input->meta;
	ei = eigrp_intf_lookup_by_ifindex(eigrp, input->ifindex);
	if (!ei) {
		eigrp_prefix_t peer_addr;

		eigrp_eventlog_addr_from_legacy(&peer_addr, &src);
		(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_PACKET_REJECT,
			&peer_addr, 0, EIGRP_EVENTLOG_PACKET_REJECT_NO_INTERFACE,
			input->ifindex, 0);
		return;
	}

	/* The stream is the receive-side memory boundary.  The process thread
	 * validates this metadata before enqueue, but keep the same boundary at
	 * the protocol consumer so a future producer cannot make checksum, auth,
	 * or TLV processing read beyond bytes actually present in ibuf. */
	endp = eigrp_stream_get_endp(ibuf);
	if (!eigrp_packet_input_bounds_validate(endp, meta.network_header_length,
					meta.eigrp_length, &offset, &length)) {
		eigrp_packet_reject_event(ei, &src, 0, EIGRP_EVENTLOG_PACKET_REJECT_BOUNDS,
			meta.eigrp_length, endp);
		return;
	}

	eigrp_stream_set_getp(ibuf, offset);
	eigrph = (struct eigrp_header *)eigrp_stream_pnt(ibuf);

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

		eigrp_packet_reject_event(ei, &src, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_PASSIVE_INTERFACE, 0, 0);
		if (meta.destination_multicast)
			eigrp_intf_multicast_update(EIGRP_SET, ei);
		return;
	}

	/* Preserve the established full header/auth/source validation in the AF.
	 * The process receive thread has already done framing/version/checksum
	 * validation before this packet can reach packetQ. */
	ret = eigrp_packet_header_validate(ei, &src, eigrph, length);
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
	/* A process-wide suspension blocks new Hello adjacency discovery as well as
	 * existing neighbor traffic. The periodic sender is quiesced separately. */
	if (eigrp->eigrp_pfxlimits_af_state && opcode == EIGRP_OPC_HELLO)
		return;

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
	/* Prefix-limit suspension retains the peer identity, not its adjacency.
	 * Reject all opcodes, including Hellos and ACKs, before RTP/DUAL. */
	if (nbr && !eigrp_nbr_is_eligible(nbr))
		return;
	if (!input->ingress_authenticated
	    || (input->ingress_neighbor_generation != 0
	        && (!nbr || nbr->generation != input->ingress_neighbor_generation)))
		return;
	/* Retained penalized peers are lookup-visible but not protocol-eligible.
	 * Reject even ACK Hellos before they can revive stale RTP state. */
	if (nbr && !eigrp_nbr_is_eligible(nbr))
		return;
	if (opcode == EIGRP_OPC_HELLO) {
		/* Several discovery Hellos can be queued before a new neighbor is
		 * published. Reconcile those sequences exactly once after creation. */
		if (nbr && input->ingress_neighbor_generation == 0) {
			bool fresh;

			pthread_mutex_lock(&eigrp->peer_lock);
			fresh = nbr->published
				&& eigrp_auth_discovery_sequence_accept(nbr, eigrph);
			pthread_mutex_unlock(&eigrp->peer_lock);
			if (!fresh)
				return;
		}
		/* Ingress authenticated and consumed the crypto sequence already. */
		eigrp_packet_receive_stats_update(ei, eigrph);
		if (ntohl(eigrph->ack)) {
			/* An EIGRP ACK is a Hello opcode with sequence zero and a
			 * nonzero ACK field.  Consume it in RTP before Hello TLV
			 * processing; an ACK must never create a new adjacency. */
			if (nbr)
				eigrp_packet_ack(eigrp, eigrph, nbr);
			else
				eigrp_packet_reject_event(ei, &src, opcode,
					EIGRP_EVENTLOG_PACKET_REJECT_NO_NEIGHBOR,
					ntohl(eigrph->ack), 0);
			return;
		}
		eigrp_hello_receive(eigrp, eigrph, &src, ei, ibuf, length);
		return;
	}

	/* A neighbor must exist before accepting non-Hello packets. */
	if (!nbr) {
		eigrp_packet_reject_event(ei, &src, opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_NO_NEIGHBOR, 0, 0);
		return;
	}

	/* The authenticated ingress result is authoritative for cryptographic
	 * sequence and digest; remaining RTP and route checks are AF-owned. */
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
			eigrp_packet_reject_event(ei, &src, opcode,
				EIGRP_EVENTLOG_PACKET_REJECT_CR_NOT_ELIGIBLE, sequence,
				nbr->cr_sequence);
			return;
		}

		nbr->cr_mode = false;
		nbr->cr_sequence = 0;
	}

	eigrp_packet_receive_stats_update(ei, eigrph);
	if (eigrp_packet_eventlog_opcode(opcode)) {
		eigrp_prefix_t peer_addr;

		eigrp_eventlog_addr_from_legacy(&peer_addr, &src);
		(void)eigrp_eventlog_msg_add(eigrp, EIGRP_EVENTLOG_OPCODE_PACKET_RX,
			&peer_addr, opcode, ntohl(eigrph->sequence), ntohl(eigrph->ack), length);
	}
	if (ntohl(eigrph->ack))
		eigrp_packet_ack(eigrp, eigrph, nbr);

	/* RFC 7868 section 5.3.6: a PENDING neighbor is not eligible for
	 * DUAL's QUERY/REPLY exchange.  In particular, never acknowledge a
	 * QUERY while PENDING: the packetizer cannot send its REPLY until UP,
	 * and acknowledging here would irretrievably consume the request.
	 * Leave RTP free to retransmit it when the INIT handshake finishes.
	 * Process a piggybacked ACK above because it may itself complete INIT. */
	if (nbr->state != EIGRP_NEIGHBOR_UP
	    && (opcode == EIGRP_OPC_QUERY || opcode == EIGRP_OPC_REPLY
		|| opcode == EIGRP_OPC_SIAQUERY || opcode == EIGRP_OPC_SIAREPLY)) {
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_ACK, eigrp, ei, nbr,
			"defer %s from pending neighbor until INIT completes",
			eigrp_message_lookup(eigrp_packet_type_str, opcode, NULL));
		return;
	}

	eigrp->eigrp_pfxlimits_af_pkt_active = true;
	switch (opcode) {
	case EIGRP_OPC_PROBE:
		eigrp_packet_reject_event(ei, &src, opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_UNSUPPORTED_OPCODE, opcode, 0);
		break;
	case EIGRP_OPC_QUERY:
		eigrp_query_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	case EIGRP_OPC_REPLY:
		eigrp_reply_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	case EIGRP_OPC_REQUEST:
		eigrp_packet_reject_event(ei, &src, opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_UNSUPPORTED_OPCODE, opcode, 0);
		break;
	case EIGRP_OPC_SIAQUERY:
		eigrp_siaquery_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	case EIGRP_OPC_SIAREPLY:
		eigrp_siareply_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	case EIGRP_OPC_UPDATE:
		eigrp_update_receive(eigrp, nbr, eigrph, ibuf, ei, length);
		break;
	default:
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP packet header type %d unsupported",
			  eigrp_intf_name_string(ei), opcode);
		eigrp_packet_reject_event(ei, &src, opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_UNSUPPORTED_OPCODE, opcode, 0);
		break;
	}
	/* A prefix-limit DOWN transition is committed inside admission, but
	 * topology/RTP teardown waits until the TLV handler has returned. */
	eigrp->eigrp_pfxlimits_af_pkt_active = false;
	if (eigrp->eigrp_pfxlimits_af_cleanup_pending)
		eigrp_pfxlimits_state_cleanup(eigrp);
	if (nbr->limit_cleanup_pending)
		eigrp_nbr_limit_cleanup(nbr);
}

static eigrp_packet_input_t *eigrp_packet_input_dequeue_locked(eigrp_instance_t *eigrp)
{
	eigrp_packet_input_t *input;

	input = eigrp->packet_head;
	if (!input)
		return NULL;
	eigrp->packet_head = input->next;
	if (eigrp->packet_count)
		eigrp->packet_count--;
	if (input->ingress_discovery && eigrp->packet_discovery_count)
		eigrp->packet_discovery_count--;
	if (!eigrp->packet_head)
		eigrp->packet_tail = NULL;
	input->next = NULL;
	return input;
}

static eigrp_message_request_t *eigrp_instance_message_dequeue_locked(
	eigrp_instance_t *eigrp)
{
	eigrp_message_request_t *message = eigrp->message_head;

	if (!message)
		return NULL;
	eigrp->message_head = message->next;
	if (eigrp->message_count)
		eigrp->message_count--;
	if (!eigrp->message_head)
		eigrp->message_tail = NULL;
	message->next = NULL;
	return message;
}

static eigrp_af_event_t *eigrp_instance_event_dequeue_locked(
	eigrp_instance_t *eigrp)
{
	eigrp_af_event_t *event = eigrp->event_head;

	if (!event)
		return NULL;
	eigrp->event_head = event->next;
	if (eigrp->event_count)
		eigrp->event_count--;
	if (!eigrp->event_head)
		eigrp->event_tail = NULL;
	event->next = NULL;
	return event;
}

static eigrp_rib_event_t *eigrp_instance_rib_dequeue_locked(
	eigrp_instance_t *eigrp)
{
	eigrp_rib_event_t *event = eigrp->rib_head;

	if (!event)
		return NULL;
	eigrp->rib_head = event->next;
	if (eigrp->rib_count)
		eigrp->rib_count--;
	if (!eigrp->rib_head)
		eigrp->rib_tail = NULL;
	event->next = NULL;
	return event;
}

static void eigrp_instance_rib_free(eigrp_rib_event_t *event)
{
	if (!event)
		return;
	free(event->nexthops);
	free(event);
}

static void eigrp_instance_rib_queue_clear_locked(eigrp_instance_t *eigrp)
{
	eigrp_rib_event_t *event;

	while ((event = eigrp_instance_rib_dequeue_locked(eigrp)) != NULL)
		eigrp_instance_rib_free(event);
}

static void eigrp_instance_event_free(eigrp_af_event_t *event)
{
	if (!event)
		return;
	free(event->interface_name);
	free(event);
}

static void eigrp_instance_event_queue_clear_locked(eigrp_instance_t *eigrp)
{
	eigrp_af_event_t *event;

	while ((event = eigrp_instance_event_dequeue_locked(eigrp)) != NULL)
		eigrp_instance_event_free(event);
}

static void eigrp_instance_message_complete(eigrp_instance_t *eigrp,
	eigrp_message_request_t *message, eigrp_result_t result)
{
	pthread_mutex_lock(&eigrp->work_lock);
	message->result = result;
	message->complete = true;
	pthread_cond_signal(&message->complete_cond);
	pthread_mutex_unlock(&eigrp->work_lock);
}

static void eigrp_instance_message_fail_locked(eigrp_instance_t *eigrp,
	eigrp_result_t result)
{
	eigrp_message_request_t *message;

	while ((message = eigrp_instance_message_dequeue_locked(eigrp)) != NULL) {
		message->result = result;
		message->complete = true;
		pthread_cond_signal(&message->complete_cond);
	}
}

/*
 * AF scheduler service limits.
 *
 * These bounds apply between independent AF work atoms.  DUAL, RTP, packet
 * processing, and packetizer operations themselves run to completion.  Keep
 * the initial values deliberately conservative; they preserve the current
 * one-atom-per-turn behavior while making the scheduling policy explicit and
 * independently tunable from protocol algorithms.
 */
#define EIGRP_AF_SYSTEM_BATCH 1U
#define EIGRP_AF_PACKET_BATCH 1U
#define EIGRP_AF_PACKETIZER_BATCH 1U
#define EIGRP_AF_RIB_BATCH 1U
#define EIGRP_AF_USER_BATCH 1U

static bool eigrp_instance_timer_due_dequeue(eigrp_instance_t *eigrp,
	eigrp_timer_event_t **timer)
{
	uint64_t now;

	*timer = NULL;
	pthread_mutex_lock(&eigrp->work_lock);
	now = eigrp_sys_monotime_msec();
	if (eigrp->timer_head && eigrp->timer_head->due_msec <= now) {
		*timer = eigrp->timer_head;
		eigrp->timer_head = (*timer)->next;
		(*timer)->next = NULL;
		if ((*timer)->owner && *(*timer)->owner == *timer)
			*(*timer)->owner = NULL;
	}
	pthread_mutex_unlock(&eigrp->work_lock);
	return *timer != NULL;
}

static bool eigrp_instance_packetizer_dequeue(eigrp_instance_t *eigrp,
	eigrp_packetizer_work_t **packetizer)
{
	*packetizer = NULL;
	pthread_mutex_lock(&eigrp->work_lock);
	if (eigrp->packetizer_head) {
		*packetizer = eigrp->packetizer_head;
		eigrp->packetizer_head = (*packetizer)->next;
		if (!eigrp->packetizer_head)
			eigrp->packetizer_tail = NULL;
		if (eigrp->packetizer_count)
			eigrp->packetizer_count--;
		(*packetizer)->next = NULL;
	}
	pthread_mutex_unlock(&eigrp->work_lock);
	return *packetizer != NULL;
}

static bool eigrp_instance_write_ready_take(eigrp_instance_t *eigrp)
{
	bool ready = false;

	pthread_mutex_lock(&eigrp->work_lock);
	if (eigrp->write_ready) {
		eigrp->write_ready = false;
		ready = true;
	}
	pthread_mutex_unlock(&eigrp->work_lock);
	return ready;
}

static bool eigrp_instance_wait_deadline(eigrp_instance_t *eigrp,
					 uint64_t due_msec, uint64_t now_msec,
					 struct timespec *deadline)
{
	uint64_t wait_msec;

	if (!eigrp || !deadline)
		return false;

#if defined(__APPLE__) || !defined(CLOCK_MONOTONIC)
	/* Darwin condition variables use CLOCK_REALTIME.  EIGRP timer ordering
	 * remains monotonic, so convert only the remaining interval to a realtime
	 * absolute deadline and never mix absolute values from the two clocks. */
	wait_msec = due_msec > now_msec ? due_msec - now_msec : 0;
	if (clock_gettime(CLOCK_REALTIME, deadline) != 0)
		return false;
	deadline->tv_sec += (time_t)(wait_msec / 1000U);
	deadline->tv_nsec += (long)((wait_msec % 1000U) * 1000000U);
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
#else
	/* eigrp_instance_create() binds work_cond to CLOCK_MONOTONIC. */
	(void)now_msec;
	(void)wait_msec;
	deadline->tv_sec = (time_t)(due_msec / 1000U);
	deadline->tv_nsec = (long)((due_msec % 1000U) * 1000000U);
#endif
	return true;
}

static bool eigrp_instance_wait(eigrp_instance_t *eigrp)
{
	pthread_mutex_lock(&eigrp->work_lock);
	for (;;) {
		uint64_t now;

		if (!eigrp->thread_running)
			break;

		now = eigrp_sys_monotime_msec();
		if (eigrp->event_head || eigrp->packet_head || eigrp->packetizer_head
		    || eigrp->rib_head || eigrp->message_head || eigrp->write_ready
		    || (eigrp->timer_head && eigrp->timer_head->due_msec <= now))
			break;

		if (eigrp->timer_head) {
			struct timespec deadline;

			if (!eigrp_instance_wait_deadline(eigrp,
						 eigrp->timer_head->due_msec, now,
						 &deadline))
				break;
			(void)pthread_cond_timedwait(&eigrp->work_cond,
				&eigrp->work_lock, &deadline);
		} else {
			(void)pthread_cond_wait(&eigrp->work_cond,
				&eigrp->work_lock);
		}
	}

	if (!eigrp->thread_running) {
		eigrp_instance_event_queue_clear_locked(eigrp);
		eigrp_instance_rib_queue_clear_locked(eigrp);
		eigrp_instance_message_fail_locked(eigrp, EIGRP_RESULT_CONFLICT);
		pthread_mutex_unlock(&eigrp->work_lock);
		return false;
	}
	pthread_mutex_unlock(&eigrp->work_lock);
	return true;
}

static void eigrp_instance_system_service(eigrp_instance_t *eigrp)
{
	unsigned int count;

	for (count = 0; count < EIGRP_AF_SYSTEM_BATCH; count++) {
		eigrp_af_event_t *event;

		pthread_mutex_lock(&eigrp->work_lock);
		event = eigrp_instance_event_dequeue_locked(eigrp);
		pthread_mutex_unlock(&eigrp->work_lock);
		if (!event)
			break;
		eigrp_instance_event_process(eigrp, event);
		eigrp_instance_event_free(event);
	}
}

static void eigrp_instance_timer_service(eigrp_instance_t *eigrp)
{
	eigrp_timer_event_t *timer;

	if (!eigrp_instance_timer_due_dequeue(eigrp, &timer))
		return;
	{
		eigrp_event_callback_t callback = timer->callback;
		void *timer_arg = timer->arg;

		free(timer);
		callback(timer_arg);
	}
}

static void eigrp_instance_protocol_service(eigrp_instance_t *eigrp)
{
	unsigned int count;

	/* Timers, receive, packetization, and transmit readiness form one
	 * convergence-critical service class.  Each work atom runs to completion;
	 * the AF loop bounds only how many independent atoms it selects per turn. */
	eigrp_instance_timer_service(eigrp);

	/* Preserve the established AF transport ordering: protocol output already
	 * queued by prior topology work gets a chance to advance before another
	 * receive atom mutates topology state.  This is especially important for
	 * packetizer work, whose queued work represents consequences of topology
	 * changes that have already completed. */
	for (count = 0; count < EIGRP_AF_PACKETIZER_BATCH; count++) {
		eigrp_packetizer_work_t *packetizer;

		if (!eigrp_instance_packetizer_dequeue(eigrp, &packetizer))
			break;
		eigrp_packetizer_work_process(eigrp, packetizer);
		eigrp_packetizer_work_free(packetizer);
	}

	if (eigrp_instance_write_ready_take(eigrp))
		eigrp_packet_write(eigrp);

	for (count = 0; count < EIGRP_AF_PACKET_BATCH; count++) {
		eigrp_packet_input_t *input;

		pthread_mutex_lock(&eigrp->work_lock);
		input = eigrp_packet_input_dequeue_locked(eigrp);
		pthread_mutex_unlock(&eigrp->work_lock);
		if (!input)
			break;
		eigrp_packet_input_process(eigrp, input);
		eigrp_packet_input_free(input);
	}

	/* A protocol turn can create or expose a newly due timer.  Packetizer/TX
	 * work created by this receive atom remains pending and wakes the next AF
	 * turn immediately; it is not delayed behind sleep or lower-priority work. */
	eigrp_instance_timer_service(eigrp);
}

static void eigrp_instance_rib_service(eigrp_instance_t *eigrp)
{
	unsigned int count;

	for (count = 0; count < EIGRP_AF_RIB_BATCH; count++) {
		eigrp_rib_event_t *event;

		pthread_mutex_lock(&eigrp->work_lock);
		event = eigrp_instance_rib_dequeue_locked(eigrp);
		pthread_mutex_unlock(&eigrp->work_lock);
		if (!event)
			break;
		(void)eigrp_rib_event_process(eigrp, event);
		eigrp_instance_rib_free(event);
	}
}

static void eigrp_instance_user_service(eigrp_instance_t *eigrp)
{
	unsigned int count;

	for (count = 0; count < EIGRP_AF_USER_BATCH; count++) {
		eigrp_message_request_t *message;
		eigrp_result_t result;

		pthread_mutex_lock(&eigrp->work_lock);
		message = eigrp_instance_message_dequeue_locked(eigrp);
		pthread_mutex_unlock(&eigrp->work_lock);
		if (!message)
			break;
		result = message->handler(eigrp, message->arg);
		eigrp_instance_message_complete(eigrp, message, result);
	}
}

static void *eigrp_instance_thread(void *arg)
{
	eigrp_instance_t *eigrp = arg;

	eigrp_log_start();
	while (eigrp_instance_wait(eigrp)) {
		/* SYSTEM: runtime reality first.  Keep this bounded because one system
		 * event (for example interface-down) may itself trigger substantial DUAL
		 * work before packetization returns control to the scheduler. */
		eigrp_instance_system_service(eigrp);

		/* PROTOCOL: maintain balanced inbound/outbound EIGRP progress. */
		eigrp_instance_protocol_service(eigrp);

		/* RIB: host route snapshots are already normalized into EIGRP-owned
		 * objects.  Consume a bounded number after active protocol progress. */
		eigrp_instance_rib_service(eigrp);

		/* USER: synchronous configuration/exec work is bounded and lowest
		 * priority.  Age-based tuning can be added after stress measurements. */
		eigrp_instance_user_service(eigrp);
	}
	eigrp_log_stop();
	return NULL;
}

bool eigrp_instance_thread_start(eigrp_instance_t *eigrp)
{
	if (!eigrp)
		return false;

	pthread_mutex_lock(&eigrp->work_lock);
	if (eigrp->thread_started) {
		pthread_mutex_unlock(&eigrp->work_lock);
		return true;
	}
	eigrp->thread_running = true;
	pthread_mutex_unlock(&eigrp->work_lock);

	if (pthread_create(&eigrp->thread, NULL,
			   eigrp_instance_thread, eigrp) != 0) {
		pthread_mutex_lock(&eigrp->work_lock);
		eigrp->thread_running = false;
		pthread_mutex_unlock(&eigrp->work_lock);
		return false;
	}

	pthread_mutex_lock(&eigrp->work_lock);
	eigrp->thread_started = true;
	pthread_mutex_unlock(&eigrp->work_lock);
	return true;
}

void eigrp_instance_thread_stop(eigrp_instance_t *eigrp)
{
	bool join = false;

	if (!eigrp)
		return;
	pthread_mutex_lock(&eigrp->work_lock);
	if (eigrp->thread_started) {
		eigrp->thread_running = false;
		join = true;
		pthread_cond_broadcast(&eigrp->work_cond);
	}
	pthread_mutex_unlock(&eigrp->work_lock);

	if (join) {
		(void)pthread_join(eigrp->thread, NULL);
		pthread_mutex_lock(&eigrp->work_lock);
		eigrp->thread_started = false;
		pthread_mutex_unlock(&eigrp->work_lock);
	}
}

eigrp_result_t eigrp_instance_rib_event_enqueue(eigrp_instance_t *eigrp,
	eigrp_rib_event_type_t type, const eigrp_rib_route_t *route)
{
	eigrp_rib_event_t *event;

	if (!eigrp || !route)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (route->nexthop_count && !route->nexthops)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	event = calloc(1, sizeof(*event));
	if (!event)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	event->type = type;
	event->route = *route;
	if (route->nexthop_count) {
		event->nexthops = calloc(route->nexthop_count,
					 sizeof(*event->nexthops));
		if (!event->nexthops) {
			free(event);
			return EIGRP_RESULT_INTERNAL_FAILURE;
		}
		memcpy(event->nexthops, route->nexthops,
		       route->nexthop_count * sizeof(*event->nexthops));
		event->route.nexthops = event->nexthops;
	}

	pthread_mutex_lock(&eigrp->work_lock);
	if (eigrp->shutdown || !eigrp->thread_running || !eigrp->thread_started) {
		pthread_mutex_unlock(&eigrp->work_lock);
		eigrp_instance_rib_free(event);
		return EIGRP_RESULT_CONFLICT;
	}
	if (eigrp->rib_tail)
		eigrp->rib_tail->next = event;
	else
		eigrp->rib_head = event;
	eigrp->rib_tail = event;
	eigrp->rib_count++;
	pthread_cond_signal(&eigrp->work_cond);
	pthread_mutex_unlock(&eigrp->work_lock);
	return EIGRP_RESULT_SUCCESS;
}

static bool eigrp_instance_event_accept(eigrp_instance_t *eigrp,
	eigrp_af_event_t *event)
{
	if (!eigrp || !event)
		return false;
	pthread_mutex_lock(&eigrp->work_lock);
	if (eigrp->shutdown || !eigrp->thread_running || !eigrp->thread_started) {
		pthread_mutex_unlock(&eigrp->work_lock);
		eigrp_instance_event_free(event);
		return false;
	}
	if (eigrp->event_tail)
		eigrp->event_tail->next = event;
	else
		eigrp->event_head = event;
	eigrp->event_tail = event;
	eigrp->event_count++;
	pthread_cond_signal(&eigrp->work_cond);
	pthread_mutex_unlock(&eigrp->work_lock);
	return true;
}

bool eigrp_instance_event_enqueue(eigrp_instance_t *eigrp,
	eigrp_af_event_type_t type)
{
	eigrp_af_event_t *event;
	if (!eigrp)
		return false;
	event = calloc(1, sizeof(*event));
	if (!event)
		return false;
	event->type = type;
	return eigrp_instance_event_accept(eigrp, event);
}

bool eigrp_instance_intf_update_event_enqueue(eigrp_instance_t *eigrp,
	const eigrp_intf_runtime_state_t *state)
{
	eigrp_af_event_t *event;
	if (!eigrp || !state || !state->interface_name)
		return false;
	event = calloc(1, sizeof(*event));
	if (!event)
		return false;
	event->interface_name = strdup(state->interface_name);
	if (!event->interface_name) {
		free(event);
		return false;
	}
	event->type = EIGRP_AF_EVENT_INTF_UPDATE;
	event->data.intf_update = *state;
	event->data.intf_update.interface_name = event->interface_name;
	return eigrp_instance_event_accept(eigrp, event);
}

bool eigrp_instance_intf_down_event_enqueue(eigrp_instance_t *eigrp,
	eigrp_ifindex_t ifindex, uint8_t type, uint32_t bandwidth, uint32_t mtu)
{
	eigrp_af_event_t *event;
	if (!eigrp || !ifindex)
		return false;
	event = calloc(1, sizeof(*event));
	if (!event)
		return false;
	event->type = EIGRP_AF_EVENT_INTF_DOWN;
	event->data.intf_down.ifindex = ifindex;
	event->data.intf_down.type = type;
	event->data.intf_down.bandwidth = bandwidth;
	event->data.intf_down.mtu = mtu;
	return eigrp_instance_event_accept(eigrp, event);
}

bool eigrp_instance_intf_remove_event_enqueue(eigrp_instance_t *eigrp,
	eigrp_ifindex_t ifindex, eigrp_intf_remove_reason_t reason)
{
	eigrp_af_event_t *event;
	if (!eigrp || !ifindex)
		return false;
	event = calloc(1, sizeof(*event));
	if (!event)
		return false;
	event->type = EIGRP_AF_EVENT_INTF_REMOVE;
	event->data.intf_remove.ifindex = ifindex;
	event->data.intf_remove.reason = reason;
	return eigrp_instance_event_accept(eigrp, event);
}

bool eigrp_instance_intf_addr_update_event_enqueue(eigrp_instance_t *eigrp,
	eigrp_ifindex_t ifindex, const eigrp_prefix_t *address,
	eigrp_intf_remove_reason_t reason)
{
	eigrp_af_event_t *event;
	if (!eigrp || !ifindex || !address)
		return false;
	event = calloc(1, sizeof(*event));
	if (!event)
		return false;
	event->type = EIGRP_AF_EVENT_INTF_ADDR_UPDATE;
	event->data.intf_addr_update.ifindex = ifindex;
	event->data.intf_addr_update.address = *address;
	event->data.intf_addr_update.reason = reason;
	return eigrp_instance_event_accept(eigrp, event);
}

bool eigrp_packet_input_enqueue(eigrp_instance_t *eigrp,
				eigrp_packet_input_t *input)
{
	if (!eigrp || !input)
		return false;
	pthread_mutex_lock(&eigrp->work_lock);
	if (eigrp->shutdown || !eigrp->thread_running
	    || eigrp->packet_count >= EIGRP_AF_PACKET_INPUT_QUEUE_MAX
	    || (input->ingress_discovery
		&& eigrp->packet_discovery_count >= EIGRP_AF_PACKET_DISCOVERY_QUEUE_MAX)) {
		if (!eigrp->shutdown && eigrp->thread_running)
			eigrp->packet_drop_count++;
		pthread_mutex_unlock(&eigrp->work_lock);
		return false;
	}
	input->next = NULL;
	if (eigrp->packet_tail)
		eigrp->packet_tail->next = input;
	else
		eigrp->packet_head = input;
	eigrp->packet_tail = input;
	eigrp->packet_count++;
	if (input->ingress_discovery)
		eigrp->packet_discovery_count++;
	pthread_cond_signal(&eigrp->work_cond);
	pthread_mutex_unlock(&eigrp->work_lock);
	return true;
}

void eigrp_packet_input_queue_clear(eigrp_instance_t *eigrp)
{
	eigrp_packet_input_t *input;
	eigrp_packet_input_t *next;

	if (!eigrp)
		return;
	pthread_mutex_lock(&eigrp->work_lock);
	input = eigrp->packet_head;
	eigrp->packet_head = NULL;
	eigrp->packet_tail = NULL;
	eigrp->packet_count = 0;
	eigrp->packet_discovery_count = 0;
	pthread_mutex_unlock(&eigrp->work_lock);

	while (input) {
		next = input->next;
		eigrp_packet_input_free(input);
		input = next;
	}
}

/* Host socket-read callback.  Keep the existing Unix/FRR receive mechanics,
 * but hand the received packet to the process-owned receive/demux thread. */
void eigrp_packet_read(void *arg)
{
	eigrp_instance_t *eigrp = arg;
	eigrp_packet_input_t *input;
	eigrp_intf_t *ei = NULL;

	if (!eigrp || eigrp->shutdown)
		return;

	/* Read readiness is one-shot in the host adapters. */
	eigrp_sys_read_add(&eigrp->t_read, eigrp, eigrp_packet_read, eigrp);

	input = calloc(1, sizeof(*input));
	if (!input)
		return;
	input->stream = eigrp_stream_create(EIGRP_PACKET_MAX_LEN + 1);
	if (!input->stream) {
		free(input);
		return;
	}
	input->afi = eigrp->af_vectors.afi;

	if (!eigrp->af_vectors.packet_receive(eigrp, input->stream, &ei,
					      &input->source, &input->destination,
					      &input->meta)
	    || !ei) {
		eigrp_packet_input_free(input);
		return;
	}
	input->ifindex = eigrp_intf_ifindex(ei);

	if (!eigrp_process_packet_submit(input))
		eigrp_packet_input_free(input);
}

eigrp_packet_queue_t *eigrp_packet_queue_create(void)
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
void eigrp_packet_queue_clear(eigrp_packet_queue_t *queue)
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

eigrp_packet_t *eigrp_packet_create(size_t size, eigrp_nbr_t *nbr)
{
	eigrp_packet_t *new;

	new = calloc(1, sizeof(eigrp_packet_t));
	if (!new)
		return NULL;
	new->s = eigrp_stream_create(size);
	if (!new->s) {
		free(new);
		return NULL;
	}
	new->retrans_counter = 0;
	new->nbr = nbr;

	return new;
}

void eigrp_packet_output_enqueue(eigrp_instance_t *eigrp, eigrp_intf_t *ei,
				      eigrp_packet_t *packet)
{
	if (!eigrp || !ei || !packet)
		return;

	/* HELLOs (including ACKs and CR sequence Hellos) must make progress
	 * even behind a long reliable UPDATE stream.  HELLO carries no RTP
	 * sequence, so its priority does not reorder reliable packets. */
	if (packet->s && packet->length >= EIGRP_HEADER_LEN
	    && ((struct eigrp_header *)eigrp_stream_data(packet->s))->opcode
		== EIGRP_OPC_HELLO) {
		packet->next = NULL;
		packet->previous = ei->obuf->tail;
		if (ei->obuf->tail)
			ei->obuf->tail->next = packet;
		else
			ei->obuf->head = packet;
		ei->obuf->tail = packet;
		ei->obuf->count++;
	} else
		eigrp_packet_enqueue(ei->obuf, packet);

	if (ei->on_write_q == 0) {
		eigrp_list_add(eigrp->oi_write_q, ei);
		ei->on_write_q = 1;
	}
	eigrp_packet_write_schedule(eigrp);
}

/* An epoch is assigned on every UP transition.  A multicast's epoch is
 * captured before TRANSMIT owns the packet.  Peers joining afterward, and
 * restarted peers whose old reliable state was invalidated, cannot inherit
 * that old packet even if their address or neighbor object is reused. */
bool eigrp_packet_multicast_peer_eligible(const eigrp_nbr_t *nbr,
	uint64_t epoch)
{
	return nbr && nbr->state == EIGRP_NEIGHBOR_UP && nbr->retrans_queue
		&& nbr->multicast_epoch != 0 && nbr->multicast_epoch <= epoch;
}

/* An RTP head with a later sequence is held behind deferred multicast.
 * Only earlier packets make the recipient busy for CR and RTP admission. */
unsigned int eigrp_packet_reliable_prior_count(const eigrp_nbr_t *nbr,
	uint32_t sequence)
{
	const eigrp_packet_t *queued;
	unsigned int count = 0;

	if (!nbr || !nbr->retrans_queue)
		return 0;
	for (queued = nbr->retrans_queue->tail; queued;
	     queued = queued->previous) {
		if (!eigrp_packet_sequence_before(queued->sequence_number, sequence))
			break;
		count++;
	}
	return count;
}

/* Keep per-neighbor RTP in sequence order even when an older deferred
 * multicast is admitted after later unicast packets were already queued. */
static void eigrp_packet_reliable_ordered_enqueue(eigrp_packet_queue_t *queue,
					 eigrp_packet_t *packet)
{
	eigrp_packet_t *at;

	/* tail is the oldest packet, previous walks toward newer packets. */
	for (at = queue->tail; at; at = at->previous) {
		if (!eigrp_packet_sequence_before(at->sequence_number,
							packet->sequence_number)) {
			packet->previous = at;
			packet->next = at->next;
			if (at->next)
				at->next->previous = packet;
			else
				queue->tail = packet;
			at->next = packet;
			queue->count++;
			return;
		}
	}
	eigrp_packet_enqueue(queue, packet);
}

/* New reliable unicast packets must not overtake an earlier multicast for
 * this same adjacency.  Other peers and previously transmitted heads are
 * unaffected.  Skip packets from earlier adjacency generations. */
static bool eigrp_packet_unicast_behind_multicast(const eigrp_nbr_t *nbr,
					uint32_t sequence)
{
	const eigrp_packet_t *pending;
	const eigrp_intf_t *ei;

	if (!nbr || !(ei = nbr->ei))
		return false;
	for (pending = ei->multicast_pending.tail; pending;
	     pending = pending->previous) {
		if (!eigrp_packet_sequence_before(pending->sequence_number,
							sequence))
			break;
		/* A newly-UP neighbor is ineligible for older multicasts,
		 * but may still need a later multicast before this unicast. */
		if (eigrp_packet_multicast_peer_eligible(nbr,
						pending->multicast_epoch))
			return true;
	}
	return false;
}

/* Packetizer hands off an immutable wire image.  TRANSMIT owns it from
 * this point forward, without eagerly multiplying it by all peers. */
bool eigrp_packet_multicast_reliable_enqueue(eigrp_instance_t *eigrp,
					      eigrp_intf_t *ei,
					      eigrp_packet_t *packet)
{
	if (!eigrp || !ei || !packet
	    || packet->sequence_number == 0)
		return false;
	packet->multicast_epoch = ei->multicast_epoch;
	eigrp_packet_enqueue(&ei->multicast_pending, packet);
	eigrp_packet_multicast_resume(ei);
	return true;
}

/* An ACK or neighbor teardown wakes TRANSMIT, never DUAL or PACKETIZER.
 * CR overflow / high watermark resumes only after all outstanding RTP
 * work for the interface has drained, avoiding boundary oscillation. */
void eigrp_packet_multicast_resume(eigrp_intf_t *ei)
{
	eigrp_packet_t *packet;
	eigrp_nbr_t *nbr;
	eigrp_list_item_t *node;
	bool any_outstanding = false;
	bool any_ready = false;
	bool any_receivers = false;

	if (!ei || !ei->eigrp || !ei->multicast_pending.count || ei->t_multicast_retry)
		return;
	packet = eigrp_packet_queue_next(&ei->multicast_pending);
	if (!packet)
		return;
	for (EIGRP_LIST_ITERATE_RO(ei->nbrs, node, nbr)) {
		unsigned int prior;

		if (!eigrp_packet_multicast_peer_eligible(nbr,
						packet->multicast_epoch))
			continue;
		any_receivers = true;
		prior = eigrp_packet_reliable_prior_count(nbr,
						packet->sequence_number);
		if (prior)
			any_outstanding = true;
		else
			any_ready = true;
	}
	if (ei->multicast_backoff) {
		if (any_outstanding)
			return;
		ei->multicast_backoff = false;
	}
	/* With no eligible peers, TRANSMIT cancels only this stale packet,
	 * leaving later multicasts for their own captured recipients. */
	if (ei->multicast_waiting && any_receivers && !any_ready)
		return;
	ei->multicast_waiting = false;
	if (!ei->on_write_q) {
		eigrp_list_add(ei->eigrp->oi_write_q, ei);
		ei->on_write_q = 1;
	}
	eigrp_packet_write_schedule(ei->eigrp);
}

/* Allocation failures have no corresponding ACK to wake this producer. */
static void eigrp_packet_multicast_retry(void *arg)
{
	eigrp_packet_multicast_resume(arg);
}

/* Limit RTP per-peer fanout without modifying CR or reliable retransmission.
 * A single deferred multicast consumes one packet, not N peer copies. */
#define EIGRP_MULTICAST_RELIABLE_MAX 4U

static bool eigrp_packet_multicast_admit(eigrp_intf_t *ei)
{
	eigrp_instance_t *eigrp = ei->eigrp;
	eigrp_packet_t *packet;
	eigrp_packet_t **copies;
	eigrp_nbr_t **receivers;
	eigrp_list_item_t *node;
	eigrp_nbr_t *nbr;
	size_t count = 0, ready = 0, busy = 0, i = 0;
	bool fits;

	if (ei->multicast_backoff || ei->multicast_waiting
	    || ei->t_multicast_retry)
		return false;
	packet = eigrp_packet_queue_next(&ei->multicast_pending);
	if (!packet)
		return false;
	for (EIGRP_LIST_ITERATE_RO(ei->nbrs, node, nbr)) {
		unsigned int prior;

		if (!eigrp_packet_multicast_peer_eligible(nbr,
						packet->multicast_epoch))
			continue;
		count++;
		prior = eigrp_packet_reliable_prior_count(nbr,
						packet->sequence_number);
		if (prior)
			busy++;
		else
			ready++;
		if (prior >= EIGRP_MULTICAST_RELIABLE_MAX) {
			ei->multicast_backoff = true;
			ei->multicast_backoff_count++;
			return false;
		}
	}
	if (!count) {
		/* Only this packet's recipients have disappeared.  A following
		 * multicast may have been handed off for newer adjacencies. */
		(void)eigrp_packet_dequeue(&ei->multicast_pending);
		eigrp_packet_free(packet);
		eigrp_packet_multicast_resume(ei);
		return false;
	}
	if (!ready) {
		/* No useful multicast receiver yet; wait for the next RTP ACK.
		 * Do not enter full-drain hysteresis just because all are busy. */
		ei->multicast_waiting = true;
		return false;
	}
	fits = !busy || eigrp_hello_sequence_fits(ei,
			packet->sequence_number, packet->multicast_epoch);
	if (!fits) {
		ei->multicast_backoff = true;
		ei->multicast_backoff_count++;
		return false;
	}
	/* Set the multicast destination before cloning.  RTP unicast retries
	 * must retain the original multicast origin and exception accounting. */
	if (eigrp_instance_afi(eigrp) == EIGRP_AFI_IPV6) {
		packet->dst.afi = AF_INET6;
		if (inet_pton(AF_INET6, "ff02::a", &packet->dst.ip.v6) != 1)
			return false;
	} else {
		packet->dst.afi = AF_INET;
		packet->dst.ip.v4.s_addr = htonl(EIGRP_MULTICAST_ADDRESS);
	}
	/* All allocations precede any RTP mutation: failure retains the packet
	 * for retry without delivering a partial multicast to some peers. */
	receivers = calloc(count, sizeof(*receivers));
	copies = calloc(count, sizeof(*copies));
	if (!receivers || !copies)
		goto allocation_failed;
	for (EIGRP_LIST_ITERATE_RO(ei->nbrs, node, nbr)) {
		if (!eigrp_packet_multicast_peer_eligible(nbr,
						packet->multicast_epoch))
			continue;
		receivers[i] = nbr;
		copies[i] = eigrp_packet_dup(packet, nbr);
		if (!copies[i])
			goto allocation_failed;
		i++;
	}
	if (busy && !eigrp_hello_send_sequence(ei, packet->sequence_number,
					 packet->multicast_epoch))
		goto allocation_failed;
	if (busy) {
		struct eigrp_header *header =
			(struct eigrp_header *)eigrp_stream_data(packet->s);
		header->flags = htonl(ntohl(header->flags) | EIGRP_CR_FLAG);
		/* The clones must carry the same CR flag as the wire image. */
		for (i = 0; i < count; i++) {
			struct eigrp_header *copy_header =
				(struct eigrp_header *)eigrp_stream_data(copies[i]->s);
			copy_header->flags = header->flags;
		}
	}
	for (i = 0; i < count; i++) {
		eigrp_packet_reliable_ordered_enqueue(receivers[i]->retrans_queue,
						      copies[i]);
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_LINK, eigrp,
				ei, receivers[i], "admitted multicast seq %u (depth %lu)",
				packet->sequence_number, receivers[i]->retrans_queue->count);
	}
	free(receivers);
	free(copies);
	(void)eigrp_packet_dequeue(&ei->multicast_pending);
	eigrp_packet_output_enqueue(eigrp, ei, packet);
	return true;

allocation_failed:
	for (size_t j = 0; j < i; j++)
		eigrp_packet_free(copies[j]);
	free(receivers);
	free(copies);
	/* Allocation failure has no ACK to wake us; retry after a delay.
	 * The packet remains in multicast_pending throughout this interval. */
	eigrp_timer_add(eigrp, &ei->t_multicast_retry,
			eigrp_packet_multicast_retry, ei, 250);
	return false;
}

void eigrp_packet_retransmit_timer_start(eigrp_nbr_t *nbr)
{
	eigrp_packet_t *packet;
	uint32_t rto_msec;

	if (!nbr || !nbr->retrans_queue)
		return;

	packet = eigrp_packet_queue_next(nbr->retrans_queue);
	if (!packet)
		return;

	rto_msec = eigrp_nbr_rto(nbr);
	if (IS_DEBUG_EIGRP(0, TIMERS)) {
		char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: start retransmit timer nbr %s seq %u interval %u ms",
			   eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src,
						  address, sizeof(address)),
			   packet->sequence_number, rto_msec);
	}
	eigrp_timer_add(nbr->ei->eigrp, &packet->t_retrans_timer,
				 eigrp_packet_unack_retrans, nbr, rto_msec);
}

/* A local allocation failure is not evidence of congestion on the link.
 * Retry promptly without incrementing RTP retransmission counters or RTO. */
#define EIGRP_PACKET_ALLOCATION_RETRY_MSEC 250U

static void eigrp_packet_allocation_retry(eigrp_nbr_t *nbr)
{
	eigrp_timer_add(nbr->ei->eigrp,
			&nbr->retrans_queue->tail->t_retrans_timer,
			eigrp_packet_unack_retrans, nbr,
			EIGRP_PACKET_ALLOCATION_RETRY_MSEC);
}

void eigrp_packet_send_reliably(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr)
{
	eigrp_packet_t *packet;

	if (!eigrp || !nbr || !nbr->retrans_queue)
		return;
	packet = eigrp_packet_queue_next(nbr->retrans_queue);

	if (packet) {
		eigrp_packet_t *duplicate;
		/* Only the actual RTP head is eligible, and not while an
		 * earlier multicast still owns this peer's delivery obligation. */
		if (eigrp_packet_unicast_behind_multicast(nbr,
						    packet->sequence_number))
			return;
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_LINK, eigrp, nbr->ei, nbr,
				   "send reliable queue head seq %u (depth %lu)",
				   packet->sequence_number, nbr->retrans_queue->count);
		duplicate = eigrp_packet_dup(packet, nbr);
		if (!duplicate) {
			/* No wire send occurred.  Keep the RTP head, and make sure
			 * allocation recovery cannot depend on a nonexistent ACK. */
			eigrp_packet_allocation_retry(nbr);
			return;
		}
		if (eigrp_packet_destination_is_multicast(packet))
			duplicate->multicast_exception = true;
		eigrp_addr_cpy(&duplicate->dst, &nbr->src);
		eigrp_packet_output_enqueue(eigrp, nbr->ei, duplicate);
	}
}

/* Calculate EIGRP checksum */
void eigrp_packet_checksum(eigrp_intf_t *ei, eigrp_stream_t *s,
			   uint16_t length)
{
	struct eigrp_header *eigrph;

	eigrph = (struct eigrp_header *)eigrp_stream_data(s);

	/* eigrp_checksum() returns a host-order numeric value.  The packed
	 * header field is part of the wire image and must be network order. */
	eigrph->checksum = htons(eigrp_checksum(eigrph, length));
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

	eigrp_stream_clear(s);
	eigrph = (struct eigrp_header *)eigrp_stream_data(s);

	eigrph->version = (uint8_t)EIGRP_HEADER_VERSION;
	eigrph->opcode = (uint8_t)type;
	eigrph->checksum = 0;

	eigrph->vrid = htons(eigrp->virt_router->vrid);
	eigrph->ASNumber = htons(eigrp->AS);
	eigrph->ack = htonl(ack);
	eigrph->sequence = htonl(sequence);
	//  if(flags == EIGRP_INIT_FLAG)
	//    eigrph->sequence = htonl(3);
	eigrph->flags = htonl(flags);

	if (IS_DEBUG_EIGRP_TRANSMIT(0, BUILD))
		eigrp_log(EIGRP_LOG_DEBUG, "Packet Header Init Seq [%u] Ack [%u]",
			   ntohl(eigrph->sequence), ntohl(eigrph->ack));

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

void eigrp_packet_delete(eigrp_intf_t *ei)
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

	eigrp_timer_cancel(packet->nbr && packet->nbr->ei ? packet->nbr->ei->eigrp : NULL, &packet->t_retrans_timer);

	free(packet);
}

/* Return authentication TLV when present and validate TLV framing. */
static int eigrp_packet_auth_tlv_lookup(struct eigrp_header *eigrph,
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

static int eigrp_packet_auth_tlv_validate(eigrp_intf_t *ei,
					  const eigrp_addr_t *source, uint8_t opcode,
					  struct eigrp_tlv_hdr_type *auth_tlv,
					  uint16_t length)
{
	uint16_t auth_type;
	uint16_t auth_length;

	if (length < EIGRP_AUTH_MD5_TLV_SIZE) {
		eigrp_packet_reject_event(ei, source, opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_LENGTH, length, EIGRP_AUTH_MD5_TLV_SIZE);
		return -1;
	}

	auth_type = ntohs(((struct TLV_MD5_Authentication_Type *)auth_tlv)->auth_type);
	auth_length = ntohs(((struct TLV_MD5_Authentication_Type *)auth_tlv)->auth_length);

	if (auth_type != ei->params.auth_type) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication type mismatch: received %u expected %u",
			  eigrp_intf_name_string(ei), auth_type, ei->params.auth_type);
		eigrp_packet_reject_event(ei, source, opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_TYPE_MISMATCH, auth_type, ei->params.auth_type);
		return -1;
	}

	switch (auth_type) {
	case EIGRP_AUTH_TYPE_MD5:
		if (length != EIGRP_AUTH_MD5_TLV_SIZE
		    || auth_length != EIGRP_AUTH_TYPE_MD5_LEN) {
			eigrp_packet_reject_event(ei, source, opcode,
				EIGRP_EVENTLOG_PACKET_REJECT_AUTH_LENGTH, length, auth_length);
			return -1;
		}
		return 0;
	case EIGRP_AUTH_TYPE_SHA256:
		if (length != EIGRP_AUTH_SHA256_TLV_SIZE
		    || auth_length != EIGRP_AUTH_TYPE_SHA256_LEN) {
			eigrp_packet_reject_event(ei, source, opcode,
				EIGRP_EVENTLOG_PACKET_REJECT_AUTH_LENGTH, length, auth_length);
			return -1;
		}
		return 0;
	default:
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: unsupported EIGRP authentication type %u",
			  eigrp_intf_name_string(ei), auth_type);
		eigrp_packet_reject_event(ei, source, opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_UNSUPPORTED, auth_type, 0);
		return -1;
	}
}

static int eigrp_packet_auth_header_validate(eigrp_intf_t *ei,
					     const eigrp_addr_t *source,
					     struct eigrp_header *eigrph,
					     uint16_t length)
{
	struct eigrp_tlv_hdr_type *auth_tlv;
	bool auth_first;
	int ret;

	ret = eigrp_packet_auth_tlv_lookup(eigrph, length, &auth_tlv, &auth_first);
	if (ret < 0) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: malformed EIGRP TLV framing",
			  eigrp_intf_name_string(ei));
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_TLV_FRAMING, 0, 0);
		return -1;
	}

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_NONE) {
		if (auth_tlv) {
			eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication TLV received on unauthenticated interface",
				  eigrp_intf_name_string(ei));
			eigrp_packet_reject_event(ei, source, eigrph->opcode,
				EIGRP_EVENTLOG_PACKET_REJECT_AUTH_UNEXPECTED, 0, 0);
			return -1;
		}
		return 0;
	}

	if (!eigrp_auth_material_available(ei)) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication configured without key material",
			  eigrp_intf_name_string(ei));
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_NO_KEY, ei->params.auth_type, 0);
		return -1;
	}

	if (!auth_tlv) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authenticated interface received packet without auth TLV",
			  eigrp_intf_name_string(ei));
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_MISSING, ei->params.auth_type, 0);
		return -1;
	}

	if (!auth_first) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication TLV is not first TLV",
			  eigrp_intf_name_string(ei));
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_NOT_FIRST, 0, 0);
		return -1;
	}

	return eigrp_packet_auth_tlv_validate(ei, source, eigrph->opcode, auth_tlv,
		ntohs(auth_tlv->length));
}

static uint8_t eigrp_packet_auth_flags(struct eigrp_header *eigrph)
{
	if (eigrph->opcode == EIGRP_OPC_HELLO)
		return EIGRP_AUTH_BASIC_HELLO_FLAG;

	if (eigrph->opcode == EIGRP_OPC_UPDATE
	    && (ntohl(eigrph->flags) & EIGRP_INIT_FLAG))
		return EIGRP_AUTH_UPDATE_INIT_FLAG;

	return EIGRP_AUTH_UPDATE_FLAG;
}

static int eigrp_packet_auth_digest_validate(eigrp_intf_t *ei,
					     eigrp_nbr_t *nbr,
					     const eigrp_addr_t *source,
					     struct eigrp_header *eigrph,
					     uint16_t length)
{
	struct eigrp_tlv_hdr_type *auth_tlv;
	eigrp_stream_t *auth_stream;
	eigrp_nbr_t tmp_nbr;
	bool auth_first;
	int ret;

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_NONE)
		return 0;

	ret = eigrp_packet_auth_tlv_lookup(eigrph, length, &auth_tlv, &auth_first);
	if (ret < 0 || !auth_tlv || !auth_first) {
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_TLV_FRAMING, 0, 0);
		return -1;
	}

	if (ei->params.auth_type != EIGRP_AUTH_TYPE_MD5
	    && ei->params.auth_type != EIGRP_AUTH_TYPE_SHA256) {
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_UNSUPPORTED, ei->params.auth_type, 0);
		return -1;
	}

	memset(&tmp_nbr, 0, sizeof(tmp_nbr));
	if (!nbr) {
		if (!source)
			return -1;
		tmp_nbr.ei = ei;
		tmp_nbr.src = *source;
		nbr = &tmp_nbr;
	}

	auth_stream = eigrp_stream_create(length);
	if (!auth_stream)
		return -1;
	if (eigrp_stream_put(auth_stream, eigrph, length) != length) {
		eigrp_stream_free(auth_stream);
		return -1;
	}

	if (ei->params.auth_type == EIGRP_AUTH_TYPE_SHA256)
		ret = eigrp_check_sha256_digest(
			auth_stream,
			(struct TLV_SHA256_Authentication_Type *)(eigrp_stream_data(auth_stream)
							      + EIGRP_HEADER_LEN),
			nbr, eigrp_packet_auth_flags(eigrph));
	else
		ret = eigrp_check_md5_digest(
			auth_stream,
			(struct TLV_MD5_Authentication_Type *)(eigrp_stream_data(auth_stream)
							   + EIGRP_HEADER_LEN),
			nbr, eigrp_packet_auth_flags(eigrph));

	eigrp_stream_free(auth_stream);
	if (!ret) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP authentication failed",
			  eigrp_intf_name_string(ei));
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AUTH_DIGEST, ei->params.auth_type, 0);
		return -1;
	}

	return 0;
}

/* Admit packets before the AF queue can be delayed by topology processing.
 * The process thread owns authentication and liveness; the AF owns RTP/DUAL.
 * A packet that was authenticated here MUST NOT consume the same sequence
 * a second time in the AF.  peer_lock protects neighbor lifetime and the
 * ingress crypto watermark through teardown. */
bool eigrp_packet_ingress_authenticate(eigrp_instance_t *instance,
				       eigrp_packet_input_t *input)
{
	struct eigrp_header *header;
	eigrp_intf_t *ei;
	eigrp_nbr_t *nbr;
	size_t offset;
	uint16_t length;
	bool accepted = false;

	if (!instance || !input || !input->stream
	    || !eigrp_packet_input_bounds_validate(input->stream->endp,
		input->meta.network_header_length, input->meta.eigrp_length,
		&offset, &length))
		return false;

	pthread_mutex_lock(&instance->peer_lock);
	ei = eigrp_intf_lookup_by_ifindex(instance, input->ifindex);
	if (!ei || eigrp_intf_is_passive(ei))
		goto done;
	header = (struct eigrp_header *)(input->stream->data + offset);
	if (eigrp_packet_header_validate(ei, &input->source, header, length) < 0)
		goto done;

	switch (header->opcode) {
	case EIGRP_OPC_HELLO:
	case EIGRP_OPC_UPDATE:
	case EIGRP_OPC_QUERY:
	case EIGRP_OPC_REPLY:
	case EIGRP_OPC_SIAQUERY:
	case EIGRP_OPC_SIAREPLY:
		break;
	default:
		goto done;
	}
	if (header->opcode == EIGRP_OPC_HELLO && !ntohl(header->ack)
	    && (!eigrp_hello_tlvs_validate(ei,
			(const uint8_t *)header->tlv, length - EIGRP_HEADER_LEN)
		|| !eigrp_nbr_static_source_allowed(ei, &input->source)))
		goto done;

	nbr = eigrp_nbr_lookup(ei, header, &input->source);
	if (nbr && !nbr->published)
		nbr = NULL;
	/* Suspended neighbors keep their identity but are not eligible for any
	 * ingress work, including authentication or hold-timer refresh. */
	if (nbr && !eigrp_nbr_is_eligible(nbr))
		goto done;
	/* Only discovery Hellos may arrive without an existing peer. */
	if (!nbr && (header->opcode != EIGRP_OPC_HELLO || ntohl(header->ack)))
		goto done;
	/* Reject excess sources before spending work authenticating them. */
	if (!nbr && !eigrp_nbr_admit_locked(ei))
		goto done;
	if (eigrp_packet_auth_digest_validate(ei, nbr, &input->source,
					    header, length) < 0)
		goto done;

	input->ingress_authenticated = true;
	input->ingress_neighbor_generation = nbr ? nbr->generation : 0;
	input->ingress_discovery = !nbr || nbr->state != EIGRP_NEIGHBOR_UP;
	if (nbr)
		eigrp_nbr_holddown_update(nbr);
	accepted = true;
done:
	pthread_mutex_unlock(&instance->peer_lock);
	return accepted;
}

/* EIGRP Header verification. */
static int eigrp_packet_header_validate(eigrp_intf_t *ei, eigrp_addr_t *source,
			       struct eigrp_header *eigrph, uint16_t length)
{
	char source_text[EIGRP_PACKET_ADDR_TEXT_SIZE];
	uint16_t checksum;

	if (length < EIGRP_HEADER_LEN) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP packet too short: %u",
			  eigrp_intf_name_string(ei), length);
		eigrp_packet_reject_event(ei, source, 0,
			EIGRP_EVENTLOG_PACKET_REJECT_SHORT_HEADER, length, EIGRP_HEADER_LEN);
		return -1;
	}

	if (eigrph->version != EIGRP_HEADER_VERSION) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: unsupported EIGRP header version %u",
			  eigrp_intf_name_string(ei), eigrph->version);
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_VERSION, eigrph->version, EIGRP_HEADER_VERSION);
		return -1;
	}

	if (ntohs(eigrph->ASNumber) != ei->eigrp->AS) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP AS mismatch: received %u expected %u",
			  eigrp_intf_name_string(ei), ntohs(eigrph->ASNumber),
			  ei->eigrp->AS);
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_AS_MISMATCH, ntohs(eigrph->ASNumber),
			ei->eigrp->AS);
		return -1;
	}

	if (ntohs(eigrph->vrid) != ei->eigrp->virt_router->vrid) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP VRID mismatch: received %u expected %u",
			  eigrp_intf_name_string(ei), ntohs(eigrph->vrid),
			  ei->eigrp->virt_router->vrid);
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_VRID_MISMATCH, ntohs(eigrph->vrid),
			ei->eigrp->virt_router->vrid);
		return -1;
	}

	checksum = eigrp_checksum(eigrph, length);
	if (checksum != 0) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: EIGRP checksum failed from %s",
			  eigrp_intf_name_string(ei),
			  eigrp_packet_addr_text(ei->eigrp, source, source_text,
					 sizeof(source_text)));
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_CHECKSUM, checksum, 0);
		return -1;
	}

	if (eigrp_packet_auth_header_validate(ei, source, eigrph, length) < 0)
		return -1;

	/* Raw sockets can receive protocol-matched packets from other links. */
	if (!ei->eigrp->af_vectors.packet_source_on_link(ei, source)) {
		eigrp_log(EIGRP_LOG_WARNING, "interface %s: eigrp_packet_read source is not on-link [%s]",
			  eigrp_intf_name_string(ei),
			  eigrp_packet_addr_text(ei->eigrp, source, source_text,
					 sizeof(source_text)));
		eigrp_packet_reject_event(ei, source, eigrph->opcode,
			EIGRP_EVENTLOG_PACKET_REJECT_SOURCE_OFFLINK, 0, 0);
		return -1;
	}

	return 0;
}

void eigrp_packet_unack_retrans(void *arg)
{
	eigrp_nbr_t *nbr = arg;
	eigrp_packet_t *packet;
	eigrp_packet_t *duplicate;

	packet = eigrp_packet_queue_next(nbr->retrans_queue);
	if (!packet || packet->retry_queued)
		return;

	/* Give the peer all 16 retries.  Tear it down only after retry 16
	 * has itself gone unacknowledged. */
	if (packet->retrans_counter >= EIGRP_TRANSPORT_RETRANS_MAX) {
		eigrp_prefix_t peer_addr;

		eigrp_eventlog_addr_from_legacy(&peer_addr, &nbr->src);
		(void)eigrp_eventlog_msg_add(nbr->ei->eigrp, EIGRP_EVENTLOG_OPCODE_RTP_RETRY_LIMIT,
			&peer_addr, packet->sequence_number, packet->retrans_counter, 0, 0);
		eigrp_packet_retransmit_limit_exceeded(nbr);
		return;
	}

	/* If the first-send image could not be allocated (or the host
	 * rejected it), this is still a first transmission, not an RTP retry. */
	duplicate = eigrp_packet_dup(packet, nbr);
	if (!duplicate) {
		/* This timer was dequeued before callback.  Another retry must
		 * always be armed; no peer ACK can recover an unsent packet. */
		eigrp_packet_allocation_retry(nbr);
		return;
	}
	if (packet->sent_msec) {
		eigrp_nbr_rto_backoff(nbr);
		if (IS_DEBUG_EIGRP(0, TIMERS)) {
			char address[EIGRP_PACKET_ADDR_TEXT_SIZE];

			eigrp_log(EIGRP_LOG_DEBUG,
				"EIGRP: retransmit timer expired nbr %s seq %u retry %u",
				eigrp_packet_addr_text(nbr->ei->eigrp, &nbr->src,
						address, sizeof(address)),
				packet->sequence_number, packet->retrans_counter + 1);
		}
		eigrp_debug_packet_retry(nbr, packet, packet->retrans_counter + 1);
		{
			eigrp_prefix_t peer_addr;

			eigrp_eventlog_addr_from_legacy(&peer_addr, &nbr->src);
			(void)eigrp_eventlog_msg_add(nbr->ei->eigrp,
				EIGRP_EVENTLOG_OPCODE_RTP_RETRANSMIT,
				&peer_addr, packet->sequence_number,
				packet->retrans_counter + 1, 0, 0);
		}
		duplicate->retransmission = true;
	}
	if (eigrp_packet_destination_is_multicast(packet))
		duplicate->multicast_exception = true;
	eigrp_addr_cpy(&duplicate->dst, &nbr->src);
	packet->retry_queued = true;
	eigrp_packet_output_enqueue(nbr->ei->eigrp, nbr->ei, duplicate);
	/* Pacing wait is not a transmitted attempt.  The successful wire send
	 * starts the next RTO and increments the retry counter. */
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

eigrp_packet_t *eigrp_packet_dup(eigrp_packet_t *old,
				       eigrp_nbr_t *nbr)
{
	eigrp_packet_t *new;

	if (!old || !old->s)
		return NULL;

	new = eigrp_packet_create(old->length, nbr);
	if (!new)
		return NULL;
	new->length = old->length;
	new->retrans_counter = old->retrans_counter;
	new->dst = old->dst;
	new->sequence_number = old->sequence_number;
	new->retransmission = false;
	new->multicast_exception = false;
	eigrp_stream_copy(new->s, old->s);

	return new;
}

struct TLV_Sequence_Type *eigrp_sequence_tlv_create(void)
{
	struct TLV_Sequence_Type *new;

	new = calloc(1, sizeof(struct TLV_Sequence_Type));

	return new;
}
