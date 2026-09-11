// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP redistribution configuration targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stddef.h>

#include "eigrp_redistribute.h"

eigrp_result_t eigrp_redistribute_update(eigrp_instance_context_t *context,
					 const char *protocol,
					 const eigrp_metric_values_t *metric,
					 const char *route_map)
{
	if (!protocol || !protocol[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (metric && (!metric->bandwidth || !metric->load || !metric->mtu))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (route_map && !route_map[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_redistribute_delete(eigrp_instance_context_t *context,
					 const char *protocol)
{
	return eigrp_redistribute_update(context, protocol, NULL, NULL);
}


eigrp_result_t eigrp_redistribute_maximum_prefix_update(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit)
{
	if (!limit || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_redistribute_maximum_prefix_delete(
	eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
