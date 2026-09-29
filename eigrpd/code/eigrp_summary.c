// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP summarization configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdlib.h>
#include <string.h>

#include "eigrpd.h"
#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_metric.h"
#include "eigrp_instance.h"
#include "eigrp_packetizer.h"
#include "eigrp_packet.h"
#include "eigrp_prefix.h"
#include "eigrp_table.h"
#include "eigrp_topology.h"
#include "eigrp_summary.h"
#include "eigrp_sys.h"

struct eigrp_summary_config {
	eigrp_prefix_t prefix;
	uint8_t administrative_distance;
	char *leak_map;
	eigrp_summary_config_t *next;
};

struct eigrp_summary_metric_entry {
	eigrp_prefix_t prefix;
	eigrp_summary_metric_config_t config;
	struct eigrp_summary_metric_entry *next;
};

struct eigrp_summary_state {
	bool auto_summary;
	struct eigrp_summary_metric_entry *metrics;
};

static bool eigrp_summary_prefix_valid(const eigrp_prefix_t *prefix)
{
	if (!prefix)
		return false;

	switch (prefix->address.afi) {
	case EIGRP_AFI_IPV4:
		return prefix->prefix_length <= 32;
	case EIGRP_AFI_IPV6:
		return prefix->prefix_length <= 128;
	}
	return false;
}

static void eigrp_summary_prefix_normalize(eigrp_prefix_t *prefix)
{
	uint8_t full_bytes;
	uint8_t remaining_bits;
	uint8_t address_bytes;

	if (!eigrp_summary_prefix_valid(prefix))
		return;

	address_bytes = prefix->address.afi == EIGRP_AFI_IPV4 ? 4 : 16;
	full_bytes = prefix->prefix_length / 8U;
	remaining_bits = prefix->prefix_length % 8U;

	if (remaining_bits && full_bytes < address_bytes) {
		prefix->address.bytes[full_bytes] &=
			(uint8_t)(0xffU << (8U - remaining_bits));
		full_bytes++;
	}
	if (full_bytes < address_bytes)
		memset(prefix->address.bytes + full_bytes, 0,
		       address_bytes - full_bytes);
	if (address_bytes < sizeof(prefix->address.bytes))
		memset(prefix->address.bytes + address_bytes, 0,
		       sizeof(prefix->address.bytes) - address_bytes);
}

static bool eigrp_summary_prefix_match(const eigrp_prefix_t *a,
				       const eigrp_prefix_t *b)
{
	return a && b && a->address.afi == b->address.afi
	       && a->prefix_length == b->prefix_length
	       && memcmp(a->address.bytes, b->address.bytes,
			 sizeof(a->address.bytes)) == 0;
}

static const eigrp_af_vectors_t *eigrp_summary_context_vectors(
	const eigrp_instance_context_t *context)
{
	if (!context)
		return NULL;
	if (context->config)
		return &context->config->af_vectors;
	if (context->runtime)
		return &context->runtime->af_vectors;
	return NULL;
}


static eigrp_intf_config_t *eigrp_summary_intf_config(
	eigrp_instance_t *eigrp, const eigrp_intf_t *ei)
{
	eigrp_af_instance_t *af;
	eigrp_intf_config_t *config;

	if (!eigrp || !ei)
		return NULL;
	af = eigrp_instance_runtime_config(eigrp);
	if (!af)
		return NULL;
	config = eigrp_intf_config_read(af, ei->name);
	return config ? config : eigrp_intf_config_read(af, "default");
}

static eigrp_summary_config_t *eigrp_summary_match(
	eigrp_instance_t *eigrp, const eigrp_intf_t *ei,
	const eigrp_prefix_t *destination)
{
	eigrp_intf_config_t *config;
	eigrp_summary_config_t *summary, *best = NULL;

	config = eigrp_summary_intf_config(eigrp, ei);
	if (!config || !destination)
		return NULL;
	for (summary = config->summaries; summary; summary = summary->next) {
		if (summary->prefix.address.afi != destination->address.afi
		    || summary->prefix.prefix_length >= destination->prefix_length
		    || !eigrp_prefix_address_match(&summary->prefix,
					   &destination->address))
			continue;
		if (!best || summary->prefix.prefix_length > best->prefix.prefix_length)
			best = summary;
	}
	return best;
}

