// SPDX-License-Identifier: ISC
/*
 * QUERY-retained route metric recomputation regression.
 * Copyright (C) 2026 Donnie V. Savage
 *
 * Exercises the production topology/metric implementation, while stubbing
 * only host RIB/event boundaries.  An unknown QUERY creates a PASSIVE
 * destination with infinite local distance and a finite retained RDB.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp/code/eigrp.h"
#include "eigrp/code/eigrp_log.h"
#include "eigrp/code/eigrp_structs.h"
#include "eigrp/code/eigrp_debug.h"
#include "eigrp/code/eigrp_neighbor.h"
#include "eigrp/code/eigrp_metric.h"
#include "eigrp/code/eigrp_rib.h"
#include "eigrp/code/eigrp_topology.h"

static unsigned installs, withdrawals, updates;
unsigned long term_debug_eigrp;
unsigned long term_debug_eigrp_nei;
unsigned long term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX];
unsigned long term_debug_eigrp_transmit;
unsigned long term_debug_eigrp_notifications;

void eigrp_log(eigrp_log_level_t level, const char *format, ...)
{
	(void)level;
	(void)format;
}
bool eigrp_debug_af_enabled(eigrp_instance_t *eigrp,
                            eigrp_debug_af_category_t category,
                            const eigrp_addr_t *neighbor)
{
	(void)eigrp; (void)category; (void)neighbor;
	return false;
}
eigrp_result_t eigrp_eventlog_msg_add(eigrp_instance_t *eigrp,
    uint16_t opcode, const eigrp_prefix_t *addr, eventmsg_arg_t a,
    eventmsg_arg_t b, eventmsg_arg_t c, eventmsg_arg_t d)
{
	(void)eigrp; (void)opcode; (void)addr;
	(void)a; (void)b; (void)c; (void)d;
	return EIGRP_RESULT_SUCCESS;
}
eigrp_result_t eigrp_instance_rib_event_enqueue(eigrp_instance_t *eigrp,
    eigrp_rib_event_type_t type, const eigrp_rib_route_t *route)
{
	(void)eigrp; (void)type; (void)route;
	return EIGRP_RESULT_SUCCESS;
}
eigrp_result_t eigrp_rib_route_add(eigrp_instance_t *eigrp,
    const eigrp_rib_route_t *route)
{
	(void)eigrp; (void)route;
	installs++;
	return EIGRP_RESULT_SUCCESS;
}
eigrp_result_t eigrp_rib_route_del(eigrp_instance_t *eigrp,
    const eigrp_prefix_t *prefix)
{
	(void)eigrp; (void)prefix;
	withdrawals++;
	return EIGRP_RESULT_SUCCESS;
}
void eigrp_update_send_all(eigrp_instance_t *eigrp, eigrp_intf_t *exception)
{
	(void)eigrp; (void)exception;
	updates++;
}
void eigrp_query_send_all(eigrp_instance_t *eigrp) { (void)eigrp; }
void eigrp_sys_event_cancel(eigrp_event_t **e) { if (e) *e = NULL; }

static void init_runtime(eigrp_instance_t *eigrp, eigrp_nbr_t *self)
{
	memset(eigrp, 0, sizeof(*eigrp));
	eigrp->neighbor_self = self;
	eigrp->AS = 4453;
	eigrp->k_values[0] = 1;
	eigrp->k_values[2] = 1;
	eigrp->variance = 1;
	eigrp->max_paths = 4;
	eigrp->max_hops = 100;
	eigrp->metric_version = EIGRP_TLV_32B_VERSION;
	eigrp->af_vectors.afi = EIGRP_AFI_IPV4;
	eigrp->af_vectors.classic_internal_tlv_type = EIGRP_TLV_IPv4_INT;
	eigrp->af_vectors.classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
	eigrp->topology_table = eigrp_topology_table_create();
	eigrp->topology_changes = eigrp_list_create();
	assert(eigrp->topology_table && eigrp->topology_changes);
}

static eigrp_metrics_t metric(void)
{
	eigrp_metrics_t m = {0};
	m.bandwidth = eigrp_bandwidth_to_scaled(100000);
	m.delay = eigrp_delay_to_scaled(200);
	m.reliability = 255;
	m.load = 1;
	return m;
}

/* Build exactly the storage relationships used by
 * eigrp_query_unknown_route_record(), without invoking route_descriptor_add()
 * (which would install an un-elected path in the RIB). */
