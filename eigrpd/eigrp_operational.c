// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Portable EIGRP operational command targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include "eigrp_operational.h"

static eigrp_result_t
eigrp_operational_request_validate(const eigrp_operational_request_t *request)
{
	if (!request)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (request->afi != EIGRP_ADDRESS_FAMILY_IPV4
	    && request->afi != EIGRP_ADDRESS_FAMILY_IPV6)
		return EIGRP_RESULT_UNSUPPORTED;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_accounting_show(const eigrp_operational_request_t *request)
{
	eigrp_result_t result = eigrp_operational_request_validate(request);

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_event_show(const eigrp_operational_request_t *request)
{
	eigrp_result_t result = eigrp_operational_request_validate(request);

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_timer_show(const eigrp_operational_request_t *request)
{
	eigrp_result_t result = eigrp_operational_request_validate(request);

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_traffic_show(const eigrp_operational_request_t *request)
{
	eigrp_result_t result = eigrp_operational_request_validate(request);

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_protocol_show(void)
{
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_tech_support_show(void)
{
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
