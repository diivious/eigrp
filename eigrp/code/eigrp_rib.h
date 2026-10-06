// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Public EIGRP routing-table exchange contract.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef EIGRPD_EIGRP_RIB_H_
#define EIGRPD_EIGRP_RIB_H_

#include "eigrp.h"

typedef struct eigrp_rib_event eigrp_rib_event_t;

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
	/* Host/RIB scalar metadata.  This is not an EIGRP seed vector. */
	uint64_t metric;
	uint32_t tag;
	union {
		struct {
			uint32_t admin_dist;
			eigrp_rib_route_type_t type;
		} install;
		struct {
			eigrp_redist_source_t source;
			/* A zero bandwidth+delay pair means no inherent EIGRP vector.
			 * The parent/host process populates this before enqueue for
			 * connected routes, interface-static routes, and preserved
			 * EIGRP routes.  The AF never resolves host interface state.
			 */
			eigrp_metrics_t vecmetric;
		} redist;
	};
} eigrp_rib_route_t;


typedef enum eigrp_rib_event_type {
	EIGRP_RIB_EVENT_UPDATE = 0,
	EIGRP_RIB_EVENT_DELETE,
} eigrp_rib_event_type_t;

/*
 * Host-originated RIB work crosses the process/AF boundary as the same
 * portable source-route snapshot consumed by redistribution.  Successful
 * enqueue transfers ownership of this event to the AF thread.
 */
struct eigrp_rib_event {
	eigrp_rib_event_type_t type;
	eigrp_rib_route_t route;
	eigrp_rib_nexthop_t *nexthops;
	eigrp_rib_event_t *next;
};

void eigrp_rib_init(void);
void eigrp_rib_finish(void);
void eigrp_rib_instance_delete(eigrp_instance_t *eigrp);
eigrp_result_t eigrp_rib_route_add(eigrp_instance_t *eigrp,
					const eigrp_rib_route_t *route);
eigrp_result_t eigrp_rib_route_del(eigrp_instance_t *eigrp,
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
eigrp_result_t eigrp_rib_redist_add(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *route);
eigrp_result_t eigrp_rib_redist_del(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *route);
eigrp_result_t eigrp_rib_event_process(
	eigrp_instance_t *eigrp, const eigrp_rib_event_t *event);

#endif /* EIGRPD_EIGRP_RIB_H_ */
