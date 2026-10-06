// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP instance and address-family configuration ownership.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_INSTANCE_H_
#define EIGRPD_EIGRP_INSTANCE_H_

#include "eigrp_types.h"
#include "eigrp_cli.h"

/*
 * A named CLI parent is only a configuration container.  The address-family
 * object beneath it is the protocol configuration context consumed by the
 * semantic EIGRP modules.
 */
struct eigrp_named_config {
	char *name;
	eigrp_virt_router_t *runtime;
	bool shutdown;
	eigrp_af_config_t *address_families;
	eigrp_named_config_t *next;
};

struct eigrp_af_config {
	eigrp_afi_t afi;
	eigrp_af_vectors_t af_vectors;
	uint16_t asn;
	char *vrf_name;
	/* Runtime binding owned by this named address-family context. */
	eigrp_instance_t *runtime;
	bool router_id_configured;
	uint32_t router_id;
	bool shutdown;
	bool event_log_size_configured;
	uint32_t event_log_size;
	eigrp_network_config_t *networks;
	eigrp_nbr_config_t *neighbors;
	eigrp_intf_config_t *interfaces;
	eigrp_redist_config_t *redistributions;
	eigrp_distribute_list_config_t *distribute_lists;
	eigrp_offset_config_t *offsets;
	eigrp_metric_config_t *metric_config;
	eigrp_summary_state_t *summary_state;
	eigrp_timer_config_t *timer_config;
	eigrp_nbr_policy_state_t *neighbor_policy;
	eigrp_redist_policy_config_t *redistribute_policy;
	bool topology_base_configured;
	bool default_information_enabled[2];
	char *default_information_access_list[2];
	bool topology_maximum_prefix_configured;
	eigrp_prefix_limit_t topology_maximum_prefix;
	eigrp_af_config_t *next;
};

typedef eigrp_result_t (*eigrp_af_config_iterate_cb)(
	const char *instance_name, eigrp_af_config_t *af, void *arg);
typedef eigrp_result_t (*eigrp_instance_vrf_iterate_cb)(
	eigrp_vrf_id_t vrf_id, void *arg);

eigrp_virt_router_t *eigrp_virt_router_create(const char *name, eigrp_vrid_t vrid);
eigrp_virt_router_t *eigrp_virt_router_lookup(const char *name);
eigrp_result_t eigrp_virt_router_delete(eigrp_virt_router_t *virt_router);

void eigrp_instance_delete_final(eigrp_instance_t *instance);
bool eigrp_instance_thread_start(eigrp_instance_t *instance);
void eigrp_instance_thread_stop(eigrp_instance_t *instance);
void eigrp_router_id_update(eigrp_instance_t *instance);
bool eigrp_instance_event_enqueue(eigrp_instance_t *instance,
	eigrp_af_event_type_t type);
bool eigrp_instance_intf_update_event_enqueue(eigrp_instance_t *instance,
	const eigrp_intf_runtime_state_t *state);
bool eigrp_instance_intf_down_event_enqueue(eigrp_instance_t *instance,
	eigrp_ifindex_t ifindex, uint8_t type, uint32_t bandwidth, uint32_t mtu);
bool eigrp_instance_intf_remove_event_enqueue(eigrp_instance_t *instance,
	eigrp_ifindex_t ifindex, eigrp_intf_remove_reason_t reason);
bool eigrp_instance_intf_addr_update_event_enqueue(eigrp_instance_t *instance,
	eigrp_ifindex_t ifindex, const eigrp_prefix_t *address,
	eigrp_intf_remove_reason_t reason);
eigrp_result_t eigrp_instance_rib_event_enqueue(eigrp_instance_t *instance,
	eigrp_rib_event_type_t type, const eigrp_rib_route_t *route);
void eigrp_instance_event_process(eigrp_instance_t *instance,
	eigrp_af_event_t *event);
void eigrp_process_routerid_cb(eigrp_vrf_id_t vrf_id);

eigrp_result_t eigrp_instance_classic_validate(
	uint16_t asn, eigrp_vrf_id_t vrf_id, const char **owner_name);
eigrp_result_t eigrp_instance_classic_create(
	uint16_t asn, eigrp_vrf_id_t vrf_id, eigrp_instance_t **runtime);
eigrp_instance_t *eigrp_instance_classic_read(uint16_t asn,
					eigrp_vrf_id_t vrf_id);
eigrp_result_t eigrp_instance_classic_delete(eigrp_instance_t *runtime);

eigrp_result_t eigrp_named_config_create(const char *name);
eigrp_named_config_t *eigrp_named_config_read(const char *name);
eigrp_result_t eigrp_named_config_delete(const char *name);

eigrp_result_t eigrp_af_config_create(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_af_config_t *eigrp_af_config_read(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_af_config_context_read(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn, eigrp_instance_context_t *context);
eigrp_result_t eigrp_af_config_delete(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_af_config_iterate(
	const eigrp_state_request_t *request,
	eigrp_af_config_iterate_cb callback, void *arg);
eigrp_result_t eigrp_instance_vrf_iterate(
	eigrp_instance_vrf_iterate_cb callback, void *arg);

/* Clear any named address-family binding to a runtime being destroyed. */
void eigrp_af_config_runtime_remove(eigrp_instance_t *runtime);
eigrp_af_config_t *eigrp_af_config_runtime_read(eigrp_instance_t *runtime);

eigrp_result_t eigrp_instance_router_id_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint32_t router_id);
eigrp_result_t eigrp_af_config_shutdown_update(eigrp_operation_t operation,
	eigrp_af_config_t *af);
eigrp_result_t eigrp_instance_start(eigrp_instance_t *runtime);
eigrp_result_t eigrp_instance_stop(eigrp_instance_t *runtime);

eigrp_result_t eigrp_named_config_shutdown_update(eigrp_operation_t operation,
	eigrp_named_config_t *parent);
eigrp_result_t eigrp_af_config_distance_update(eigrp_operation_t operation,
	eigrp_af_config_t *af,
	uint8_t internal_distance,
	uint8_t external_distance);

void eigrp_named_config_delete_all(void);

#endif /* EIGRPD_EIGRP_INSTANCE_H_ */
