// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP redistribution configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdlib.h>
#include <string.h>

#include "eigrp_redistribute.h"
#include "eigrp_rib.h"

eigrp_result_t eigrp_topology_redistributed_route_update(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route,
	const eigrp_metrics_t *metric);
eigrp_result_t eigrp_topology_redistributed_route_remove(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route);

struct eigrp_redistribute_config {
	eigrp_redistribute_source_t source;
	bool metric_configured;
	eigrp_metric_values_t metric;
	char *route_map;
	eigrp_redistribute_config_t *next;
};

struct eigrp_redistribute_policy_config {
	bool maximum_prefix_configured;
	eigrp_prefix_limit_t maximum_prefix;
};

typedef enum eigrp_redistribute_metric_origin {
	EIGRP_REDISTRIBUTE_METRIC_NONE = 0,
	EIGRP_REDISTRIBUTE_METRIC_EXPLICIT,
	EIGRP_REDISTRIBUTE_METRIC_SOURCE_EIGRP,
	EIGRP_REDISTRIBUTE_METRIC_DEFAULT,
} eigrp_redistribute_metric_origin_t;

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

static bool eigrp_redistribute_source_valid(
	const eigrp_redistribute_source_t *source)
{
	if (!source)
		return false;
	return source->protocol > EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED
	       && source->protocol <= EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
}

static bool eigrp_redistribute_source_equal(
	const eigrp_redistribute_source_t *left,
	const eigrp_redistribute_source_t *right)
{
	return left && right && left->protocol == right->protocol
	       && left->route_instance == right->route_instance;
}

static eigrp_result_t eigrp_redistribute_validate(
	eigrp_instance_context_t *context,
	const eigrp_redistribute_source_t *source,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	if (!eigrp_redistribute_source_valid(source))
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
	eigrp_address_family_config_t *af,
	const eigrp_redistribute_source_t *source)
{
	eigrp_redistribute_config_t *config;

	if (!af || !source)
		return NULL;
	for (config = af->redistributions; config; config = config->next)
		if (eigrp_redistribute_source_equal(&config->source, source))
			return config;
	return NULL;
}

static bool eigrp_redistribute_runtime_result_committable(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS
	       || result == EIGRP_RESULT_NOT_IMPLEMENTED;
}

static bool eigrp_redistribute_native_vector_usable(
	const eigrp_rib_source_route_t *route)
{
	return route && route->source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP
	       && route->eigrp_vector_present && route->eigrp_vector.bandwidth != 0;
}

static eigrp_redistribute_metric_origin_t eigrp_redistribute_metric_select(
	const eigrp_address_family_config_t *af,
	const eigrp_redistribute_config_t *config,
	const eigrp_rib_source_route_t *route, eigrp_metrics_t *metric)
{
	eigrp_metric_values_t values;

	if (!metric)
		return EIGRP_REDISTRIBUTE_METRIC_NONE;
	memset(metric, 0, sizeof(*metric));
	if (!config || !route)
		return EIGRP_REDISTRIBUTE_METRIC_NONE;

	if (config->metric_configured) {
		eigrp_metric_values_convert(&config->metric, metric);
		return EIGRP_REDISTRIBUTE_METRIC_EXPLICIT;
	}
	if (eigrp_redistribute_native_vector_usable(route)) {
		*metric = route->eigrp_vector;
		return EIGRP_REDISTRIBUTE_METRIC_SOURCE_EIGRP;
	}
	if (eigrp_metric_default_get(af, &values)) {
		eigrp_metric_values_convert(&values, metric);
		return EIGRP_REDISTRIBUTE_METRIC_DEFAULT;
	}
	return EIGRP_REDISTRIBUTE_METRIC_NONE;
}

/*
 * Named redistribution owns retained configuration.  Host subscription state
 * is keyed only by the exact EIGRP-owned {protocol, route-instance} source.
 */
