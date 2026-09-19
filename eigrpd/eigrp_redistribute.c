// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP redistribution configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdlib.h>
#include <string.h>

#include "eigrp_redistribute.h"
#include "eigrp_southbound.h"

struct eigrp_redistribute_config {
	char *protocol;
	bool metric_configured;
	eigrp_metric_values_t metric;
	char *route_map;
	eigrp_redistribute_config_t *next;
};

struct eigrp_redistribute_policy_config {
	bool maximum_prefix_configured;
	eigrp_prefix_limit_t maximum_prefix;
};

static char *eigrp_redistribute_string_duplicate(const char *value)
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

static eigrp_result_t eigrp_redistribute_validate(
	eigrp_instance_context_t *context, const char *protocol,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	if (!protocol || !protocol[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (metric && (!metric->bandwidth || !metric->load || !metric->mtu))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (route_map && !route_map[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_redistribute_config_t *eigrp_redistribute_config_find(
	eigrp_address_family_config_t *af, const char *protocol)
{
	eigrp_redistribute_config_t *config;

	if (!af || !protocol)
		return NULL;
	for (config = af->redistributions; config; config = config->next) {
		if (strcmp(config->protocol, protocol) == 0)
			return config;
	}
	return NULL;
}

static bool eigrp_redistribute_runtime_result_committable(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS
	       || result == EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_redistribute_update(eigrp_instance_context_t *context,
					 const char *protocol,
					 const eigrp_metric_values_t *metric,
					 const char *route_map)
{
	eigrp_redistribute_config_t *config = NULL;
	eigrp_redistribute_config_t *new_config = NULL;
	char *new_route_map = NULL;
	eigrp_result_t result;

	result = eigrp_redistribute_validate(context, protocol, metric, route_map);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	/* Allocate all retained configuration before changing runtime state. */
	if (route_map) {
		new_route_map = eigrp_redistribute_string_duplicate(route_map);
		if (!new_route_map)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	if (context->config) {
		config = eigrp_redistribute_config_find(context->config, protocol);
		if (!config) {
			new_config = calloc(1, sizeof(*new_config));
			if (!new_config) {
				free(new_route_map);
				return EIGRP_RESULT_INTERNAL_FAILURE;
			}
			new_config->protocol =
				eigrp_redistribute_string_duplicate(protocol);
			if (!new_config->protocol) {
				free(new_route_map);
				free(new_config);
				return EIGRP_RESULT_INTERNAL_FAILURE;
			}
			new_config->route_map = new_route_map;
			new_route_map = NULL;
			new_config->metric_configured = metric != NULL;
			if (metric)
				new_config->metric = *metric;
		}
	}

	result = EIGRP_RESULT_SUCCESS;
	if (context->runtime)
		result = eigrp_southbound_redistribute_update(
			context->runtime, protocol, metric, route_map);
	if (!eigrp_redistribute_runtime_result_committable(result)) {
		free(new_route_map);
		if (new_config) {
			free(new_config->route_map);
			free(new_config->protocol);
			free(new_config);
		}
		return result;
	}

	if (context->config) {
		if (config) {
			free(config->route_map);
			config->route_map = new_route_map;
			new_route_map = NULL;
			config->metric_configured = metric != NULL;
			memset(&config->metric, 0, sizeof(config->metric));
			if (metric)
				config->metric = *metric;
		} else if (new_config) {
			new_config->next = context->config->redistributions;
			context->config->redistributions = new_config;
			new_config = NULL;
		}
	}

	free(new_route_map);
	return result;
}

eigrp_result_t eigrp_redistribute_delete(eigrp_instance_context_t *context,
					 const char *protocol)
{
	eigrp_redistribute_config_t **cursor = NULL;
	eigrp_redistribute_config_t *config = NULL;
	eigrp_result_t result = EIGRP_RESULT_NOT_FOUND;

	if (!protocol || !protocol[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	if (context->config) {
		for (cursor = &context->config->redistributions; *cursor;
		     cursor = &(*cursor)->next) {
			if (strcmp((*cursor)->protocol, protocol) == 0) {
				config = *cursor;
				break;
			}
		}
		/* Named retained state owns whether this command has a host
		 * subscription to remove.  Do not tear down a subscription that may
		 * belong to the unchanged classic surface.
		 */
		if (!config)
			return EIGRP_RESULT_NOT_FOUND;
	}

	if (context->runtime) {
		result = eigrp_southbound_redistribute_delete(context->runtime,
								      protocol);
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND
		    && result != EIGRP_RESULT_NOT_IMPLEMENTED)
			return result;
	}

	if (config) {
		*cursor = config->next;
		free(config->route_map);
		free(config->protocol);
		free(config);
		if (result == EIGRP_RESULT_NOT_FOUND)
			result = EIGRP_RESULT_SUCCESS;
		else if (!context->runtime)
			result = EIGRP_RESULT_SUCCESS;
	}

	return result;
}

void eigrp_redistribute_config_delete_all(eigrp_address_family_config_t *af)
{
	eigrp_redistribute_config_t *config;
	eigrp_redistribute_config_t *next;

	if (!af)
		return;
	for (config = af->redistributions; config; config = next) {
		next = config->next;
		free(config->route_map);
		free(config->protocol);
		free(config);
	}
	af->redistributions = NULL;
}


eigrp_result_t eigrp_redistribute_maximum_prefix_update(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit)
{
	if (!limit || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		if (!context->config->redistribute_policy) {
			context->config->redistribute_policy =
				calloc(1, sizeof(*context->config->redistribute_policy));
			if (!context->config->redistribute_policy)
				return EIGRP_RESULT_INTERNAL_FAILURE;
		}
		context->config->redistribute_policy->maximum_prefix = *limit;
		context->config->redistribute_policy->maximum_prefix_configured = true;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_redistribute_maximum_prefix_delete(
	eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->redistribute_policy) {
		context->config->redistribute_policy->maximum_prefix_configured = false;
		memset(&context->config->redistribute_policy->maximum_prefix, 0,
		       sizeof(context->config->redistribute_policy->maximum_prefix));
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

void eigrp_redistribute_policy_delete_all(eigrp_address_family_config_t *af)
{
	if (!af)
		return;
	free(af->redistribute_policy);
	af->redistribute_policy = NULL;
}
