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

typedef enum eigrp_timer_state_type {
	EIGRP_TIMER_STATE_HELLO = 0,
	EIGRP_TIMER_STATE_PEER_HOLD,
} eigrp_timer_state_type_t;

typedef struct eigrp_timer_state {
	eigrp_timer_state_type_t type;
	const char *interface_name;
	bool neighbor_present;
	eigrp_address_t neighbor_address;
	uint32_t expiration_seconds;
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
