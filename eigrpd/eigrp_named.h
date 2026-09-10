// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP named-mode configuration ownership.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _EIGRP_NAMED_H_
#define _EIGRP_NAMED_H_

#include <stdbool.h>
#include <stdint.h>

#include "eigrp_result.h"

typedef enum eigrp_address_family {
	EIGRP_ADDRESS_FAMILY_IPV4 = 4,
	EIGRP_ADDRESS_FAMILY_IPV6 = 6,
} eigrp_address_family_t;


typedef struct eigrp_named_address {
	eigrp_address_family_t afi;
	uint8_t bytes[16];
} eigrp_named_address_t;

typedef struct eigrp_named_prefix {
	eigrp_named_address_t address;
	uint8_t prefix_length;
} eigrp_named_prefix_t;

typedef enum eigrp_named_authentication_mode {
	EIGRP_NAMED_AUTHENTICATION_NONE = 0,
	EIGRP_NAMED_AUTHENTICATION_MD5,
	EIGRP_NAMED_AUTHENTICATION_HMAC_SHA256,
} eigrp_named_authentication_mode_t;


typedef enum eigrp_named_offset_direction {
	EIGRP_NAMED_OFFSET_IN = 0,
	EIGRP_NAMED_OFFSET_OUT,
} eigrp_named_offset_direction_t;

typedef struct eigrp_named_metric_values {
	uint32_t bandwidth;
	uint32_t delay;
	uint8_t reliability;
	uint8_t load;
	uint16_t mtu;
} eigrp_named_metric_values_t;

typedef enum eigrp_named_default_information_direction {
	EIGRP_NAMED_DEFAULT_INFORMATION_IN = 0,
	EIGRP_NAMED_DEFAULT_INFORMATION_OUT,
} eigrp_named_default_information_direction_t;

typedef struct eigrp_named_metric_weights {
	uint8_t tos;
	uint8_t k1;
	uint8_t k2;
	uint8_t k3;
	uint8_t k4;
	uint8_t k5;
} eigrp_named_metric_weights_t;

typedef struct eigrp_named_process eigrp_named_process_t;
typedef struct eigrp_named_address_family eigrp_named_address_family_t;
typedef struct eigrp_named_af_interface eigrp_named_af_interface_t;

/* Named parent lifecycle. Names are intentionally case-sensitive. */
eigrp_result_t eigrp_named_process_create(const char *name);
eigrp_result_t eigrp_named_process_delete(const char *name);
eigrp_named_process_t *eigrp_named_process_lookup(const char *name);

/* Address-family configuration lifecycle beneath a named parent. */
eigrp_result_t eigrp_named_address_family_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_address_family_delete(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_named_address_family_t *eigrp_named_address_family_lookup(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);

/* Address-family child configuration targets. */
eigrp_result_t eigrp_named_router_id_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint32_t router_id);
eigrp_result_t eigrp_named_router_id_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_network_add(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_prefix_t *prefix);
eigrp_result_t eigrp_named_network_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_prefix_t *prefix);
eigrp_result_t eigrp_named_neighbor_add(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_address_t *address,
	const char *interface_name);
eigrp_result_t eigrp_named_neighbor_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_address_t *address,
	const char *interface_name);
eigrp_result_t eigrp_named_address_family_shutdown_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, bool shutdown);

/* Named-mode feature targets that are not runtime-complete yet. */
eigrp_result_t eigrp_named_process_shutdown_set(const char *name, bool shutdown);
eigrp_result_t eigrp_named_distance_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint8_t internal_distance, uint8_t external_distance);
eigrp_result_t eigrp_named_offset_list_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *access_list, eigrp_named_offset_direction_t direction,
	uint32_t offset, const char *interface_name);
eigrp_result_t eigrp_named_summary_metric_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_prefix_t *prefix,
	const eigrp_named_metric_values_t *metric);
eigrp_result_t eigrp_named_topology_base_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_topology_base_delete(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_auto_summary_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, bool enabled);
eigrp_result_t eigrp_named_default_information_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, eigrp_named_default_information_direction_t direction,
	bool enabled);
eigrp_result_t eigrp_named_default_metric_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_metric_values_t *metric);
eigrp_result_t eigrp_named_default_metric_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_distance_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_maximum_prefix_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint32_t maximum);
eigrp_result_t eigrp_named_maximum_prefix_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_metric_weights_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_metric_weights_t *weights);
eigrp_result_t eigrp_named_metric_weights_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_offset_list_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *access_list, eigrp_named_offset_direction_t direction,
	uint32_t offset, const char *interface_name);
eigrp_result_t eigrp_named_redistribute_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *protocol,
	const eigrp_named_metric_values_t *metric);
eigrp_result_t eigrp_named_redistribute_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *protocol);
eigrp_result_t eigrp_named_summary_metric_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const eigrp_named_prefix_t *prefix);
eigrp_result_t eigrp_named_active_time_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint16_t seconds);
eigrp_result_t eigrp_named_active_time_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_named_traffic_share_balanced_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, bool enabled);
eigrp_result_t eigrp_named_variance_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, uint8_t variance);
eigrp_result_t eigrp_named_variance_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);

/* Address-family interface configuration targets. */
eigrp_result_t eigrp_named_af_interface_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name);
eigrp_result_t eigrp_named_af_interface_delete(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name);
eigrp_named_af_interface_t *eigrp_named_af_interface_lookup(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name);
eigrp_result_t eigrp_named_af_interface_bandwidth_percent_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, uint32_t percent);
eigrp_result_t eigrp_named_af_interface_bandwidth_percent_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name);
eigrp_result_t eigrp_named_af_interface_hello_interval_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, uint16_t seconds);
eigrp_result_t eigrp_named_af_interface_hello_interval_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name);
eigrp_result_t eigrp_named_af_interface_hold_time_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, uint16_t seconds);
eigrp_result_t eigrp_named_af_interface_hold_time_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name);
eigrp_result_t eigrp_named_af_interface_passive_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, bool passive);
eigrp_result_t eigrp_named_af_interface_authentication_mode_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name,
	eigrp_named_authentication_mode_t mode);
eigrp_result_t eigrp_named_af_interface_authentication_mode_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name);
eigrp_result_t eigrp_named_af_interface_keychain_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, const char *keychain);
eigrp_result_t eigrp_named_af_interface_keychain_clear(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name);
eigrp_result_t eigrp_named_af_interface_next_hop_self_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, bool enabled);
eigrp_result_t eigrp_named_af_interface_split_horizon_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, bool enabled);
eigrp_result_t eigrp_named_af_interface_summary_add(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name,
	const eigrp_named_address_t *address, const eigrp_named_address_t *mask);
eigrp_result_t eigrp_named_af_interface_summary_remove(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name,
	const eigrp_named_address_t *address, const eigrp_named_address_t *mask);
eigrp_result_t eigrp_named_af_interface_shutdown_set(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, const char *interface_name, bool shutdown);

/* Process shutdown cleanup. */
void eigrp_named_finish(void);

#endif /* _EIGRP_NAMED_H_ */
