// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Interface Functions.
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

#ifndef _ZEBRA_EIGRP_INTERFACE_H_
#define _ZEBRA_EIGRP_INTERFACE_H_

#include "eigrp_cli.h"
#include "eigrp_mgnt.h"
#include "eigrp_types.h"
#include "eigrp_structs.h"

struct eigrp_intf_config {
	char *interface_name;
	bool bandwidth_percent_configured;
	uint32_t bandwidth_percent;
	bool bandwidth_configured;
	uint32_t bandwidth;
	bool delay_configured;
	uint32_t delay;
	bool hello_interval_configured;
	uint16_t hello_interval;
	bool hold_time_configured;
	uint16_t hold_time;
	bool passive_configured;
	bool passive;
	bool authentication_mode_configured;
	uint8_t authentication_mode;
	uint8_t authentication_encryption_type;
	char *authentication_password;
	char *keychain;
	bool next_hop_self;
	bool split_horizon;
	bool shutdown_configured;
	bool shutdown;
	eigrp_summary_config_t *summaries;
	eigrp_intf_config_t *next;
};

/* Prototypes */
extern bool eigrp_intf_is_passive(eigrp_intf_t *ei);
bool eigrp_intf_shutdown_effective(eigrp_intf_t *ei);
extern void eigrp_del_intf_params(eigrp_intf_params_t *);
eigrp_intf_t *eigrp_intf_runtime_create(
	eigrp_instance_t *eigrp, const eigrp_intf_runtime_state_t *state);
void eigrp_intf_runtime_state_update_values(eigrp_intf_t *ei,
				    const eigrp_intf_runtime_state_t *state);
eigrp_result_t eigrp_intf_runtime_update(
	eigrp_operation_t operation, eigrp_instance_t *eigrp,
	const eigrp_intf_runtime_state_t *state, eigrp_intf_t *runtime);
void eigrp_intf_runtime_delete(
	eigrp_intf_t *ei, eigrp_intf_remove_reason_t reason);
extern int eigrp_intf_up(eigrp_instance_t *, eigrp_intf_t *);
extern void eigrp_intf_multicast_update(eigrp_operation_t, eigrp_intf_t *);
extern void eigrp_intf_free(eigrp_instance_t *, eigrp_intf_t *,
			    eigrp_intf_remove_reason_t);
extern int eigrp_intf_down(eigrp_intf_t *);
extern const char *eigrp_intf_name_string(eigrp_intf_t *);
extern void eigrp_intf_encoder_peer_add(eigrp_intf_t *, uint8_t);
extern void eigrp_intf_encoder_peer_remove(eigrp_intf_t *, uint8_t);
extern void eigrp_intf_encoder_clear(eigrp_intf_t *);

eigrp_intf_t *eigrp_intf_lookup_by_local_addr(eigrp_instance_t *,
					    const eigrp_addr_t *address);
eigrp_intf_t *eigrp_intf_lookup_by_ifindex(eigrp_instance_t *,
					 eigrp_ifindex_t ifindex);
eigrp_intf_t *eigrp_intf_lookup_by_vrf_ifindex(
	eigrp_vrf_id_t vrf_id, eigrp_ifindex_t ifindex);
eigrp_intf_t *eigrp_intf_lookup_by_name(eigrp_instance_t *,
				      const char *);

eigrp_result_t eigrp_intf_state_iterate(
	eigrp_af_instance_t *config, eigrp_instance_t *runtime,
	const char *interface_name, eigrp_intf_state_iterate_cb callback,
	void *arg);

/* Portable runtime reset used after interface-affecting metric changes. */

/* Portable interface configuration targets. */
eigrp_result_t eigrp_intf_config_create(eigrp_af_instance_t *af,
					     const char *interface_name);
eigrp_intf_config_t *eigrp_intf_config_read(
	eigrp_af_instance_t *af, const char *interface_name);
eigrp_result_t eigrp_intf_config_delete(eigrp_af_instance_t *af,
					     const char *interface_name);
void eigrp_intf_config_delete_all(eigrp_af_instance_t *af);
void eigrp_intf_config_update(eigrp_intf_t *runtime,
                                  const eigrp_intf_config_t *config);

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

#endif /* ZEBRA_EIGRP_INTERFACE_H_ */