static eigrp_route_descriptor_t *add_path(eigrp_instance_t *eigrp,
    eigrp_nbr_t *nbr, eigrp_intf_t *link, unsigned id, bool external,
    bool elected, bool self)
{
	eigrp_prefix_descriptor_t *pfx = eigrp_topology_prefix_create();
	eigrp_route_descriptor_t *r = eigrp_topology_route_create(link);
	eigrp_list_t *queue;
	assert(pfx && r);
	pfx->destination.address.afi = EIGRP_AFI_IPV4;
	pfx->destination.address.bytes[0] = 198;
	pfx->destination.address.bytes[1] = 51;
	pfx->destination.address.bytes[2] = 100;
	pfx->destination.address.bytes[3] = (uint8_t)id;
	pfx->destination.prefix_length = 32;
	pfx->state = EIGRP_FSM_STATE_PASSIVE;
	pfx->nt = external ? EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL
	                   : (self ? EIGRP_TOPOLOGY_TYPE_CONNECTED
	                           : EIGRP_TOPOLOGY_TYPE_REMOTE);
	pfx->reported_metric.delay = EIGRP_MAX_METRIC;
	r->type = external ? EIGRP_TLV_IPv4_EXT : EIGRP_TLV_IPv4_INT;
	r->dest = pfx->destination;
	r->prefix = pfx;
	r->adv_router = self ? eigrp->neighbor_self : nbr;
	r->metric = r->reported_metric = r->total_metric = metric();
	r->reported_distance = self ? 0 : eigrp_metric_calculate(eigrp, r->reported_metric);
	r->distance = self ? eigrp_metric_calculate(eigrp, r->metric)
	                   : eigrp_metric_total_calculate(eigrp, r);
	assert(r->distance != EIGRP_MAX_METRIC);
	if (elected) {
		pfx->distance = pfx->fdistance = pfx->rdistance = r->distance;
		pfx->reported_metric = r->total_metric;
		r->flags = EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
	}
	eigrp_prefix_descriptor_add(eigrp->topology_table, pfx);
	queue = external ? pfx->external_routes : pfx->internal_routes;
	assert(eigrp_list_insert(queue, r));
	return r;
}

static void assert_unelected(eigrp_instance_t *eigrp,
                            eigrp_route_descriptor_t *route)
{
	eigrp_prefix_descriptor_t *pfx = route->prefix;
	assert(pfx->state == EIGRP_FSM_STATE_PASSIVE);
	assert(pfx->distance == EIGRP_MAX_METRIC);
	assert(pfx->fdistance == EIGRP_MAX_METRIC);
	assert(pfx->rdistance == EIGRP_MAX_METRIC);
	assert(pfx->reported_metric.delay == EIGRP_MAX_METRIC);
	assert(route->distance != EIGRP_MAX_METRIC);
	assert(route->reported_distance != EIGRP_MAX_METRIC);
	assert(!(route->flags & (EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG |
	                         EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG)));
	assert(!pfx->req_action);
	assert(!eigrp_list_lookup(eigrp->topology_changes, pfx));
}

int main(void)
{
	eigrp_instance_t eigrp;
	eigrp_nbr_t self = {0}, peer = {0};
	eigrp_intf_t link = {0};
	eigrp_route_descriptor_t *retained_int, *retained_ext, *elected;
	eigrp_route_descriptor_t *local, *connected;
	uint32_t orig_ri, orig_re;
	link.params.bandwidth = 100000;
	link.params.delay = 100;
	link.params.reliability = 255;
	link.params.load = 1;
	init_runtime(&eigrp, &self);
	retained_int = add_path(&eigrp, &peer, &link, 10, false, false, false);
	retained_ext = add_path(&eigrp, &peer, &link, 11, true, false, false);
	elected = add_path(&eigrp, &peer, &link, 12, false, true, false);
	orig_ri = retained_int->distance;
	orig_re = retained_ext->distance;

	/* Recalculation must still refresh route metrics, just not elect them. */
	link.params.delay = 350;
	eigrp.k_values[0] = 2;
	for (unsigned pass = 0; pass < 3; pass++) {
		eigrp_topology_metric_update(&eigrp);
		assert_unelected(&eigrp, retained_int);
		assert_unelected(&eigrp, retained_ext);
		assert(elected->prefix->distance == elected->distance);
		assert(elected->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
		assert(elected->prefix->req_action & EIGRP_FSM_NEED_UPDATE);
		assert(installs == pass + 1);
	}
	assert(retained_int->distance != orig_ri);
	assert(retained_ext->distance != orig_re);
	assert(withdrawals == 0);

	/* A locally originated redistributed route may legitimately recover
	 * from infinity, even if all prefix metrics currently indicate poison. */
	local = add_path(&eigrp, &self, NULL, 13, true, false, true);
	eigrp_topology_metric_update(&eigrp);
	assert(local->prefix->distance == local->distance);
	assert(local->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
	assert_unelected(&eigrp, retained_int);
	assert_unelected(&eigrp, retained_ext);
	assert(installs == 4);
	connected = add_path(&eigrp, &self, &link, 14, false, false, true);
	eigrp_topology_metric_update(&eigrp);
	assert(connected->prefix->distance == connected->distance);
	assert(connected->prefix->reported_metric.delay ==
	       eigrp_delay_to_scaled(link.params.delay));
	assert(connected->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
	assert_unelected(&eigrp, retained_int);
	assert_unelected(&eigrp, retained_ext);
	assert(updates == 5);
	return 0;
}
