// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Public EIGRP routing-table exchange contract.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_RIB_H_
#define EIGRPD_EIGRP_RIB_H_

#include "eigrp.h"

typedef enum eigrp_rib_route_type {
	EIGRP_RIB_ROUTE_INTERNAL = 0,
	EIGRP_RIB_ROUTE_EXTERNAL,
} eigrp_rib_route_type_t;

typedef struct eigrp_rib_nexthop {
	eigrp_ifindex_t ifindex;
	bool gateway_present;
	eigrp_address_t gateway;
	/* Forwarding share hint. Zero means host-default/unweighted behavior. */
	uint64_t weight;
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
	/* Host/RIB scalar metadata.  This is not an EIGRP seed vector. */
	uint64_t metric;
	uint32_t tag;
	eigrp_redist_source_t source;
	/* Optional native vector supplied only when the source protocol can
	 * preserve EIGRP per-route metric state.  Keep the native vector intact;
	 * do not reconstruct it from the scalar RIB metric above.
	 */
	bool eigrp_vector_present;
	eigrp_metrics_t eigrp_vector;
} eigrp_rib_source_route_t;

void eigrp_rib_init(void);
void eigrp_rib_finish(void);
void eigrp_rib_instance_delete(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_rib_route_install(eigrp_instance_t *eigrp,
					const eigrp_rib_route_t *route);
eigrp_result_t eigrp_rib_route_remove(eigrp_instance_t *eigrp,
				       const eigrp_prefix_t *prefix);
/* Replay currently installed EIGRP routes after host-RIB reconnect. */
eigrp_result_t eigrp_rib_routes_replay_instance(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_rib_routes_replay(void);

/* Redistribution subscription/configuration at the host RIB boundary. */
eigrp_result_t eigrp_rib_redistribute_add(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source);
eigrp_result_t eigrp_rib_redistribute_remove(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source);

/* Host-originated route lifecycle.  Runtime consumption may be capability-gated. */
eigrp_result_t eigrp_rib_source_route_add(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *route);
eigrp_result_t eigrp_rib_source_route_remove(
	eigrp_instance_t *eigrp, const eigrp_rib_source_route_t *route);

#endif /* EIGRPD_EIGRP_RIB_H_ */
