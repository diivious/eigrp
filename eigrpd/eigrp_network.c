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
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_topology.h"

struct eigrp_network_config {
	eigrp_prefix_t prefix;
	eigrp_network_config_t *next;
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
			result = eigrp_southbound_network_create(
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
			runtime_result = eigrp_southbound_network_delete(
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

eigrp_result_t eigrp_network_create(eigrp_instance_context_t *context,
				    const eigrp_prefix_t *prefix)
{
	return eigrp_network_process(context, prefix,
				     EIGRP_NETWORK_OPERATION_CREATE, NULL);
}

eigrp_result_t eigrp_network_delete(eigrp_instance_context_t *context,
				    const eigrp_prefix_t *prefix)
{
	return eigrp_network_process(context, prefix,
				     EIGRP_NETWORK_OPERATION_DELETE, NULL);
}

void eigrp_external_routes_refresh(eigrp_instance_t *eigrp, int type)
{
}
