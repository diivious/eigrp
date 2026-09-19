// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP instance and address-family configuration ownership.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_INSTANCE_H_
#define EIGRPD_EIGRP_INSTANCE_H_

#include "eigrp_types.h"
#include "eigrp_result.h"

/*
 * A named CLI parent is only a configuration container.  The address-family
 * object beneath it is the protocol configuration context consumed by the
 * semantic EIGRP modules.
 */
struct eigrp_instance_parent_config {
	char *name;
	eigrp_address_family_config_t *address_families;
	eigrp_instance_parent_config_t *next;
};

struct eigrp_address_family_config {
	eigrp_address_family_t afi;
	eigrp_af_vectors_t af_vectors;
	uint16_t asn;
	char *vrf_name;
	/*
	 * Runtime/control binding owned by this named address-family context.
	 * IPv6 has a real control context while runtime->data_path_ready remains
	 * false until packet/socket support is implemented.
	 */
	eigrp_instance_t *runtime;
	bool router_id_configured;
	uint32_t router_id;
	bool shutdown;
	bool event_log_size_configured;
	uint32_t event_log_size;
	eigrp_network_config_t *networks;
	eigrp_neighbor_config_t *neighbors;
	eigrp_interface_config_t *interfaces;
	eigrp_redistribute_config_t *redistributions;
	eigrp_distribute_list_config_t *distribute_lists;
	eigrp_offset_config_t *offsets;
	eigrp_metric_config_t *metric_config;
	eigrp_summary_state_t *summary_state;
	eigrp_timer_config_t *timer_config;
	eigrp_neighbor_policy_state_t *neighbor_policy;
	eigrp_redistribute_policy_config_t *redistribute_policy;
	eigrp_address_family_config_t *next;
};

typedef struct eigrp_instance_context {
	eigrp_address_family_config_t *config;
	eigrp_instance_t *runtime;
	eigrp_topology_id_t topology_id;
} eigrp_instance_context_t;

typedef eigrp_result_t (*eigrp_instance_address_family_walk_cb)(
	const char *instance_name, eigrp_address_family_config_t *af, void *arg);

eigrp_result_t eigrp_instance_parent_create(const char *name);
eigrp_instance_parent_config_t *eigrp_instance_parent_read(const char *name);
eigrp_result_t eigrp_instance_parent_delete(const char *name);

eigrp_result_t eigrp_instance_address_family_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_address_family_config_t *eigrp_instance_address_family_read(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_instance_address_family_delete(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn);
eigrp_result_t eigrp_instance_address_family_walk(
	const eigrp_state_request_t *request,
	eigrp_instance_address_family_walk_cb callback, void *arg);

/* Clear any named address-family binding to a runtime being destroyed. */
void eigrp_instance_runtime_unbind(eigrp_instance_t *runtime);
eigrp_address_family_config_t *eigrp_instance_runtime_config(eigrp_instance_t *runtime);

eigrp_result_t eigrp_instance_router_id_update(eigrp_instance_context_t *context,
					       uint32_t router_id);
eigrp_result_t eigrp_instance_router_id_delete(eigrp_instance_context_t *context);
eigrp_result_t eigrp_instance_address_family_shutdown_update(
	eigrp_address_family_config_t *af, bool shutdown);

eigrp_result_t eigrp_instance_parent_shutdown_update(
	eigrp_instance_parent_config_t *parent, bool shutdown);
eigrp_result_t eigrp_instance_distance_update(eigrp_address_family_config_t *af,
					      uint8_t internal_distance,
					      uint8_t external_distance);
eigrp_result_t eigrp_instance_distance_delete(eigrp_address_family_config_t *af);

void eigrp_instance_config_finish(void);

#endif /* EIGRPD_EIGRP_INSTANCE_H_ */
