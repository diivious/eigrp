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
 */

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_tlv1.h"
#include "eigrpd/eigrp_tlv2.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_debug.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_summary.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"
static bool eigrp_interface_destination_get(const eigrp_interface_t *ei,
					    eigrp_prefix_t *destination)
{
	if (!ei || !destination || !eigrp_prefix_valid(&ei->address))
		return false;

	*destination = ei->address;
	eigrp_prefix_normalize(destination);
	return true;
}

static char *eigrp_interface_string_duplicate(const char *value)
{
	size_t len;
	char *copy;

	if (!value)
		return NULL;
	len = strlen(value) + 1;
	copy = malloc(len);
	if (!copy)
		return NULL;
	memcpy(copy, value, len);
	return copy;
}

static unsigned long eigrp_interface_reliable_queue_count(eigrp_interface_t *ei)
{
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *node;
	unsigned long count = 0;

	if (!ei || !ei->nbrs)
		return 0;

	for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
		if (nbr->retrans_queue)
			count += nbr->retrans_queue->count;
	}
	return count;
}

static void eigrp_interface_state_from_config(eigrp_interface_state_t *state,
					      eigrp_interface_config_t *config)
{
	if (!state || !config)
		return;

	state->config_present = true;
	state->shutdown = config->shutdown;
	state->passive = config->passive;
	state->authentication_configured = config->authentication_mode_configured;
	state->authentication_mode = config->authentication_mode;
	state->next_hop_self = config->next_hop_self;
	state->split_horizon = config->split_horizon;
	state->bandwidth_percent_configured = config->bandwidth_percent_configured;
	state->hello_interval_configured = config->hello_interval_configured;
	state->hold_time_configured = config->hold_time_configured;
	if (config->bandwidth_percent_configured)
		state->bandwidth_percent = config->bandwidth_percent;
	if (config->hello_interval_configured)
		state->hello_interval = config->hello_interval;
	if (config->hold_time_configured)
		state->hold_time = config->hold_time;
}

static eigrp_result_t eigrp_interface_state_emit(
	eigrp_interface_t *ei, eigrp_interface_config_t *config,
	eigrp_interface_state_walk_cb callback, void *arg)
{
	eigrp_interface_state_t state = {0};

	if (config) {
		state.interface_name = config->interface_name;
		eigrp_interface_state_from_config(&state, config);
	}
	if (ei) {
		state.interface_name = eigrp_intf_name_string(ei);
		state.runtime_present = true;
		state.passive = eigrp_intf_is_passive(ei);
		state.multicast_enabled = ei->member_allrouters;
		state.authentication_configured = ei->params.auth_type != 0;
		state.authentication_mode = (uint8_t)ei->params.auth_type;
		state.bandwidth = ei->params.bandwidth;
		state.delay = ei->params.delay;
		state.mtu = ei->curr_mtu;
		state.hello_interval = ei->params.v_hello;
		state.hold_time = ei->params.v_wait;
		state.peer_count = ei->nbrs ? ei->nbrs->count : 0;
		state.output_queue_count = ei->obuf ? ei->obuf->count : 0;
		state.reliable_queue_count = eigrp_interface_reliable_queue_count(ei);
		state.reliability = ei->params.reliability;
		state.load = ei->params.load;
		state.tlv1_peer_count = ei->tlv1_peer_count;
		state.tlv2_peer_count = ei->tlv2_peer_count;
		state.split_horizon = ei->split_horizon;
		state.hello_timer_running = ei->t_hello != NULL;
		state.hello_timer_remaining =
			eigrp_sys_timer_remaining_seconds(ei->t_hello);
		state.unreliable_multicast_sent =
			ei->stats.unreliable_multicast_sent;
		state.reliable_multicast_sent = ei->stats.reliable_multicast_sent;
		state.unreliable_unicast_sent = ei->stats.unreliable_unicast_sent;
		state.reliable_unicast_sent = ei->stats.reliable_unicast_sent;
		state.multicast_exceptions = ei->stats.multicast_exceptions;
		state.cr_packets_sent = ei->stats.cr_packets_sent;
		state.retransmissions_sent = ei->stats.retransmissions_sent;
	}

	return callback(&state, arg);
}

/*
 * Syntax:
 *   EXEC: `show eigrp address-family <ipv4|ipv6> ... interfaces [IFNAME] [detail]`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Walks EIGRP interface state for operational output.
 * The callback receives portable EIGRP state rather than FRR interface or VTY objects.
 */
