// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP daemon northbound adapter API.
 * Copyright (C) 2026 Donnie V. Savage
 */

#ifndef _FRR_EIGRP_NORTHBOUND_H_
#define _FRR_EIGRP_NORTHBOUND_H_

#include <stdbool.h>
#include <stddef.h>
#include <netinet/in.h>

#include "eigrp_cli.h"

eigrp_result_t eigrp_northbound_neighbor_clear_address(
	eigrp_instance_t *runtime, eigrp_afi_t afi,
	const struct in_addr *ipv4_address,
	const struct in6_addr *ipv6_address, bool soft,
	eigrp_nbr_clear_cb callback, void *arg, size_t *affected_count);


/* FRR northbound/YANG integration declarations shared by the FRR adapter. */
struct frr_yang_module_info;
extern const struct frr_yang_module_info frr_eigrpd_info;

struct interface;
struct lyd_node;
struct in_addr;
struct in6_addr;
struct nb_cb_create_args;
struct nb_cb_destroy_args;
struct nb_cb_modify_args;

bool eigrp_northbound_ipv4_neighbor_address_copy(eigrp_address_t *destination,
                                                  const struct in_addr *address);
bool eigrp_northbound_ipv6_neighbor_address_copy(eigrp_address_t *destination,
                                                  const struct in6_addr *address);

int eigrpd_instance_create(struct nb_cb_create_args *args);
int eigrpd_instance_destroy(struct nb_cb_destroy_args *args);
int eigrpd_instance_router_id_modify(struct nb_cb_modify_args *args);
int eigrpd_instance_router_id_destroy(struct nb_cb_destroy_args *args);
int
eigrpd_instance_passive_interface_create(struct nb_cb_create_args *args);
int
eigrpd_instance_passive_interface_destroy(struct nb_cb_destroy_args *args);
int eigrpd_instance_active_time_modify(struct nb_cb_modify_args *args);
int eigrpd_instance_variance_modify(struct nb_cb_modify_args *args);
int eigrpd_instance_variance_destroy(struct nb_cb_destroy_args *args);
int eigrpd_instance_maximum_paths_modify(struct nb_cb_modify_args *args);
int
eigrpd_instance_maximum_paths_destroy(struct nb_cb_destroy_args *args);
int eigrpd_instance_event_log_size_modify(struct nb_cb_modify_args *args);
int eigrpd_instance_event_log_size_destroy(
	struct nb_cb_destroy_args *args);
int
eigrpd_instance_metric_weights_K1_modify(struct nb_cb_modify_args *args);
int
eigrpd_instance_metric_weights_K1_destroy(struct nb_cb_destroy_args *args);
int
eigrpd_instance_metric_weights_K2_modify(struct nb_cb_modify_args *args);
int
eigrpd_instance_metric_weights_K2_destroy(struct nb_cb_destroy_args *args);
int
eigrpd_instance_metric_weights_K3_modify(struct nb_cb_modify_args *args);
int
eigrpd_instance_metric_weights_K3_destroy(struct nb_cb_destroy_args *args);
int
eigrpd_instance_metric_weights_K4_modify(struct nb_cb_modify_args *args);
int
eigrpd_instance_metric_weights_K4_destroy(struct nb_cb_destroy_args *args);
int
eigrpd_instance_metric_weights_K5_modify(struct nb_cb_modify_args *args);
int
eigrpd_instance_metric_weights_K5_destroy(struct nb_cb_destroy_args *args);
int
eigrpd_instance_metric_weights_K6_modify(struct nb_cb_modify_args *args);
int
eigrpd_instance_metric_weights_K6_destroy(struct nb_cb_destroy_args *args);
int eigrpd_instance_network_create(struct nb_cb_create_args *args);
int eigrpd_instance_network_destroy(struct nb_cb_destroy_args *args);
int eigrpd_instance_neighbor_create(struct nb_cb_create_args *args);
int eigrpd_instance_neighbor_destroy(struct nb_cb_destroy_args *args);
int eigrp_northbound_distribute_list_create(
	struct nb_cb_create_args *args);
int eigrpd_instance_redistribute_create(struct nb_cb_create_args *args);
int eigrpd_instance_redistribute_destroy(struct nb_cb_destroy_args *args);
int
eigrpd_instance_redistribute_route_map_modify(struct nb_cb_modify_args *args);
int
eigrpd_instance_redistribute_route_map_destroy(struct nb_cb_destroy_args *args);
int eigrpd_instance_redistribute_metrics_bandwidth_modify(
	struct nb_cb_modify_args *args);
int eigrpd_instance_redistribute_metrics_bandwidth_destroy(
	struct nb_cb_destroy_args *args);
int eigrpd_instance_redistribute_metrics_delay_modify(
	struct nb_cb_modify_args *args);
int eigrpd_instance_redistribute_metrics_delay_destroy(
	struct nb_cb_destroy_args *args);
int eigrpd_instance_redistribute_metrics_reliability_modify(
	struct nb_cb_modify_args *args);
int eigrpd_instance_redistribute_metrics_reliability_destroy(
	struct nb_cb_destroy_args *args);
int
eigrpd_instance_redistribute_metrics_load_modify(struct nb_cb_modify_args *args);
int eigrpd_instance_redistribute_metrics_load_destroy(
	struct nb_cb_destroy_args *args);
int
eigrpd_instance_redistribute_metrics_mtu_modify(struct nb_cb_modify_args *args);
int eigrpd_instance_redistribute_metrics_mtu_destroy(
	struct nb_cb_destroy_args *args);
int lib_interface_eigrp_delay_modify(struct nb_cb_modify_args *args);
int lib_interface_eigrp_bandwidth_modify(struct nb_cb_modify_args *args);
int
lib_interface_eigrp_hello_interval_modify(struct nb_cb_modify_args *args);
int lib_interface_eigrp_hold_time_modify(struct nb_cb_modify_args *args);
int
lib_interface_eigrp_split_horizon_modify(struct nb_cb_modify_args *args);
int lib_interface_eigrp_instance_create(struct nb_cb_create_args *args);
int lib_interface_eigrp_instance_destroy(struct nb_cb_destroy_args *args);
int lib_interface_eigrp_instance_summarize_addresses_create(
	struct nb_cb_create_args *args);
int lib_interface_eigrp_instance_summarize_addresses_destroy(
	struct nb_cb_destroy_args *args);
int lib_interface_eigrp_instance_authentication_modify(
	struct nb_cb_modify_args *args);
int
lib_interface_eigrp_instance_keychain_modify(struct nb_cb_modify_args *args);
int
lib_interface_eigrp_instance_keychain_destroy(struct nb_cb_destroy_args *args);

#endif /* _FRR_EIGRP_NORTHBOUND_H_ */
