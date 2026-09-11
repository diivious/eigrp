// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP event/history state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_EVENT_H_
#define EIGRPD_EIGRP_EVENT_H_

#include "eigrp_result.h"
#include "eigrp_types.h"
#include "eigrp_instance.h"

eigrp_result_t eigrp_event_show(const eigrp_state_request_t *request);
eigrp_result_t eigrp_event_log_size_update(eigrp_instance_context_t *context,
					   uint32_t size);
eigrp_result_t eigrp_event_log_size_delete(eigrp_instance_context_t *context);

#endif /* EIGRPD_EIGRP_EVENT_H_ */
