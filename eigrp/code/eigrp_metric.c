// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Metric Math Functions.
 * Copyright (C) 2013-2016, 2026
 * Authors:
 *   Donnie Savage
 */
#include <stdlib.h>
#include <string.h>

#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_metric.h"
#include "eigrp_neighbor.h"
#include "eigrp_topology.h"

struct eigrp_metric_config {
	bool default_metric_configured;
	eigrp_metric_values_t default_metric;
	bool weights_configured;
	eigrp_metric_weights_t weights;
	bool variance_configured;
	uint8_t variance;
	bool traffic_share_balanced;
	bool maximum_hops_configured;
	uint8_t maximum_hops;
	bool version_32bit;
};


void eigrp_metric_values_convert(const eigrp_metric_values_t *values,
				eigrp_metrics_t *metric)
{
	if (!metric)
		return;
	memset(metric, 0, sizeof(*metric));
	if (!values)
		return;

	/* CLI seed bandwidth is Kbit/s; the route metric stores the classic
	 * inverse-bandwidth component used by the TLV codecs.  Delay stays in
	 * the normalized 10-microsecond units consumed by the classic codec.
	 */
	metric->bandwidth = eigrp_bandwidth_to_scaled(values->bandwidth);
	metric->delay = values->delay;
	metric->reliability = values->reliability;
	metric->load = values->load;
	metric->mtu[0] = values->mtu & 0xff;
	metric->mtu[1] = (values->mtu >> 8) & 0xff;
}

eigrp_scaled_t eigrp_bandwidth_to_scaled(eigrp_bandwidth_t bandwidth)
{
	eigrp_bandwidth_t scaled = EIGRP_BANDWIDTH_MAX;

	if (bandwidth != EIGRP_BANDWIDTH_MAX) {
		scaled = (EIGRP_CLASSIC_SCALER * EIGRP_BANDWIDTH_SCALER);
		scaled = scaled / bandwidth;

		scaled = scaled ? scaled : EIGRP_BANDWIDTH_MIN;
	}

	scaled = (scaled < EIGRP_METRIC_MAX) ? scaled : EIGRP_METRIC_MAX;
	return (eigrp_scaled_t)scaled;
}

eigrp_bandwidth_t eigrp_scaled_to_bandwidth(eigrp_scaled_t scaled)
{
	eigrp_bandwidth_t bandwidth = EIGRP_BANDWIDTH_MAX;

	if (scaled != EIGRP_CLASSIC_MAX) {
		bandwidth = (EIGRP_CLASSIC_SCALER * EIGRP_BANDWIDTH_SCALER);
		bandwidth = scaled * bandwidth;
		bandwidth = (bandwidth < EIGRP_METRIC_MAX)
				    ? bandwidth
				    : EIGRP_BANDWIDTH_MAX;
	}

	return bandwidth;
}

eigrp_scaled_t eigrp_delay_to_scaled(eigrp_delay_t delay)
{
	delay = delay ? delay : EIGRP_DELAY_MIN;
	return delay * EIGRP_CLASSIC_SCALER;
}

eigrp_delay_t eigrp_scaled_to_delay(eigrp_scaled_t scaled)
{
	scaled = scaled / EIGRP_CLASSIC_SCALER;
	scaled = scaled ? scaled : EIGRP_DELAY_MIN;

	return scaled;
}

