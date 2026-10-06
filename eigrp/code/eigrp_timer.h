// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP timer configuration and state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_TIMER_H_
#define EIGRPD_EIGRP_TIMER_H_

#include "eigrp_cli.h"
#include "eigrp_mgnt.h"
#include "eigrp_instance.h"
#include "eigrp.h"
#include "eigrp_types.h"

eigrp_result_t eigrp_timer_active_time_update(eigrp_operation_t operation,
	eigrp_instance_context_t *context,
	uint16_t seconds);
void eigrp_timer_config_delete_all(eigrp_af_config_t *af);
uint16_t eigrp_timer_active_time_seconds(const eigrp_instance_t *runtime);
eigrp_result_t eigrp_timer_state_iterate(const eigrp_instance_context_t *context,
				eigrp_timer_state_cb callback, void *arg);

/* AF-owned protocol timers.  Timer callbacks execute on the AF thread. */
void eigrp_timer_add(eigrp_instance_t *eigrp, eigrp_timer_event_t **owner,
		     eigrp_event_callback_t callback, void *arg,
		     uint32_t delay_msec);
void eigrp_timer_cancel(eigrp_instance_t *eigrp, eigrp_timer_event_t **owner);
uint32_t eigrp_timer_remaining_seconds(eigrp_instance_t *eigrp,
				       const eigrp_timer_event_t *event);
void eigrp_timer_cancel_all(eigrp_instance_t *eigrp);

#endif /* EIGRPD_EIGRP_TIMER_H_ */
