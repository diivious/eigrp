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
	eigrp_address_t address;
	eigrp_address_t mask;
	uint8_t administrative_distance;
	char *leak_map;
	eigrp_summary_config_t *next;
};

static bool eigrp_summary_equal(const eigrp_address_t *a,
					const eigrp_address_t *b)
{
	size_t len;

	if (!a || !b || a->afi != b->afi)
		return false;
	len = a->afi == EIGRP_ADDRESS_FAMILY_IPV4 ? 4 : 16;
	return memcmp(a->bytes, b->bytes, len) == 0;
}

static bool eigrp_summary_ipv4_pair_valid(const eigrp_address_t *address,
					  const eigrp_address_t *mask)
{
	return address && mask && address->afi == EIGRP_ADDRESS_FAMILY_IPV4
	       && mask->afi == EIGRP_ADDRESS_FAMILY_IPV4;
}

eigrp_result_t eigrp_summary_create(
	eigrp_interface_context_t *context, const eigrp_address_t *address,
	const eigrp_address_t *mask, const eigrp_summary_options_t *options)
{
	eigrp_summary_config_t *summary;
	char *leak_map = NULL;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (!eigrp_summary_ipv4_pair_valid(address, mask))
		return EIGRP_RESULT_UNSUPPORTED;
	if (options && options->leak_map && !options->leak_map[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	if (options && options->leak_map) {
		leak_map = strdup(options->leak_map);
		if (!leak_map)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	for (summary = context->config->summaries; summary; summary = summary->next) {
		if (!eigrp_summary_equal(&summary->address, address)
		    || !eigrp_summary_equal(&summary->mask, mask))
			continue;
		free(summary->leak_map);
		summary->administrative_distance =
			options ? options->administrative_distance : 0;
		summary->leak_map = leak_map;
		return EIGRP_RESULT_SUCCESS;
	}

	summary = calloc(1, sizeof(*summary));
	if (!summary)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	summary->address = *address;
	summary->mask = *mask;
	summary->administrative_distance = options ? options->administrative_distance : 0;
	summary->leak_map = leak_map;
	summary->next = context->config->summaries;
	context->config->summaries = summary;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_summary_delete(
	eigrp_interface_context_t *context, const eigrp_address_t *address,
	const eigrp_address_t *mask)
{
	eigrp_summary_config_t **cursor;
	eigrp_summary_config_t *summary;

	if (!eigrp_summary_ipv4_pair_valid(address, mask))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->runtime)
		return EIGRP_RESULT_NOT_IMPLEMENTED;

	for (cursor = &context->config->summaries; *cursor;
	     cursor = &(*cursor)->next) {
		summary = *cursor;
		if (!eigrp_summary_equal(&summary->address, address)
		    || !eigrp_summary_equal(&summary->mask, mask))
			continue;
		*cursor = summary->next;
		free(summary->leak_map);
		free(summary);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
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
	eigrp_address_family_t afi;

	(void)enabled;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	afi = context->config ? context->config->afi : EIGRP_ADDRESS_FAMILY_IPV4;
	if (afi != EIGRP_ADDRESS_FAMILY_IPV4)
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