eigrp_metric_t eigrp_metric_calculate(eigrp_instance_t *eigrp,
				       eigrp_metrics_t metric)
{
	uint64_t throughput = 0;
	uint64_t latency = 0;
	uint64_t composite;
	uint64_t scale;

	if (!eigrp || metric.delay == EIGRP_METRIC_MAX)
		return EIGRP_METRIC_MAX;
	if (metric.delay == EIGRP_MAX_METRIC)
		return EIGRP_MAX_METRIC;

	/* Native vector storage uses the classic 256-scaled bandwidth/delay
	 * components. Release-2/Wide metric calculation promotes those values to
	 * the RFC 7868 65536 scale; forced 32-bit policy retains classic scale.
	 */
	scale = eigrp->metric_version >= EIGRP_TLV_64B_VERSION
			? (EIGRP_METRIC_SCALER / EIGRP_CLASSIC_SCALER) : 1;
	if (eigrp->k_values[0])
		throughput = (uint64_t)eigrp->k_values[0] * metric.bandwidth * scale;
	if (eigrp->k_values[1])
		throughput += ((uint64_t)eigrp->k_values[1] * metric.bandwidth * scale)
			      / (EIGRP_MAX_LOAD - metric.load);
	if (eigrp->k_values[2])
		latency = (uint64_t)eigrp->k_values[2] * metric.delay * scale;

	composite = throughput + latency;
	/* K6 controls RFC 7868 extended attributes. This implementation does not
	 * yet source jitter/energy, so ExtAttr is correctly zero. */
	if (eigrp->k_values[4]) {
		uint64_t denominator = (uint64_t)metric.reliability + eigrp->k_values[3];
		if (!denominator)
			return EIGRP_METRIC_MAX;
		composite = (composite * eigrp->k_values[4]) / denominator;
	}

	if (eigrp->metric_version < EIGRP_TLV_64B_VERSION
	    && composite > EIGRP_CLASSIC_MAX)
		return EIGRP_CLASSIC_MAX;
	return composite > EIGRP_METRIC_MAX ? EIGRP_METRIC_MAX : composite;
}

eigrp_metric_t eigrp_metric_total_calculate(eigrp_instance_t *eigrp,
					     eigrp_route_descriptor_t *entry)
{
	eigrp_intf_t *ei = entry->ei;
	eigrp_delay_t link_delay;
	eigrp_bandwidth_t bw;

	entry->total_metric = entry->reported_metric;
	if (entry->reported_metric.delay == EIGRP_MAX_METRIC) {
		entry->total_metric.delay = EIGRP_MAX_METRIC;
		return EIGRP_MAX_METRIC;
	}
	if (entry->reported_metric.delay == EIGRP_METRIC_MAX) {
		entry->total_metric.delay = EIGRP_METRIC_MAX;
		return EIGRP_METRIC_MAX;
	}
	link_delay = eigrp_delay_to_scaled(ei->params.delay);
	if (entry->total_metric.delay >= EIGRP_METRIC_MAX - link_delay)
		entry->total_metric.delay = EIGRP_METRIC_MAX;
	else
		entry->total_metric.delay += link_delay;

	if (entry->total_metric.hop_count == UINT8_MAX)
		entry->total_metric.delay = EIGRP_METRIC_MAX;
	else
		entry->total_metric.hop_count++;
	if (entry->total_metric.hop_count > eigrp->max_hops)
		entry->total_metric.delay = EIGRP_METRIC_MAX;

	bw = eigrp_bandwidth_to_scaled(ei->params.bandwidth);
	entry->total_metric.bandwidth = entry->total_metric.bandwidth > bw
						? bw
						: entry->total_metric.bandwidth;

	return eigrp_metric_calculate(eigrp, entry->total_metric);
}

bool eigrp_metrics_match(eigrp_metrics_t metric1, eigrp_metrics_t metric2)
{
	if ((metric1.bandwidth == metric2.bandwidth)
	    && (metric1.delay == metric2.delay)
	    && (metric1.hop_count == metric2.hop_count)
	    && (metric1.load == metric2.load)
	    && (metric1.reliability == metric2.reliability)
	    && (metric1.mtu[0] == metric2.mtu[0])
	    && (metric1.mtu[1] == metric2.mtu[1])
	    && (metric1.mtu[2] == metric2.mtu[2])) {
		return TRUE;
	}

	return FALSE; // if different
}

static bool eigrp_metric_context_valid(const eigrp_instance_context_t *context)
{
	return context && (context->config || context->runtime);
}

static bool eigrp_metric_values_valid(const eigrp_metric_values_t *metric)
{
	return metric && metric->bandwidth && metric->load && metric->mtu;
}

