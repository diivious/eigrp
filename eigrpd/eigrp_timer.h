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

typedef struct eigrp_timer_state {
	const char *interface_name;
	bool config_present;
	bool runtime_present;
	bool hello_interval_configured;
	bool hold_time_configured;
	uint32_t hello_interval;
	uint16_t hold_time;
} eigrp_timer_state_t;

typedef eigrp_result_t (*eigrp_timer_state_cb)(const eigrp_timer_state_t *state,
					       void *arg);

eigrp_result_t eigrp_timer_active_time_update(eigrp_instance_context_t *context,
					      uint16_t seconds);
eigrp_result_t eigrp_timer_active_time_delete(eigrp_instance_context_t *context);
void eigrp_timer_config_delete_all(eigrp_address_family_config_t *af);
eigrp_result_t eigrp_timer_show(const eigrp_instance_context_t *context,
				eigrp_timer_state_cb callback, void *arg);

#endif /* EIGRPD_EIGRP_TIMER_H_ */
