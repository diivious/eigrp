/*
 * EIGRP Metric Math Functions.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 */
#ifndef _ZEBRA_EIGRP_METRIC_H_
#define _ZEBRA_EIGRP_METRIC_H_

#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_result.h"

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

typedef struct eigrp_metric_values {
	uint32_t bandwidth;
	uint32_t delay;
	uint8_t reliability;
	uint8_t load;
	uint16_t mtu;
} eigrp_metric_values_t;

typedef struct eigrp_metric_weights {
	uint8_t tos;
	uint8_t k1;
	uint8_t k2;
	uint8_t k3;
	uint8_t k4;
	uint8_t k5;
} eigrp_metric_weights_t;


/* Prototypes */
extern eigrp_scaled_t eigrp_bandwidth_to_scaled(eigrp_bandwidth_t);
extern eigrp_bandwidth_t eigrp_scaled_to_bandwidth(eigrp_scaled_t);
extern eigrp_scaled_t eigrp_delay_to_scaled(eigrp_delay_t);
extern eigrp_delay_t eigrp_scaled_to_delay(eigrp_scaled_t);

extern eigrp_metric_t eigrp_calculate_metrics(eigrp_instance_t *, eigrp_metrics_t);
extern eigrp_metric_t eigrp_calculate_total_metrics(eigrp_instance_t *,
						    eigrp_route_descriptor_t *);
extern bool eigrp_metrics_is_same(eigrp_metrics_t, eigrp_metrics_t);

eigrp_result_t eigrp_metric_default_update(eigrp_instance_context_t *context,
					   const eigrp_metric_values_t *metric);
eigrp_result_t eigrp_metric_default_delete(eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_weights_update(eigrp_instance_context_t *context,
					   const eigrp_metric_weights_t *weights);
eigrp_result_t eigrp_metric_weights_delete(eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_variance_update(eigrp_instance_context_t *context,
					    uint8_t variance);
eigrp_result_t eigrp_metric_variance_delete(eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_traffic_share_balanced_update(
	eigrp_instance_context_t *context, bool enabled);
eigrp_result_t eigrp_metric_maximum_hops_update(
	eigrp_instance_context_t *context, uint8_t maximum_hops);
eigrp_result_t eigrp_metric_maximum_hops_delete(
	eigrp_instance_context_t *context);
eigrp_result_t eigrp_metric_holddown_update(eigrp_instance_context_t *context,
					    bool enabled);
eigrp_result_t eigrp_metric_holddown_delete(eigrp_instance_context_t *context);

#endif /* _ZEBRA_EIGRP_METRIC_H_ */
