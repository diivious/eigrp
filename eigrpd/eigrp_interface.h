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

#include "eigrpd/eigrp_result.h"
#include "eigrpd/eigrp_types.h"

#define EIGRP_INTERFACE_BANDWIDTH_MIN 1U
#define EIGRP_INTERFACE_BANDWIDTH_MAX 10000000U
#define EIGRP_INTERFACE_DELAY_MIN 1U
#define EIGRP_INTERFACE_DELAY_MAX 16777215U

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

typedef struct eigrp_interface_context {
	eigrp_interface_config_t *config;
	eigrp_interface_t *runtime;
} eigrp_interface_context_t;

typedef struct eigrp_interface_state {
	const char *interface_name;
	bool config_present;
	bool runtime_present;
	bool passive;
	bool shutdown;
	bool multicast_enabled;
	bool authentication_configured;
	uint8_t authentication_mode;
	uint32_t bandwidth;
	uint32_t bandwidth_percent;
	uint32_t delay;
	uint32_t mtu;
	uint32_t hello_interval;
	uint16_t hold_time;
	uint32_t peer_count;
	unsigned long output_queue_count;
	unsigned long reliable_queue_count;
	uint8_t reliability;
	uint8_t load;
	uint16_t tlv1_peer_count;
	uint16_t tlv2_peer_count;
	bool split_horizon;
	bool next_hop_self;
	bool hello_timer_running;
	uint32_t hello_timer_remaining;
	uint64_t unreliable_multicast_sent;
	uint64_t reliable_multicast_sent;
	uint64_t unreliable_unicast_sent;
	uint64_t reliable_unicast_sent;
	uint64_t multicast_exceptions;
	uint64_t cr_packets_sent;
	uint64_t retransmissions_sent;
	bool bandwidth_percent_configured;
	bool hello_interval_configured;
	bool hold_time_configured;
} eigrp_interface_state_t;

typedef eigrp_result_t (*eigrp_interface_state_walk_cb)(
	const eigrp_interface_state_t *state, void *arg);

/* Host-independent runtime interface state supplied by a southbound adapter. */
typedef struct eigrp_interface_runtime_state {
	const char *interface_name;
	eigrp_ifindex_t ifindex;
	eigrp_prefix_t address;
	uint8_t type;
	bool operative;
	uint32_t bandwidth;
	uint32_t mtu;
} eigrp_interface_runtime_state_t;

/* Prototypes */
extern bool eigrp_intf_is_passive(eigrp_interface_t *ei);
extern void eigrp_del_intf_params(eigrp_intf_params_t *);
eigrp_interface_t *eigrp_interface_runtime_create(
	eigrp_instance_t *eigrp, const eigrp_interface_runtime_state_t *state);
void eigrp_interface_runtime_update(eigrp_interface_t *ei,
				    const eigrp_interface_runtime_state_t *state);
void eigrp_interface_runtime_delete(eigrp_interface_t *ei, int source);
extern int eigrp_intf_up(eigrp_instance_t *, eigrp_interface_t *);
extern void eigrp_intf_set_multicast(eigrp_interface_t *);
extern void eigrp_intf_free(eigrp_instance_t *, eigrp_interface_t *, int);
extern int eigrp_intf_down(eigrp_interface_t *);
extern const char *eigrp_intf_name_string(eigrp_interface_t *);
extern void eigrp_interface_encoder_bind(eigrp_interface_t *, uint8_t);
extern void eigrp_interface_encoder_unbind(eigrp_interface_t *, uint8_t);
extern void eigrp_interface_encoder_clear(eigrp_interface_t *);

eigrp_interface_t *eigrp_intf_lookup_by_local_addr(eigrp_instance_t *,
					    const eigrp_addr_t *address);
eigrp_interface_t *eigrp_intf_lookup_by_ifindex(eigrp_instance_t *,
					 eigrp_ifindex_t ifindex);
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

eigrp_result_t eigrp_interface_bandwidth_percent_update(
	eigrp_interface_context_t *context, uint32_t percent);
eigrp_result_t eigrp_interface_bandwidth_percent_delete(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_bandwidth_set(
	eigrp_interface_context_t *context, uint32_t bandwidth);
eigrp_result_t eigrp_interface_bandwidth_reset(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_delay_set(
	eigrp_interface_context_t *context, uint32_t delay);
eigrp_result_t eigrp_interface_delay_reset(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_hello_interval_update(
	eigrp_interface_context_t *context, uint16_t seconds);
eigrp_result_t eigrp_interface_hello_interval_delete(
	eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_hold_time_update(eigrp_interface_context_t *context,
					       uint16_t seconds);
eigrp_result_t eigrp_interface_hold_time_delete(eigrp_interface_context_t *context);
eigrp_result_t eigrp_interface_passive_update(eigrp_interface_context_t *context,
					      bool passive);
eigrp_result_t eigrp_interface_next_hop_self_update(
	eigrp_interface_context_t *context, bool enabled);
eigrp_result_t eigrp_interface_split_horizon_update(
	eigrp_interface_context_t *context, bool enabled);
eigrp_result_t eigrp_interface_shutdown_update(eigrp_interface_context_t *context,
					       bool shutdown);

#endif /* ZEBRA_EIGRP_INTERFACE_H_ */
