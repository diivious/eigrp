// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Public EIGRP semantic configuration and administrative API.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_CLI_H_
#define EIGRPD_EIGRP_CLI_H_

#include "eigrpd/eigrp.h"

#define EIGRP_INTERFACE_BANDWIDTH_MIN 1U
#define EIGRP_INTERFACE_BANDWIDTH_MAX 10000000U
#define EIGRP_INTERFACE_DELAY_MIN 1U
#define EIGRP_INTERFACE_DELAY_MAX 16777215U

typedef struct eigrp_intf_context {
	eigrp_af_instance_t *address_family;
	eigrp_intf_config_t *config;
	eigrp_intf_t *runtime;
} eigrp_intf_context_t;

typedef enum eigrp_authentication_mode {
	EIGRP_AUTHENTICATION_NONE = 0,
	EIGRP_AUTHENTICATION_MD5,
	EIGRP_AUTHENTICATION_HMAC_SHA256,
} eigrp_authentication_mode_t;

typedef struct eigrp_auth_hmac_config {
	uint8_t encryption_type;
	const char *password;
} eigrp_auth_hmac_config_t;

typedef struct eigrp_summary_options {
	uint8_t administrative_distance;
	const char *leak_map;
} eigrp_summary_options_t;

typedef struct eigrp_summary_metric_config {
	bool metric_configured;
	eigrp_metric_values_t metric;
	bool distance_configured;
	uint8_t distance;
} eigrp_summary_metric_config_t;

typedef enum eigrp_nbr_log_type {
	EIGRP_NEIGHBOR_LOG_CHANGES = 0,
	EIGRP_NEIGHBOR_LOG_WARNINGS
} eigrp_nbr_log_type_t;

typedef enum eigrp_default_information_direction {
	EIGRP_DEFAULT_INFORMATION_IN = 0,
	EIGRP_DEFAULT_INFORMATION_OUT,
} eigrp_default_information_direction_t;

typedef struct eigrp_nbr_clear_request {
	const char *interface_name;
	const eigrp_address_t *address;
	bool soft;
} eigrp_nbr_clear_request_t;

typedef struct eigrp_nbr_clear_state {
	eigrp_address_t address;
	const char *interface_name;
	bool soft;
} eigrp_nbr_clear_state_t;

typedef void (*eigrp_nbr_clear_cb)(
	const eigrp_nbr_clear_state_t *state, void *arg);

typedef struct eigrp_topology_clear_request {
	const eigrp_prefix_t *destination;
} eigrp_topology_clear_request_t;

/* Instance/address-family lifecycle and configuration. */
eigrp_result_t eigrp_instance_classic_validate(
	uint16_t asn, eigrp_vrf_id_t vrf_id, const char **owner_name);
eigrp_result_t eigrp_instance_classic_create(
	uint16_t asn, eigrp_vrf_id_t vrf_id, eigrp_instance_t **runtime);
eigrp_instance_t *eigrp_instance_classic_read(uint16_t asn,
					       eigrp_vrf_id_t vrf_id);