static eigrp_metric_config_t *eigrp_metric_config_create(
	eigrp_af_config_t *af)
{
	if (!af)
		return NULL;
	if (!af->metric_config) {
		af->metric_config = calloc(1, sizeof(*af->metric_config));
		if (!af->metric_config)
			return NULL;
		af->metric_config->traffic_share_balanced = true;
	}
	return af->metric_config;
}

typedef enum eigrp_metric_message_type {
	EIGRP_METRIC_MESSAGE_DEFAULT,
	EIGRP_METRIC_MESSAGE_WEIGHTS,
	EIGRP_METRIC_MESSAGE_VARIANCE,
	EIGRP_METRIC_MESSAGE_TRAFFIC_SHARE,
	EIGRP_METRIC_MESSAGE_MAXIMUM_HOPS,
	EIGRP_METRIC_MESSAGE_VERSION,
} eigrp_metric_message_type_t;

typedef struct eigrp_metric_message_args {
	eigrp_metric_message_type_t type;
	eigrp_operation_t operation;
	eigrp_instance_context_t *context;
	union {
		const eigrp_metric_values_t *metric;
		const eigrp_metric_weights_t *weights;
		uint8_t value;
	} data;
} eigrp_metric_message_args_t;

static eigrp_result_t eigrp_metric_message_process(eigrp_instance_t *eigrp,
	void *arg);

/*
 * Syntax:
 *   Named: `default-metric BW DELAY RELIABILITY LOAD MTU` / `no default-metric ...`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets the default seed metric used by redistribution when a source-specific metric is not supplied.
 * Runtime redistribution fallback remains owned by the metric/redistribution path.
 */
