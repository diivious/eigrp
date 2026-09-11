// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cross-module EIGRP status aggregation targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_STATUS_H_
#define EIGRPD_EIGRP_STATUS_H_

#include "eigrp_instance.h"
#include "eigrp_result.h"

typedef struct eigrp_status_protocol_state {
	const char *instance_name;
	eigrp_address_family_config_t *config;
	eigrp_address_family_t afi;
	const char *vrf_name;
	uint16_t asn;
	bool shutdown;
	bool router_id_configured;
	uint32_t router_id;
} eigrp_status_protocol_state_t;

typedef eigrp_result_t (*eigrp_status_protocol_cb)(
	const eigrp_status_protocol_state_t *state, void *arg);

/*
 * Protocol summary and tech-support are intentionally aggregate operations:
 * neither belongs to one protocol subsystem, so they share this small status
 * module rather than being left in a miscellaneous operational bucket.
 */
eigrp_result_t eigrp_status_protocol_show(eigrp_status_protocol_cb callback,
					  void *arg);
eigrp_result_t eigrp_status_tech_support_show(eigrp_status_protocol_cb callback,
					      void *arg);

#endif /* EIGRPD_EIGRP_STATUS_H_ */
