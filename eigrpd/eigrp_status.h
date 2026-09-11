// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cross-module EIGRP status aggregation targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_STATUS_H_
#define EIGRPD_EIGRP_STATUS_H_

#include "eigrp_result.h"

/*
 * Protocol summary and tech-support are intentionally aggregate operations:
 * neither belongs to one protocol subsystem, so they share this small status
 * module rather than being left in a miscellaneous operational bucket.
 */
eigrp_result_t eigrp_status_protocol_show(void);
eigrp_result_t eigrp_status_tech_support_show(void);

#endif /* EIGRPD_EIGRP_STATUS_H_ */