eigrp_result_t eigrp_instance_classic_delete(eigrp_instance_t *runtime);
eigrp_result_t eigrp_instance_parent_create(const char *name);
eigrp_instance_parent_config_t *eigrp_instance_parent_read(const char *name);
eigrp_result_t eigrp_instance_parent_delete(const char *name);
eigrp_result_t eigrp_af_instance_create(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_af_instance_t *eigrp_af_instance_read(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_af_instance_delete(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_instance_router_id_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint32_t router_id);
eigrp_result_t eigrp_af_instance_shutdown_update(eigrp_operation_t operation,
	eigrp_af_instance_t *af);
eigrp_result_t eigrp_instance_parent_shutdown_update(eigrp_operation_t operation,
	eigrp_instance_parent_config_t *parent);
eigrp_result_t eigrp_instance_distance_update(eigrp_operation_t operation,
	eigrp_af_instance_t *af,
	uint8_t internal_distance,
	uint8_t external_distance);

/* Network participation. */
eigrp_result_t eigrp_network_create(eigrp_instance_context_t *context,
				    const eigrp_prefix_t *prefix);
eigrp_result_t eigrp_network_delete(eigrp_instance_context_t *context,
				    const eigrp_prefix_t *prefix);
eigrp_result_t eigrp_network_runtime_exists(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix, bool *exists);

/* Interface configuration. */
eigrp_result_t eigrp_intf_config_create(eigrp_af_instance_t *af,
					      const char *interface_name);
eigrp_intf_config_t *eigrp_intf_config_read(
	eigrp_af_instance_t *af, const char *interface_name);
eigrp_result_t eigrp_intf_config_delete(eigrp_af_instance_t *af,
					      const char *interface_name);
eigrp_intf_t *eigrp_intf_runtime_lookup(
	eigrp_instance_t *runtime, const char *interface_name);
eigrp_result_t eigrp_intf_bandwidth_percent_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context,
	uint32_t percent);
eigrp_result_t eigrp_intf_bandwidth_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context,
	uint32_t bandwidth);
eigrp_result_t eigrp_intf_delay_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context,
	uint32_t delay);
eigrp_result_t eigrp_intf_hello_interval_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context,
	uint16_t seconds);
eigrp_result_t eigrp_intf_hold_time_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context,
	uint16_t seconds);
eigrp_result_t eigrp_intf_passive_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context);
eigrp_result_t eigrp_intf_nexthop_self_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context);
eigrp_result_t eigrp_intf_split_horizon_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context);
eigrp_result_t eigrp_intf_shutdown_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context);

/* Neighbor configuration and operational clear. */
eigrp_result_t eigrp_nbr_static_create(eigrp_af_instance_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name);
eigrp_result_t eigrp_nbr_static_delete(eigrp_af_instance_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name);
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
eigrp_result_t eigrp_nbr_clear(
	eigrp_instance_t *runtime, const eigrp_nbr_clear_request_t *request,
	eigrp_nbr_clear_cb callback, void *arg, size_t *affected_count);

/* Authentication. */
eigrp_result_t eigrp_auth_mode_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context,
	eigrp_authentication_mode_t mode,
	const eigrp_auth_hmac_config_t *hmac);
eigrp_result_t eigrp_auth_keychain_update(eigrp_operation_t operation,
	eigrp_intf_context_t *context,
	const char *keychain);

/* Summaries. */
eigrp_result_t eigrp_summary_create(
	eigrp_intf_context_t *context, const eigrp_prefix_t *prefix,
	const eigrp_summary_options_t *options);
eigrp_result_t eigrp_summary_delete(
	eigrp_intf_context_t *context, const eigrp_prefix_t *prefix);
eigrp_result_t eigrp_summary_auto_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_summary_metric_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_prefix_t *prefix,
	const eigrp_summary_metric_config_t *config);

/* Metrics and topology controls. */
eigrp_result_t eigrp_metric_default_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_metric_values_t *metric);
eigrp_result_t eigrp_metric_weights_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_metric_weights_t *weights);
eigrp_result_t eigrp_metric_variance_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint8_t variance);
eigrp_result_t eigrp_traffic_share_balanced_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_maximum_hops_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint8_t maximum_hops);
eigrp_result_t eigrp_metric_holddown_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	bool enabled);
eigrp_result_t eigrp_metric_version_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_topology_create(eigrp_instance_context_t *context);
eigrp_result_t eigrp_topology_delete(eigrp_instance_context_t *context);
eigrp_result_t eigrp_topology_default_information_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	eigrp_default_information_direction_t direction,
	const char *access_list);
eigrp_result_t eigrp_topology_max_prefix_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_prefix_limit_t *limit);
eigrp_result_t eigrp_topology_maximum_paths_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint8_t maximum_paths);
eigrp_result_t eigrp_topology_clear(
	eigrp_instance_context_t *context,
	const eigrp_topology_clear_request_t *request, size_t *affected_count);

