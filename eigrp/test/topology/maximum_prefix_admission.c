// SPDX-License-Identifier: ISC
/*
 * Maximum-prefix admission for QUERY-retained and poisoned destinations.
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
#include "eigrp/code/eigrp_instance.h"
#include "eigrp/code/eigrp_eventlog.h"

/* This fixture tests topology slot accounting; process penalty lifecycle
 * is exercised by the dedicated process-limit UUT scenarios. */
void eigrp_process_limit_exceeded(eigrp_instance_t *runtime,
                                  const eigrp_prefix_limit_t *limit)
{
    (void)runtime;
    (void)limit;
}

static unsigned installs, withdrawals, updates;
static eigrp_af_config_t policy;
#ifdef TEST_NEIGHBOR_LIMIT
void eigrp_eventlog_addr_from_legacy(eigrp_prefix_t *p, const eigrp_addr_t *a)
{ (void)a; memset(p, 0, sizeof(*p)); }
uint64_t eigrp_sys_monotime_msec(void) { return 123456U; }
/* The real neighbor policy lives in eigrp_neighbor.c.  Its unrelated
 * static-peer methods refer to these host-facing functions.  None is
 * called by this admission test; only the missing link targets are stubbed. */
void eigrp_hello_send_unicast(eigrp_intf_t *ei, const eigrp_addr_t *dst)
{ (void)ei; (void)dst; }
eigrp_intf_t *eigrp_intf_lookup_by_name(eigrp_instance_t *eigrp,
                                       const char *name)
{ (void)eigrp; (void)name; return NULL; }
const char *eigrp_intf_name_string(eigrp_intf_t *ei)
{ return ei ? ei->name : "-"; }
#endif


eigrp_af_config_t *eigrp_af_config_runtime_read(eigrp_instance_t *runtime)
{
    (void)runtime;
    return &policy;
}

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

/* Test real acceptance semantics for an installed prefix with an
 * unselected but usable external path.  A poisoned internal successor
 * cannot free capacity while a finite alternate still owns that prefix;
 * removing the alternate does free capacity, and reviving it consumes it. */
static void assert_usable_alternate_counts(void)
{
    eigrp_instance_t eigrp;
    eigrp_nbr_t self = {0}, peer = {0};
    eigrp_intf_t link = {0};
    eigrp_route_descriptor_t *selected, *alternate;
    eigrp_prefix_t target = {0};
    link.params.bandwidth = 100000;
    link.params.delay = 100;
    link.params.reliability = 255;
    link.params.load = 1;
    init_runtime(&eigrp, &self);
    memset(&policy, 0, sizeof(policy));
    policy.topology_maximum_prefix_configured = true;
    policy.topology_maximum_prefix.maximum = 1;
    selected = add_path(&eigrp, &self, &link, 20, false, true, true);
    alternate = eigrp_topology_route_create(&link);
    assert(alternate);
    alternate->type = EIGRP_TLV_IPv4_EXT;
    alternate->adv_router = &peer;
    alternate->prefix = selected->prefix;
    alternate->dest = selected->dest;
    alternate->metric = alternate->reported_metric = alternate->total_metric = metric();
    alternate->reported_distance = eigrp_metric_calculate(&eigrp, metric());
    alternate->distance = eigrp_metric_total_calculate(&eigrp, alternate);
    alternate->flags = 0; /* Genuine alternate with no successor/FS flags. */
    assert(eigrp_list_insert(selected->prefix->external_routes, alternate));
    target = selected->dest;
    target.address.bytes[3] = 21;
    selected->distance = EIGRP_MAX_METRIC;
    selected->metric.delay = EIGRP_MAX_METRIC;
    selected->flags = 0;
    assert(!eigrp_topology_prefix_admit(&eigrp, &target));
    alternate->metric.delay = EIGRP_MAX_METRIC;
    /* Reachability changes alone do not release topology capacity. */
    assert(!eigrp_topology_prefix_admit(&eigrp, &target));
    alternate->metric.delay = eigrp_delay_to_scaled(200);
    assert(!eigrp_topology_prefix_admit(&eigrp, &target));
    /* Actual descriptor retirement releases the occupied destination. */
    eigrp_prefix_descriptor_delete(&eigrp, eigrp.topology_table,
                                   selected->prefix);
    assert(eigrp_topology_prefix_admit(&eigrp, &target));
    eigrp_topology_table_delete(&eigrp, eigrp.topology_table);
    eigrp_list_delete(&eigrp.topology_changes);
}

/* Boundary test uses real topology table, elected flags and admission API.
 * QUERY-retained RDBs have finite neighbor distance but no DUAL successor. */