eigrp_result_t eigrp_interface_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, eigrp_interface_state_walk_cb callback,
	void *arg)
{
	eigrp_interface_t *ei;
	eigrp_interface_config_t *configured;
	eigrp_list_node_t *node;
	bool matched = false;
	eigrp_result_t result;

	if (!callback || (!config && !runtime))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (runtime && !runtime->data_path_ready)
		return EIGRP_RESULT_NOT_IMPLEMENTED;

	if (runtime && runtime->eiflist) {
		for (EIGRP_LIST_ELEMENTS_RO(runtime->eiflist, node, ei)) {
			const char *name = eigrp_intf_name_string(ei);

			if (interface_name && strcmp(name, interface_name) != 0)
				continue;
			configured = config ? eigrp_interface_config_read(config, name) : NULL;
			result = eigrp_interface_state_emit(ei, configured, callback, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
			matched = true;
		}
	}

	if (config) {
		for (configured = config->interfaces; configured;
		     configured = configured->next) {
			if (interface_name
			    && strcmp(configured->interface_name, interface_name) != 0)
				continue;
			if (runtime
			    && eigrp_intf_lookup_by_name(runtime,
							configured->interface_name))
				continue;
			result = eigrp_interface_state_emit(NULL, configured, callback, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
			matched = true;
		}
	}

	return matched ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;
}

eigrp_interface_config_t *eigrp_interface_config_read(
	eigrp_address_family_config_t *af, const char *interface_name)
{
	eigrp_interface_config_t *interface;

	if (!af || !interface_name || !interface_name[0])
		return NULL;
	for (interface = af->interfaces; interface; interface = interface->next)
		if (strcmp(interface->interface_name, interface_name) == 0)
			return interface;
	return NULL;
}

/*
 * Syntax:
 *   Named: `af-interface <default|IFNAME>` / `no af-interface <default|IFNAME>`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Creates or removes named-mode interface configuration, including the default interface template.
 * Runtime application stays behind the EIGRP interface abstraction.
 */
eigrp_result_t eigrp_interface_config_create(eigrp_address_family_config_t *af,
					     const char *interface_name)
{
	eigrp_interface_config_t *interface;

	if (!interface_name || !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	if (eigrp_interface_config_read(af, interface_name))
		return EIGRP_RESULT_SUCCESS;

	interface = calloc(1, sizeof(*interface));
	if (!interface)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	interface->interface_name = eigrp_interface_string_duplicate(interface_name);
	if (!interface->interface_name) {
		free(interface);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	interface->next_hop_self = true;
	interface->split_horizon = true;
	interface->next = af->interfaces;
	af->interfaces = interface;
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_interface_config_free(eigrp_interface_config_t *interface)
{
	if (!interface)
		return;
	eigrp_summary_delete_all(interface);
	free(interface->authentication_password);
	free(interface->keychain);
	free(interface->interface_name);
	free(interface);
}

/*
 * Syntax:
 *   Named: `af-interface <default|IFNAME>` / `no af-interface <default|IFNAME>`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Creates or removes named-mode interface configuration, including the default interface template.
 * Runtime application stays behind the EIGRP interface abstraction.
 */
eigrp_result_t eigrp_interface_config_delete(eigrp_address_family_config_t *af,
					     const char *interface_name)
{
	eigrp_interface_config_t **cursor;
	eigrp_interface_config_t *interface;

	if (!interface_name || !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;

	for (cursor = &af->interfaces; *cursor; cursor = &(*cursor)->next) {
		interface = *cursor;
		if (strcmp(interface->interface_name, interface_name) != 0)
			continue;
		*cursor = interface->next;
		eigrp_interface_config_free(interface);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

void eigrp_interface_config_delete_all(eigrp_address_family_config_t *af)
{
	eigrp_interface_config_t *interface;
	eigrp_interface_config_t *next;

	if (!af)
		return;
	for (interface = af->interfaces; interface; interface = next) {
		next = interface->next;
		eigrp_interface_config_free(interface);
	}
	af->interfaces = NULL;
}

void eigrp_interface_runtime_bind(eigrp_interface_t *runtime,
                                  const eigrp_interface_config_t *config)
{
	eigrp_interface_context_t context = {.runtime = runtime};

	if (!runtime || !config)
		return;

	if (config->bandwidth_configured)
		runtime->params.bandwidth = config->bandwidth;
	if (config->delay_configured)
		runtime->params.delay = config->delay;
	if (config->hello_interval_configured)
		runtime->params.v_hello = config->hello_interval;
	if (config->hold_time_configured)
		runtime->params.v_wait = config->hold_time;
	runtime->params.passive_interface = config->passive
		? EIGRP_INTF_PASSIVE : EIGRP_INTF_ACTIVE;
	runtime->split_horizon = config->split_horizon;

	/* Authentication is retained on the named af-interface object.  When a
	 * network statement creates the runtime interface after configuration was
	 * committed, bind the already-supported MD5/key-chain state as part of
	 * the same interface configuration handoff.  Direct-password SHA-256
	 * remains explicitly unsupported by the authentication target.
	 */
	if (config->authentication_mode_configured
	    && config->authentication_mode == EIGRP_AUTHENTICATION_MD5)
		(void)eigrp_auth_mode_set(&context, EIGRP_AUTHENTICATION_MD5, NULL);
	if (config->keychain)
		(void)eigrp_auth_keychain_set(&context, config->keychain);
}

static bool eigrp_interface_context_valid(const eigrp_interface_context_t *context)
{
	return context && (context->config || context->runtime);
}

/*
 * Syntax:
 *   Named: `bandwidth-percent PERCENT` / `no bandwidth-percent`
 * Supported: Named
 * Placement:
 *   Named: af-interface mode
 * Description:
 * Retains the configured EIGRP bandwidth percentage for interface pacing.
 * The real target reports NOT_IMPLEMENTED when live pacing application is not yet complete.
 */
eigrp_result_t eigrp_interface_bandwidth_percent_set(
	eigrp_interface_context_t *context, uint32_t percent)
{
	if (percent == 0 || percent > 999999)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->bandwidth_percent = percent;
		context->config->bandwidth_percent_configured = true;
	}
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `bandwidth-percent PERCENT` / `no bandwidth-percent`
 * Supported: Named
 * Placement:
 *   Named: af-interface mode
 * Description:
 * Retains the configured EIGRP bandwidth percentage for interface pacing.
 * The real target reports NOT_IMPLEMENTED when live pacing application is not yet complete.
 */
eigrp_result_t eigrp_interface_bandwidth_percent_reset(
	eigrp_interface_context_t *context)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->bandwidth_percent = 0;
		context->config->bandwidth_percent_configured = false;
	}
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `eigrp bandwidth KBPS` / `no eigrp bandwidth [KBPS]`
 *   Named: `bandwidth KBPS` / `no bandwidth [KBPS]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Sets or restores the EIGRP interface bandwidth metric input.
 * Named mode reuses the EIGRP runtime behavior represented by the classic interface command.
 */
eigrp_result_t eigrp_interface_bandwidth_set(
	eigrp_interface_context_t *context, uint32_t bandwidth)
{
	if (bandwidth < EIGRP_INTERFACE_BANDWIDTH_MIN
	    || bandwidth > EIGRP_INTERFACE_BANDWIDTH_MAX)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->bandwidth = bandwidth;
		context->config->bandwidth_configured = true;
	}
	if (context->runtime) {
		context->runtime->params.bandwidth = bandwidth;
		eigrp_interface_runtime_reset(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `eigrp bandwidth KBPS` / `no eigrp bandwidth [KBPS]`
 *   Named: `bandwidth KBPS` / `no bandwidth [KBPS]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Sets or restores the EIGRP interface bandwidth metric input.
 * Named mode reuses the EIGRP runtime behavior represented by the classic interface command.
 */
eigrp_result_t eigrp_interface_bandwidth_reset(
	eigrp_interface_context_t *context)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->bandwidth = 0;
		context->config->bandwidth_configured = false;
	}
	if (context->runtime) {
		context->runtime->params.bandwidth = EIGRP_BANDWIDTH_DEFAULT;
		eigrp_interface_runtime_reset(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `delay TENS-OF-MICROSECONDS` / `no delay [VALUE]`
 *   Named: `delay TENS-OF-MICROSECONDS` / `no delay [VALUE]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Sets or restores the EIGRP interface delay metric input.
 * The common interface target owns the runtime refresh required after the metric changes.
 */
eigrp_result_t eigrp_interface_delay_set(
	eigrp_interface_context_t *context, uint32_t delay)
{
	if (delay < EIGRP_INTERFACE_DELAY_MIN
	    || delay > EIGRP_INTERFACE_DELAY_MAX)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->delay = delay;
		context->config->delay_configured = true;
	}
	if (context->runtime) {
		context->runtime->params.delay = delay;
		eigrp_interface_runtime_reset(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `delay TENS-OF-MICROSECONDS` / `no delay [VALUE]`
 *   Named: `delay TENS-OF-MICROSECONDS` / `no delay [VALUE]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Sets or restores the EIGRP interface delay metric input.
 * The common interface target owns the runtime refresh required after the metric changes.
 */
eigrp_result_t eigrp_interface_delay_reset(
	eigrp_interface_context_t *context)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->delay = 0;
		context->config->delay_configured = false;
	}
	if (context->runtime) {
		context->runtime->params.delay = EIGRP_DELAY_DEFAULT;
		eigrp_interface_runtime_reset(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `ip hello-interval eigrp AS SECONDS` / `no ip hello-interval eigrp [SECONDS]`
 *   Named: `hello-interval SECONDS` / `no hello-interval`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Changes the EIGRP hello transmission interval or restores the default.
 * The runtime hello timer is rescheduled by EIGRP-owned interface behavior.
 */
eigrp_result_t eigrp_interface_hello_interval_set(
	eigrp_interface_context_t *context, uint16_t seconds)
{
	if (seconds == 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->hello_interval = seconds;
		context->config->hello_interval_configured = true;
	}
	if (context->runtime)
		context->runtime->params.v_hello = seconds;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `ip hello-interval eigrp AS SECONDS` / `no ip hello-interval eigrp [SECONDS]`
 *   Named: `hello-interval SECONDS` / `no hello-interval`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Changes the EIGRP hello transmission interval or restores the default.
 * The runtime hello timer is rescheduled by EIGRP-owned interface behavior.
 */
eigrp_result_t eigrp_interface_hello_interval_reset(
	eigrp_interface_context_t *context)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->hello_interval = 0;
		context->config->hello_interval_configured = false;
	}
	if (context->runtime)
		context->runtime->params.v_hello = EIGRP_HELLO_INTERVAL_DEFAULT;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `ip hold-time eigrp AS SECONDS` / `no ip hold-time eigrp [SECONDS]`
 *   Named: `hold-time SECONDS` / `no hold-time`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Changes the advertised EIGRP neighbor hold time or restores the default.
 * Named mode uses the common interface state instead of a named-only timer implementation.
 */
eigrp_result_t eigrp_interface_hold_time_set(eigrp_interface_context_t *context,
					       uint16_t seconds)
{
	if (seconds == 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->hold_time = seconds;
		context->config->hold_time_configured = true;
	}
	if (context->runtime)
		context->runtime->params.v_wait = seconds;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `ip hold-time eigrp AS SECONDS` / `no ip hold-time eigrp [SECONDS]`
 *   Named: `hold-time SECONDS` / `no hold-time`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Changes the advertised EIGRP neighbor hold time or restores the default.
 * Named mode uses the common interface state instead of a named-only timer implementation.
 */
eigrp_result_t eigrp_interface_hold_time_reset(eigrp_interface_context_t *context)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->hold_time = 0;
		context->config->hold_time_configured = false;
	}
	if (context->runtime)
		context->runtime->params.v_wait = EIGRP_HOLD_INTERVAL_DEFAULT;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `passive-interface IFNAME` / `no passive-interface IFNAME`
 *   Named: `passive-interface` / `no passive-interface`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: af-interface mode
 * Description:
 * Controls whether EIGRP forms adjacencies on the interface while retaining the connected prefix behavior required by the configuration.
 * The CLI placement differs, but the named path terminates in EIGRP-owned interface state.
 */
static eigrp_result_t eigrp_interface_passive_apply(eigrp_interface_context_t *context,
					      bool passive)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config)
		context->config->passive = passive;
	if (context->runtime) {
		context->runtime->params.passive_interface =
			passive ? EIGRP_INTF_PASSIVE : EIGRP_INTF_ACTIVE;
		eigrp_intf_set_multicast(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_interface_passive_set(eigrp_interface_context_t *context)
{
	return eigrp_interface_passive_apply(context, true);
}

eigrp_result_t eigrp_interface_passive_reset(eigrp_interface_context_t *context)
{
	return eigrp_interface_passive_apply(context, false);
}

/*
 * Syntax:
 *   Named: `next-hop-self` / `no next-hop-self`
 * Supported: Named
 * Placement:
 *   Named: af-interface mode
 * Description:
 * Controls EIGRP next-hop-self behavior for the interface.
 * The target owns retained/runtime state and reports NOT_IMPLEMENTED if the live packet path cannot apply the setting yet.
 */
static eigrp_result_t eigrp_interface_next_hop_self_apply(
	eigrp_interface_context_t *context, bool enabled)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config)
		context->config->next_hop_self = enabled;
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_interface_next_hop_self_set(
	eigrp_interface_context_t *context)
{
	return eigrp_interface_next_hop_self_apply(context, true);
}

eigrp_result_t eigrp_interface_next_hop_self_reset(
	eigrp_interface_context_t *context)
{
	return eigrp_interface_next_hop_self_apply(context, false);
}

/*
 * Syntax:
 *   Classic: `ip split-horizon eigrp AS` / `no ip split-horizon eigrp AS`
 *   Named: `split-horizon` / `no split-horizon`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Controls EIGRP split-horizon state on the interface.
 * The FRR reference carried only an unimplemented classic XPath marker, so named mode keeps a real EIGRP target without pretending a working classic runtime existed.
 */
static eigrp_result_t eigrp_interface_split_horizon_apply(
	eigrp_interface_context_t *context, bool enabled)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config)
		context->config->split_horizon = enabled;
	if (context->runtime)
		context->runtime->split_horizon = enabled;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_interface_split_horizon_set(
	eigrp_interface_context_t *context)
{
	return eigrp_interface_split_horizon_apply(context, true);
}

eigrp_result_t eigrp_interface_split_horizon_reset(
	eigrp_interface_context_t *context)
{
	return eigrp_interface_split_horizon_apply(context, false);
}

/*
 * Syntax:
 *   Named: `shutdown` / `no shutdown`
 * Supported: Named
 * Placement:
 *   Named: af-interface mode
 * Description:
 * Administratively disables or enables EIGRP on the selected named af-interface.
 * Interface runtime transitions remain owned by the common EIGRP interface layer.
 */
static eigrp_result_t eigrp_interface_shutdown_apply(eigrp_interface_context_t *context,
					       bool shutdown)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config)
		context->config->shutdown = shutdown;
	if (context->runtime) {
		if (shutdown && context->runtime->t_hello) {
			eigrp_hello_send(context->runtime,
					 EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);
			eigrp_intf_down(context->runtime);
		} else if (!shutdown && context->runtime->operative
			   && !context->runtime->t_hello) {
			(void)eigrp_instance_address_family_start(
				context->runtime->eigrp);
		}
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_interface_shutdown_set(eigrp_interface_context_t *context)
{
	return eigrp_interface_shutdown_apply(context, true);
}

eigrp_result_t eigrp_interface_shutdown_reset(eigrp_interface_context_t *context)
{
	return eigrp_interface_shutdown_apply(context, false);
}


void eigrp_interface_encoder_clear(eigrp_interface_t *ei)
{
	if (!ei)
		return;

	ei->tlv1_peer_count = 0;
	ei->tlv2_peer_count = 0;
	ei->encoder = eigrp_packet_encoder_safe;
}

static void eigrp_interface_encoder_select(eigrp_interface_t *ei)
{
	if (!ei)
		return;

	if (ei->tlv1_peer_count && ei->tlv2_peer_count) {
		ei->encoder = eigrp_packet_encoder_both;
		return;
	}

	if (ei->tlv1_peer_count) {
		assert(ei->eigrp->tlv1_codec.encoder);
		ei->encoder = ei->eigrp->tlv1_codec.encoder;
		return;
	}

	if (ei->tlv2_peer_count) {
		assert(ei->eigrp->tlv2_codec.encoder);
		ei->encoder = ei->eigrp->tlv2_codec.encoder;
		return;
	}

	ei->encoder = eigrp_packet_encoder_safe;
}

void eigrp_interface_encoder_bind(eigrp_interface_t *ei, uint8_t tlv_version)
{
	if (!ei)
		return;

	switch (tlv_version) {
	case EIGRP_TLV_32B_VERSION:
		ei->tlv1_peer_count++;
		break;
	case EIGRP_TLV_64B_VERSION:
		ei->tlv2_peer_count++;
		break;
	default:
		return;
	}

	eigrp_interface_encoder_select(ei);
}

void eigrp_interface_encoder_unbind(eigrp_interface_t *ei, uint8_t tlv_version)
{
	if (!ei)
		return;

	switch (tlv_version) {
	case EIGRP_TLV_32B_VERSION:
		if (ei->tlv1_peer_count)
			ei->tlv1_peer_count--;
		break;
	case EIGRP_TLV_64B_VERSION:
		if (ei->tlv2_peer_count)
			ei->tlv2_peer_count--;
		break;
	default:
		return;
	}

	eigrp_interface_encoder_select(ei);
}

static void eigrp_intf_stream_set(eigrp_interface_t *ei)
{
	/* set output queue. */
	if (ei->obuf == NULL)
		ei->obuf = eigrp_packet_queue_new();
}

static void eigrp_intf_stream_unset(eigrp_interface_t *ei)
{
	eigrp_instance_t *eigrp = ei->eigrp;

	if (ei->on_write_q) {
		eigrp_list_delete_data(eigrp->oi_write_q, ei);
		if (eigrp_list_isempty(eigrp->oi_write_q))
			eigrp_sys_event_cancel(&eigrp->t_write);
		ei->on_write_q = 0;
	}
}

const char *eigrp_intf_name_string(eigrp_interface_t *ei)
{
	if (!ei || !ei->name)
		return "inactive";

	return ei->name;
}

static void eigrp_interface_runtime_state_apply(
	eigrp_interface_t *ei, const eigrp_interface_runtime_state_t *state)
{
	char *name;

	if (!ei || !state)
		return;

	if (state->interface_name
	    && (!ei->name || strcmp(ei->name, state->interface_name) != 0)) {
		name = strdup(state->interface_name);
		if (name) {
			if (ei->name)
				free(ei->name);
			ei->name = name;
		}
	}

	ei->ifindex = state->ifindex;
	if (eigrp_prefix_valid(&state->address)) {
		/* Preserve the host address on the runtime interface.  Callers that
		 * need the connected network prefix normalize a copy through
		 * eigrp_interface_destination_get().
		 */
		ei->address = state->address;
	}
	ei->type = state->type;
	ei->operative = state->operative;
	ei->curr_bandwidth = state->bandwidth;
	ei->curr_mtu = state->mtu;
}

eigrp_interface_t *eigrp_interface_runtime_create(
	eigrp_instance_t *eigrp, const eigrp_interface_runtime_state_t *state)
{
	eigrp_interface_t *ei;

	if (!eigrp || !state || !state->interface_name
	    || !state->interface_name[0] || !eigrp_prefix_valid(&state->address))
		return NULL;

	ei = eigrp_intf_lookup_by_ifindex(eigrp, state->ifindex);
	if (!ei)
		ei = eigrp_intf_lookup_by_name(eigrp, state->interface_name);
	if (ei) {
		eigrp_interface_runtime_state_apply(ei, state);
		return ei;
	}

	ei = calloc(1, sizeof(*ei));
	ei->eigrp = eigrp;
	eigrp_interface_runtime_state_apply(ei, state);
	if (!ei->name) {
		free(ei);
		return NULL;
	}

	eigrp_list_add(eigrp->eiflist, ei);
	ei->nbrs = eigrp_list_new();
	ei->crypt_seqnum = time(NULL);
	eigrp_interface_encoder_clear(ei);
	ei->split_horizon = true;

	ei->params.type = state->type;
	ei->params.v_hello = EIGRP_HELLO_INTERVAL_DEFAULT;
	ei->params.v_wait = EIGRP_HOLD_INTERVAL_DEFAULT;
	ei->params.bandwidth = EIGRP_BANDWIDTH_DEFAULT;
	ei->params.delay = EIGRP_DELAY_DEFAULT;
	ei->params.reliability = EIGRP_RELIABILITY_DEFAULT;
	ei->params.load = EIGRP_LOAD_DEFAULT;
	ei->params.auth_type = EIGRP_AUTH_TYPE_NONE;
	ei->params.auth_keychain = NULL;

	return ei;
}

void eigrp_interface_runtime_update(eigrp_interface_t *ei,
				    const eigrp_interface_runtime_state_t *state)
{
	if (!ei || !state)
		return;

	eigrp_interface_runtime_state_apply(ei, state);
	ei->params.type = state->type;
}

eigrp_result_t eigrp_interface_runtime_refresh(
	eigrp_instance_t *eigrp, const eigrp_interface_runtime_state_t *state)
{
	eigrp_address_family_config_t *af;
	eigrp_interface_config_t *config;
	eigrp_interface_t *ei;
	bool was_running;
	uint32_t old_mtu;

	if (!eigrp || !state || !state->interface_name
	    || !state->interface_name[0] || !eigrp_prefix_valid(&state->address))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	ei = eigrp_intf_lookup_by_ifindex(eigrp, state->ifindex);
	was_running = ei && ei->t_hello;
	old_mtu = ei ? ei->curr_mtu : state->mtu;
	if (ei)
		eigrp_interface_runtime_update(ei, state);
	else
		ei = eigrp_interface_runtime_create(eigrp, state);
	if (!ei)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	af = eigrp_instance_runtime_config(eigrp);
	config = af ? eigrp_interface_config_read(af, state->interface_name) : NULL;
	if (config)
		eigrp_interface_runtime_bind(ei, config);

	if ((af && af->shutdown) || (config && config->shutdown)
	    || !state->operative) {
		if (was_running)
			eigrp_intf_down(ei);
		return EIGRP_RESULT_SUCCESS;
	}

	if (was_running && old_mtu != state->mtu)
		eigrp_interface_runtime_reset(ei);
	else if (!was_running)
		eigrp_intf_up(eigrp, ei);

	return EIGRP_RESULT_SUCCESS;
}

void eigrp_interface_runtime_delete(
	eigrp_interface_t *ei, eigrp_interface_remove_reason_t reason)
{
	if (!ei)
		return;

	eigrp_intf_free(ei->eigrp, ei, reason);
}

void eigrp_sys_interface_link_down(
	eigrp_vrf_id_t vrf_id, eigrp_ifindex_t ifindex, const char *interface_name,
	uint8_t type, uint32_t bandwidth, uint32_t mtu)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	eigrp_interface_runtime_state_t state;
	eigrp_list_node_t *node;

	if (!ifindex || !eigrp_om || !eigrp_om->eigrp)
		return;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp)) {
		if (eigrp->vrf_id != vrf_id)
			continue;
		ei = eigrp_intf_lookup_by_ifindex(eigrp, ifindex);
		if (!ei)
			continue;

		memset(&state, 0, sizeof(state));
		state.interface_name = interface_name ? interface_name : ei->name;
		state.ifindex = ifindex;
		state.address = ei->address;
		state.type = type;
		state.operative = false;
		state.bandwidth = bandwidth;
		state.mtu = mtu;
		eigrp_interface_runtime_update(ei, &state);
		eigrp_intf_down(ei);
	}
}

void eigrp_sys_interface_link_remove(
	eigrp_vrf_id_t vrf_id, eigrp_ifindex_t ifindex,
	eigrp_interface_remove_reason_t reason)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (!ifindex || !eigrp_om || !eigrp_om->eigrp)
		return;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp)) {
		if (eigrp->vrf_id != vrf_id)
			continue;
		ei = eigrp_intf_lookup_by_ifindex(eigrp, ifindex);
		if (ei)
			eigrp_interface_runtime_delete(ei, reason);
	}
}

void eigrp_sys_interface_address_remove(
	eigrp_vrf_id_t vrf_id, eigrp_ifindex_t ifindex,
	const eigrp_prefix_t *address, eigrp_interface_remove_reason_t reason)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (!ifindex || !address || !eigrp_prefix_valid(address) || !eigrp_om
	    || !eigrp_om->eigrp)
		return;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp)) {
		if (eigrp->vrf_id != vrf_id)
			continue;
		ei = eigrp_intf_lookup_by_ifindex(eigrp, ifindex);
		if (!ei || ei->address.prefix_length != address->prefix_length
		    || ei->address.address.afi != address->address.afi
		    || memcmp(ei->address.address.bytes, address->address.bytes,
			      sizeof(address->address.bytes)) != 0)
			continue;

		eigrp_interface_runtime_delete(ei, reason);
	}
}

