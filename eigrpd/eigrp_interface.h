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

#include "eigrpd/eigrp_cli.h"
#include "eigrpd/eigrp_mgnt.h"
#include "eigrpd/eigrp_types.h"

struct eigrp_interface_config {
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
	bool passive;
	bool authentication_mode_configured;
	uint8_t authentication_mode;
	uint8_t authentication_encryption_type;
	char *authentication_password;
	char *keychain;
	bool next_hop_self;
	bool split_horizon;
	bool shutdown;
	eigrp_summary_config_t *summaries;
	eigrp_interface_config_t *next;
};

/* Prototypes */
extern bool eigrp_intf_is_passive(eigrp_interface_t *ei);
extern void eigrp_del_intf_params(eigrp_intf_params_t *);
eigrp_interface_t *eigrp_interface_runtime_create(
	eigrp_instance_t *eigrp, const eigrp_interface_runtime_state_t *state);
void eigrp_interface_runtime_update(eigrp_interface_t *ei,
				    const eigrp_interface_runtime_state_t *state);
eigrp_result_t eigrp_interface_runtime_refresh(
	eigrp_instance_t *eigrp, const eigrp_interface_runtime_state_t *state);
void eigrp_interface_runtime_delete(
	eigrp_interface_t *ei, eigrp_interface_remove_reason_t reason);
extern int eigrp_intf_up(eigrp_instance_t *, eigrp_interface_t *);
extern void eigrp_intf_set_multicast(eigrp_interface_t *);
extern void eigrp_intf_free(eigrp_instance_t *, eigrp_interface_t *,
			    eigrp_interface_remove_reason_t);
extern int eigrp_intf_down(eigrp_interface_t *);
extern const char *eigrp_intf_name_string(eigrp_interface_t *);
extern void eigrp_interface_encoder_bind(eigrp_interface_t *, uint8_t);
extern void eigrp_interface_encoder_unbind(eigrp_interface_t *, uint8_t);
extern void eigrp_interface_encoder_clear(eigrp_interface_t *);

eigrp_interface_t *eigrp_intf_lookup_by_local_addr(eigrp_instance_t *,
					    const eigrp_addr_t *address);
eigrp_interface_t *eigrp_intf_lookup_by_ifindex(eigrp_instance_t *,
					 eigrp_ifindex_t ifindex);
eigrp_interface_t *eigrp_intf_lookup_by_vrf_ifindex(
	eigrp_vrf_id_t vrf_id, eigrp_ifindex_t ifindex);
eigrp_interface_t *eigrp_intf_lookup_by_name(eigrp_instance_t *,
				      const char *);

eigrp_result_t eigrp_interface_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, eigrp_interface_state_walk_cb callback,
	void *arg);

/* Portable runtime reset used after interface-affecting metric changes. */
void eigrp_interface_runtime_reset(eigrp_interface_t *ei);

/* Portable interface configuration targets. */
eigrp_result_t eigrp_interface_config_create(eigrp_address_family_config_t *af,
					     const char *interface_name);
eigrp_interface_config_t *eigrp_interface_config_read(
	eigrp_address_family_config_t *af, const char *interface_name);
eigrp_result_t eigrp_interface_config_delete(eigrp_address_family_config_t *af,
					     const char *interface_name);
void eigrp_interface_config_delete_all(eigrp_address_family_config_t *af);
void eigrp_interface_runtime_bind(eigrp_interface_t *runtime,
                                  const eigrp_interface_config_t *config);

eigrp_result_t eigrp_interface_bandwidth_percent_set(
	eigrp_interface_context_t *context, uint32_t percent);
eigrp_result_t eigrp_interface_bandwidth_percent_reset(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_bandwidth_set(
	eigrp_interface_context_t *context, uint32_t bandwidth);
eigrp_result_t eigrp_interface_bandwidth_reset(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_delay_set(
	eigrp_interface_context_t *context, uint32_t delay);
eigrp_result_t eigrp_interface_delay_reset(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_hello_interval_set(
	eigrp_interface_context_t *context, uint16_t seconds);
eigrp_result_t eigrp_interface_hello_interval_reset(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_hold_time_set(eigrp_interface_context_t *context,
					       uint16_t seconds);
eigrp_result_t eigrp_interface_hold_time_reset(eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_passive_set(eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_passive_reset(eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_next_hop_self_set(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_next_hop_self_reset(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_split_horizon_set(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_split_horizon_reset(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_shutdown_set(eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_shutdown_reset(eigrp_interface_context_t *context);

#endif /* ZEBRA_EIGRP_INTERFACE_H_ */
