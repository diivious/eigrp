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

/* Common configuration/state mutation opcodes. */
typedef enum eigrp_operation {
	EIGRP_SET = 0,
	EIGRP_RESET,
} eigrp_operation_t;

typedef enum eigrp_afi {
	EIGRP_AFI_IPV4 = 4,
	EIGRP_AFI_IPV6 = 6,
} eigrp_afi_t;

typedef uint16_t eigrp_topology_id_t;
typedef uint16_t eigrp_vrid_t;
typedef uint32_t eigrp_vrf_id_t;
typedef uint32_t eigrp_ifindex_t;

#define EIGRP_TOPOLOGY_ID_BASE ((eigrp_topology_id_t)0)
#define EIGRP_VRF_DEFAULT ((eigrp_vrf_id_t)0)

typedef struct eigrp_address {
	eigrp_afi_t afi;
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

static inline bool eigrp_prefix_limit_runtime_supported(
	const eigrp_prefix_limit_t *limit)
{
	return limit && !limit->dampened && !limit->reset_time_minutes
	       && !limit->restart_minutes && !limit->restart_count;
}

static inline bool eigrp_prefix_limit_allows(const eigrp_prefix_limit_t *limit,
	uint32_t current, bool already_present)
{
	if (!limit || already_present || limit->warning_only
	    || !eigrp_prefix_limit_runtime_supported(limit))
		return true;
	return current < limit->maximum;
}

static inline bool eigrp_prefix_limit_threshold_crossed(
	const eigrp_prefix_limit_t *limit, uint32_t current)
{
	uint64_t before;
	uint64_t after;
	uint64_t boundary;

	if (!limit || !limit->threshold || current >= limit->maximum)
		return false;
	before = (uint64_t)current * 100U;
	after = (uint64_t)(current + 1U) * 100U;
	boundary = (uint64_t)limit->maximum * limit->threshold;
	return before < boundary && after >= boundary;
}


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

typedef enum eigrp_redist_protocol {
	EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED = 0,
	EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED,
	EIGRP_REDISTRIBUTE_PROTOCOL_STATIC,
	EIGRP_REDISTRIBUTE_PROTOCOL_RIP,
	EIGRP_REDISTRIBUTE_PROTOCOL_OSPF,
	EIGRP_REDISTRIBUTE_PROTOCOL_ISIS,
	EIGRP_REDISTRIBUTE_PROTOCOL_BGP,
	EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP,
} eigrp_redist_protocol_t;

typedef uint32_t eigrp_route_instance_t;

typedef struct eigrp_redist_source {
	eigrp_redist_protocol_t protocol;
	eigrp_route_instance_t route_instance;
} eigrp_redist_source_t;

typedef struct eigrp_state_request {
	eigrp_afi_t afi;
	const char *vrf_name;
	uint16_t asn; /* zero means all configured AS contexts */
	bool all_vrfs;
	/* Multicast address-family selector; current FRR/BIRD CLIs do not expose it. */
	bool multicast;
} eigrp_state_request_t;

/* Public opaque identities.  Their layouts remain private to portable EIGRP. */
typedef struct eigrp_process eigrp_process_t;
typedef struct eigrp_virt_router eigrp_virt_router_t;
typedef struct eigrp_instance eigrp_instance_t;
typedef struct eigrp_interface eigrp_intf_t;
typedef struct eigrp_neighbor eigrp_nbr_t;
typedef struct eigrp_event eigrp_event_t;
typedef struct eigrp_work_queue eigrp_work_queue_t;
typedef struct eigrp_named_config eigrp_named_config_t;
typedef struct eigrp_af_config eigrp_af_config_t;
typedef struct eigrp_intf_config eigrp_intf_config_t;

/* Small public contexts may carry opaque EIGRP identities across contracts. */
typedef struct eigrp_instance_context {
	eigrp_af_config_t *config;
	eigrp_instance_t *runtime;
	eigrp_topology_id_t topology_id;
} eigrp_instance_context_t;

typedef enum eigrp_debug_scope {
	EIGRP_DEBUG_SCOPE_TERMINAL = 0,
	EIGRP_DEBUG_SCOPE_CONFIG
} eigrp_debug_scope_t;

typedef enum eigrp_debug_af_category {
	EIGRP_DEBUG_AF_ROUTE = 0,
	EIGRP_DEBUG_AF_NEIGHBOR,
	EIGRP_DEBUG_AF_NOTIFICATIONS,
	EIGRP_DEBUG_AF_SUMMARY,
	EIGRP_DEBUG_AF_CATEGORY_MAX
} eigrp_debug_af_category_t;


/* EIGRP library lifecycle and runtime instance access. */
typedef eigrp_result_t (*eigrp_instance_iterate_cb)(eigrp_instance_t *instance,
	void *arg);

void eigrp_init(void);
void eigrp_terminate(void);
eigrp_instance_t *eigrp_instance_create(eigrp_virt_router_t *virt_router,
	eigrp_afi_t afi, uint16_t as, eigrp_vrf_id_t vrf_id);
void eigrp_instance_delete(eigrp_instance_t *instance);
eigrp_instance_t *eigrp_lookup(eigrp_vrf_id_t vrf_id);
eigrp_instance_t *eigrp_lookup_by_as_vrf(uint16_t as, eigrp_vrf_id_t vrf_id);
eigrp_instance_t *eigrp_lookup_by_af_as_vrf(eigrp_afi_t afi, uint16_t as,
	eigrp_vrf_id_t vrf_id);
eigrp_result_t eigrp_instance_iterate(eigrp_instance_iterate_cb callback,
	void *arg);

/* Opaque-object identity/capability accessors used by platform adapters. */
eigrp_vrf_id_t eigrp_instance_vrf_id(const eigrp_instance_t *eigrp);
eigrp_afi_t eigrp_instance_afi(const eigrp_instance_t *eigrp);
uint16_t eigrp_instance_asn(const eigrp_instance_t *eigrp);
const char *eigrp_instance_name(const eigrp_instance_t *eigrp);
eigrp_ifindex_t eigrp_intf_ifindex(const eigrp_intf_t *ei);
const char *eigrp_intf_name(const eigrp_intf_t *ei);
eigrp_result_t eigrp_intf_address_read(const eigrp_intf_t *ei,
					     eigrp_prefix_t *address);

#endif /* EIGRPD_EIGRP_H_ */
