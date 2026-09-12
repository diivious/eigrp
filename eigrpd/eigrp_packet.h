// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP General Sending and Receiving of EIGRP Packets.
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
 *
 */

#ifndef _ZEBRA_EIGRP_PACKET_H
#define _ZEBRA_EIGRP_PACKET_H

#include "lib/stream.h"
#include "lib/sockunion.h"
#include "lib/sockopt.h"

/**
 * every TLV has a header - might as well define it here
 */
#define EIGRP_TLV_HDR                                                          \
	uint16_t type;                                                         \
	uint16_t length;
#define EIGRP_TLV_HDR_SIZE 4

typedef struct eigrp_tlv_header {
	uint16_t type;
	uint16_t length;
} eigrp_tlv_header_t;

/*Prototypes*/
extern void eigrp_packet_read(struct event *);
extern void eigrp_packet_write(struct event *);

extern eigrp_packet_t *eigrp_packet_new(size_t, eigrp_neighbor_t *);
extern eigrp_packet_t *eigrp_packet_duplicate(eigrp_packet_t *,
					      eigrp_neighbor_t *);
extern void eigrp_packet_free(eigrp_packet_t *);
extern void eigrp_packet_delete(eigrp_interface_t *);
extern uint32_t eigrp_packet_sequence_reserve(eigrp_instance_t *);
extern void eigrp_packet_header_init(int, eigrp_instance_t *, struct stream *,
				     uint32_t, uint32_t, uint32_t);
extern void eigrp_packet_checksum(eigrp_interface_t *, struct stream *,
				  uint16_t);

extern eigrp_packet_queue_t *eigrp_packet_queue_new(void);
extern eigrp_packet_t *eigrp_packet_queue_next(eigrp_packet_queue_t *);
extern eigrp_packet_t *eigrp_packet_dequeue(eigrp_packet_queue_t *);
extern void eigrp_packet_enqueue(eigrp_packet_queue_t *, eigrp_packet_t *);
extern void eigrp_packet_queue_free(eigrp_packet_queue_t *);
extern void eigrp_packet_queue_reset(eigrp_packet_queue_t *);

extern void eigrp_packet_output_enqueue(eigrp_instance_t *, eigrp_interface_t *,
				       eigrp_packet_t *);
extern void eigrp_packet_retransmit_timer_start(eigrp_neighbor_t *);
extern void eigrp_packet_send_reliably(eigrp_instance_t *, eigrp_neighbor_t *);

extern void eigrp_packet_unack_retrans(struct event *);
extern void eigrp_packet_unack_multicast_retrans(struct event *);

extern eigrp_route_descriptor_t *eigrp_packet_decoder_safe(
	eigrp_instance_t *, eigrp_neighbor_t *, eigrp_stream_t *, uint16_t);
extern uint16_t eigrp_packet_encoder_safe(eigrp_instance_t *,
					 eigrp_interface_t *, eigrp_neighbor_t *,
					 eigrp_stream_t *,
					 eigrp_route_descriptor_t *);
extern uint16_t eigrp_packet_encoder_both(eigrp_instance_t *,
					 eigrp_interface_t *, eigrp_neighbor_t *,
					 eigrp_stream_t *,
					 eigrp_route_descriptor_t *);

/*
 * untill there is reason to have their own header, these externs are found in
 * eigrp_hello.c
 */
extern void eigrp_sw_version_init(void);
extern void eigrp_hello_send(eigrp_interface_t *, uint8_t, eigrp_addr_t *);
extern void eigrp_hello_send_ack(eigrp_neighbor_t *);
extern void eigrp_hello_receive(eigrp_instance_t *, eigrp_header_t *,
			 eigrp_addr_t *, eigrp_interface_t *,
			 struct stream *, int);
extern void eigrp_hello_timer(struct event *);

/*
 * These externs are found in eigrp_update.c
 */
extern bool eigrp_update_prefix_apply(eigrp_instance_t *eigrp,
				      eigrp_interface_t *ei, int in,
				      struct prefix *prefix);
extern void eigrp_update_send(eigrp_instance_t *, eigrp_neighbor_t *,
			      eigrp_interface_t *);
extern void eigrp_update_receive(eigrp_instance_t *, eigrp_neighbor_t *,
				 eigrp_header_t *, struct stream *,
				 eigrp_interface_t *, int);
extern void eigrp_update_send_all(eigrp_instance_t *, eigrp_interface_t *);
extern void eigrp_update_packetize_all(eigrp_instance_t *, eigrp_interface_t *);
extern void eigrp_update_send_init(eigrp_instance_t *, eigrp_neighbor_t *);
extern void eigrp_update_send_EOT(eigrp_neighbor_t *);
extern void eigrp_update_send_GR_event(struct event *);
extern void eigrp_update_send_GR(eigrp_neighbor_t *, enum GR_type,
				 struct vty *);
extern void eigrp_update_send_interface_GR(eigrp_interface_t *, enum GR_type,
					   struct vty *);
extern void eigrp_update_send_process_GR(eigrp_instance_t *, enum GR_type,
					 struct vty *);

/*
 * These externs are found in eigrp_query.c
 */
extern void eigrp_query_receive(eigrp_instance_t *, eigrp_neighbor_t *,
				eigrp_header_t *, struct stream *,
				eigrp_interface_t *, int);
extern uint32_t eigrp_query_send_all(eigrp_instance_t *);

/*
 * These externs are found in eigrp_reply.c
 */
extern void eigrp_reply_send(eigrp_instance_t *, eigrp_neighbor_t *,
			     eigrp_prefix_descriptor_t *);
extern void eigrp_reply_send_route(eigrp_instance_t *, eigrp_neighbor_t *,
				   eigrp_prefix_descriptor_t *,
				   eigrp_route_descriptor_t *, uint32_t);
extern void eigrp_reply_receive(eigrp_instance_t *, eigrp_neighbor_t *,
				eigrp_header_t *, struct stream *,
				eigrp_interface_t *, int);

/*
 * These externs are found in eigrp_siaquery.c
 */
extern void eigrp_siaquery_send(eigrp_instance_t *, eigrp_neighbor_t *,
				eigrp_prefix_descriptor_t *);
extern void eigrp_siaquery_receive(eigrp_instance_t *, eigrp_neighbor_t *,
				   eigrp_header_t *, struct stream *,
				   eigrp_interface_t *, int);

/*
 * These externs are found in eigrp_siareply.c
 */
extern void eigrp_siareply_send(eigrp_instance_t *, eigrp_neighbor_t *,
				eigrp_prefix_descriptor_t *);
extern void eigrp_siareply_receive(eigrp_instance_t *, eigrp_neighbor_t *,
				   eigrp_header_t *, struct stream *,
				   eigrp_interface_t *, int);

/*
 * These externs need to cleaned up
 */
extern void eigrp_IPv4_InternalTLV_free(struct TLV_IPv4_Internal_type *);

extern struct TLV_Sequence_Type *eigrp_SequenceTLV_new(void);

extern const struct message eigrp_packet_type_str[];
extern const size_t eigrp_packet_type_str_max;

#endif /* _ZEBRA_EIGRP_PACKET_H */
