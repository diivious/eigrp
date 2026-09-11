/*
 * EIGRP Definition of Data Types
 * Copyright (C) 2018
 * Authors:
 *   Donnie Savage
 */
#ifndef _ZEBRA_EIGRP_TYPES_H_
#define _ZEBRA_EIGRP_TYPES_H_

#include <stdbool.h>
#include <stdint.h>

#include "eigrpd/eigrp_const.h"
#include "eigrpd/eigrp_macros.h"

typedef enum eigrp_address_family {
	EIGRP_ADDRESS_FAMILY_IPV4 = 4,
	EIGRP_ADDRESS_FAMILY_IPV6 = 6,
} eigrp_address_family_t;

/* EIGRP topology identifiers are 16-bit values on the wire. */
typedef uint16_t eigrp_topology_id_t;

#define EIGRP_TOPOLOGY_ID_BASE ((eigrp_topology_id_t)0)

/* Common prefix-limit policy used by process, neighbor, and redistribution. */
typedef struct eigrp_prefix_limit {
	uint32_t maximum;
	uint8_t threshold;
	bool warning_only;
	bool dampened;
	uint16_t reset_time_minutes;
	uint16_t restart_minutes;
	uint16_t restart_count;
} eigrp_prefix_limit_t;

/*
 * Host-independent management/configuration address types.  Runtime packet
 * code still uses eigrp_addr_t where appropriate; these types are the clean
 * northbound-to-core representation and do not depend on FRR prefix objects.
 */
typedef struct eigrp_address {
	eigrp_address_family_t afi;
	uint8_t bytes[16];
} eigrp_address_t;

typedef struct eigrp_prefix {
	eigrp_address_t address;
	uint8_t prefix_length;
} eigrp_prefix_t;

/**
 * Nice type modifers to make code more readable (and maybe portable)
 */
typedef struct stream eigrp_stream_t;

typedef uint64_t eigrp_bandwidth_t;
typedef uint64_t eigrp_delay_t;
typedef uint64_t eigrp_metric_t;
typedef uint32_t eigrp_scaled_t;

typedef uint32_t eigrp_system_metric_t;
typedef uint32_t eigrp_system_delay_t;
typedef uint32_t eigrp_system_bandwidth_t;

/**
 * define some primitive types for use in pointer passing. This will allow for
 * better type  checking, especially when dealing with classic metrics (32bit)
 * and wide metrics (64bit).
 *
 * If you need structure details, include the appropriate header file
 */
typedef struct eigrp_instance eigrp_instance_t;
typedef struct eigrp_interface eigrp_interface_t;
typedef struct eigrp_neighbor eigrp_neighbor_t;
typedef struct eigrp_metrics eigrp_metrics_t;
typedef struct eigrp_prefix_descriptor eigrp_prefix_descriptor_t;
typedef struct eigrp_route_descriptor eigrp_route_descriptor_t;
typedef struct eigrp_fsm_action_message eigrp_fsm_action_message_t;
typedef struct eigrp_work_queue eigrp_work_queue_t;

/* Portable configuration objects used by classic/named management adapters. */
typedef struct eigrp_instance_parent_config eigrp_instance_parent_config_t;
typedef struct eigrp_address_family_config eigrp_address_family_config_t;
typedef struct eigrp_interface_config eigrp_interface_config_t;
typedef struct eigrp_network_config eigrp_network_config_t;
typedef struct eigrp_neighbor_config eigrp_neighbor_config_t;
typedef struct eigrp_summary_config eigrp_summary_config_t;

typedef struct eigrp_state_request {
	eigrp_address_family_t afi;
	const char *vrf_name;
	uint16_t asn; /* zero means all configured AS contexts */
	bool all_vrfs; /* otherwise a NULL VRF name means the default VRF */
} eigrp_state_request_t;

// basic packet processor definitions
typedef struct eigrp_packet eigrp_packet_t;
typedef struct eigrp_tlv_header eigrp_tlv_header_t;

typedef eigrp_route_descriptor_t *(*eigrp_packet_decoder_t)(
	eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr, eigrp_stream_t *pkt,
	uint16_t pktlen);

typedef uint16_t (*eigrp_packet_encoder_t)(
	eigrp_instance_t *eigrp, eigrp_interface_t *ei, eigrp_neighbor_t *nbr,
	eigrp_stream_t *pkt, eigrp_route_descriptor_t *route);

typedef struct eigrp_tlv_codec {
	eigrp_packet_encoder_t encoder;
	eigrp_packet_decoder_t decoder;
} eigrp_tlv_codec_t;

#endif /* _ZEBRA_EIGRP_TYPES_H_ */