static const eigrp_summary_metric_config_t *eigrp_summary_metric_lookup(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix)
{
	eigrp_af_instance_t *af;
	struct eigrp_summary_metric_entry *entry;

	af = eigrp_instance_runtime_config(eigrp);
	if (!af || !af->summary_state)
		return NULL;
	for (entry = af->summary_state->metrics; entry; entry = entry->next)
		if (eigrp_summary_prefix_match(&entry->prefix, prefix))
			return &entry->config;
	return NULL;
}

static bool eigrp_summary_auto_enabled(eigrp_instance_t *eigrp)
{
	eigrp_af_instance_t *af = eigrp_instance_runtime_config(eigrp);

	return af && af->summary_state && af->summary_state->auto_summary;
}

static bool eigrp_summary_auto_match(eigrp_instance_t *eigrp,
				     const eigrp_intf_t *ei,
				     const eigrp_prefix_t *component,
				     eigrp_prefix_t *summary)
{
	eigrp_prefix_t interface_major;

	if (!eigrp || !ei || !component || !summary
	    || !eigrp_summary_auto_enabled(eigrp)
	    || !eigrp->af_vectors.summary_auto_prefix)
		return false;
	if (eigrp->af_vectors.summary_auto_prefix(component, summary)
	    != EIGRP_RESULT_SUCCESS)
		return false;
	if (eigrp->af_vectors.summary_auto_prefix(&ei->address, &interface_major)
	    != EIGRP_RESULT_SUCCESS)
		return false;

	/* Automatic summarization is a classful-boundary operation.  Keep
	 * subnet detail inside the same major network and advertise the major
	 * network only when the outgoing interface belongs to another one. */
	return !eigrp_summary_prefix_match(summary, &interface_major);
}

bool eigrp_summary_specific_leak(eigrp_instance_t *eigrp, eigrp_intf_t *ei,
				 const eigrp_prefix_t *destination)
{
	eigrp_summary_config_t *summary;
	eigrp_filter_decision_t decision = EIGRP_FILTER_DECISION_DENY;

	if (!eigrp || !ei || !destination)
		return false;
	summary = eigrp_summary_match(eigrp, ei, destination);
	if (!summary || !summary->leak_map)
		return false;

	/* A missing policy or evaluation failure fails closed: the configured
	 * aggregate remains advertised, but no covered specific leaks. */
	if (eigrp_sys_summary_leak_map_evaluate(eigrp, summary->leak_map,
					       destination, &decision)
	    != EIGRP_RESULT_SUCCESS)
		return false;
	return decision == EIGRP_FILTER_DECISION_PERMIT;
}

bool eigrp_summary_route_build(eigrp_instance_t *eigrp, eigrp_intf_t *ei,
			       const eigrp_prefix_descriptor_t *prefix,
			       const eigrp_route_descriptor_t *route,
			       eigrp_prefix_descriptor_t *summary_prefix,
			       eigrp_route_descriptor_t *summary_route)
{
	eigrp_summary_config_t *summary;
	const eigrp_summary_metric_config_t *metric_config;

	if (!eigrp || !ei || !prefix || !route || !summary_prefix || !summary_route)
		return false;
	summary = eigrp_summary_match(eigrp, ei, &prefix->destination);
	memset(summary_prefix, 0, sizeof(*summary_prefix));
	if (summary)
		summary_prefix->destination = summary->prefix;
	else if (!eigrp_summary_auto_match(eigrp, ei, &prefix->destination,
					  &summary_prefix->destination))
		return false;

	memset(summary_route, 0, sizeof(*summary_route));
	*summary_route = *route;
	/* A summary represents the current aggregate, not whichever component
	 * happened to trigger this UPDATE. Select the best reachable covered
	 * component so one component withdrawal cannot poison a still-live
	 * aggregate. */
	if (eigrp->topology_table) {
		eigrp_table_node_t *rn;
		eigrp_route_descriptor_t *best_route = NULL;
		for (rn = eigrp_table_first(eigrp->topology_table); rn;
		     rn = eigrp_table_next(rn)) {
			eigrp_prefix_descriptor_t *candidate = rn->info;
			eigrp_route_descriptor_t *candidate_route;
			if (!candidate
			    || candidate->destination.address.afi != summary_prefix->destination.address.afi
			    || candidate->destination.prefix_length <= summary_prefix->destination.prefix_length
			    || !eigrp_prefix_address_match(&summary_prefix->destination,
						   &candidate->destination.address))
				continue;
			candidate_route = eigrp_topology_route_read(candidate);
			if (!candidate_route || candidate_route->distance == EIGRP_MAX_METRIC)
				continue;
			if (!best_route || candidate_route->distance < best_route->distance)
				best_route = candidate_route;
		}
		if (best_route)
			*summary_route = *best_route;
		else
			summary_route->metric.delay = EIGRP_MAX_METRIC;
	}
	summary_prefix->reported_metric = summary_route->metric;
	summary_prefix->state = EIGRP_FSM_STATE_PASSIVE;
	summary_route->prefix = summary_prefix;
	summary_route->dest = summary_prefix->destination;

	metric_config = eigrp_summary_metric_lookup(eigrp, &summary_prefix->destination);
	if (metric_config && metric_config->metric_configured) {
		eigrp_metric_values_convert(&metric_config->metric, &summary_route->metric);
		summary_route->reported_metric = summary_route->metric;
		summary_route->total_metric = summary_route->metric;
	}
	return true;
}

