/*
 * EIGRP Metric Math Functions.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 */
#include <stdlib.h>
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_metric.h"

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
	bool holddown_enabled;
};


void eigrp_metric_values_convert(const eigrp_metric_values_t *values,
				eigrp_metrics_t *metric)
{
	if (!metric)
		return;
	memset(metric, 0, sizeof(*metric));
	if (!values)
		return;

	metric->bandwidth = values->bandwidth;
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

eigrp_metric_t eigrp_calculate_metrics(eigrp_instance_t *eigrp,
				       eigrp_metrics_t metric)
{
	eigrp_metric_t composite;
	composite = 0;

	if (metric.delay == EIGRP_METRIC_MAX)
		return EIGRP_METRIC_MAX;

	// EIGRP Composite =
	// {K1*BW+[(K2*BW)/(256-load)]+(K3*delay)}*{K5/(reliability+K4)}

	if (eigrp->k_values[0])
		composite += (eigrp->k_values[0] * metric.bandwidth);
	if (eigrp->k_values[1])
		composite += ((eigrp->k_values[1] * metric.bandwidth)
			      / (256 - metric.load));
	if (eigrp->k_values[2])
		composite += (eigrp->k_values[2] * metric.delay);
	if (eigrp->k_values[3] && !eigrp->k_values[4])
		composite *= eigrp->k_values[3];
	if (!eigrp->k_values[3] && eigrp->k_values[4])
		composite *= (eigrp->k_values[4] / metric.reliability);
	if (eigrp->k_values[3] && eigrp->k_values[4])
		composite *= ((eigrp->k_values[4] / metric.reliability)
			      + eigrp->k_values[3]);

	composite =
		(composite <= EIGRP_METRIC_MAX) ? composite : EIGRP_METRIC_MAX;

	return composite;
}

eigrp_metric_t eigrp_calculate_total_metrics(eigrp_instance_t *eigrp,
					     eigrp_route_descriptor_t *entry)
{
	eigrp_interface_t *ei = entry->ei;
	eigrp_delay_t link_delay;
	eigrp_bandwidth_t bw;

	entry->total_metric = entry->reported_metric;
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

	return eigrp_calculate_metrics(eigrp, entry->total_metric);
}

bool eigrp_metrics_is_same(eigrp_metrics_t metric1, eigrp_metrics_t metric2)
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

static eigrp_metric_config_t *eigrp_metric_config_get(
	eigrp_address_family_config_t *af)
{
	if (!af)
		return NULL;
	if (!af->metric_config) {
		af->metric_config = calloc(1, sizeof(*af->metric_config));
		if (!af->metric_config)
			return NULL;
		af->metric_config->traffic_share_balanced = true;
		af->metric_config->holddown_enabled = true;
	}
	return af->metric_config;
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
eigrp_result_t eigrp_metric_default_update(eigrp_instance_context_t *context,
					   const eigrp_metric_values_t *metric)
{
	eigrp_metric_config_t *config;

	if (!eigrp_metric_values_valid(metric))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_get(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->default_metric = *metric;
		config->default_metric_configured = true;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
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
eigrp_result_t eigrp_metric_default_delete(eigrp_instance_context_t *context)
{
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->metric_config) {
		context->config->metric_config->default_metric_configured = false;
		memset(&context->config->metric_config->default_metric, 0,
		       sizeof(context->config->metric_config->default_metric));
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
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
eigrp_result_t eigrp_metric_weights_update(eigrp_instance_context_t *context,
					   const eigrp_metric_weights_t *weights)
{
	eigrp_metric_config_t *config;

	if (!weights || weights->tos != 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_get(context->config);
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
eigrp_result_t eigrp_metric_weights_delete(eigrp_instance_context_t *context)
{
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
eigrp_result_t eigrp_metric_variance_update(eigrp_instance_context_t *context,
					    uint8_t variance)
{
	eigrp_metric_config_t *config;

	if (variance < 1 || variance > 128)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_get(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->variance = variance;
		config->variance_configured = true;
	}
	if (context->runtime)
		context->runtime->variance = variance;
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
eigrp_result_t eigrp_metric_variance_delete(eigrp_instance_context_t *context)
{
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->metric_config) {
		context->config->metric_config->variance_configured = false;
		context->config->metric_config->variance = 0;
	}
	if (context->runtime)
		context->runtime->variance = EIGRP_VARIANCE_DEFAULT;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `traffic-share balanced` / `no traffic-share balanced`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Selects retained balanced traffic-sharing behavior.
 * The target reports NOT_IMPLEMENTED until the forwarding/runtime application path exists.
 */
eigrp_result_t eigrp_metric_traffic_share_balanced_update(
	eigrp_instance_context_t *context, bool enabled)
{
	eigrp_metric_config_t *config;

	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_get(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->traffic_share_balanced = enabled;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
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
eigrp_result_t eigrp_metric_maximum_hops_update(
	eigrp_instance_context_t *context, uint8_t maximum_hops)
{
	eigrp_metric_config_t *config;

	if (!maximum_hops)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_get(context->config);
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
eigrp_result_t eigrp_metric_maximum_hops_delete(
	eigrp_instance_context_t *context)
{
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

/*
 * Syntax:
 *   Named: `metric holddown` / `no metric holddown`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets retained metric holddown configuration.
 * The target returns NOT_IMPLEMENTED when no corresponding runtime behavior exists.
 */
eigrp_result_t eigrp_metric_holddown_update(eigrp_instance_context_t *context,
					    bool enabled)
{
	eigrp_metric_config_t *config;

	if (!eigrp_metric_context_valid(context))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		config = eigrp_metric_config_get(context->config);
		if (!config)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		config->holddown_enabled = enabled;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `metric holddown` / `no metric holddown`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets retained metric holddown configuration.
 * The target returns NOT_IMPLEMENTED when no corresponding runtime behavior exists.
 */
eigrp_result_t eigrp_metric_holddown_delete(eigrp_instance_context_t *context)
{
	return eigrp_metric_holddown_update(context, true);
}

void eigrp_metric_config_delete_all(eigrp_address_family_config_t *af)
{
	if (!af)
		return;
	free(af->metric_config);
	af->metric_config = NULL;
}
