// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Public EIGRP routing-table exchange contract.
 *
 * Host implements install/remove and redistribute subscribe.
 * Portable EIGRP implements source-route add/remove.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_RIB_H_
#define EIGRPD_EIGRP_RIB_H_

#include "eigrpd/eigrp.h"

typedef enum eigrp_rib_route_type {
	EIGRP_RIB_ROUTE_INTERNAL = 0,
	EIGRP_RIB_ROUTE_EXTERNAL,
} eigrp_rib_route_type_t;

typedef struct eigrp_rib_nexthop {
	eigrp_ifindex_t ifindex;
	bool gateway_present;
	eigrp_address_t gateway;
} eigrp_rib_nexthop_t;

typedef struct eigrp_rib_route {
	eigrp_prefix_t prefix;
	const eigrp_rib_nexthop_t *nexthops;
	size_t nexthop_count;
	uint64_t metric;
	uint32_t administrative_distance;
	uint32_t tag;
	eigrp_rib_route_type_t type;
} eigrp_rib_route_t;

typedef struct eigrp_rib_source_route {
	eigrp_prefix_t prefix;
	eigrp_address_t gateway;
	bool gateway_present;
	eigrp_ifindex_t ifindex;
	uint64_t metric;
	uint32_t tag;
	const char *source_protocol;
} eigrp_rib_source_route_t;

void eigrp_rib_init(void);
void eigrp_rib_finish(void);
void eigrp_rib_instance_delete(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_rib_route_install(eigrp_instance_t *eigrp,
					const eigrp_rib_route_t *route);
eigrp_result_t eigrp_rib_route_remove(eigrp_instance_t *eigrp,
				       const eigrp_prefix_t *prefix);

/* Redistribution subscription/configuration at the host RIB boundary. */
eigrp_result_t eigrp_rib_redistribute_add(
	eigrp_instance_t *eigrp, const char *protocol,
	const eigrp_metric_values_t *metric, const char *route_map);
eigrp_result_t eigrp_rib_redistribute_remove(eigrp_instance_t *eigrp,
					      const char *protocol);

/* Host-originated route lifecycle.  Runtime consumption may be capability-gated. */
eigrp_result_t eigrp_rib_source_route_add(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *route);
eigrp_result_t eigrp_rib_source_route_remove(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *route);

#endif /* EIGRPD_EIGRP_RIB_H_ */
