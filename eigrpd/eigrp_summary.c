// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP summarization configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdlib.h>
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_summary.h"

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
	case EIGRP_ADDRESS_FAMILY_IPV4:
		return prefix->prefix_length <= 32;
	case EIGRP_ADDRESS_FAMILY_IPV6:
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

	address_bytes = prefix->address.afi == EIGRP_ADDRESS_FAMILY_IPV4 ? 4 : 16;
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

static bool eigrp_summary_prefix_equal(const eigrp_prefix_t *a,
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
 * The FRR reference classic callback did not implement runtime summary behavior, so the common target preserves configuration and truthfully reports NOT_IMPLEMENTED where runtime support is absent.
 */
eigrp_result_t eigrp_summary_create(
	eigrp_interface_context_t *context, const eigrp_prefix_t *prefix,
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
		return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
					: EIGRP_RESULT_NOT_FOUND;
	if (options && options->leak_map) {
		leak_map = strdup(options->leak_map);
		if (!leak_map)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	normalized = *prefix;
	eigrp_summary_prefix_normalize(&normalized);
	for (summary = context->config->summaries; summary; summary = summary->next) {
		if (!eigrp_summary_prefix_equal(&summary->prefix, &normalized))
			continue;
		free(summary->leak_map);
		summary->administrative_distance =
			options ? options->administrative_distance : 0;
		summary->leak_map = leak_map;
		return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
					: EIGRP_RESULT_SUCCESS;
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
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
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
 * The FRR reference classic callback did not implement runtime summary behavior, so the common target preserves configuration and truthfully reports NOT_IMPLEMENTED where runtime support is absent.
 */
eigrp_result_t eigrp_summary_delete(
	eigrp_interface_context_t *context, const eigrp_prefix_t *prefix)
{
	eigrp_summary_config_t **cursor;
	eigrp_summary_config_t *summary;
	eigrp_prefix_t normalized;

	if (!eigrp_summary_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (!context->config)
		return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
					: EIGRP_RESULT_NOT_FOUND;

	normalized = *prefix;
	eigrp_summary_prefix_normalize(&normalized);
	for (cursor = &context->config->summaries; *cursor;
	     cursor = &(*cursor)->next) {
		summary = *cursor;
		if (!eigrp_summary_prefix_equal(&summary->prefix, &normalized))
			continue;
		*cursor = summary->next;
		free(summary->leak_map);
		free(summary);
		return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
					: EIGRP_RESULT_SUCCESS;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_NOT_FOUND;
}

void eigrp_summary_delete_all(eigrp_interface_config_t *interface)
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

static eigrp_summary_state_t *eigrp_summary_state_get(
	eigrp_address_family_config_t *af)
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
eigrp_result_t eigrp_summary_auto_update(eigrp_instance_context_t *context,
					 bool enabled)
{
	const eigrp_af_vectors_t *vectors;
	eigrp_summary_state_t *state;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	vectors = eigrp_summary_context_vectors(context);
	if (!vectors || !vectors->summary_auto_prefix)
		return EIGRP_RESULT_UNSUPPORTED;
	if (context->config) {
		state = eigrp_summary_state_get(context->config);
		if (!state)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		state->auto_summary = enabled;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `summary-metric PREFIX <metric-vector|distance DISTANCE>` / `no summary-metric PREFIX`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Creates, updates, or removes an explicit summary metric override.
 * The target retains the configuration and reports NOT_IMPLEMENTED until summary-metric runtime application is complete.
 */
eigrp_result_t eigrp_summary_metric_update(
	eigrp_instance_context_t *context, const eigrp_prefix_t *prefix,
	const eigrp_summary_metric_config_t *config)
{
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
		state = eigrp_summary_state_get(context->config);
		if (!state)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		normalized = *prefix;
		eigrp_summary_prefix_normalize(&normalized);
		for (entry = state->metrics; entry; entry = entry->next) {
			if (!eigrp_summary_prefix_equal(&entry->prefix, &normalized))
				continue;
			entry->config = *config;
			return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
						: EIGRP_RESULT_SUCCESS;
		}
		entry = calloc(1, sizeof(*entry));
		if (!entry)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		entry->prefix = normalized;
		entry->config = *config;
		entry->next = state->metrics;
		state->metrics = entry;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `summary-metric PREFIX <metric-vector|distance DISTANCE>` / `no summary-metric PREFIX`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Creates, updates, or removes an explicit summary metric override.
 * The target retains the configuration and reports NOT_IMPLEMENTED until summary-metric runtime application is complete.
 */
eigrp_result_t eigrp_summary_metric_delete(eigrp_instance_context_t *context,
					   const eigrp_prefix_t *prefix)
{
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
			if (!eigrp_summary_prefix_equal(&entry->prefix, &normalized))
				continue;
			*cursor = entry->next;
			free(entry);
			return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
						: EIGRP_RESULT_SUCCESS;
		}
		if (!context->runtime)
			return EIGRP_RESULT_NOT_FOUND;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_NOT_FOUND;
}

void eigrp_summary_state_delete_all(eigrp_address_family_config_t *af)
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