void eigrp_del_intf_params(eigrp_intf_params_t *eip)
{
	if (eip->auth_keychain)
		free(eip->auth_keychain);
}

int eigrp_intf_up(eigrp_instance_t *eigrp, eigrp_interface_t *ei)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_metrics_t metric;

	eigrp_sys_socket_send_buffer_ensure(eigrp, ei->curr_mtu);
	eigrp_intf_stream_set(ei);

	/* Set multicast memberships appropriately for new state. */
	eigrp_intf_set_multicast(ei);

	eigrp_sys_event_add(&ei->t_hello, eigrp_hello_timer, ei);

	/*Prepare metrics*/
	metric.bandwidth = eigrp_bandwidth_to_scaled(ei->params.bandwidth);
	metric.delay = eigrp_delay_to_scaled(ei->params.delay);
	metric.load = ei->params.load;
	metric.reliability = ei->params.reliability;
	metric.mtu[0] = 0xDC;
	metric.mtu[1] = 0x05;
	metric.mtu[2] = 0x00;
	metric.hop_count = 0;
	metric.flags = 0;
	metric.tag = 0;

	/*Add connected route to topology table*/
	route = eigrp_topology_route_create(ei);
	route->type = EIGRP_TLV_IPv4_INT;

	route->reported_metric = metric;
	route->total_metric = metric;
	route->distance = eigrp_calculate_metrics(eigrp, metric);
	route->reported_distance = 0;
	route->adv_router = eigrp->neighbor_self;
	route->flags = EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;

	eigrp_prefix_t destination;

	if (!eigrp_interface_destination_get(ei, &destination)) {
		eigrp_topology_route_free(route);
		return 0;
	}
	prefix = eigrp_topology_table_lookup(eigrp->topology_table, &destination);

	if (prefix == NULL) {
		prefix = eigrp_topology_prefix_create();
		prefix->serno = eigrp->serno;
		prefix->destination = destination;
		prefix->nt = EIGRP_TOPOLOGY_TYPE_CONNECTED;
		prefix->reported_metric = metric;
		prefix->state = EIGRP_FSM_STATE_PASSIVE;
		prefix->fdistance = eigrp_calculate_metrics(eigrp, metric);
		prefix->req_action |= EIGRP_FSM_NEED_UPDATE;

		eigrp_prefix_descriptor_add(eigrp->topology_table, prefix);

		eigrp_list_add(eigrp->topology_changes, prefix);

		route->prefix = prefix;
		eigrp_route_descriptor_add(eigrp, prefix, route);

		eigrp_update_send_all(eigrp, NULL);

	} else {
		eigrp_fsm_action_message_t msg;

		route->prefix = prefix;
		eigrp_route_descriptor_add(eigrp, prefix, route);

		msg.packet_type = EIGRP_OPC_UPDATE;
		msg.eigrp = eigrp;
		msg.data_type = EIGRP_CONNECTED;
		msg.adv_router = NULL;
		msg.route = route;
		msg.prefix = prefix;

		eigrp_fsm_event(&msg);
	}

	return 1;
}

