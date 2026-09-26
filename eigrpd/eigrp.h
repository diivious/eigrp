// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Public EIGRP integration values and opaque identities.
 *
 * Host shims include this header. Read specs/integration-spec.md before
 * adding a host adapter. Layouts of opaque types stay private.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_H_
#define EIGRPD_EIGRP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum eigrp_result {
	EIGRP_RESULT_SUCCESS = 0,
	EIGRP_RESULT_NOT_IMPLEMENTED,
	EIGRP_RESULT_INVALID_ARGUMENT,
	EIGRP_RESULT_NOT_FOUND,
	EIGRP_RESULT_CONFLICT,
	EIGRP_RESULT_UNSUPPORTED,
	EIGRP_RESULT_INTERNAL_FAILURE,
} eigrp_result_t;

typedef enum eigrp_address_family {
	EIGRP_ADDRESS_FAMILY_IPV4 = 4,
	EIGRP_ADDRESS_FAMILY_IPV6 = 6,
} eigrp_address_family_t;

typedef uint16_t eigrp_topology_id_t;
typedef uint32_t eigrp_vrf_id_t;
typedef uint32_t eigrp_ifindex_t;

#define EIGRP_TOPOLOGY_ID_BASE ((eigrp_topology_id_t)0)
#define EIGRP_VRF_DEFAULT ((eigrp_vrf_id_t)0)

typedef struct eigrp_address {
	eigrp_address_family_t afi;
	uint8_t bytes[16];
} eigrp_address_t;

typedef struct eigrp_prefix {
	eigrp_address_t address;
	uint8_t prefix_length;
} eigrp_prefix_t;

typedef uint64_t eigrp_bandwidth_t;
typedef uint64_t eigrp_delay_t;
typedef uint64_t eigrp_metric_t;
typedef uint32_t eigrp_scaled_t;
typedef uint32_t eigrp_system_metric_t;
typedef uint32_t eigrp_system_delay_t;
typedef uint32_t eigrp_system_bandwidth_t;

typedef struct eigrp_metrics {
	eigrp_delay_t delay;
	eigrp_bandwidth_t bandwidth;
	uint8_t mtu[3];
	uint8_t hop_count;
	uint8_t reliability;
	uint8_t load;
	uint8_t tag;
	uint8_t flags;
} eigrp_metrics_t;

typedef struct eigrp_metric_values {
	uint32_t bandwidth;
	uint32_t delay;
	uint8_t reliability;
	uint8_t load;
	uint16_t mtu;
} eigrp_metric_values_t;

typedef struct eigrp_metric_weights {
	uint8_t tos;
	uint8_t k1;
	uint8_t k2;
	uint8_t k3;
	uint8_t k4;
	uint8_t k5;
	uint8_t k6;
} eigrp_metric_weights_t;

typedef struct eigrp_prefix_limit {
	uint32_t maximum;
	uint8_t threshold;
	bool warning_only;
	bool dampened;
	uint16_t reset_time_minutes;
	uint16_t restart_minutes;
	uint16_t restart_count;
} eigrp_prefix_limit_t;

typedef enum eigrp_offset_direction {
	EIGRP_OFFSET_IN = 0,
	EIGRP_OFFSET_OUT,
} eigrp_offset_direction_t;

typedef enum eigrp_distribute_list_type {
	EIGRP_DISTRIBUTE_ACCESS_LIST = 0,
	EIGRP_DISTRIBUTE_PREFIX_LIST,
} eigrp_distribute_list_type_t;

typedef enum eigrp_filter_decision {
	EIGRP_FILTER_DECISION_PERMIT = 0,
	EIGRP_FILTER_DECISION_DENY,
} eigrp_filter_decision_t;

typedef enum eigrp_redistribute_protocol {
	EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED = 0,
	EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED,
	EIGRP_REDISTRIBUTE_PROTOCOL_STATIC,
	EIGRP_REDISTRIBUTE_PROTOCOL_RIP,
	EIGRP_REDISTRIBUTE_PROTOCOL_OSPF,
	EIGRP_REDISTRIBUTE_PROTOCOL_ISIS,
	EIGRP_REDISTRIBUTE_PROTOCOL_BGP,
	EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP,
} eigrp_redistribute_protocol_t;

typedef uint32_t eigrp_route_instance_t;

typedef struct eigrp_redistribute_source {
	eigrp_redistribute_protocol_t protocol;
	eigrp_route_instance_t route_instance;
} eigrp_redistribute_source_t;

typedef struct eigrp_state_request {
	eigrp_address_family_t afi;
	const char *vrf_name;
	uint16_t asn; /* zero means all configured AS contexts */
	bool all_vrfs;
	/* Cisco selector for EIGRP Multicast Address Family (MAF). */
	bool multicast;
} eigrp_state_request_t;

/* Public opaque identities.  Their layouts remain private to portable EIGRP. */
typedef struct eigrp_instance eigrp_instance_t;
typedef struct eigrp_interface eigrp_interface_t;
typedef struct eigrp_neighbor eigrp_neighbor_t;
typedef struct eigrp_event eigrp_event_t;
typedef struct eigrp_work_queue eigrp_work_queue_t;
typedef struct eigrp_instance_parent_config eigrp_instance_parent_config_t;
typedef struct eigrp_address_family_config eigrp_address_family_config_t;
typedef struct eigrp_interface_config eigrp_interface_config_t;

/* Small public contexts may carry opaque EIGRP identities across contracts. */
typedef struct eigrp_instance_context {
	eigrp_address_family_config_t *config;
	eigrp_instance_t *runtime;
	eigrp_topology_id_t topology_id;
} eigrp_instance_context_t;

typedef enum eigrp_debug_scope {
	EIGRP_DEBUG_SCOPE_TERMINAL = 0,
	EIGRP_DEBUG_SCOPE_CONFIG
} eigrp_debug_scope_t;

typedef enum eigrp_debug_address_family_category {
	EIGRP_DEBUG_AF_ROUTE = 0,
	EIGRP_DEBUG_AF_NEIGHBOR,
	EIGRP_DEBUG_AF_NOTIFICATIONS,
	EIGRP_DEBUG_AF_SUMMARY,
	EIGRP_DEBUG_AF_CATEGORY_MAX
} eigrp_debug_address_family_category_t;

/* Opaque-object identity/capability accessors used by platform adapters. */
eigrp_vrf_id_t eigrp_instance_vrf_id(const eigrp_instance_t *eigrp);
eigrp_address_family_t eigrp_instance_address_family(const eigrp_instance_t *eigrp);
uint16_t eigrp_instance_asn(const eigrp_instance_t *eigrp);
const char *eigrp_instance_name(const eigrp_instance_t *eigrp);
bool eigrp_instance_data_path_ready(const eigrp_instance_t *runtime);
eigrp_ifindex_t eigrp_interface_ifindex(const eigrp_interface_t *ei);
const char *eigrp_interface_name(const eigrp_interface_t *ei);
eigrp_result_t eigrp_interface_address_read(const eigrp_interface_t *ei,
					     eigrp_prefix_t *address);

#endif /* EIGRPD_EIGRP_H_ */
