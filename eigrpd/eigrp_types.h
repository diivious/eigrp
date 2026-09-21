/*
 * EIGRP Definition of Data Types
 * Copyright (C) 2018
 * Authors:
 *   Donnie Savage
 */
#ifndef _ZEBRA_EIGRP_TYPES_H_
#define _ZEBRA_EIGRP_TYPES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "eigrpd/eigrp_const.h"
#include "eigrpd/eigrp_result.h"
#include "eigrpd/eigrp_list.h"
#include "eigrpd/eigrp_stream.h"

typedef enum eigrp_address_family {
	EIGRP_ADDRESS_FAMILY_IPV4 = 4,
	EIGRP_ADDRESS_FAMILY_IPV6 = 6,
} eigrp_address_family_t;

typedef enum eigrp_offset_direction {
	EIGRP_OFFSET_IN = 0,
	EIGRP_OFFSET_OUT,
} eigrp_offset_direction_t;

typedef enum eigrp_distribute_list_type {
	EIGRP_DISTRIBUTE_ACCESS_LIST = 0,
	EIGRP_DISTRIBUTE_PREFIX_LIST,
} eigrp_distribute_list_type_t;

/* Portable policy result returned by a host policy adapter. */
typedef enum eigrp_filter_decision {
	EIGRP_FILTER_DECISION_PERMIT = 0,
	EIGRP_FILTER_DECISION_DENY,
} eigrp_filter_decision_t;

/*
 * Runtime filter references are EIGRP-owned names, never host policy objects.
 * The host adapter resolves these names when a prefix decision is required.
 */
typedef struct eigrp_filter_runtime_state {
	char *access_list[EIGRP_FILTER_MAX];
	char *prefix_list[EIGRP_FILTER_MAX];
} eigrp_filter_runtime_state_t;

typedef struct eigrp_filter_runtime_snapshot {
	const char *access_list[EIGRP_FILTER_MAX];
	const char *prefix_list[EIGRP_FILTER_MAX];
} eigrp_filter_runtime_snapshot_t;

/* EIGRP topology identifiers are 16-bit values on the wire. */
typedef uint16_t eigrp_topology_id_t;
typedef uint32_t eigrp_vrf_id_t;
typedef uint32_t eigrp_ifindex_t;

#define EIGRP_TOPOLOGY_ID_BASE ((eigrp_topology_id_t)0)
#define EIGRP_VRF_DEFAULT ((eigrp_vrf_id_t)0)

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

/* Host-independent runtime interface state supplied by a host adapter. */
typedef struct eigrp_interface_runtime_state {
	const char *interface_name;
	eigrp_ifindex_t ifindex;
	eigrp_prefix_t address;
	uint8_t type;
	bool secondary;
	bool operative;
	uint32_t bandwidth;
	uint32_t mtu;
} eigrp_interface_runtime_state_t;

typedef enum eigrp_interface_remove_reason {
	EIGRP_INTERFACE_REMOVE_HOST = 1,
	EIGRP_INTERFACE_REMOVE_CONFIG,
	EIGRP_INTERFACE_REMOVE_FINAL,
} eigrp_interface_remove_reason_t;

/**
 * Nice type modifers to make code more readable (and maybe portable)
 */

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
typedef struct eigrp_addr eigrp_addr_t;
typedef struct eigrp_metrics eigrp_metrics_t;
typedef struct eigrp_prefix_descriptor eigrp_prefix_descriptor_t;
typedef struct eigrp_route_descriptor eigrp_route_descriptor_t;
typedef struct eigrp_fsm_action_message eigrp_fsm_action_message_t;
typedef struct eigrp_work_queue eigrp_work_queue_t;
typedef struct eigrp_event eigrp_event_t;
typedef struct eigrp_eventlog eigrp_eventlog_t;
typedef struct eigrp_table eigrp_table_t;
typedef struct eigrp_table_node eigrp_table_node_t;