int eigrp_intf_down(eigrp_interface_t *ei)
{
	eigrp_list_node_t *node, *nnode;
	eigrp_neighbor_t *nbr;

	if (ei == NULL)
		return 0;

	/* Shutdown packet reception and sending */
	if (ei->t_hello)
		eigrp_sys_event_cancel(&ei->t_hello);

	eigrp_intf_stream_unset(ei);

	/*Set infinite metrics to routes learned by this interface and start
	 * query process*/
	for (EIGRP_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
		eigrp_nbr_delete(nbr);
	}
	eigrp_interface_encoder_clear(ei);

	return 1;
}

bool eigrp_intf_is_passive(eigrp_interface_t *ei)
{
	return ei && ei->params.passive_interface == EIGRP_INTF_PASSIVE;
}

void eigrp_intf_set_multicast(eigrp_interface_t *ei)
{
	if (!ei)
		return;

	if (!eigrp_intf_is_passive(ei)) {
		if (!ei->member_allrouters
		    && eigrp_sys_multicast_join(ei->eigrp, ei) >= 0)
			ei->member_allrouters = true;
	} else if (ei->member_allrouters) {
		(void)eigrp_sys_multicast_leave(ei->eigrp, ei);
		ei->member_allrouters = false;
	}
}

void eigrp_intf_free(eigrp_instance_t *eigrp, eigrp_interface_t *ei,
		    eigrp_interface_remove_reason_t reason)
{
	eigrp_prefix_t destination;
	eigrp_prefix_descriptor_t *pe = NULL;

	if (reason == EIGRP_INTERFACE_REMOVE_CONFIG) {
		eigrp_sys_event_cancel(&ei->t_hello);
		eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);
	}

	if (eigrp_interface_destination_get(ei, &destination))
		pe = eigrp_topology_table_lookup(eigrp->topology_table,
					 &destination);
	if (pe)
		eigrp_prefix_descriptor_delete(eigrp, eigrp->topology_table,
					       pe);

	eigrp_intf_down(ei);

	eigrp_list_delete_data(ei->eigrp->eiflist, ei);
	eigrp_list_delete(&ei->nbrs);
	eigrp_packet_queue_free(ei->obuf);
	eigrp_filter_runtime_state_clear(&ei->filter);
	if (ei->name)
		free(ei->name);
	free(ei);
}

