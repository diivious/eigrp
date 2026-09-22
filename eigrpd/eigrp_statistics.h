// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP traffic and accounting state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_STATISTICS_H_
#define EIGRPD_EIGRP_STATISTICS_H_

#include "eigrp_cli.h"
#include "eigrp_mgnt.h"
#include "eigrp_instance.h"
#include "eigrp.h"
#include "eigrp_types.h"

#define EIGRP_STATISTICS_TRAFFIC_ACK (1U << 0)
#define EIGRP_STATISTICS_TRAFFIC_HELLO (1U << 1)
#define EIGRP_STATISTICS_TRAFFIC_QUERY (1U << 2)
#define EIGRP_STATISTICS_TRAFFIC_REPLY (1U << 3)
#define EIGRP_STATISTICS_TRAFFIC_UPDATE (1U << 4)
#define EIGRP_STATISTICS_TRAFFIC_SIA_QUERY (1U << 5)
#define EIGRP_STATISTICS_TRAFFIC_SIA_REPLY (1U << 6)

eigrp_result_t eigrp_statistics_accounting_show(
	const eigrp_instance_context_t *context, uint32_t *total_prefix_count,
	eigrp_statistics_accounting_cb callback, void *arg);
eigrp_result_t eigrp_statistics_traffic_show(
	const eigrp_instance_context_t *context,
	eigrp_statistics_traffic_state_t *state);

#endif /* EIGRPD_EIGRP_STATISTICS_H_ */
