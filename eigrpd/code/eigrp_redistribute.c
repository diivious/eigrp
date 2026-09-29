// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP redistribution configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp_redistribute.h"
#include "eigrp_structs.h"
#include "eigrp_table.h"
#include "eigrp_rib.h"
#include "eigrp_topology.h"
#include "eigrpd.h"

eigrp_result_t eigrp_topology_redistributed_route_update(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route,
	const eigrp_metrics_t *metric);
eigrp_result_t eigrp_topology_redistributed_route_remove(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *source_route);
eigrp_result_t eigrp_topology_redistributed_source_remove(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source);

struct eigrp_redist_config {
	eigrp_redist_source_t source;
	bool metric_configured;
	eigrp_metric_values_t metric;
	char *route_map;
	eigrp_redist_config_t *next;
};

struct eigrp_redist_policy_config {
	bool maximum_prefix_configured;
	eigrp_prefix_limit_t maximum_prefix;
};

typedef enum eigrp_redist_metric_origin {
	EIGRP_REDISTRIBUTE_METRIC_NONE = 0,
	EIGRP_REDISTRIBUTE_METRIC_EXPLICIT,
	EIGRP_REDISTRIBUTE_METRIC_SOURCE_EIGRP,
	EIGRP_REDISTRIBUTE_METRIC_DEFAULT,
} eigrp_redist_metric_origin_t;

static char *eigrp_redist_string_dup(const char *value)
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

static bool eigrp_redist_source_valid(
	const eigrp_redist_source_t *source)
{
	if (!source)
		return false;
	return source->protocol > EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED
	       && source->protocol <= EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
}

static bool eigrp_redist_source_match(
	const eigrp_redist_source_t *left,
	const eigrp_redist_source_t *right)
{
	return left && right && left->protocol == right->protocol
	       && left->route_instance == right->route_instance;
}