void eigrp_interface_runtime_reset(eigrp_interface_t *ei)
{
	if (!ei)
		return;

	eigrp_intf_down(ei);
	eigrp_intf_up(ei->eigrp, ei);
}

eigrp_interface_t *eigrp_intf_lookup_by_local_addr(eigrp_instance_t *eigrp,
						   const eigrp_addr_t *address)
{
	eigrp_list_node_t *node;
	eigrp_interface_t *ei;

	if (!eigrp || !address)
		return NULL;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		if (address->afi == AF_INET
		    && ei->address.address.afi == EIGRP_ADDRESS_FAMILY_IPV4
		    && memcmp(ei->address.address.bytes, &address->ip.v4,
			      sizeof(address->ip.v4)) == 0)
			return ei;
		if (address->afi == AF_INET6
		    && ei->address.address.afi == EIGRP_ADDRESS_FAMILY_IPV6
		    && memcmp(ei->address.address.bytes, &address->ip.v6,
			      sizeof(address->ip.v6)) == 0)
			return ei;
	}

	return NULL;
}

eigrp_interface_t *eigrp_intf_lookup_by_ifindex(eigrp_instance_t *eigrp,
						 eigrp_ifindex_t ifindex)
{
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (!eigrp || !ifindex)
		return NULL;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei))
		if (ei->ifindex == ifindex)
			return ei;
	return NULL;
}

eigrp_interface_t *eigrp_intf_lookup_by_vrf_ifindex(
	eigrp_vrf_id_t vrf_id, eigrp_ifindex_t ifindex)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (!ifindex)
		return NULL;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp)) {
		if (eigrp->vrf_id != vrf_id)
			continue;
		ei = eigrp_intf_lookup_by_ifindex(eigrp, ifindex);
		if (ei)
			return ei;
	}
	return NULL;
}

eigrp_interface_t *eigrp_intf_lookup_by_name(eigrp_instance_t *eigrp,
					     const char *if_name)
{
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (!eigrp || !if_name)
		return NULL;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei))
		if (ei->name && strcmp(ei->name, if_name) == 0)
			return ei;

	return NULL;
}


eigrp_ifindex_t eigrp_interface_ifindex(const eigrp_interface_t *ei)
{
	return ei ? ei->ifindex : 0;
}

const char *eigrp_interface_name(const eigrp_interface_t *ei)
{
	return ei ? ei->name : NULL;
}

eigrp_result_t eigrp_interface_address_read(const eigrp_interface_t *ei,
					     eigrp_prefix_t *address)
{
	if (!ei || !address)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	*address = ei->address;
	return EIGRP_RESULT_SUCCESS;
}
