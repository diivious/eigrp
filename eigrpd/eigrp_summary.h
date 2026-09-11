// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP summarization configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_SUMMARY_H_
#define EIGRPD_EIGRP_SUMMARY_H_

#include "eigrp_interface.h"
#include "eigrp_metric.h"
#include "eigrp_result.h"

typedef struct eigrp_summary_options {
	uint8_t administrative_distance; /* 0 means protocol default. */
	const char *leak_map;
} eigrp_summary_options_t;

typedef struct eigrp_summary_metric_config {
	bool metric_configured;
	eigrp_metric_values_t metric;
	bool distance_configured;
	uint8_t distance;
} eigrp_summary_metric_config_t;

eigrp_result_t eigrp_summary_create(
	eigrp_interface_context_t *context, const eigrp_address_t *address,
	const eigrp_address_t *mask, const eigrp_summary_options_t *options);
eigrp_result_t eigrp_summary_delete(
	eigrp_interface_context_t *context, const eigrp_address_t *address,
	const eigrp_address_t *mask);
void eigrp_summary_delete_all(eigrp_interface_config_t *interface);

eigrp_result_t eigrp_summary_auto_update(eigrp_instance_context_t *context,
					 bool enabled);
eigrp_result_t eigrp_summary_metric_update(eigrp_instance_context_t *context,
					   const eigrp_prefix_t *prefix,
					   const eigrp_summary_metric_config_t *config);
eigrp_result_t eigrp_summary_metric_delete(eigrp_instance_context_t *context,
					   const eigrp_prefix_t *prefix);

#endif /* EIGRPD_EIGRP_SUMMARY_H_ */
