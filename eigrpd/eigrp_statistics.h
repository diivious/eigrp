// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP traffic and accounting state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_STATISTICS_H_
#define EIGRPD_EIGRP_STATISTICS_H_

#include "eigrp_instance.h"
#include "eigrp_result.h"
#include "eigrp_types.h"

#define EIGRP_STATISTICS_TRAFFIC_ACK (1U << 0)
#define EIGRP_STATISTICS_TRAFFIC_HELLO (1U << 1)
#define EIGRP_STATISTICS_TRAFFIC_QUERY (1U << 2)
#define EIGRP_STATISTICS_TRAFFIC_REPLY (1U << 3)
#define EIGRP_STATISTICS_TRAFFIC_UPDATE (1U << 4)
#define EIGRP_STATISTICS_TRAFFIC_SIA_QUERY (1U << 5)
#define EIGRP_STATISTICS_TRAFFIC_SIA_REPLY (1U << 6)

typedef struct eigrp_statistics_traffic_state {
	uint16_t sent_valid;
	uint16_t received_valid;
	uint64_t sent_ack;
	uint64_t sent_hello;
	uint64_t sent_query;
	uint64_t sent_reply;
	uint64_t sent_update;
	uint64_t sent_sia_query;
	uint64_t sent_sia_reply;
	uint64_t received_ack;
	uint64_t received_hello;
	uint64_t received_query;
	uint64_t received_reply;
	uint64_t received_update;
	uint64_t received_sia_query;
	uint64_t received_sia_reply;
} eigrp_statistics_traffic_state_t;

typedef struct eigrp_statistics_accounting_state {
	eigrp_address_t neighbor_address;
	const char *interface_name;
	const char *neighbor_state;
	uint32_t prefix_count;
} eigrp_statistics_accounting_state_t;

typedef eigrp_result_t (*eigrp_statistics_accounting_cb)(
	const eigrp_statistics_accounting_state_t *state, void *arg);

eigrp_result_t eigrp_statistics_accounting_show(
	const eigrp_instance_context_t *context, uint32_t *total_prefix_count,
	eigrp_statistics_accounting_cb callback, void *arg);
eigrp_result_t eigrp_statistics_traffic_show(
	const eigrp_instance_context_t *context,
	eigrp_statistics_traffic_state_t *state);

#endif /* EIGRPD_EIGRP_STATISTICS_H_ */
