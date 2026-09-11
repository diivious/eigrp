// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP timer configuration and state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_TIMER_H_
#define EIGRPD_EIGRP_TIMER_H_

#include "eigrp_instance.h"
#include "eigrp_result.h"
#include "eigrp_types.h"

eigrp_result_t eigrp_timer_active_time_update(eigrp_instance_context_t *context,
					      uint16_t seconds);
eigrp_result_t eigrp_timer_active_time_delete(eigrp_instance_context_t *context);
eigrp_result_t eigrp_timer_show(const eigrp_state_request_t *request);

#endif /* EIGRPD_EIGRP_TIMER_H_ */
