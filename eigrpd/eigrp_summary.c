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

eigrp_result_t eigrp_summary_auto_update(eigrp_instance_context_t *context,
					 bool enabled)
{
	const eigrp_af_vectors_t *vectors;

	(void)enabled;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	vectors = eigrp_summary_context_vectors(context);
	if (!vectors || !vectors->summary_auto_prefix)
		return EIGRP_RESULT_UNSUPPORTED;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_summary_metric_update(
	eigrp_instance_context_t *context, const eigrp_prefix_t *prefix,
	const eigrp_summary_metric_config_t *config)
{
	if (!prefix || !config
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
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_summary_metric_delete(eigrp_instance_context_t *context,
					   const eigrp_prefix_t *prefix)
{
	if (!prefix)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
