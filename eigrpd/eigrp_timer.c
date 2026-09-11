// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP timer configuration and state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stddef.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrp_timer.h"
#include "eigrp_interface.h"

struct eigrp_timer_walk_context {
	eigrp_timer_state_cb callback;
	void *arg;
};

static eigrp_result_t eigrp_timer_interface_state(
	const eigrp_interface_state_t *interface, void *arg)
{
	struct eigrp_timer_walk_context *context = arg;
	eigrp_timer_state_t state = {
		.interface_name = interface->interface_name,
		.config_present = interface->config_present,
		.runtime_present = interface->runtime_present,
		.hello_interval_configured = interface->hello_interval_configured,
		.hold_time_configured = interface->hold_time_configured,
		.hello_interval = interface->hello_interval,
		.hold_time = interface->hold_time,
	};

	return context->callback(&state, context->arg);
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

eigrp_result_t eigrp_timer_show(const eigrp_instance_context_t *context,
				eigrp_timer_state_cb callback, void *arg)
{
	struct eigrp_timer_walk_context walk = {
		.callback = callback,
		.arg = arg,
	};
	eigrp_result_t result;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (!callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	result = eigrp_interface_state_walk(context->config, context->runtime, NULL,
					    eigrp_timer_interface_state, &walk);
	if (result == EIGRP_RESULT_NOT_FOUND)
		return EIGRP_RESULT_SUCCESS;
	return result;
}
