// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Portable EIGRP operational command targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _EIGRP_OPERATIONAL_H_
#define _EIGRP_OPERATIONAL_H_

#include <stdint.h>

#include "eigrp_named.h"
#include "eigrp_result.h"

typedef struct eigrp_operational_request {
	eigrp_address_family_t afi;
	const char *vrf_name;
	uint16_t asn; /* zero means all configured AS contexts */
} eigrp_operational_request_t;

/*
 * These are the stable core targets for operational commands.  A target may
 * return EIGRP_RESULT_NOT_IMPLEMENTED until its data provider is completed,
 * but CLI/VTY code must call the feature-specific target rather than a shared
 * stub dispatcher.
 */
eigrp_result_t eigrp_accounting_show(const eigrp_operational_request_t *request);
eigrp_result_t eigrp_event_show(const eigrp_operational_request_t *request);
eigrp_result_t eigrp_timer_show(const eigrp_operational_request_t *request);
eigrp_result_t eigrp_traffic_show(const eigrp_operational_request_t *request);
eigrp_result_t eigrp_protocol_show(void);
eigrp_result_t eigrp_tech_support_show(void);

#endif /* _EIGRP_OPERATIONAL_H_ */