eigrp_result_t eigrp_redistribute_add(
	eigrp_instance_context_t *context, const eigrp_redistribute_source_t *source,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	eigrp_redistribute_config_t *config = NULL;
	eigrp_redistribute_config_t *new_config = NULL;
	char *new_route_map = NULL;
	eigrp_result_t result;

	result = eigrp_redistribute_validate(context, source, metric, route_map);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	if (route_map) {
		new_route_map = eigrp_redistribute_string_duplicate(route_map);
		if (!new_route_map)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	if (context->config) {
		config = eigrp_redistribute_config_find(context->config, source);
		if (!config) {
			new_config = calloc(1, sizeof(*new_config));
			if (!new_config) {
				free(new_route_map);
				return EIGRP_RESULT_INTERNAL_FAILURE;
			}
			new_config->source = *source;
			new_config->route_map = new_route_map;
			new_route_map = NULL;
			new_config->metric_configured = metric != NULL;
			if (metric)
				new_config->metric = *metric;
		}
	}

	result = EIGRP_RESULT_SUCCESS;
	if (context->runtime && !eigrp_instance_data_path_ready(context->runtime))
		result = EIGRP_RESULT_NOT_IMPLEMENTED;
	else if (context->runtime)
		result = eigrp_rib_redistribute_add(context->runtime, source);
	if (!eigrp_redistribute_runtime_result_committable(result)) {
		free(new_route_map);
		if (new_config) {
			free(new_config->route_map);
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

eigrp_result_t eigrp_redistribute_remove(
	eigrp_instance_context_t *context, const eigrp_redistribute_source_t *source)
{
	eigrp_redistribute_config_t **cursor = NULL;
	eigrp_redistribute_config_t *config = NULL;
	eigrp_result_t result = EIGRP_RESULT_NOT_FOUND;

	if (!eigrp_redistribute_source_valid(source))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	if (context->config) {
		for (cursor = &context->config->redistributions; *cursor;
		     cursor = &(*cursor)->next) {
			if (eigrp_redistribute_source_equal(&(*cursor)->source, source)) {
				config = *cursor;
				break;
			}
		}
		if (!config)
			return EIGRP_RESULT_NOT_FOUND;
	}

	if (context->runtime) {
		result = !eigrp_instance_data_path_ready(context->runtime)
			 ? EIGRP_RESULT_NOT_IMPLEMENTED
			 : eigrp_rib_redistribute_remove(context->runtime, source);
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND
		    && result != EIGRP_RESULT_NOT_IMPLEMENTED)
			return result;
	}

	if (config) {
		*cursor = config->next;
		free(config->route_map);
		free(config);
		if (result == EIGRP_RESULT_NOT_FOUND || !context->runtime)
			result = EIGRP_RESULT_SUCCESS;
	}
	return result;
}

static eigrp_result_t eigrp_redistribute_source_route_receive(
	eigrp_instance_t *runtime, const eigrp_rib_source_route_t *route,
	bool importing)
{
	eigrp_address_family_config_t *af;
	eigrp_redistribute_config_t *config;
	eigrp_metrics_t metric;

	if (!runtime || !route || !eigrp_redistribute_source_valid(&route->source))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_instance_data_path_ready(runtime))
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	af = eigrp_instance_runtime_config(runtime);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	config = eigrp_redistribute_config_find(af, &route->source);
	if (!config)
		return EIGRP_RESULT_SUCCESS;

	if (!importing)
		return eigrp_topology_redistributed_route_remove(runtime, route);

	if (importing && config->route_map)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	if (eigrp_redistribute_metric_select(af, config, route, &metric)
	    == EIGRP_REDISTRIBUTE_METRIC_NONE)
		return EIGRP_RESULT_SUCCESS;
	return eigrp_topology_redistributed_route_update(runtime, route, &metric);
}

/* Zebra uses ADD for both first appearance and changed route snapshots. */
eigrp_result_t eigrp_rib_source_route_add(
	eigrp_instance_t *runtime, const eigrp_rib_source_route_t *route)
{
	return eigrp_redistribute_source_route_receive(runtime, route, true);
}

eigrp_result_t eigrp_rib_source_route_remove(
	eigrp_instance_t *runtime, const eigrp_rib_source_route_t *route)
{
	return eigrp_redistribute_source_route_receive(runtime, route, false);
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
		free(config);
	}
	af->redistributions = NULL;
}



/*
 * Syntax:
 *   Named: `redistribute maximum-prefix LIMIT [...]` / `no redistribute maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or removes the redistribution prefix-limit policy.
 * Retained configuration stays here while enforcement remains a structured NOT_IMPLEMENTED runtime path.
 */
eigrp_result_t eigrp_redistribute_maximum_prefix_set(
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

/*
 * Syntax:
 *   Named: `redistribute maximum-prefix LIMIT [...]` / `no redistribute maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or removes the redistribution prefix-limit policy.
 * Retained configuration stays here while enforcement remains a structured NOT_IMPLEMENTED runtime path.
 */
eigrp_result_t eigrp_redistribute_maximum_prefix_reset(
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
