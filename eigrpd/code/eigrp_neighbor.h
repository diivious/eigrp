// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Neighbor Handling.
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

#ifndef _ZEBRA_EIGRP_NEIGHBOR_H
#define _ZEBRA_EIGRP_NEIGHBOR_H

#include <stddef.h>

#include "eigrp_cli.h"
#include "eigrp_mgnt.h"
#include "eigrp_instance.h"
#include "eigrp_types.h"

/* Neighbor Data Structure */
typedef struct eigrp_neighbor {

	uint8_t os_rel_major; // system version - just for show
	uint8_t os_rel_minor; // system version - just for show

	/* TLV version and packet vectors for this neighbor. */
	uint8_t tlv_rel_major; // eigrp version - tells us what TLV format to use
	uint8_t tlv_rel_minor; // eigrp version - tells us what TLV format to use
	uint8_t tlv_version;

	eigrp_packet_decoder_t decoder;
	eigrp_packet_encoder_t encoder;

	uint8_t K1;
	uint8_t K2;
	uint8_t K3;
	uint8_t K4;
	uint8_t K5;
	uint8_t K6;

	/* This neighbor's parent eigrp interface. */
	eigrp_intf_t *ei;

	/* EIGRP neighbor Information */
	uint8_t state; /* neigbor status. */

	uint32_t recv_sequence_number; /* Last received sequence Number. */
	uint32_t init_sequence_number;

	/* If a packet is unacknowledged, retry it up to the transport limit. */
	uint8_t retrans_counter;
	uint64_t retransmissions;
	uint64_t up_since_msec;

	/* Per-neighbor Reliable Transport Protocol RTT/RTO state. */
	bool srtt_valid;
	uint32_t srtt_msec;
	uint32_t rttvar_msec;
	uint32_t rto_msec;

	eigrp_addr_t src;		/* Neighbor Src address. */

	/* Timer values. */
	uint16_t v_holddown;

	/* Events. */
	eigrp_event_t *t_holddown;
	eigrp_event_t *t_nbr_send_gr; /* event for sending multiple GR packet
					 chunks */

	eigrp_packet_queue_t *retrans_queue;

	/* Reliable multicast conditional-receive state. */
	bool cr_mode;
	uint32_t cr_sequence;

	uint32_t crypt_seqnum; /* Cryptographic Sequence Number. */

	/* prefixes not received from neighbor during Graceful restart */
	eigrp_list_t *nbr_gr_prefixes;
	/* prefixes not yet send to neighbor during Graceful restart */
	eigrp_list_t *nbr_gr_prefixes_send;
	/* if packet is first or last during Graceful restart */
	enum Packet_part_type nbr_gr_packet_type;

} eigrp_nbr_t;


/* Prototypes */
extern eigrp_nbr_t *eigrp_nbr_lookup(eigrp_intf_t *, struct eigrp_header *,
					  eigrp_addr_t *);
extern eigrp_nbr_t *eigrp_nbr_create(eigrp_intf_t *, eigrp_addr_t *);
extern void eigrp_nbr_delete(eigrp_nbr_t *neigh);

extern void eigrp_nbr_holddown_expired(void *arg);

extern void eigrp_nbr_holddown_update(eigrp_nbr_t *);
extern void eigrp_nbr_state_update(eigrp_operation_t, eigrp_nbr_t *, uint8_t state);
extern void eigrp_nbr_codec_select(eigrp_nbr_t *, uint8_t tlv_version);
extern void eigrp_nbr_codec_update(eigrp_instance_t *);
extern uint8_t eigrp_nbr_state(eigrp_nbr_t *);
extern int eigrp_nbr_count(eigrp_instance_t *);
extern const char *eigrp_nbr_state_str(eigrp_nbr_t *);

/* Per-neighbor Reliable Transport Protocol RTT/RTO state. */
void eigrp_nbr_rtt_clear(eigrp_nbr_t *nbr);
void eigrp_nbr_srtt_update(eigrp_nbr_t *nbr,
				const eigrp_packet_t *packet);
void eigrp_nbr_rto_backoff(eigrp_nbr_t *nbr);
uint32_t eigrp_nbr_rto(const eigrp_nbr_t *nbr);
extern eigrp_nbr_t *eigrp_nbr_lookup_by_addr(eigrp_intf_t *,
						  struct in_addr *);
extern eigrp_nbr_t *eigrp_nbr_lookup_by_addr_process(eigrp_instance_t *,
							  struct in_addr addr);

extern int eigrp_nbr_split_horizon(eigrp_route_descriptor_t *,
					 eigrp_intf_t *);

eigrp_result_t eigrp_nbr_static_create(eigrp_af_instance_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name);
eigrp_result_t eigrp_nbr_static_delete(eigrp_af_instance_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name);
void eigrp_nbr_static_delete_all(eigrp_af_instance_t *af);
bool eigrp_nbr_static_hello_send(eigrp_intf_t *ei);
bool eigrp_nbr_static_source_allowed(eigrp_intf_t *ei,
                                          const eigrp_addr_t *src);
eigrp_result_t eigrp_nbr_state_iterate(
	eigrp_af_instance_t *config, eigrp_instance_t *runtime,
	const char *interface_name, bool static_only,
	eigrp_nbr_state_iterate_cb callback, void *arg);
eigrp_result_t eigrp_nbr_clear(
	eigrp_instance_t *runtime, const eigrp_nbr_clear_request_t *request,
	eigrp_nbr_clear_cb callback, void *arg, size_t *affected_count);

eigrp_result_t eigrp_nbr_description_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_address_t *address,
	const char *description);
eigrp_result_t eigrp_nbr_max_prefix_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_address_t *address,
	const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_nbr_max_prefix_all_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_nbr_log_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	eigrp_nbr_log_type_t type,
	bool enabled,
	uint16_t seconds);
void eigrp_nbr_policy_delete_all(eigrp_af_instance_t *af);

#endif /* _ZEBRA_EIGRP_NEIGHBOR_H */
