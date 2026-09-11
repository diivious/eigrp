// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP traffic and accounting state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_STATISTICS_H_
#define EIGRPD_EIGRP_STATISTICS_H_

#include "eigrp_result.h"
#include "eigrp_types.h"

eigrp_result_t eigrp_statistics_accounting_show(const eigrp_state_request_t *request);
eigrp_result_t eigrp_statistics_traffic_show(const eigrp_state_request_t *request);

#endif /* EIGRPD_EIGRP_STATISTICS_H_ */
