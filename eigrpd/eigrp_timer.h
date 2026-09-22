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

eigrp_result_t eigrp_timer_active_time_set(eigrp_instance_context_t *context,
					      uint16_t seconds);
eigrp_result_t eigrp_timer_active_time_reset(eigrp_instance_context_t *context);
void eigrp_timer_config_delete_all(eigrp_address_family_config_t *af);
eigrp_result_t eigrp_timer_show(const eigrp_instance_context_t *context,
				eigrp_timer_state_cb callback, void *arg);

#endif /* EIGRPD_EIGRP_TIMER_H_ */