/* Filtering and redistribution. */
eigrp_result_t eigrp_offset_add(eigrp_instance_context_t *context,
				   const char *access_list,
				   eigrp_offset_direction_t direction,
				   uint32_t offset,
				   const char *interface_name);
eigrp_result_t eigrp_offset_remove(eigrp_instance_context_t *context,
				   const char *access_list,
				   eigrp_offset_direction_t direction,
				   uint32_t offset,
				   const char *interface_name);
eigrp_result_t eigrp_distribute_add(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name);
eigrp_result_t eigrp_distribute_remove(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name);
eigrp_result_t eigrp_redist_add(
	eigrp_instance_context_t *context, const eigrp_redist_source_t *source,
	const eigrp_metric_values_t *metric, const char *route_map);
eigrp_result_t eigrp_redist_remove(
	eigrp_instance_context_t *context, const eigrp_redist_source_t *source);
eigrp_result_t eigrp_redist_max_prefix_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	const eigrp_prefix_limit_t *limit);

/* Timers and event log administrative operations. */
eigrp_result_t eigrp_timer_active_time_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint16_t seconds);
eigrp_result_t eigrp_eventlog_clear(eigrp_instance_context_t *context);
eigrp_result_t eigrp_eventlog_size_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint32_t size);

/* Public debug selection controls. */
typedef enum eigrp_debug_packet_category {
	EIGRP_DEBUG_PACKET_UPDATE = 0,
	EIGRP_DEBUG_PACKET_REQUEST,
	EIGRP_DEBUG_PACKET_QUERY,
	EIGRP_DEBUG_PACKET_REPLY,
	EIGRP_DEBUG_PACKET_HELLO,
	EIGRP_DEBUG_PACKET_PROBE,
	EIGRP_DEBUG_PACKET_ACK,
	EIGRP_DEBUG_PACKET_RETRY,
	EIGRP_DEBUG_PACKET_SIAQUERY,
	EIGRP_DEBUG_PACKET_SIAREPLY,
	EIGRP_DEBUG_PACKET_CATEGORY_MAX
} eigrp_debug_packet_category_t;

typedef enum eigrp_debug_target {
	EIGRP_DEBUG_TARGET_GENERAL = 0,
	EIGRP_DEBUG_TARGET_NEIGHBOR,
	EIGRP_DEBUG_TARGET_NOTIFICATIONS,
	EIGRP_DEBUG_TARGET_TRANSMIT
} eigrp_debug_target_t;

