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

#include "eigrpd/eigrp_cli.h"
#include "eigrpd/eigrp_mgnt.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_types.h"

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
	eigrp_interface_t *ei;

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

} eigrp_neighbor_t;


/* Prototypes */
extern eigrp_neighbor_t *eigrp_nbr_lookup(eigrp_interface_t *, struct eigrp_header *,
					  eigrp_addr_t *);
extern eigrp_neighbor_t *eigrp_nbr_create(eigrp_interface_t *, eigrp_addr_t *);
extern void eigrp_nbr_delete(eigrp_neighbor_t *neigh);

extern void eigrp_neighbor_holddown_expired(void *arg);

extern int eigrp_neighborship_check(eigrp_neighbor_t *,
				    struct TLV_Parameter_Type *tlv);
extern void eigrp_nbr_state_update(eigrp_neighbor_t *);
extern void eigrp_nbr_state_set(eigrp_neighbor_t *, uint8_t state);
extern void eigrp_neighbor_codec_bind(eigrp_neighbor_t *, uint8_t tlv_version);
extern uint8_t eigrp_nbr_state_get(eigrp_neighbor_t *);
extern int eigrp_nbr_count_get(eigrp_instance_t *);
extern const char *eigrp_nbr_state_str(eigrp_neighbor_t *);

/* Per-neighbor Reliable Transport Protocol RTT/RTO state. */
void eigrp_neighbor_rtt_reset(eigrp_neighbor_t *nbr);
void eigrp_neighbor_srtt_update(eigrp_neighbor_t *nbr,
				const eigrp_packet_t *packet);
void eigrp_neighbor_rto_backoff(eigrp_neighbor_t *nbr);
uint32_t eigrp_neighbor_rto_get(const eigrp_neighbor_t *nbr);
extern eigrp_neighbor_t *eigrp_nbr_lookup_by_addr(eigrp_interface_t *,
						  struct in_addr *);
extern eigrp_neighbor_t *eigrp_nbr_lookup_by_addr_process(eigrp_instance_t *,
							  struct in_addr addr);

extern int eigrp_nbr_split_horizon_check(eigrp_route_descriptor_t *,
					 eigrp_interface_t *);

eigrp_result_t eigrp_neighbor_static_create(eigrp_address_family_config_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name);
eigrp_result_t eigrp_neighbor_static_delete(eigrp_address_family_config_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name);
void eigrp_neighbor_static_delete_all(eigrp_address_family_config_t *af);
bool eigrp_neighbor_static_hello_send(eigrp_interface_t *ei);
bool eigrp_neighbor_static_source_allowed(eigrp_interface_t *ei,
                                          const eigrp_addr_t *src);
eigrp_result_t eigrp_neighbor_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, bool static_only,
	eigrp_neighbor_state_walk_cb callback, void *arg);
eigrp_result_t eigrp_neighbor_clear(
	eigrp_instance_t *runtime, const eigrp_neighbor_clear_request_t *request,
	eigrp_neighbor_clear_cb callback, void *arg, size_t *affected_count);

eigrp_result_t eigrp_neighbor_description_set(
	eigrp_instance_context_t *context, const eigrp_address_t *address,
	const char *description);
eigrp_result_t eigrp_neighbor_description_reset(
	eigrp_instance_context_t *context, const eigrp_address_t *address);
eigrp_result_t eigrp_neighbor_maximum_prefix_set(
	eigrp_instance_context_t *context, const eigrp_address_t *address,
	const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_neighbor_maximum_prefix_reset(
	eigrp_instance_context_t *context, const eigrp_address_t *address);
eigrp_result_t eigrp_neighbor_maximum_prefix_all_set(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_neighbor_maximum_prefix_all_reset(
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_neighbor_log_set(eigrp_instance_context_t *context,
				      eigrp_neighbor_log_type_t type,
				      bool enabled, uint16_t seconds);
eigrp_result_t eigrp_neighbor_log_reset(eigrp_instance_context_t *context,
					eigrp_neighbor_log_type_t type);
void eigrp_neighbor_policy_delete_all(eigrp_address_family_config_t *af);

#endif /* _ZEBRA_EIGRP_NEIGHBOR_H */
