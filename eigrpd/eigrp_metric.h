// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Metric Math Functions.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 */
#ifndef _ZEBRA_EIGRP_METRIC_H_
#define _ZEBRA_EIGRP_METRIC_H_

#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp.h"

/* Constants */
#define EIGRP_BANDWIDTH_MIN 0x1ull		  // 1
#define EIGRP_BANDWIDTH_SCALER 10000000ull	  // Inversion value
#define EIGRP_BANDWIDTH_MAX 0xffffffffffffffffull // 1.84467441x10^19

#define EIGRP_DELAY_MIN 0x1ull // 1
#define EIGRP_DELAY_PICO 1000000ull
#define EIGRP_DELAY_MAX 0xffffffffffffffffull // 1.84467441x10^19

#define EIGRP_MAX_LOAD 256
#define EIGRP_MAX_HOPS 100

#define EIGRP_INACCESSIBLE 0xFFFFFFFFFFFFFFFFull

#define EIGRP_METRIC_MAX 0xffffffffffffffffull // 1.84467441x10^19
#define EIGRP_METRIC_SCALER 65536	       // CLASSIC to WIDE conversion

#define EIGRP_CLASSIC_MAX 0xffffffff // 4294967295
#define EIGRP_CLASSIC_SCALER 256     // IGRP to EIGRP conversion


/* Prototypes */
extern eigrp_scaled_t eigrp_bandwidth_to_scaled(eigrp_bandwidth_t);
extern eigrp_bandwidth_t eigrp_scaled_to_bandwidth(eigrp_scaled_t);
extern eigrp_scaled_t eigrp_delay_to_scaled(eigrp_delay_t);
extern eigrp_delay_t eigrp_scaled_to_delay(eigrp_scaled_t);

extern eigrp_metric_t eigrp_calculate_metrics(eigrp_instance_t *, eigrp_metrics_t);
extern eigrp_metric_t eigrp_calculate_total_metrics(eigrp_instance_t *,
						    eigrp_route_descriptor_t *);
extern bool eigrp_metrics_is_same(eigrp_metrics_t, eigrp_metrics_t);
void eigrp_metric_values_convert(const eigrp_metric_values_t *values,
				eigrp_metrics_t *metric);

eigrp_result_t eigrp_metric_default_set(eigrp_instance_context_t *context,
					   const eigrp_metric_values_t *metric);
eigrp_result_t eigrp_metric_default_reset(eigrp_instance_context_t *context);
bool eigrp_metric_default_get(const eigrp_address_family_config_t *af,
			      eigrp_metric_values_t *metric);
eigrp_result_t eigrp_metric_weights_set(eigrp_instance_context_t *context,
					   const eigrp_metric_weights_t *weights);
eigrp_result_t eigrp_metric_weights_reset(eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_variance_set(eigrp_instance_context_t *context,
					    uint8_t variance);
eigrp_result_t eigrp_metric_variance_reset(eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_traffic_share_balanced_set(
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_traffic_share_balanced_reset(
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_maximum_hops_set(
	eigrp_instance_context_t *context, uint8_t maximum_hops);
eigrp_result_t eigrp_metric_maximum_hops_reset(
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_holddown_set(eigrp_instance_context_t *context,
					    bool enabled);
eigrp_result_t eigrp_metric_holddown_reset(eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_version_set(eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_version_reset(eigrp_instance_context_t *context);
uint8_t eigrp_metric_version_select(const eigrp_instance_t *eigrp, uint8_t peer_version);
void eigrp_metric_config_delete_all(eigrp_address_family_config_t *af);

#endif /* _ZEBRA_EIGRP_METRIC_H_ */