#define EIGRP_DEBUG_EVENT 0x01
#define EIGRP_DEBUG_DETAIL 0x02
#define EIGRP_DEBUG_TIMERS 0x04
#define EIGRP_DEBUG_FSM 0x08
#define EIGRP_DEBUG_NSF 0x10
#define EIGRP_DEBUG_FAST_REROUTE 0x20
#define EIGRP_DEBUG_NEI 0x01
#define EIGRP_DEBUG_NEI_SIATIMER 0x02
#define EIGRP_DEBUG_NEI_STATIC 0x04
#define EIGRP_DEBUG_NEI_ALL (EIGRP_DEBUG_NEI | EIGRP_DEBUG_NEI_SIATIMER | EIGRP_DEBUG_NEI_STATIC)
#define EIGRP_DEBUG_UPDATE (1U << EIGRP_DEBUG_PACKET_UPDATE)
#define EIGRP_DEBUG_REQUEST (1U << EIGRP_DEBUG_PACKET_REQUEST)
#define EIGRP_DEBUG_QUERY (1U << EIGRP_DEBUG_PACKET_QUERY)
#define EIGRP_DEBUG_REPLY (1U << EIGRP_DEBUG_PACKET_REPLY)
#define EIGRP_DEBUG_HELLO (1U << EIGRP_DEBUG_PACKET_HELLO)
#define EIGRP_DEBUG_PROBE (1U << EIGRP_DEBUG_PACKET_PROBE)
#define EIGRP_DEBUG_ACK (1U << EIGRP_DEBUG_PACKET_ACK)
#define EIGRP_DEBUG_RETRY (1U << EIGRP_DEBUG_PACKET_RETRY)
#define EIGRP_DEBUG_SIAQUERY (1U << EIGRP_DEBUG_PACKET_SIAQUERY)
#define EIGRP_DEBUG_SIAREPLY (1U << EIGRP_DEBUG_PACKET_SIAREPLY)
#define EIGRP_DEBUG_PACKETS_ALL (EIGRP_DEBUG_UPDATE | EIGRP_DEBUG_REQUEST | EIGRP_DEBUG_QUERY | EIGRP_DEBUG_REPLY | EIGRP_DEBUG_HELLO | EIGRP_DEBUG_PROBE | EIGRP_DEBUG_ACK | EIGRP_DEBUG_SIAQUERY | EIGRP_DEBUG_SIAREPLY)
#define EIGRP_DEBUG_PACKETS_TERSE (EIGRP_DEBUG_PACKETS_ALL & ~EIGRP_DEBUG_HELLO)
#define EIGRP_DEBUG_PACKET_VALID_MASK (EIGRP_DEBUG_PACKETS_ALL | EIGRP_DEBUG_RETRY)
#define EIGRP_DEBUG_SEND 0x01
#define EIGRP_DEBUG_RECV 0x02
#define EIGRP_DEBUG_SEND_RECV 0x03
#define EIGRP_DEBUG_PACKET_DETAIL 0x04
#define EIGRP_DEBUG_PACKET_FLAG_MASK (EIGRP_DEBUG_SEND_RECV | EIGRP_DEBUG_PACKET_DETAIL)
#define EIGRP_DEBUG_TRANSMIT_ACK 0x0001
#define EIGRP_DEBUG_TRANSMIT_BUILD 0x0002
#define EIGRP_DEBUG_TRANSMIT_DETAIL 0x0004
#define EIGRP_DEBUG_TRANSMIT_LINK 0x0008
#define EIGRP_DEBUG_TRANSMIT_PACKETIZE 0x0010
#define EIGRP_DEBUG_TRANSMIT_PEERDOWN 0x0020
#define EIGRP_DEBUG_TRANSMIT_SIA 0x0040
#define EIGRP_DEBUG_TRANSMIT_STARTUP 0x0080
#define EIGRP_DEBUG_TRANSMIT_STRANGE 0x0100
#define EIGRP_DEBUG_TRANSMIT_ALL (EIGRP_DEBUG_TRANSMIT_ACK | EIGRP_DEBUG_TRANSMIT_BUILD | EIGRP_DEBUG_TRANSMIT_DETAIL | EIGRP_DEBUG_TRANSMIT_LINK | EIGRP_DEBUG_TRANSMIT_PACKETIZE | EIGRP_DEBUG_TRANSMIT_PEERDOWN | EIGRP_DEBUG_TRANSMIT_SIA | EIGRP_DEBUG_TRANSMIT_STARTUP | EIGRP_DEBUG_TRANSMIT_STRANGE)
#define EIGRP_DEBUG_NOTIFICATION_INTERFACE 0x01
#define EIGRP_DEBUG_NOTIFICATION_RIB 0x02
#define EIGRP_DEBUG_NOTIFICATIONS 0x03

eigrp_result_t eigrp_debug_update(eigrp_operation_t operation,
	eigrp_debug_target_t target,
	unsigned long flags,
	eigrp_debug_scope_t scope);
eigrp_result_t eigrp_debug_af_update(eigrp_operation_t operation,
	const eigrp_state_request_t *request,
	eigrp_debug_af_category_t category,
	const eigrp_address_t *neighbor,
	eigrp_debug_scope_t scope);
eigrp_result_t eigrp_debug_packet_update(eigrp_operation_t operation,
	uint32_t packet_mask,
	unsigned long flags,
	eigrp_debug_scope_t scope);
const char *eigrp_debug_packet_category_name(eigrp_debug_packet_category_t category);
const char *eigrp_debug_packet_category_cli_name(eigrp_debug_packet_category_t category);

#endif /* EIGRPD_EIGRP_CLI_H_ */