static eigrp_result_t eigrp_redist_validate(
	eigrp_instance_context_t *context,
	const eigrp_redist_source_t *source,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	if (!eigrp_redist_source_valid(source))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (metric && (!metric->bandwidth || !metric->load || !metric->mtu))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (route_map && !route_map[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_redist_config_t *eigrp_redist_config_lookup(
	eigrp_af_instance_t *af,
	const eigrp_redist_source_t *source)
{
	eigrp_redist_config_t *config;

	if (!af || !source)
		return NULL;
	for (config = af->redistributions; config; config = config->next)
		if (eigrp_redist_source_match(&config->source, source))
			return config;
	return NULL;
}

static bool eigrp_redist_runtime_result_committable(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS;
}

static bool eigrp_redist_native_vector_usable(
	const eigrp_rib_source_route_t *route)
{
	return route && route->source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP
	       && route->eigrp_vector_present && route->eigrp_vector.bandwidth != 0;
}

static eigrp_redist_metric_origin_t eigrp_redist_metric_select(
	const eigrp_af_instance_t *af,
	const eigrp_redist_config_t *config,
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
	if (eigrp_redist_native_vector_usable(route)) {
		*metric = route->eigrp_vector;
		return EIGRP_REDISTRIBUTE_METRIC_SOURCE_EIGRP;
	}
	if (eigrp_metric_default(af, &values)) {
		eigrp_metric_values_convert(&values, metric);
		return EIGRP_REDISTRIBUTE_METRIC_DEFAULT;
	}
	return EIGRP_REDISTRIBUTE_METRIC_NONE;
}

/*
 * Named redistribution owns retained configuration.  Host subscription state
 * is keyed only by the exact EIGRP-owned {protocol, route-instance} source.
 */
eigrp_result_t eigrp_redist_add(
	eigrp_instance_context_t *context, const eigrp_redist_source_t *source,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	eigrp_redist_config_t *config = NULL;
	eigrp_redist_config_t *new_config = NULL;
	char *new_route_map = NULL;
	eigrp_result_t result;

	result = eigrp_redist_validate(context, source, metric, route_map);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	if (route_map) {
		new_route_map = eigrp_redist_string_dup(route_map);
		if (!new_route_map)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	if (context->config) {
		config = eigrp_redist_config_lookup(context->config, source);
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
	if (context->runtime)
		result = eigrp_rib_redistribute_add(context->runtime, source);
	if (!eigrp_redist_runtime_result_committable(result)) {
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

eigrp_result_t eigrp_redist_remove(
	eigrp_instance_context_t *context, const eigrp_redist_source_t *source)
{
	eigrp_redist_config_t **cursor = NULL;
	eigrp_redist_config_t *config = NULL;
	eigrp_result_t result = EIGRP_RESULT_NOT_FOUND;

	if (!eigrp_redist_source_valid(source))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	if (context->config) {
		for (cursor = &context->config->redistributions; *cursor;
		     cursor = &(*cursor)->next) {
			if (eigrp_redist_source_match(&(*cursor)->source, source)) {
				config = *cursor;
				break;
			}
		}
		if (!config)
			return EIGRP_RESULT_NOT_FOUND;
	}

	if (context->runtime) {
		eigrp_result_t withdraw_result =
			eigrp_topology_redistributed_source_remove(context->runtime, source);

		if (withdraw_result != EIGRP_RESULT_SUCCESS
		    && withdraw_result != EIGRP_RESULT_NOT_FOUND)
			return withdraw_result;
		result = eigrp_rib_redistribute_remove(context->runtime, source);
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND)
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

static uint32_t eigrp_redist_prefix_count(eigrp_instance_t *runtime)
{
	eigrp_table_node_t *node;
	eigrp_prefix_descriptor_t *prefix;
	eigrp_list_item_t *item;
	eigrp_route_descriptor_t *route;
	uint32_t count = 0;

	if (!runtime || !runtime->topology_table || !runtime->neighbor_self)
		return 0;
	for (node = eigrp_table_first(runtime->topology_table); node;
	     node = eigrp_table_next(node)) {
		prefix = node->info;
		if (!prefix)
			continue;
		for (EIGRP_LIST_ITERATE_RO(prefix->external_routes, item, route)) {
			if (route->adv_router == runtime->neighbor_self) {
				count++;
				break;
			}
		}
	}
	return count;
}

bool eigrp_redist_prefix_admit(eigrp_instance_t *runtime,
	const eigrp_rib_source_route_t *route)
{
	eigrp_af_instance_t *af;
	eigrp_prefix_descriptor_t *prefix;
	const eigrp_prefix_limit_t *limit;

	if (!runtime || !route)
		return false;
	af = eigrp_instance_runtime_config(runtime);
	if (!af || !af->redistribute_policy
	    || !af->redistribute_policy->maximum_prefix_configured)
		return true;
	prefix = eigrp_topology_table_lookup(runtime->topology_table, &route->prefix);
	if (prefix && eigrp_prefix_descriptor_lookup(prefix, runtime->neighbor_self))
		return true;
	limit = &af->redistribute_policy->maximum_prefix;
	{
		uint32_t count = eigrp_redist_prefix_count(runtime);
		if (eigrp_prefix_limit_threshold_crossed(limit, count))
			eigrp_log(EIGRP_LOG_WARNING,
				  "EIGRP redistribution maximum-prefix threshold reached (%u/%u)",
				  count + 1U, limit->maximum);
		return eigrp_prefix_limit_allows(limit, count, false);
	}
}

static eigrp_result_t eigrp_redist_source_route_receive(
	eigrp_instance_t *runtime, const eigrp_rib_source_route_t *route,
	bool importing)
{
	eigrp_af_instance_t *af;
	eigrp_redist_config_t *config;
	eigrp_metrics_t metric;

	if (!runtime || !route || !eigrp_redist_source_valid(&route->source))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	af = eigrp_instance_runtime_config(runtime);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	config = eigrp_redist_config_lookup(af, &route->source);
	if (!config)
		return EIGRP_RESULT_SUCCESS;

	if (!importing)
		return eigrp_topology_redistributed_route_remove(runtime, route);

	if (importing && config->route_map) {
		eigrp_filter_decision_t decision = EIGRP_FILTER_DECISION_DENY;
		eigrp_result_t policy_result = eigrp_sys_redistribute_route_map_evaluate(
			runtime, config->route_map, route, &decision);

		/* A configured-but-missing route-map and a no-match both fail closed.
		 * If this prefix had previously been admitted, withdraw it now. */
		if (policy_result != EIGRP_RESULT_SUCCESS
		    || decision == EIGRP_FILTER_DECISION_DENY)
			return eigrp_topology_redistributed_route_remove(runtime, route);
	}
	if (eigrp_redist_metric_select(af, config, route, &metric)
	    == EIGRP_REDISTRIBUTE_METRIC_NONE)
		return EIGRP_RESULT_SUCCESS;
	if (!eigrp_redist_prefix_admit(runtime, route)
	    || !eigrp_topology_prefix_admit(runtime, &route->prefix))
		return EIGRP_RESULT_CONFLICT;
	return eigrp_topology_redistributed_route_update(runtime, route, &metric);
}

/* Zebra uses ADD for both first appearance and changed route snapshots. */
eigrp_result_t eigrp_rib_source_route_add(
	eigrp_instance_t *runtime, const eigrp_rib_source_route_t *route)
{
	return eigrp_redist_source_route_receive(runtime, route, true);
}

eigrp_result_t eigrp_rib_source_route_remove(
	eigrp_instance_t *runtime, const eigrp_rib_source_route_t *route)
{
	return eigrp_redist_source_route_receive(runtime, route, false);
}

void eigrp_redist_config_delete_all(eigrp_af_instance_t *af)
{
	eigrp_redist_config_t *config;
	eigrp_redist_config_t *next;

	if (!af)
		return;
	for (config = af->redistributions; config; config = next) {
		next = config->next;
		free(config->route_map);
		free(config);
	}
	af->redistributions = NULL;
}

void eigrp_redist_policy_update_all(void)
{
	eigrp_instance_t *runtime;
	eigrp_af_instance_t *af;
	eigrp_redist_config_t *config;
	eigrp_list_item_t *node;

	if (!eigrp_om || !eigrp_om->eigrp)
		return;
	for (EIGRP_LIST_ITERATE_RO(eigrp_om->eigrp, node, runtime)) {
		if (!runtime)
			continue;
		af = eigrp_instance_runtime_config(runtime);
		if (!af)
			continue;
		for (config = af->redistributions; config; config = config->next) {
			if (!config->route_map)
				continue;
			/* Re-subscription makes Zebra replay the current source RIB so
			 * route-map edits are applied to already-existing IPv4/IPv6
			 * candidates, not only to future route events. */
			(void)eigrp_rib_redistribute_remove(runtime, &config->source);
			(void)eigrp_rib_redistribute_add(runtime, &config->source);
		}
	}
}



/*
 * Syntax:
 *   Named: `redistribute maximum-prefix LIMIT [...]` / `no redistribute maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or removes the redistribution prefix-limit policy.
 * Retained configuration stays here while supported runtime limit semantics are enforced.
 */
eigrp_result_t eigrp_redist_max_prefix_update(eigrp_operation_t operation, eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit)
{
	if (operation == EIGRP_RESET) {
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->redistribute_policy) {
		context->config->redistribute_policy->maximum_prefix_configured = false;
		memset(&context->config->redistribute_policy->maximum_prefix, 0,
		       sizeof(context->config->redistribute_policy->maximum_prefix));
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

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
	return context->runtime && (limit->dampened || limit->reset_time_minutes
		|| limit->restart_minutes || limit->restart_count)
		       ? EIGRP_RESULT_UNSUPPORTED : EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `redistribute maximum-prefix LIMIT [...]` / `no redistribute maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or removes the redistribution prefix-limit policy.
 * Retained configuration stays here while supported runtime limit semantics are enforced.
 */


void eigrp_redist_policy_delete_all(eigrp_af_instance_t *af)
{
	if (!af)
		return;
	free(af->redistribute_policy);
	af->redistribute_policy = NULL;
}
