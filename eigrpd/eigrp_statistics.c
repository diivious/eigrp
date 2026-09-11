// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP traffic and accounting state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include "eigrp_statistics.h"

static eigrp_result_t
 eigrp_statistics_request_validate(const eigrp_state_request_t *request)
{
	if (!request)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (request->afi != EIGRP_ADDRESS_FAMILY_IPV4
	    && request->afi != EIGRP_ADDRESS_FAMILY_IPV6)
		return EIGRP_RESULT_UNSUPPORTED;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_statistics_accounting_show(const eigrp_state_request_t *request)
{
	eigrp_result_t result = eigrp_statistics_request_validate(request);

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_statistics_traffic_show(const eigrp_state_request_t *request)
{
	eigrp_result_t result = eigrp_statistics_request_validate(request);

	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