void eigrp_summary_runtime_update(eigrp_instance_t *eigrp)
{
	eigrp_table_node_t *rn;
	eigrp_prefix_descriptor_t *prefix;

	if (!eigrp || !eigrp->topology_table)
		return;
	for (rn = eigrp_table_first(eigrp->topology_table); rn;
	     rn = eigrp_table_next(rn)) {
		prefix = rn->info;
		if (!prefix || prefix->state != EIGRP_FSM_STATE_PASSIVE)
			continue;
		prefix->req_action |= EIGRP_FSM_NEED_UPDATE;
		if (!eigrp_list_lookup(eigrp->topology_changes, prefix))
			eigrp_list_add(eigrp->topology_changes, prefix);
	}
	eigrp_update_send_all(eigrp, NULL);
}

static void eigrp_summary_withdraw(eigrp_instance_t *eigrp,
			   const eigrp_prefix_t *prefix)
{
	eigrp_packetizer_work_t *work;
	eigrp_prefix_descriptor_t *withdraw_prefix;
	eigrp_route_descriptor_t *withdraw_route;

	if (!eigrp || !prefix)
		return;
	withdraw_prefix = eigrp_topology_prefix_create();
	withdraw_route = eigrp_topology_route_create(NULL);
	work = eigrp_packetizer_work_create(EIGRP_OPC_UPDATE);
	if (!withdraw_prefix || !withdraw_route || !work) {
		eigrp_topology_prefix_free(withdraw_prefix);
		eigrp_topology_route_free(withdraw_route);
		eigrp_packetizer_work_free(work);
		return;
	}
	withdraw_prefix->destination = *prefix;
	withdraw_prefix->state = EIGRP_FSM_STATE_PASSIVE;
	withdraw_prefix->reported_metric.delay = EIGRP_MAX_METRIC;
	withdraw_route->prefix = withdraw_prefix;
	withdraw_route->dest = *prefix;
	withdraw_route->type = eigrp->af_vectors.classic_internal_tlv_type;
	withdraw_route->metric.delay = EIGRP_MAX_METRIC;
	withdraw_route->reported_metric.delay = EIGRP_MAX_METRIC;
	work->prefix = withdraw_prefix;
	work->route = withdraw_route;
	work->flags = EIGRP_PACKETIZER_WORK_F_OWN_PREFIX
		      | EIGRP_PACKETIZER_WORK_F_OWN_ROUTE;
	eigrp_packetizer_enqueue(eigrp, work);
}
/*
 * Syntax:
 *   Classic: `ip summary-address eigrp AS PREFIX` / `no ip summary-address eigrp AS PREFIX`
 *   Named: `summary-address PREFIX [DISTANCE [leak-map NAME]]` / `no summary-address ...`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Creates or removes manual EIGRP interface summarization state.
 * The common target retains configuration and applies manual summary advertisement at the portable packetizer boundary; leak-map policy selectively permits covered specifics on that same packetization path.
 */
