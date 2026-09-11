// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP event/history state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include "eigrp_event.h"

eigrp_result_t eigrp_event_show(const eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_event_log_size_update(eigrp_instance_context_t *context,
					   uint32_t size)
{
	(void)size;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_event_log_size_delete(eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