int main(void)
{
    eigrp_instance_t eigrp;
    eigrp_nbr_t self = {0}, peer = {0};
    eigrp_intf_t link = {0};
    eigrp_route_descriptor_t *installed, *query_int, *query_ext, *replacement;
    eigrp_prefix_t absent = {0};
    link.params.bandwidth = 100000;
    link.params.delay = 100;
    link.params.reliability = 255;
    link.params.load = 1;
    init_runtime(&eigrp, &self);
    memset(&policy, 0, sizeof(policy));
    policy.topology_maximum_prefix_configured = true;
    policy.topology_maximum_prefix.maximum = 3;

#ifdef TEST_NEIGHBOR_LIMIT
    { /* Configure the real neighbor policy, not a test-specific shortcut. */
        eigrp_instance_context_t context = {.config = &policy};
        eigrp_prefix_limit_t neighbor_limit = {.maximum = 3};
        assert(eigrp_nbr_max_prefix_all_update(EIGRP_SET, &context,
                     &neighbor_limit) == EIGRP_RESULT_SUCCESS);
    }
    peer.src.afi = AF_INET;
    peer.src.ip.v4.s_addr = htonl(0x0a000002);
#endif
    installed = add_path(&eigrp, &peer, &link, 10, false, true, false);
    query_int = add_path(&eigrp, &peer, &link, 11, false, false, false);
    query_ext = add_path(&eigrp, &peer, &link, 12, true, false, false);
    assert_unelected(&eigrp, query_int);
    assert_unelected(&eigrp, query_ext);
    absent = query_int->prefix->destination;
    absent.address.bytes[3] = 14;

    /* All three destinations, including QUERY-only entries, consume slots.
     * Refreshes of retained destinations do not allocate a fourth slot. */
    assert(eigrp_topology_prefix_admit(&eigrp, &installed->prefix->destination));
    assert(!eigrp_topology_prefix_admit(&eigrp, &absent));
    assert(eigrp_topology_prefix_admit(&eigrp, &query_int->dest));
    assert(eigrp_topology_prefix_admit(&eigrp, &query_ext->dest));
#ifdef TEST_NEIGHBOR_LIMIT
    assert(eigrp_nbr_prefix_admit(&eigrp, &peer, &installed->dest));
    assert(!eigrp_nbr_prefix_admit(&eigrp, &peer, &absent));
    assert(eigrp_nbr_prefix_admit(&eigrp, &peer, &query_int->dest));
    assert(eigrp_nbr_prefix_admit(&eigrp, &peer, &query_ext->dest));
#endif

    /* An unreachable path retains its descriptor and continues to count.
     * Only actual retirement releases capacity. */
    installed->metric.delay = EIGRP_MAX_METRIC;
    installed->distance = EIGRP_MAX_METRIC;
    installed->prefix->distance = EIGRP_MAX_METRIC;
    installed->prefix->reported_metric.delay = EIGRP_MAX_METRIC;
    assert(!eigrp_topology_prefix_admit(&eigrp, &absent));
    assert(eigrp_topology_prefix_admit(&eigrp, &query_int->dest));
    assert(eigrp_topology_prefix_admit(&eigrp, &query_ext->dest));
#ifdef TEST_NEIGHBOR_LIMIT
    assert(!eigrp_nbr_prefix_admit(&eigrp, &peer, &absent));
    assert(eigrp_nbr_prefix_admit(&eigrp, &peer, &query_int->dest));
    assert(eigrp_nbr_prefix_admit(&eigrp, &peer, &query_ext->dest));
#endif

    /* An incomplete recalculation cannot convert a QUERY-only destination
     * into an accepted path merely by making the scalar distance finite.
     * Its DUAL feasible distance remains infinite. */
    query_int->prefix->distance = query_int->distance;
    query_int->prefix->reported_metric = query_int->total_metric;
    assert(!eigrp_topology_prefix_admit(&eigrp, &absent));
    query_int->prefix->distance = EIGRP_MAX_METRIC;
    query_int->prefix->reported_metric.delay = EIGRP_MAX_METRIC;

    /* A fixture-inserted replacement is also retained, and cannot free
     * capacity until its descriptor is retired. */
    replacement = add_path(&eigrp, &peer, &link, 13, false, true, false);
    assert(eigrp_topology_prefix_admit(&eigrp, &replacement->dest));
    assert(!eigrp_topology_prefix_admit(&eigrp, &absent));
    assert(eigrp_topology_prefix_admit(&eigrp, &query_int->dest));
    assert(eigrp_topology_prefix_admit(&eigrp, &query_ext->dest));
#ifdef TEST_NEIGHBOR_LIMIT
    assert(eigrp_nbr_prefix_admit(&eigrp, &peer, &replacement->dest));
    assert(eigrp_nbr_prefix_admit(&eigrp, &peer, &query_int->dest));
    assert(eigrp_nbr_prefix_admit(&eigrp, &peer, &query_ext->dest));
#endif

    /* A genuine withdrawal of replacement frees capacity again. */
    replacement->distance = EIGRP_MAX_METRIC;
    replacement->prefix->distance = EIGRP_MAX_METRIC;
    replacement->prefix->reported_metric.delay = EIGRP_MAX_METRIC;
    assert(!eigrp_topology_prefix_admit(&eigrp, &absent));
    assert_unelected(&eigrp, query_ext);
    assert_usable_alternate_counts();
    return 0;
}