eigrp_result_t eigrp_metric_default_update(eigrp_operation_t operation, eigrp_instance_context_t *context, const eigrp_metric_values_t *metric)
{
	eigrp_metric_message_args_t message = {
		.type = EIGRP_METRIC_MESSAGE_DEFAULT, .operation = operation,
		.context = context, .data.metric = metric,
	};

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_metric_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->metric_config) {
		context->config->metric_config->default_metric_configured = false;
		memset(&context->config->metric_config->default_metric, 0,
		       sizeof(context->config->metric_config->default_metric));
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	eigrp_metric_config_t *config;

	if (!eigrp_metric_values_valid(metric))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_create(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->default_metric = *metric;
		config->default_metric_configured = true;
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `default-metric BW DELAY RELIABILITY LOAD MTU` / `no default-metric ...`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets the default seed metric used by redistribution when a source-specific metric is not supplied.
 * Runtime redistribution fallback remains owned by the metric/redistribution path.
 */


bool eigrp_metric_default(const eigrp_af_config_t *af,
			      eigrp_metric_values_t *metric)
{
	if (!af || !metric || !af->metric_config
	    || !af->metric_config->default_metric_configured)
		return false;

	*metric = af->metric_config->default_metric;
	return true;
}

/*
 * Syntax:
 *   Classic: `metric weights TOS K1 K2 K3 K4 K5` / `no metric weights ...`
 *   Named: `metric weights TOS K1 K2 K3 K4 K5 [K6]` / `no metric weights ...`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: address-family mode
 * Description:
 * Sets or restores the EIGRP metric coefficients after validating the coefficient set.
 * Named mode converges on the EIGRP-owned metric target used for protocol state instead of carrying metric behavior in the parser.
 */
eigrp_result_t eigrp_metric_weights_update(eigrp_operation_t operation, eigrp_instance_context_t *context, const eigrp_metric_weights_t *weights)
{
	eigrp_metric_message_args_t message = {
		.type = EIGRP_METRIC_MESSAGE_WEIGHTS, .operation = operation,
		.context = context, .data.weights = weights,
	};

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_metric_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->metric_config) {
		context->config->metric_config->weights_configured = false;
		memset(&context->config->metric_config->weights, 0,
		       sizeof(context->config->metric_config->weights));
	}
	if (context->runtime) {
		context->runtime->k_values[0] = EIGRP_K1_DEFAULT;
		context->runtime->k_values[1] = EIGRP_K2_DEFAULT;
		context->runtime->k_values[2] = EIGRP_K3_DEFAULT;
		context->runtime->k_values[3] = EIGRP_K4_DEFAULT;
		context->runtime->k_values[4] = EIGRP_K5_DEFAULT;
		context->runtime->k_values[5] = EIGRP_K6_DEFAULT;
		eigrp_topology_metric_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	eigrp_metric_config_t *config;

	if (!weights || weights->tos != 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_create(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->weights = *weights;
		config->weights_configured = true;
	}
	if (context->runtime) {
		context->runtime->k_values[0] = weights->k1;
		context->runtime->k_values[1] = weights->k2;
		context->runtime->k_values[2] = weights->k3;
		context->runtime->k_values[3] = weights->k4;
		context->runtime->k_values[4] = weights->k5;
		context->runtime->k_values[5] = weights->k6;
		eigrp_topology_metric_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `metric weights TOS K1 K2 K3 K4 K5` / `no metric weights ...`
 *   Named: `metric weights TOS K1 K2 K3 K4 K5 [K6]` / `no metric weights ...`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: address-family mode
 * Description:
 * Sets or restores the EIGRP metric coefficients after validating the coefficient set.
 * Named mode converges on the EIGRP-owned metric target used for protocol state instead of carrying metric behavior in the parser.
 */


/*
 * Syntax:
 *   Classic: `variance MULTIPLIER` / `no variance`
 *   Named: `variance MULTIPLIER` / `no variance`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: topology base mode
 * Description:
 * Sets or resets the unequal-cost load-sharing variance multiplier.
 * DUAL feasibility remains authoritative; variance does not make an infeasible path a successor.
 */
eigrp_result_t eigrp_metric_variance_update(eigrp_operation_t operation, eigrp_instance_context_t *context, uint8_t variance)
{
	eigrp_metric_message_args_t message = {
		.type = EIGRP_METRIC_MESSAGE_VARIANCE, .operation = operation,
		.context = context, .data.value = variance,
	};

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_metric_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->metric_config) {
		context->config->metric_config->variance_configured = false;
		context->config->metric_config->variance = 0;
	}
	if (context->runtime) {
		context->runtime->variance = EIGRP_VARIANCE_DEFAULT;
		eigrp_topology_multipath_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	eigrp_metric_config_t *config;

	if (variance < 1 || variance > 128)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_create(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->variance = variance;
		config->variance_configured = true;
	}
	if (context->runtime) {
		context->runtime->variance = variance;
		eigrp_topology_multipath_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `variance MULTIPLIER` / `no variance`
 *   Named: `variance MULTIPLIER` / `no variance`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: topology base mode
 * Description:
 * Sets or resets the unequal-cost load-sharing variance multiplier.
 * DUAL feasibility remains authoritative; variance does not make an infeasible path a successor.
 */


/*
 * Syntax:
 *   Named: `traffic-share balanced` / `no traffic-share balanced`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Selects proportional forwarding weights for the already-selected successor set.
 * Feasibility, variance, maximum-paths, and DUAL successor selection are unchanged.
 */
eigrp_result_t eigrp_traffic_share_balanced_update(eigrp_operation_t operation, eigrp_instance_context_t *context)
{
	eigrp_metric_message_args_t message = {
		.type = EIGRP_METRIC_MESSAGE_TRAFFIC_SHARE, .operation = operation,
		.context = context,
	};

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_metric_message_process, &message);
	bool enabled;
	eigrp_metric_config_t *config;

	if (operation != EIGRP_SET && operation != EIGRP_RESET)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;

	/* SET selects balanced sharing; RESET is the explicit `no` form. The
	 * northbound destroy path restores the default by calling SET. */
	enabled = operation == EIGRP_SET;
	if (context->config) {
		config = eigrp_metric_config_create(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->traffic_share_balanced = enabled;
	}
	if (context->runtime) {
		context->runtime->traffic_share_balanced = enabled;
		eigrp_topology_traffic_share_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}






/*
 * Syntax:
 *   Named: `metric maximum-hops HOPS` / `no metric maximum-hops`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets the configured EIGRP maximum-hop metric constraint.
 * The target retains the real feature endpoint even while live enforcement is incomplete.
 */
eigrp_result_t eigrp_metric_maximum_hops_update(eigrp_operation_t operation, eigrp_instance_context_t *context, uint8_t maximum_hops)
{
	eigrp_metric_message_args_t message = {
		.type = EIGRP_METRIC_MESSAGE_MAXIMUM_HOPS, .operation = operation,
		.context = context, .data.value = maximum_hops,
	};

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_metric_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->metric_config) {
		context->config->metric_config->maximum_hops_configured = false;
		context->config->metric_config->maximum_hops = 0;
	}
	if (context->runtime)
		context->runtime->max_hops = EIGRP_MAX_HOPS;
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	eigrp_metric_config_t *config;

	if (!maximum_hops)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_create(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->maximum_hops = maximum_hops;
		config->maximum_hops_configured = true;
	}
	if (context->runtime)
		context->runtime->max_hops = maximum_hops;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `metric maximum-hops HOPS` / `no metric maximum-hops`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets the configured EIGRP maximum-hop metric constraint.
 * The target retains the real feature endpoint even while live enforcement is incomplete.
 */



/* Select the highest route TLV version supported by both local policy and peer. */
uint8_t eigrp_metric_version_select(const eigrp_instance_t *eigrp,
				    uint8_t peer_version)
{
	uint8_t local_version = eigrp ? eigrp->metric_version : EIGRP_MAJOR_VERSION;

	if (local_version >= EIGRP_TLV_64B_VERSION
	    && peer_version >= EIGRP_TLV_64B_VERSION)
		return EIGRP_TLV_64B_VERSION;
	return EIGRP_TLV_32B_VERSION;
}

/* `metric version 32bit`: constrain this address family to classic metrics. */
eigrp_result_t eigrp_metric_version_update(eigrp_operation_t operation, eigrp_instance_context_t *context)
{
	eigrp_metric_message_args_t message = {
		.type = EIGRP_METRIC_MESSAGE_VERSION, .operation = operation,
		.context = context,
	};

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_metric_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->metric_config)
		context->config->metric_config->version_32bit = false;
	if (context->runtime) {
		context->runtime->metric_version = EIGRP_MAJOR_VERSION;
		eigrp_nbr_codec_update(context->runtime);
		eigrp_topology_metric_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	eigrp_metric_config_t *config;

	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_create(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->version_32bit = true;
	}
	if (context->runtime) {
		context->runtime->metric_version = EIGRP_TLV_32B_VERSION;
		eigrp_nbr_codec_update(context->runtime);
		eigrp_topology_metric_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/* `no metric version 32bit`: restore Release 2 / Wide Metrics default. */


static eigrp_result_t eigrp_metric_message_process(eigrp_instance_t *eigrp,
	void *arg)
{
	eigrp_metric_message_args_t *message = arg;

	(void)eigrp;
	switch (message->type) {
	case EIGRP_METRIC_MESSAGE_DEFAULT:
		return eigrp_metric_default_update(message->operation, message->context,
			message->data.metric);
	case EIGRP_METRIC_MESSAGE_WEIGHTS:
		return eigrp_metric_weights_update(message->operation, message->context,
			message->data.weights);
	case EIGRP_METRIC_MESSAGE_VARIANCE:
		return eigrp_metric_variance_update(message->operation, message->context,
			message->data.value);
	case EIGRP_METRIC_MESSAGE_TRAFFIC_SHARE:
		return eigrp_traffic_share_balanced_update(message->operation,
			message->context);
	case EIGRP_METRIC_MESSAGE_MAXIMUM_HOPS:
		return eigrp_metric_maximum_hops_update(message->operation,
			message->context, message->data.value);
	case EIGRP_METRIC_MESSAGE_VERSION:
		return eigrp_metric_version_update(message->operation, message->context);
	}
	return EIGRP_RESULT_INVALID_ARGUMENT;
}

void eigrp_metric_config_delete_all(eigrp_af_config_t *af)
{
	if (!af)
		return;
	free(af->metric_config);
	af->metric_config = NULL;
}
