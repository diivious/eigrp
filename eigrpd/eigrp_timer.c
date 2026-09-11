// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP timer configuration and state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include "eigrp_timer.h"

static eigrp_result_t eigrp_timer_request_validate(const eigrp_state_request_t *request)
{
	if (!request)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (request->afi != EIGRP_ADDRESS_FAMILY_IPV4
	    && request->afi != EIGRP_ADDRESS_FAMILY_IPV6)
		return EIGRP_RESULT_UNSUPPORTED;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_timer_active_time_update(eigrp_instance_context_t *context,
					      uint16_t seconds)
{
	(void)seconds;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_timer_active_time_delete(eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_timer_show(const eigrp_state_request_t *request)
{
	eigrp_result_t result = eigrp_timer_request_validate(request);

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
