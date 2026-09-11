// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cross-module EIGRP status aggregation targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include "eigrp_status.h"

struct eigrp_status_walk_context {
	eigrp_status_protocol_cb callback;
	void *arg;
};

static eigrp_result_t eigrp_status_address_family(
	const char *instance_name, eigrp_address_family_config_t *af, void *arg)
{
	struct eigrp_status_walk_context *context = arg;
	eigrp_status_protocol_state_t state = {
		.instance_name = instance_name,
		.config = af,
		.afi = af->afi,
		.vrf_name = af->vrf_name,
		.asn = af->asn,
		.shutdown = af->shutdown,
		.router_id_configured = af->router_id_configured,
		.router_id = af->router_id,
	};

	return context->callback(&state, context->arg);
}

static eigrp_result_t eigrp_status_walk(eigrp_status_protocol_cb callback,
					void *arg)
{
	struct eigrp_status_walk_context context = {
		.callback = callback,
		.arg = arg,
	};
	eigrp_state_request_t request = {
		.all_vrfs = true,
	};
	eigrp_result_t ipv4_result;
	eigrp_result_t ipv6_result;

	if (!callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	request.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	ipv4_result = eigrp_instance_address_family_walk(
		&request, eigrp_status_address_family, &context);
	if (ipv4_result != EIGRP_RESULT_SUCCESS
	    && ipv4_result != EIGRP_RESULT_NOT_FOUND)
		return ipv4_result;

	request.afi = EIGRP_ADDRESS_FAMILY_IPV6;
	ipv6_result = eigrp_instance_address_family_walk(
		&request, eigrp_status_address_family, &context);
	if (ipv6_result != EIGRP_RESULT_SUCCESS
	    && ipv6_result != EIGRP_RESULT_NOT_FOUND)
		return ipv6_result;

	if (ipv4_result == EIGRP_RESULT_NOT_FOUND
	    && ipv6_result == EIGRP_RESULT_NOT_FOUND)
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_status_protocol_show(eigrp_status_protocol_cb callback,
					  void *arg)
{
	return eigrp_status_walk(callback, arg);
}

eigrp_result_t eigrp_status_tech_support_show(eigrp_status_protocol_cb callback,
					      void *arg)
{
	return eigrp_status_walk(callback, arg);
}