/* Portable configuration objects used by classic/named management adapters. */
typedef struct eigrp_instance_parent_config eigrp_instance_parent_config_t;
typedef struct eigrp_address_family_config eigrp_address_family_config_t;
typedef struct eigrp_interface_config eigrp_interface_config_t;
typedef struct eigrp_network_config eigrp_network_config_t;
typedef struct eigrp_neighbor_config eigrp_neighbor_config_t;
typedef struct eigrp_summary_config eigrp_summary_config_t;
typedef struct eigrp_redistribute_config eigrp_redistribute_config_t;
typedef struct eigrp_distribute_list_config eigrp_distribute_list_config_t;
typedef struct eigrp_offset_config eigrp_offset_config_t;
typedef struct eigrp_metric_config eigrp_metric_config_t;
typedef struct eigrp_summary_state eigrp_summary_state_t;
typedef struct eigrp_timer_config eigrp_timer_config_t;
typedef struct eigrp_neighbor_policy_state eigrp_neighbor_policy_state_t;
typedef struct eigrp_redistribute_policy_config eigrp_redistribute_policy_config_t;

typedef struct eigrp_state_request {
	eigrp_address_family_t afi;
	const char *vrf_name;
	uint16_t asn; /* zero means all configured AS contexts */
	bool all_vrfs; /* otherwise a NULL VRF name means the default VRF */
	/* Cisco's token selects the EIGRP Multicast Address Family (MAF). */
	bool multicast;
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

typedef struct eigrp_message {
	int value;
	const char *name;
} eigrp_message_t;

const char *eigrp_message_lookup(const eigrp_message_t *messages, int value,
                                 const char *fallback);

typedef struct eigrp_tlv_codec {
	eigrp_packet_encoder_t encoder;
	eigrp_packet_decoder_t decoder;
} eigrp_tlv_codec_t;

typedef struct eigrp_packet_rx_meta {
	uint16_t network_header_length;
	uint16_t eigrp_length;
	bool destination_multicast;
} eigrp_packet_rx_meta_t;

typedef struct eigrp_af_vectors {
	eigrp_address_family_t afi;

	/*
	 * Network-layer packet envelope.  Common EIGRP packet processing owns
	 * queueing, acknowledgements, checksum/authentication, and opcode
	 * dispatch; the AF owns the IP header/socket representation and extracts
	 * the native EIGRP source/destination addresses.  Send/receive may be
	 * unbound only while an address-family data path is capability-gated.
	 * Runtime creation validates the complete vector before packet processing
	 * is started; common packet code does not probe callbacks before use.
	 */
	int (*packet_send)(eigrp_instance_t *eigrp, eigrp_interface_t *ei,
			   eigrp_packet_t *packet);
	bool (*packet_receive)(eigrp_instance_t *eigrp, int fd,
			       eigrp_stream_t *stream, eigrp_interface_t **ei,
			       eigrp_addr_t *source, eigrp_addr_t *destination,
			       eigrp_packet_rx_meta_t *meta);
	bool (*packet_source_on_link)(eigrp_interface_t *ei,
				      const eigrp_addr_t *source);

	/*
	 * Address-family wire primitives.  Address encoding is for a complete
	 * runtime address (for example, a classic-TLV next hop); prefix encoding
	 * owns the EIGRP prefix-length/significant-byte representation.
	 */
	uint8_t packet_address_bytes;
	uint16_t (*packet_address_decode)(eigrp_stream_t *stream,
					eigrp_addr_t *address);
	uint16_t (*packet_address_encode)(eigrp_stream_t *stream,
					const eigrp_addr_t *address);
	uint16_t (*packet_prefix_decode)(eigrp_stream_t *stream,
				       eigrp_prefix_t *prefix);
	uint16_t (*packet_prefix_encode)(eigrp_stream_t *stream,
				       const eigrp_prefix_t *prefix);

	/*
	 * Route-TLV address-family selectors.  TLV1/TLV2 remain the route codec
	 * owners because classic and multiprotocol metrics/exterior sections are
	 * independent of address family.  The AF supplies only the family-specific
	 * route type/AFI values and destination representation.
	 */
	uint16_t classic_internal_tlv_type;
	uint16_t classic_external_tlv_type;
	uint16_t multiprotocol_afi;

	/* Portable text presentation used by show/debug callers. */
	int (*addr_snprintf)(char *buf, size_t len,
			     const eigrp_addr_t *address);

	/* AF-specific classful automatic-summary derivation.  Every AF binds a
	 * function; families without classful semantics return UNSUPPORTED.
	 */
	eigrp_result_t (*summary_auto_prefix)(const eigrp_prefix_t *component,
					      eigrp_prefix_t *summary);
} eigrp_af_vectors_t;

/* AF modules expose only vector initialization. */
void eigrp_ipv4_init(eigrp_af_vectors_t *vectors);
void eigrp_ipv6_init(eigrp_af_vectors_t *vectors);

#endif /* _ZEBRA_EIGRP_TYPES_H_ */
