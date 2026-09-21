// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Network Related Functions.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */

#include <stdlib.h>
#include <string.h>
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_topology.h"

struct eigrp_network_config {
	eigrp_prefix_t prefix;
	eigrp_network_config_t *next;
};

struct eigrp_network_runtime {
	eigrp_prefix_t prefix;
	struct eigrp_network_runtime *next;
};

typedef enum eigrp_network_operation {
	EIGRP_NETWORK_OPERATION_CREATE = 0,
	EIGRP_NETWORK_OPERATION_DELETE,
} eigrp_network_operation_t;

static bool eigrp_network_address_equal(const eigrp_address_t *a,
					const eigrp_address_t *b)
{
	return a && b && a->afi == b->afi
	       && memcmp(a->bytes, b->bytes, sizeof(a->bytes)) == 0;
}

static bool eigrp_network_prefix_equal(const eigrp_prefix_t *a,
				       const eigrp_prefix_t *b)
{
	return a && b && a->prefix_length == b->prefix_length
	       && eigrp_network_address_equal(&a->address, &b->address);
}

static eigrp_result_t eigrp_network_config_create(
	eigrp_address_family_config_t *af, const eigrp_prefix_t *prefix,
	bool *changed)
{
	eigrp_network_config_t *network;

	if (changed)
		*changed = false;
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;

	for (network = af->networks; network; network = network->next) {
		if (!eigrp_network_prefix_equal(&network->prefix, prefix))
			continue;
		return EIGRP_RESULT_SUCCESS;
	}

	network = calloc(1, sizeof(*network));
	if (!network)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	network->prefix = *prefix;
	network->next = af->networks;
	af->networks = network;
	if (changed)
		*changed = true;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_network_config_delete(
	eigrp_address_family_config_t *af, const eigrp_prefix_t *prefix,
	bool *changed)
{
	eigrp_network_config_t **cursor;
	eigrp_network_config_t *network;

	if (changed)
		*changed = false;
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;

	for (cursor = &af->networks; *cursor; cursor = &(*cursor)->next) {
		network = *cursor;
		if (!eigrp_network_prefix_equal(&network->prefix, prefix))
			continue;
		*cursor = network->next;
		free(network);
		if (changed)
			*changed = true;
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

void eigrp_network_config_delete_all(eigrp_address_family_config_t *af)
{
	eigrp_network_config_t *network;
	eigrp_network_config_t *next;

	if (!af)
		return;
	for (network = af->networks; network; network = next) {
		next = network->next;
		free(network);
	}
	af->networks = NULL;
}

static bool eigrp_network_runtime_matches(
	const eigrp_instance_t *eigrp, const eigrp_prefix_t *connected)
{
	const struct eigrp_network_runtime *network;

	if (!eigrp || !connected || !eigrp->af_vectors.network_interface_match)
		return false;

	for (network = eigrp->networks; network; network = network->next) {
		if (eigrp->af_vectors.network_interface_match(&network->prefix,
						      connected))
			return true;
	}
	return false;
}

eigrp_result_t eigrp_network_runtime_exists(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix, bool *exists)
{
	struct eigrp_network_runtime *network;

	if (!exists || !prefix || !eigrp_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	*exists = false;
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;

	for (network = eigrp->networks; network; network = network->next) {
		if (!eigrp_network_prefix_equal(&network->prefix, prefix))
			continue;
		*exists = true;
		break;
	}
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_network_runtime_create(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix, bool *changed)
{
	struct eigrp_network_runtime *network;
	bool exists;
	eigrp_result_t result;

	if (changed)
		*changed = false;
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;

	result = eigrp_network_runtime_exists(eigrp, prefix, &exists);
	if (result != EIGRP_RESULT_SUCCESS || exists)
		return result;

	network = calloc(1, sizeof(*network));
	if (!network)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	network->prefix = *prefix;
	network->next = eigrp->networks;
	eigrp->networks = network;

	if (eigrp->router_id.s_addr == INADDR_ANY)
		eigrp_router_id_update(eigrp);
	else
		eigrp_network_interfaces_refresh(eigrp);

	if (changed)
		*changed = true;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_network_runtime_delete(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix, bool *changed)
{
	struct eigrp_network_runtime **cursor;
	struct eigrp_network_runtime *network;
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;
	eigrp_list_node_t *next;
	bool found = false;

	if (changed)
		*changed = false;
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (!prefix || !eigrp_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	for (cursor = &eigrp->networks; *cursor; cursor = &(*cursor)->next) {
		network = *cursor;
		if (!eigrp_network_prefix_equal(&network->prefix, prefix))
			continue;
		*cursor = network->next;
		free(network);
		found = true;
		break;
	}
	if (!found)
		return EIGRP_RESULT_NOT_FOUND;

	for (EIGRP_LIST_ELEMENTS(eigrp->eiflist, node, next, ei)) {
		if (!eigrp_network_runtime_matches(eigrp, &ei->address))
			eigrp_intf_free(eigrp, ei, EIGRP_INTERFACE_REMOVE_CONFIG);
	}

	if (changed)
		*changed = true;
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_network_runtime_delete_all(eigrp_instance_t *eigrp)
{
	struct eigrp_network_runtime *network;
	struct eigrp_network_runtime *next;

	if (!eigrp)
		return;
	for (network = eigrp->networks; network; network = next) {
		next = network->next;
		free(network);
	}
	eigrp->networks = NULL;
}

static void eigrp_network_interface_walk_refresh(
	const eigrp_interface_runtime_state_t *state, void *arg)
{
	eigrp_instance_t *eigrp = arg;

	if (!eigrp || !state || state->secondary
	    || eigrp->router_id.s_addr == INADDR_ANY
	    || !eigrp_network_runtime_matches(eigrp, &state->address))
		return;
	(void)eigrp_interface_runtime_refresh(eigrp, state);
}

void eigrp_network_interfaces_refresh(eigrp_instance_t *eigrp)
{
	if (!eigrp || !eigrp->data_path_ready
	    || eigrp->router_id.s_addr == INADDR_ANY)
		return;
	(void)eigrp_southbound_interface_walk(
		eigrp, eigrp_network_interface_walk_refresh, eigrp);
}

void eigrp_network_interface_refresh(
	eigrp_vrf_id_t vrf_id, const eigrp_interface_runtime_state_t *state)
{
	eigrp_instance_t *eigrp;
	eigrp_list_node_t *node;

	if (!state || state->secondary || !eigrp_prefix_valid(&state->address)
	    || !eigrp_om)
		return;

	for (EIGRP_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp)) {
		if (eigrp->vrf_id != vrf_id || !eigrp->data_path_ready
		    || eigrp->router_id.s_addr == INADDR_ANY
		    || !eigrp_network_runtime_matches(eigrp, &state->address))
			continue;
		(void)eigrp_interface_runtime_refresh(eigrp, state);
	}
}

static eigrp_result_t eigrp_network_process(eigrp_instance_context_t *context,
					    const eigrp_prefix_t *prefix,
					    eigrp_network_operation_t operation,
					    bool *runtime_changed);

static const eigrp_af_vectors_t *
eigrp_network_vectors(const eigrp_instance_context_t *context)
{
	if (!context)
		return NULL;
	if (context->config)
		return &context->config->af_vectors;
	if (context->runtime)
		return &context->runtime->af_vectors;
	return NULL;
}

static eigrp_result_t eigrp_network_validate(eigrp_instance_context_t *context,
					      const eigrp_prefix_t *prefix)
{
	const eigrp_af_vectors_t *vectors;

	if (!prefix)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	vectors = eigrp_network_vectors(context);
	if (!vectors || !vectors->network_validate)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	if (vectors->afi != prefix->address.afi)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	return vectors->network_validate(prefix);
}

static eigrp_result_t eigrp_network_process(eigrp_instance_context_t *context,
					    const eigrp_prefix_t *prefix,
					    eigrp_network_operation_t operation,
					    bool *runtime_changed)
{
	bool config_changed = false;
	bool changed = false;
	eigrp_result_t result;

	if (runtime_changed)
		*runtime_changed = false;

	result = eigrp_network_validate(context, prefix);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	if (operation == EIGRP_NETWORK_OPERATION_CREATE) {
		if (context->config) {
			result = eigrp_network_config_create(context->config, prefix,
							     &config_changed);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}

		if (context->runtime) {
			result = eigrp_network_runtime_create(
				context->runtime, prefix, &changed);
			if (result != EIGRP_RESULT_SUCCESS) {
				if (config_changed)
					(void)eigrp_network_config_delete(
						context->config, prefix, NULL);
				return result;
			}
		}
	} else {
		eigrp_result_t config_result = EIGRP_RESULT_NOT_FOUND;
		eigrp_result_t runtime_result = EIGRP_RESULT_NOT_FOUND;

		if (context->config) {
			config_result = eigrp_network_config_delete(
				context->config, prefix, &config_changed);
			if (config_result != EIGRP_RESULT_SUCCESS
			    && config_result != EIGRP_RESULT_NOT_FOUND)
				return config_result;
		}

		if (context->runtime) {
			runtime_result = eigrp_network_runtime_delete(
				context->runtime, prefix, &changed);
			if (runtime_result != EIGRP_RESULT_SUCCESS
			    && runtime_result != EIGRP_RESULT_NOT_FOUND) {
				if (config_changed)
					(void)eigrp_network_config_create(
						context->config, prefix, NULL);
				return runtime_result;
			}
		}

		/* Delete is idempotent across the retained-config and runtime views.
		 * If either side owned the network, the operation succeeded.
		 */
		if (config_result == EIGRP_RESULT_NOT_FOUND
		    && runtime_result == EIGRP_RESULT_NOT_FOUND)
			return EIGRP_RESULT_NOT_FOUND;
	}

	if (runtime_changed)
		*runtime_changed = changed;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `network NETWORK [WILDCARD]` / `no network NETWORK [WILDCARD]`
 *   Named: `network NETWORK [WILDCARD]` / `no network NETWORK [WILDCARD]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: IPv4 address-family mode
 * Description:
 * Enables or disables EIGRP on local IPv4 interfaces whose addresses match the network/wildcard.
 * The target owns matching and interface activation behavior used by named mode rather than recreating the classic logic in FRR.
 */
eigrp_result_t eigrp_network_create(eigrp_instance_context_t *context,
				    const eigrp_prefix_t *prefix)
{
	return eigrp_network_process(context, prefix,
				     EIGRP_NETWORK_OPERATION_CREATE, NULL);
}

/*
 * Syntax:
 *   Classic: `network NETWORK [WILDCARD]` / `no network NETWORK [WILDCARD]`
 *   Named: `network NETWORK [WILDCARD]` / `no network NETWORK [WILDCARD]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: IPv4 address-family mode
 * Description:
 * Enables or disables EIGRP on local IPv4 interfaces whose addresses match the network/wildcard.
 * The target owns matching and interface activation behavior used by named mode rather than recreating the classic logic in FRR.
 */
eigrp_result_t eigrp_network_delete(eigrp_instance_context_t *context,
				    const eigrp_prefix_t *prefix)
{
	return eigrp_network_process(context, prefix,
				     EIGRP_NETWORK_OPERATION_DELETE, NULL);
}