eigrp_result_t eigrp_summary_create(
	eigrp_intf_context_t *context, const eigrp_prefix_t *prefix,
	const eigrp_summary_options_t *options)
{
	eigrp_summary_config_t *summary;
	eigrp_prefix_t normalized;
	char *leak_map = NULL;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (!eigrp_summary_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (options && options->leak_map && !options->leak_map[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context->config)
		return EIGRP_RESULT_NOT_FOUND;
	if (options && options->leak_map) {
		leak_map = strdup(options->leak_map);
		if (!leak_map)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	normalized = *prefix;
	eigrp_summary_prefix_normalize(&normalized);
	for (summary = context->config->summaries; summary; summary = summary->next) {
		if (!eigrp_summary_prefix_match(&summary->prefix, &normalized))
			continue;
		free(summary->leak_map);
		summary->administrative_distance =
			options ? options->administrative_distance : 0;
		summary->leak_map = leak_map;
		if (context->runtime)
			eigrp_summary_runtime_update(context->runtime->eigrp);
		return EIGRP_RESULT_SUCCESS;
	}

	summary = calloc(1, sizeof(*summary));
	if (!summary) {
		free(leak_map);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	summary->prefix = normalized;
	summary->administrative_distance = options ? options->administrative_distance : 0;
	summary->leak_map = leak_map;
	summary->next = context->config->summaries;
	context->config->summaries = summary;
	if (context->runtime)
		eigrp_summary_runtime_update(context->runtime->eigrp);
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `ip summary-address eigrp AS PREFIX` / `no ip summary-address eigrp AS PREFIX`
 *   Named: `summary-address PREFIX [DISTANCE [leak-map NAME]]` / `no summary-address ...`
 * Supported: Classic / Named
 * Placement:
 *   Classic: interface mode
 *   Named: af-interface mode
 * Description:
 * Creates or removes manual EIGRP interface summarization state.
 * The common target retains configuration and applies manual summary advertisement at the portable packetizer boundary; leak-map policy selectively permits covered specifics on that same packetization path.
 */
eigrp_result_t eigrp_summary_delete(
	eigrp_intf_context_t *context, const eigrp_prefix_t *prefix)
{
	eigrp_summary_config_t **cursor;
	eigrp_summary_config_t *summary;
	eigrp_prefix_t normalized;

	if (!eigrp_summary_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (!context->config)
		return EIGRP_RESULT_NOT_FOUND;

	normalized = *prefix;
	eigrp_summary_prefix_normalize(&normalized);
	for (cursor = &context->config->summaries; *cursor;
	     cursor = &(*cursor)->next) {
		summary = *cursor;
		if (!eigrp_summary_prefix_match(&summary->prefix, &normalized))
			continue;
		*cursor = summary->next;
		if (context->runtime)
			eigrp_summary_withdraw(context->runtime->eigrp, &summary->prefix);
		free(summary->leak_map);
		free(summary);
		if (context->runtime)
			eigrp_summary_runtime_update(context->runtime->eigrp);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

void eigrp_summary_delete_all(eigrp_intf_config_t *interface)
{
	eigrp_summary_config_t *summary;
	eigrp_summary_config_t *next;

	if (!interface)
		return;
	for (summary = interface->summaries; summary; summary = next) {
		next = summary->next;
		free(summary->leak_map);
		free(summary);
	}
	interface->summaries = NULL;
}

static eigrp_summary_state_t *eigrp_summary_state_create(
	eigrp_af_instance_t *af)
{
	if (!af)
		return NULL;
	if (!af->summary_state) {
		af->summary_state = calloc(1, sizeof(*af->summary_state));
		if (!af->summary_state)
			return NULL;
	}
	return af->summary_state;
}

/*
 * Syntax:
 *   Named: `auto-summary` / `no auto-summary`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Controls retained automatic summarization state for the named topology.
 * This project keeps the command target separate even where modern deployment guidance treats auto-summary as retired.
 */
eigrp_result_t eigrp_summary_auto_update(eigrp_operation_t operation, eigrp_instance_context_t *context)
{
	bool enabled;

	if (operation != EIGRP_SET && operation != EIGRP_RESET)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	enabled = operation == EIGRP_SET;
	const eigrp_af_vectors_t *vectors;
	eigrp_summary_state_t *state;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	vectors = eigrp_summary_context_vectors(context);
	if (vectors->afi != EIGRP_AFI_IPV4)
		return EIGRP_RESULT_UNSUPPORTED;
	if (context->config) {
		state = eigrp_summary_state_create(context->config);
		if (!state)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		state->auto_summary = enabled;
	}
	if (context->runtime)
		eigrp_summary_runtime_update(context->runtime);
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_summary_policy_update_all(void)
{
	eigrp_instance_t *runtime;
	eigrp_list_item_t *node;

	if (!eigrp_om || !eigrp_om->eigrp)
		return;
	for (EIGRP_LIST_ITERATE_RO(eigrp_om->eigrp, node, runtime)) {
		if (!runtime)
			continue;
		eigrp_summary_runtime_update(runtime);
	}
}







/*
 * Syntax:
 *   Named: `summary-metric PREFIX <metric-vector|distance DISTANCE>` / `no summary-metric PREFIX`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Creates, updates, or removes an explicit summary metric override.
 * The target retains the configuration and refreshes live summary advertisements so an explicit metric vector takes effect immediately.
 */
eigrp_result_t eigrp_summary_metric_update(eigrp_operation_t operation, eigrp_instance_context_t *context, const eigrp_prefix_t *prefix, const eigrp_summary_metric_config_t *config)
{
	if (operation == EIGRP_RESET) {
	eigrp_summary_state_t *state;
	struct eigrp_summary_metric_entry **cursor;
	struct eigrp_summary_metric_entry *entry;
	eigrp_prefix_t normalized;

	if (!eigrp_summary_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && prefix->address.afi != context->config->afi)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (context->config && context->config->summary_state) {
		state = context->config->summary_state;
		normalized = *prefix;
		eigrp_summary_prefix_normalize(&normalized);
		for (cursor = &state->metrics; *cursor;
		     cursor = &(*cursor)->next) {
			entry = *cursor;
			if (!eigrp_summary_prefix_match(&entry->prefix, &normalized))
				continue;
			*cursor = entry->next;
			free(entry);
			if (context->runtime)
				eigrp_summary_runtime_update(context->runtime);
			return EIGRP_RESULT_SUCCESS;
		}
		if (!context->runtime)
			return EIGRP_RESULT_NOT_FOUND;
	}
	return EIGRP_RESULT_NOT_FOUND;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	eigrp_summary_state_t *state;
	struct eigrp_summary_metric_entry *entry;
	eigrp_prefix_t normalized;

	if (!eigrp_summary_prefix_valid(prefix) || !config
	    || (!config->metric_configured && !config->distance_configured))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (config->metric_configured
	    && (!config->metric.bandwidth || !config->metric.load
		|| !config->metric.mtu))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (config->distance_configured && !config->distance)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && prefix->address.afi != context->config->afi)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (context->config) {
		state = eigrp_summary_state_create(context->config);
		if (!state)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		normalized = *prefix;
		eigrp_summary_prefix_normalize(&normalized);
		for (entry = state->metrics; entry; entry = entry->next) {
			if (!eigrp_summary_prefix_match(&entry->prefix, &normalized))
				continue;
			entry->config = *config;
			if (context->runtime)
				eigrp_summary_runtime_update(context->runtime);
			return EIGRP_RESULT_SUCCESS;
		}
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		entry->prefix = normalized;
		entry->config = *config;
		entry->next = state->metrics;
		state->metrics = entry;
	}
	if (context->runtime)
		eigrp_summary_runtime_update(context->runtime);
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `summary-metric PREFIX <metric-vector|distance DISTANCE>` / `no summary-metric PREFIX`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Creates, updates, or removes an explicit summary metric override.
 * The target retains the configuration and refreshes live summary advertisements so an explicit metric vector takes effect immediately.
 */


void eigrp_summary_state_delete_all(eigrp_af_instance_t *af)
{
	struct eigrp_summary_metric_entry *entry;
	struct eigrp_summary_metric_entry *next;

	if (!af || !af->summary_state)
		return;
	for (entry = af->summary_state->metrics; entry; entry = next) {
		next = entry->next;
		free(entry);
	}
	free(af->summary_state);
	af->summary_state = NULL;
}
