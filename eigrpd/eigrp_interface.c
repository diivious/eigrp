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
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_summary.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_INTF,      "EIGRP interface");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_INTF_INFO, "EIGRP Interface Information");

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
	struct listnode *node;
	unsigned long count = 0;

	if (!ei || !ei->nbrs)
		return 0;

	for (ALL_LIST_ELEMENTS_RO(ei->nbrs, node, nbr)) {
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
	}

	return callback(&state, arg);
}

eigrp_result_t eigrp_interface_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, eigrp_interface_state_walk_cb callback,
	void *arg)
{
	eigrp_interface_t *ei;
	eigrp_interface_config_t *configured;
	struct listnode *node;
	bool matched = false;
	eigrp_result_t result;

	if (!callback || (!config && !runtime))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (runtime && runtime->eiflist) {
		for (ALL_LIST_ELEMENTS_RO(runtime->eiflist, node, ei)) {
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

static bool eigrp_interface_context_valid(const eigrp_interface_context_t *context)
{
	return context && (context->config || context->runtime);
}

eigrp_result_t eigrp_interface_bandwidth_percent_update(
	eigrp_interface_context_t *context, uint32_t percent)
{
	if (percent == 0 || percent > 999999)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	context->config->bandwidth_percent = percent;
	context->config->bandwidth_percent_configured = true;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_interface_bandwidth_percent_delete(
	eigrp_interface_context_t *context)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	context->config->bandwidth_percent = 0;
	context->config->bandwidth_percent_configured = false;
	return EIGRP_RESULT_SUCCESS;
}

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

eigrp_result_t eigrp_interface_hello_interval_update(
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

eigrp_result_t eigrp_interface_hello_interval_delete(
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

eigrp_result_t eigrp_interface_hold_time_update(eigrp_interface_context_t *context,
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

eigrp_result_t eigrp_interface_hold_time_delete(eigrp_interface_context_t *context)
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

eigrp_result_t eigrp_interface_passive_update(eigrp_interface_context_t *context,
					      bool passive)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config)
		context->config->passive = passive;
	if (context->runtime)
		context->runtime->params.passive_interface =
			passive ? EIGRP_INTF_PASSIVE : EIGRP_INTF_ACTIVE;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_interface_next_hop_self_update(
	eigrp_interface_context_t *context, bool enabled)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	context->config->next_hop_self = enabled;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_interface_split_horizon_update(
	eigrp_interface_context_t *context, bool enabled)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	context->config->split_horizon = enabled;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_interface_shutdown_update(eigrp_interface_context_t *context,
					       bool shutdown)
{
	if (!eigrp_interface_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	context->config->shutdown = shutdown;
	return EIGRP_RESULT_SUCCESS;
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
		eigrp_tlv1_interface_bind(ei, &ei->eigrp->tlv1_codec);
		return;
	}

	if (ei->tlv2_peer_count) {
		eigrp_tlv2_interface_bind(ei, &ei->eigrp->tlv2_codec);
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
		listnode_delete(eigrp->oi_write_q, ei);
		if (list_isempty(eigrp->oi_write_q))
			event_cancel(&(eigrp->t_write));
		ei->on_write_q = 0;
	}
}

static uint8_t eigrp_intf_settype(struct interface *ifp)
{
	if (if_is_pointopoint(ifp))
		return EIGRP_IFTYPE_POINTOPOINT;
	else if (if_is_loopback(ifp))
		return EIGRP_IFTYPE_LOOPBACK;
	else
		return EIGRP_IFTYPE_BROADCAST;
}

const char *eigrp_intf_name_string(eigrp_interface_t *ei)
{
	if (!ei)
		return "inactive";

	return ei->ifp->name;
}

eigrp_interface_t *eigrp_intf_new(eigrp_instance_t *eigrp, struct interface *ifp,
				  struct prefix *p)
{
	eigrp_interface_t *ei = ifp->info;
	int i;

	if (ei)
		return ei;

	ei = XCALLOC(MTYPE_EIGRP_INTF, sizeof(eigrp_interface_t));

	/* Set zebra interface pointer. */
	ei->ifp = ifp;

	/* Relate eigrp interface to eigrp instance. */
	ei->eigrp = eigrp;

	prefix_copy(&ei->address, p);

	ifp->info = ei;
	listnode_add(eigrp->eiflist, ei);

	ei->type = EIGRP_IFTYPE_BROADCAST;

	/* Initialize neighbor list. */
	ei->nbrs = list_new();

	ei->crypt_seqnum = time(NULL);
	eigrp_interface_encoder_clear(ei);

	/* Initialize lists */
	for (i = 0; i < EIGRP_FILTER_MAX; i++) {
		ei->list[i] = NULL;
		ei->prefix[i] = NULL;
		ei->routemap[i] = NULL;
	}

	ei->params.v_hello = EIGRP_HELLO_INTERVAL_DEFAULT;
	ei->params.v_wait = EIGRP_HOLD_INTERVAL_DEFAULT;
	ei->params.bandwidth = EIGRP_BANDWIDTH_DEFAULT;
	ei->params.delay = EIGRP_DELAY_DEFAULT;
	ei->params.reliability = EIGRP_RELIABILITY_DEFAULT;
	ei->params.load = EIGRP_LOAD_DEFAULT;
	ei->params.auth_type = EIGRP_AUTH_TYPE_NONE;
	ei->params.auth_keychain = NULL;

	ei->curr_bandwidth = ifp->bandwidth;
	ei->curr_mtu = ifp->mtu;

	return ei;
}

int eigrp_intf_delete_hook(struct interface *ifp)
{
	eigrp_interface_t *ei = ifp->info;
	eigrp_instance_t *eigrp;

	if (!ei)
		return 0;

	list_delete(&ei->nbrs);

	eigrp = ei->eigrp;
	listnode_delete(eigrp->eiflist, ei);

	eigrp_packet_queue_free(ei->obuf);

	XFREE(MTYPE_EIGRP_INTF_INFO, ifp->info);

	return 0;
}

static int eigrp_ifp_create(struct interface *ifp)
{
	struct listnode *node, *nnode;
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei = ifp->info;

	if (ei) {
	    ei->params.type = eigrp_intf_settype(ifp);

	    for (ALL_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		eigrp_intf_update(eigrp, ifp);
	    }
	}

	return 0;
}

static int eigrp_ifp_up(struct interface *ifp)
{
	eigrp_interface_t *ei = ifp->info;

	if (IS_DEBUG_EIGRP(zebra, ZEBRA_INTERFACE))
		zlog_debug("Zebra: Interface[%s] state change to up.",
			   ifp->name);

	if (!ei)
		return 0;

	if (ei->curr_bandwidth != ifp->bandwidth) {
		if (IS_DEBUG_EIGRP(zebra, ZEBRA_INTERFACE))
			zlog_debug(
				"Zebra: Interface[%s] bandwidth change %d -> %d.",
				ifp->name, ei->curr_bandwidth, ifp->bandwidth);

		ei->curr_bandwidth = ifp->bandwidth;
		// eigrp_intf_recalculate_output_cost (ifp);
	}

	if (ei->curr_mtu != ifp->mtu) {
		if (IS_DEBUG_EIGRP(zebra, ZEBRA_INTERFACE))
			zlog_debug("Zebra: Interface[%s] MTU change %u -> %u.",
				   ifp->name, ei->curr_mtu, ifp->mtu);

		ei->curr_mtu = ifp->mtu;
		/* Must reset the interface (simulate down/up) when MTU
		 * changes. */
		eigrp_intf_reset(ifp);
		return 0;
	}

	eigrp_intf_up(ei->eigrp, ifp->info);

	return 0;
}

static int eigrp_ifp_down(struct interface *ifp)
{
	if (IS_DEBUG_EIGRP(zebra, ZEBRA_INTERFACE))
		zlog_debug("Zebra: Interface[%s] state change to down.",
			   ifp->name);

	if (ifp->info)
		eigrp_intf_down(ifp->info);

	return 0;
}

static int eigrp_ifp_destroy(struct interface *ifp)
{
	eigrp_interface_t *ei;

	if (if_is_up(ifp))
		zlog_warn("Zebra: got delete of %s, but interface is still up",
			  ifp->name);

	if (IS_DEBUG_EIGRP(zebra, ZEBRA_INTERFACE))
		zlog_debug(
			"Zebra: interface delete %s index %d flags %llx metric %d mtu %d",
			ifp->name, ifp->ifindex, (unsigned long long)ifp->flags,
			ifp->metric, ifp->mtu);

	if (ifp->info) {
		ei = ifp->info;
		eigrp_intf_free(ei->eigrp, ei, INTERFACE_DOWN_BY_ZEBRA);
	}

	return 0;
}

struct list *eigrp_iflist;

void eigrp_intf_init(void)
{
	hook_register_prio(if_real, 0, eigrp_ifp_create);
	hook_register_prio(if_up, 0, eigrp_ifp_up);
	hook_register_prio(if_down, 0, eigrp_ifp_down);
	hook_register_prio(if_unreal, 0, eigrp_ifp_destroy);
	// hook_register_prio(if_add, 0, eigrp_intf_new);
	hook_register_prio(if_del, 0, eigrp_intf_delete_hook);
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
	eigrp_interface_t *ei2;
	struct listnode *node, *nnode;

	eigrp_adjust_sndbuflen(eigrp, ei->ifp->mtu);
	eigrp_intf_stream_set(ei);

	/* Set multicast memberships appropriately for new state. */
	eigrp_intf_set_multicast(ei);

	event_add_event(eigrpd_event, eigrp_hello_timer, ei, (1), &ei->t_hello);

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

	struct prefix dest_addr;

	dest_addr = ei->address;
	apply_mask(&dest_addr);
	prefix = eigrp_topology_table_lookup_ipv4(eigrp->topology_table, &dest_addr);

	if (prefix == NULL) {
		prefix = eigrp_topology_prefix_create();
		prefix->serno = eigrp->serno;
		prefix->destination = (struct prefix *)prefix_ipv4_new();
		prefix_copy(prefix->destination, &dest_addr);
		prefix->nt = EIGRP_TOPOLOGY_TYPE_CONNECTED;
		prefix->reported_metric = metric;
		prefix->state = EIGRP_FSM_STATE_PASSIVE;
		prefix->fdistance = eigrp_calculate_metrics(eigrp, metric);
		prefix->req_action |= EIGRP_FSM_NEED_UPDATE;

		eigrp_prefix_descriptor_add(eigrp->topology_table, prefix);

		listnode_add(eigrp->topology_changes, prefix);

		route->prefix = prefix;
		eigrp_route_descriptor_add(eigrp, prefix, route);

		for (ALL_LIST_ELEMENTS(eigrp->eiflist, node, nnode, ei2)) {
			eigrp_update_send(eigrp, eigrp->neighbor_self, ei2);
		}

		prefix->req_action &= ~EIGRP_FSM_NEED_UPDATE;
		listnode_delete(eigrp->topology_changes, prefix);

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
	struct listnode *node, *nnode;
	eigrp_neighbor_t *nbr;

	if (ei == NULL)
		return 0;

	/* Shutdown packet reception and sending */
	if (ei->t_hello)
		event_cancel(&ei->t_hello);

	eigrp_intf_stream_unset(ei);

	/*Set infinite metrics to routes learned by this interface and start
	 * query process*/
	for (ALL_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
		eigrp_nbr_delete(nbr);
	}
	eigrp_interface_encoder_clear(ei);

	return 1;
}

bool eigrp_intf_is_passive(eigrp_interface_t *ei)
{
	if (ei->params.passive_interface == EIGRP_INTF_ACTIVE)
		return false;

	if (ei->eigrp->passive_interface_default == EIGRP_INTF_ACTIVE)
		return false;

	return true;
}

void eigrp_intf_set_multicast(eigrp_interface_t *ei)
{
	if (!eigrp_intf_is_passive(ei)) {
		/* The interface should belong to the EIGRP-all-routers group.
		 */
		if (!ei->member_allrouters
		    && (eigrp_intf_add_allspfrouters(ei->eigrp, &ei->address,
						     ei->ifp->ifindex)
			>= 0))
			/* Set the flag only if the system call to join
			 * succeeded. */
			ei->member_allrouters = true;
	} else {
		/* The interface should NOT belong to the EIGRP-all-routers
		 * group. */
		if (ei->member_allrouters) {
			/* Only actually drop if this is the last reference */
			eigrp_intf_drop_allspfrouters(ei->eigrp, &ei->address,
						      ei->ifp->ifindex);
			/* Unset the flag regardless of whether the system call
			   to leave
			   the group succeeded, since it's much safer to assume
			   that
			   we are not a member. */
			ei->member_allrouters = false;
		}
	}
}

void eigrp_intf_free(eigrp_instance_t *eigrp, eigrp_interface_t *ei, int source)
{
	struct prefix dest_addr;
	eigrp_prefix_descriptor_t *pe;

	if (source == INTERFACE_DOWN_BY_VTY) {
		event_cancel(&ei->t_hello);
		eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);
	}

	dest_addr = ei->address;
	apply_mask(&dest_addr);
	pe = eigrp_topology_table_lookup_ipv4(eigrp->topology_table,
					      &dest_addr);
	if (pe)
		eigrp_prefix_descriptor_delete(eigrp, eigrp->topology_table,
					       pe);

	eigrp_intf_down(ei);

	listnode_delete(ei->eigrp->eiflist, ei);
}

void eigrp_interface_runtime_reset(eigrp_interface_t *ei)
{
	if (!ei)
		return;

	eigrp_intf_down(ei);
	eigrp_intf_up(ei->eigrp, ei);
}

/* FRR-facing compatibility wrapper.  Classic FRR callbacks continue to set
 * the runtime parameter directly and enter the same EIGRP-owned reset path.
 */
void eigrp_intf_reset(struct interface *ifp)
{
	if (!ifp)
		return;

	eigrp_interface_runtime_reset(ifp->info);
}

eigrp_interface_t *eigrp_intf_lookup_by_local_addr(eigrp_instance_t *eigrp,
						   struct interface *ifp,
						   struct in_addr address)
{
	struct listnode *node;
	eigrp_interface_t *ei;

	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		if (ifp && ei->ifp != ifp)
			continue;

		if (IPV4_ADDR_SAME(&address, &ei->address.u.prefix4))
			return ei;
	}

	return NULL;
}

/**
 * @fn eigrp_intf_lookup_by_name
 *
 * @param[in]		eigrp		EIGRP process
 * @param[in]		if_name 	Name of the interface
 *
 * @return eigrp_interface_t *
 *
 * @par
 * Function is used for lookup interface by name.
 */
eigrp_interface_t *eigrp_intf_lookup_by_name(eigrp_instance_t *eigrp,
					     const char *if_name)
{
	eigrp_interface_t *ei;
	struct listnode *node;

	/* iterate over all eigrp interfaces */
	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		/* compare int name with eigrp interface's name */
		if (strcmp(ei->ifp->name, if_name) == 0) {
			return ei;
		}
	}

	return NULL;
}
