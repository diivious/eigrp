/* SPDX-License-Identifier: ISC */
/* Copyright (C) 2026 Donnie V. Savage */
/* Portable real-FSM regression for redistributed route withdrawal. */

#include <assert.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp/code/eigrp.h"
#include "eigrp/code/eigrp_log.h"
#include "eigrp/code/eigrp_structs.h"
#include "eigrp/code/eigrp_debug.h"
#include "eigrp/code/eigrp_fsm.h"
#include "eigrp/code/eigrp_neighbor.h"
#include "eigrp/code/eigrp_redistribute.h"
#include "eigrp/code/eigrp_rib.h"
#include "eigrp/code/eigrp_topology.h"
#include "eigrp/code/eigrp_table.h"
#include "eigrp/code/eigrp_pfxlimits.h"
#include "eigrp/code/eigrp_timer.h"

#ifndef EIGRP_TEST_PROCESS_VOLUME
void eigrp_pfxlimits_prefix_exceeded(eigrp_instance_t *runtime,
    const eigrp_prefix_limit_t *limit) { (void)runtime; (void)limit; }
eigrp_result_t eigrp_pfxlimits_state_clear(eigrp_instance_t *runtime)
{ (void)runtime; return EIGRP_RESULT_SUCCESS; }
#else
/* No live peer or socket in this direct 100K-DUAL fixture. */
void eigrp_nbr_down(eigrp_nbr_t *nbr) { (void)nbr; }
void eigrp_nbr_down_deferred(eigrp_nbr_t *nbr) { (void)nbr; }
void eigrp_nbr_limit_release(eigrp_nbr_t *nbr) { (void)nbr; }
bool eigrp_intf_is_passive(eigrp_intf_t *intf) { (void)intf; return false; }
void eigrp_hello_send(eigrp_intf_t *intf, uint8_t type, eigrp_addr_t *nbr)
{ (void)intf; (void)type; (void)nbr; }
void eigrp_update_send_process_GR(eigrp_instance_t *e, enum GR_type type)
{ (void)e; (void)type; }
eigrp_result_t eigrp_nbr_clear(eigrp_instance_t *e,
    const eigrp_nbr_clear_request_t *request, eigrp_nbr_clear_cb callback,
    void *arg, size_t *count)
{ (void)e; (void)request; (void)callback; (void)arg; (void)count;
  return EIGRP_RESULT_SUCCESS; }
#endif
static eigrp_af_config_t *test_runtime_af;
static unsigned query_work;
static unsigned update_work;
static unsigned redist_subscriptions;
static unsigned redist_unsubscriptions;
static eigrp_prefix_descriptor_t *deferred_prefix;

/* This FSM-only harness has no packetizer worker. Capture the retired
 * descriptor to verify detachment occurs before deferred reclamation. */
void eigrp_packetizer_prefix_defer_free(eigrp_instance_t *eigrp,
                                       eigrp_prefix_descriptor_t *prefix)
{
    (void)eigrp;
#ifdef EIGRP_TEST_PROCESS_VOLUME
    /* Model packetizer completion and reclamation on this single-threaded
     * fixture only, after the prefix has been detached from topology. */
    eigrp_topology_prefix_free(prefix);
#else
    assert(prefix && deferred_prefix == NULL);
    deferred_prefix = prefix;
#endif
}

unsigned long term_debug_eigrp;
unsigned long term_debug_eigrp_nei;
unsigned long term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX];
unsigned long term_debug_eigrp_transmit;
unsigned long term_debug_eigrp_notifications;

void eigrp_sys_event_cancel(eigrp_event_t **event)
{
    if (event)
        *event = NULL;
}

void eigrp_log(eigrp_log_level_t level, const char *format, ...)
{
    (void)level;
    (void)format;
}

eigrp_result_t eigrp_eventlog_msg_add(
    eigrp_instance_t *eigrp, uint16_t opcode,
    const eigrp_prefix_t *addr, eventmsg_arg_t arg1,
    eventmsg_arg_t arg2, eventmsg_arg_t arg3, eventmsg_arg_t arg4)
{
    (void)eigrp; (void)opcode; (void)addr;
    (void)arg1; (void)arg2; (void)arg3; (void)arg4;
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_rib_event_enqueue(
    eigrp_instance_t *eigrp, eigrp_rib_event_type_t type,
    const eigrp_rib_route_t *route)
{
    (void)eigrp; (void)type; (void)route;
    return EIGRP_RESULT_SUCCESS;
}

bool eigrp_debug_af_enabled(
    eigrp_instance_t *eigrp,
    eigrp_debug_af_category_t category,
    const eigrp_addr_t *neighbor)
{
    (void)eigrp;
    (void)category;
    (void)neighbor;
    return false;
}


eigrp_result_t eigrp_sys_redistribute_route_map_evaluate(
    eigrp_instance_t *eigrp, const char *name,
    const eigrp_rib_route_t *route, eigrp_filter_decision_t *decision)
{
    (void)eigrp;
    (void)name;
    (void)route;
    if (decision)
        *decision = EIGRP_FILTER_DECISION_PERMIT;
    return EIGRP_RESULT_SUCCESS;
}

eigrp_afi_t
eigrp_instance_afi(const eigrp_instance_t *runtime)
{
    (void)runtime;
    return EIGRP_AFI_IPV4;
}

eigrp_af_config_t *
eigrp_af_config_runtime_read(eigrp_instance_t *runtime)
{
    (void)runtime;
    return test_runtime_af;
}

uint16_t eigrp_instance_asn(const eigrp_instance_t *runtime)
{
    return runtime ? runtime->AS : 0;
}

eigrp_result_t eigrp_rib_redistribute_add(
    eigrp_instance_t *eigrp,
    const eigrp_redist_source_t *source)
{
    (void)eigrp;
    (void)source;
    ++redist_subscriptions;
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_redistribute_remove(
    eigrp_instance_t *eigrp,
    const eigrp_redist_source_t *source)
{
    (void)eigrp;
    (void)source;
    ++redist_unsubscriptions;
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_route_add(
    eigrp_instance_t *eigrp, const eigrp_rib_route_t *route)
{
    (void)eigrp;
    (void)route;
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_route_del(
    eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix)
{
    (void)eigrp;
    (void)prefix;
    return EIGRP_RESULT_SUCCESS;
}

void eigrp_query_send_all(eigrp_instance_t *eigrp)
{
    (void)eigrp;
    query_work++;
}

void eigrp_update_send_all(eigrp_instance_t *eigrp,
                           eigrp_intf_t *exception)
{
    (void)eigrp;
    (void)exception;
    update_work++;
}


/* The test controls available peers but runs the real portable FSM, route
 * lifecycle, metric calculation, and topology implementation. */
static int neighbor_count;
int eigrp_nbr_count(eigrp_instance_t *eigrp)
{
    (void)eigrp;
    return neighbor_count;
}
void eigrp_reply_send(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
                      eigrp_prefix_descriptor_t *prefix)
{
    (void)eigrp; (void)nbr; (void)prefix;
}
void eigrp_reply_send_route(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
                           eigrp_prefix_descriptor_t *prefix,
                           eigrp_route_descriptor_t *route, uint32_t flags)
{
    (void)eigrp; (void)nbr; (void)prefix; (void)route; (void)flags;
}
static eigrp_route_descriptor_t *only_route(
    eigrp_instance_t *runtime, const eigrp_prefix_t *prefix)
{
    eigrp_prefix_descriptor_t *pd =
        eigrp_topology_table_lookup(runtime->topology_table, prefix);
    assert(pd != NULL);
    assert(pd->external_routes->count == 1);
    assert(pd->internal_routes->count == 0);
    return eigrp_list_item_data(eigrp_list_first(pd->external_routes));
}

static void set_ipv4(eigrp_address_t *address,
                     unsigned a, unsigned b,
                     unsigned c, unsigned d)
{
    address->afi = EIGRP_AFI_IPV4;
    address->bytes[0] = (uint8_t)a;
    address->bytes[1] = (uint8_t)b;
    address->bytes[2] = (uint8_t)c;
    address->bytes[3] = (uint8_t)d;
}



/* Unused timer/SIA behavior stays behind EIGRP-owned test doubles. */
void eigrp_debug_neighbor_sia(eigrp_nbr_t *nbr, const char *event)
{
    (void)nbr; (void)event;
}
void eigrp_nbr_state_update_reason(eigrp_operation_t op, eigrp_nbr_t *nbr,
                                   uint8_t state, uint16_t reason)
{
    (void)op; (void)nbr; (void)state; (void)reason;
}
void eigrp_nbr_delete(eigrp_nbr_t *nbr)
{
    (void)nbr;
}
void eigrp_siaquery_send(eigrp_instance_t *eigrp, eigrp_nbr_t *nbr,
                         eigrp_prefix_descriptor_t *prefix)
{
    (void)eigrp; (void)nbr; (void)prefix;
}
uint64_t eigrp_sys_monotime_msec(void)
{
    return 0;
}

struct fixture {
    eigrp_virt_router_t vr;
    eigrp_instance_t runtime;
    eigrp_nbr_t self;
    eigrp_af_config_t af;
    eigrp_redist_source_t source;
    eigrp_rib_route_t candidate;
};

static void fixture_init(struct fixture *f)
{
    eigrp_metric_values_t seed = {
        .bandwidth = 100000, .delay = 100,
        .reliability = 255, .load = 1, .mtu = 1500,
    };
    eigrp_instance_context_t context;

    memset(f, 0, sizeof(*f));
    f->vr.vrid = 1;
    f->af.afi = EIGRP_AFI_IPV4;
    f->af.asn = 4453;
    f->runtime.virt_router = &f->vr;
    f->runtime.AS = 4453;
    f->runtime.variance = 1;
    f->runtime.max_paths = 4;
    f->runtime.max_hops = 100;
    f->runtime.k_values[0] = 1;
    f->runtime.k_values[2] = 1;
    f->runtime.af_vectors.afi = EIGRP_AFI_IPV4;
    f->runtime.af_vectors.classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
    f->runtime.topology_table = eigrp_topology_table_create();
    f->runtime.topology_changes = eigrp_list_create();
    f->runtime.neighbor_self = &f->self;
    assert(f->runtime.topology_table && f->runtime.topology_changes);
    assert(inet_pton(AF_INET, "192.0.2.45", &f->runtime.router_id) == 1);
    test_runtime_af = &f->af;
    f->source.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
    f->source.route_instance = 100;
    context = (eigrp_instance_context_t){
        .config = &f->af, .runtime = &f->runtime,
        .topology_id = EIGRP_TOPOLOGY_ID_BASE,
    };
    assert(eigrp_redist_add(&context, &f->source, &seed, NULL)
           == EIGRP_RESULT_SUCCESS);
    set_ipv4(&f->candidate.prefix.address, 10, 77, 0, 0);
    f->candidate.prefix.prefix_length = 24;
    f->candidate.redist.source = f->source;
    f->candidate.tag = 0x11223344;
    neighbor_count = 0;
    query_work = update_work = 0;
    deferred_prefix = NULL;
    assert(eigrp_rib_redist_add(&f->runtime, &f->candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(update_work == 1);
}

static eigrp_prefix_descriptor_t *fixture_prefix(struct fixture *f)
{
    eigrp_prefix_descriptor_t *prefix =
        eigrp_topology_table_lookup(f->runtime.topology_table,
                                    &f->candidate.prefix);
    assert(prefix);
    return prefix;
}

static void fixture_drain_initial_update(struct fixture *f)
{
    /* Packetization is outside this direct FSM test. Represent successful
     * consumption of the initial advertisement before withdrawal. */
    eigrp_prefix_descriptor_t *prefix = fixture_prefix(f);
    prefix->req_action = 0;
    eigrp_list_delete_data(f->runtime.topology_changes, prefix);
    query_work = update_work = 0;
}

static void fixture_delete(struct fixture *f)
{
    eigrp_topology_table_delete(&f->runtime, f->runtime.topology_table);
    eigrp_list_delete(&f->runtime.topology_changes);
    if (deferred_prefix) {
        eigrp_topology_prefix_free(deferred_prefix);
        deferred_prefix = NULL;
    }
}

static void test_no_neighbors(void)
{
    struct fixture f;
    eigrp_prefix_descriptor_t *prefix;
    fixture_init(&f);
    fixture_drain_initial_update(&f);
    prefix = fixture_prefix(&f);
    assert(eigrp_rib_redist_del(&f.runtime, &f.candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(prefix->state == EIGRP_FSM_STATE_PASSIVE);
    assert(prefix->distance == EIGRP_MAX_METRIC);
    assert(prefix->req_action & EIGRP_FSM_NEED_UPDATE);
    assert(query_work == 0);
    /* LR and the redistribution ingress each drain actions. */
    assert(update_work >= 1);
    fixture_delete(&f);
}

static void test_query_with_poisoned_route(void)
{
    struct fixture f;
    eigrp_prefix_descriptor_t *prefix;
    eigrp_route_descriptor_t *route;
    eigrp_metric_t frozen_fd;
    fixture_init(&f);
    fixture_drain_initial_update(&f);
    prefix = fixture_prefix(&f);
    route = only_route(&f.runtime, &f.candidate.prefix);
    frozen_fd = prefix->fdistance;
    neighbor_count = 1;
    assert(eigrp_rib_redist_del(&f.runtime, &f.candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(prefix->state == EIGRP_FSM_STATE_ACTIVE_1);
    assert(prefix->fdistance == frozen_fd);
    assert(prefix->distance == frozen_fd);
    assert(route->distance == EIGRP_MAX_METRIC);
    assert(route->type == EIGRP_TLV_IPv4_EXT);
    assert(query_work == 1);
    assert(update_work == 0);

    /* A real QUERY packetizer invokes query_sent once it has selected its
     * eligible neighbors.  Zero eligible recipients must complete DUAL. */
    eigrp_fsm_query_sent(&f.runtime, prefix);
    assert(prefix->state == EIGRP_FSM_STATE_PASSIVE);
    assert(prefix->distance == EIGRP_MAX_METRIC);
    assert(prefix->req_action & EIGRP_FSM_NEED_UPDATE);
    fixture_delete(&f);
}

static void test_query_waits_for_peer_reply(void)
{
    struct fixture f;
    eigrp_prefix_descriptor_t *prefix;
    eigrp_nbr_t peer = {0};
    eigrp_intf_t intf = {0};
    eigrp_route_descriptor_t *peer_route;
    eigrp_fsm_action_message_t reply = {0};

    fixture_init(&f);
    fixture_drain_initial_update(&f);
    prefix = fixture_prefix(&f);
    neighbor_count = 1;
    assert(eigrp_rib_redist_del(&f.runtime, &f.candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(prefix->state == EIGRP_FSM_STATE_ACTIVE_1);

    /* The packetizer added the eligible UP neighbor to reply status. */
    intf.params.bandwidth = 100000;
    intf.params.delay = 100;
    intf.eigrp = &f.runtime;
    peer.ei = &intf;
    eigrp_fsm_reply_status_add(prefix, &peer);
    assert(prefix->rij->count == 1);
    eigrp_fsm_query_sent(&f.runtime, prefix);
    assert(prefix->state == EIGRP_FSM_STATE_ACTIVE_1);

    peer_route = eigrp_topology_route_create(&intf);
    assert(peer_route);
    peer_route->type = EIGRP_TLV_IPv4_EXT;
    peer_route->adv_router = &peer;
    peer_route->prefix = prefix;
    reply.packet_type = EIGRP_OPC_REPLY;
    reply.eigrp = &f.runtime;
    reply.adv_router = &peer;
    reply.route = peer_route;
    reply.prefix = prefix;
    reply.data_type = EIGRP_EXT;
    reply.metrics.bandwidth = eigrp_bandwidth_to_scaled(100000);
    reply.metrics.delay = 100;
    reply.metrics.reliability = 255;
    reply.metrics.load = 1;
    eigrp_fsm_event(&reply);
    assert(prefix->rij->count == 0);
    assert(prefix->state == EIGRP_FSM_STATE_PASSIVE);
    assert(prefix->distance != EIGRP_MAX_METRIC);
    fixture_delete(&f);
}

static void test_query_after_route_descriptor_cleanup(void)
{
    struct fixture f;
    eigrp_prefix_descriptor_t *prefix;
    fixture_init(&f);
    fixture_drain_initial_update(&f);
    prefix = fixture_prefix(&f);
    neighbor_count = 1;
    assert(eigrp_rib_redist_del(&f.runtime, &f.candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(prefix->state == EIGRP_FSM_STATE_ACTIVE_1);
    assert(query_work == 1);
    assert(prefix->req_action & EIGRP_FSM_NEED_QUERY);

    /* Path cleanup/neighbor churn can leave no route_descriptor when the
     * pending QUERY is processed. This reproduces the missing-route path
     * in eigrp_fsm_query_sent(), not a fake FSM implementation. */
    eigrp_update_topology_table_prefix(&f.runtime,
                                       f.runtime.topology_table, prefix);
    assert(prefix->external_routes->count == 0);
    assert(prefix->internal_routes->count == 0);
    assert(eigrp_topology_route_read(prefix) == NULL);

    /* Expected: no eligible QUERY recipients is the last-reply condition,
     * even when the last route_descriptor was removed. This must not
     * return early and leave the destination ACTIVE. */
    eigrp_fsm_query_sent(&f.runtime, prefix);
    if (prefix->state != EIGRP_FSM_STATE_PASSIVE) {
        fprintf(stderr, "FAIL: route-less redistribution withdrawal remained ACTIVE (%u) after QUERY selection\n", prefix->state);
        exit(1);
    }
    assert(prefix->distance == EIGRP_MAX_METRIC);
    assert(prefix->req_action & EIGRP_FSM_NEED_UPDATE);
    fixture_delete(&f);
}

static void test_empty_destination_retirement(void)
{
    struct fixture f;
    eigrp_prefix_descriptor_t *old;

    fixture_init(&f);
    fixture_drain_initial_update(&f);
    old = fixture_prefix(&f);
    assert(eigrp_rib_redist_del(&f.runtime, &f.candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(old->state == EIGRP_FSM_STATE_PASSIVE);
    /* DUAL must retain the descriptor until the withdrawal is encoded. */
    assert(old->req_action & EIGRP_FSM_NEED_UPDATE);
    eigrp_update_topology_table_prefix(&f.runtime, f.runtime.topology_table, old);
    assert(eigrp_topology_table_lookup(f.runtime.topology_table,
                                      &f.candidate.prefix) == old);
    assert(deferred_prefix == NULL);

    /* Model packetizer successful handoff of the withdrawal. */
    old->req_action = 0;
    eigrp_list_delete_data(f.runtime.topology_changes, old);
    eigrp_update_topology_table_prefix(&f.runtime, f.runtime.topology_table, old);
    assert(eigrp_topology_table_lookup(f.runtime.topology_table,
                                      &f.candidate.prefix) == NULL);
    assert(f.runtime.topology_table->head == NULL);
    assert(f.runtime.topology_table->tail == NULL);
    assert(deferred_prefix == old);
    assert(!old->internal_routes->count && !old->external_routes->count);

    /* A newly learned route gets a fresh destination, not stale DUAL state. */
    assert(eigrp_rib_redist_add(&f.runtime, &f.candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(fixture_prefix(&f) != old);
    assert(f.runtime.topology_table->head == f.runtime.topology_table->tail);
    fixture_delete(&f);
}

/* Reapplying retained redistribution configuration is not operator recovery.
 * Exercise real admission for suspension, plus a locked-state fixture that
 * models completed repeat incidents without relying on wall-clock timers. */
static void test_redist_limit_config_replay(void)
{
    struct fixture f;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit = {.maximum = 1, .restart_count = 3};
    eigrp_prefix_limit_t replay;
    eigrp_rib_route_t second;
    uint64_t restart_due, reset_due;
    unsigned subscribed, unsubscribed;
    unsigned i;

    fixture_init(&f);
    fixture_drain_initial_update(&f);
    context = (eigrp_instance_context_t){.config = &f.af,
        .runtime = &f.runtime, .topology_id = EIGRP_TOPOLOGY_ID_BASE};
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit)
           == EIGRP_RESULT_SUCCESS);
    second = f.candidate;
    set_ipv4(&second.prefix.address, 10, 77, 1, 0);
    assert(eigrp_rib_redist_add(&f.runtime, &second) == EIGRP_RESULT_CONFLICT);
    assert(f.runtime.redist_limit_state != 0);
    assert(f.runtime.redist_limit_restart_cnt == 1);
    /* This standalone FSM harness has no timer scheduler, so the real
     * violation fails closed into lockout. Model a scheduled suspension
     * separately to verify an unchanged SET preserves its deadlines. */
    f.runtime.redist_limit_state = 1;
    f.runtime.redist_limit_restart_due_msec = 60000;
    f.runtime.redist_limit_reset_due_msec = 900000;
    restart_due = f.runtime.redist_limit_restart_due_msec;
    reset_due = f.runtime.redist_limit_reset_due_msec;
    subscribed = redist_subscriptions;
    unsubscribed = redist_unsubscriptions;

    /* A separate config replay and repeated SET must preserve suspension,
     * both deadlines and the incident history without replaying the RIB. */
    for (i = 0; i < 3; i++) {
        replay = limit;
        assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &replay)
               == EIGRP_RESULT_SUCCESS);
        assert(f.runtime.redist_limit_state == 1);
        assert(f.runtime.redist_limit_restart_cnt == 1);
        assert(f.runtime.redist_limit_restart_due_msec == restart_due);
        assert(f.runtime.redist_limit_reset_due_msec == reset_due);
        assert(redist_subscriptions == subscribed);
        assert(redist_unsubscriptions == unsubscribed);
    }

    /* A locked runtime represents the third violation. Replays must not
     * silently unlock it, restart sources or erase dampening history. */
    eigrp_timer_cancel(&f.runtime, &f.runtime.t_redist_limit_restart);
    eigrp_timer_cancel(&f.runtime, &f.runtime.t_redist_limit_reset);
    f.runtime.redist_limit_restart_due_msec = 0;
    f.runtime.redist_limit_reset_due_msec = 0;
    f.runtime.redist_limit_state = 2;
    f.runtime.redist_limit_restart_cnt = 3;
    f.runtime.redist_limit_penalty_q16 = 131072;
    f.runtime.redist_limit_decay_msec = 42;
    for (i = 0; i < 3; i++) {
        /* Equal field values must compare equal despite padding bytes. */
        memset(&replay, 0xa5, sizeof(replay));
        replay.maximum = limit.maximum;
        replay.threshold = limit.threshold;
        replay.warning_only = limit.warning_only;
        replay.dampened = limit.dampened;
        replay.reset_time_minutes = limit.reset_time_minutes;
        replay.restart_minutes = limit.restart_minutes;
        replay.restart_count = limit.restart_count;
        assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &replay)
               == EIGRP_RESULT_SUCCESS);
        assert(f.runtime.redist_limit_state == 2);
        assert(f.runtime.redist_limit_restart_cnt == 3);
        assert(f.runtime.redist_limit_penalty_q16 == 131072);
        assert(f.runtime.redist_limit_decay_msec == 42);
        assert(redist_subscriptions == subscribed);
        assert(redist_unsubscriptions == unsubscribed);
        assert(eigrp_rib_redist_add(&f.runtime, &second)
               == EIGRP_RESULT_CONFLICT);
    }
    replay.maximum = 0;
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &replay)
           == EIGRP_RESULT_INVALID_ARGUMENT);
    assert(f.runtime.redist_limit_state == 2);

    /* Every policy field is meaningful. A real change may manually recover;
     * the next identical SET must again be an idempotent no-op. */
#define CHECK_CHANGED_POLICY() do { \
    f.runtime.redist_limit_state = 2; \
    f.runtime.redist_limit_restart_cnt = 3; \
    f.runtime.redist_limit_penalty_q16 = 131072; \
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit) \
           == EIGRP_RESULT_SUCCESS); \
    assert(f.runtime.redist_limit_state == 0); \
    assert(f.runtime.redist_limit_restart_cnt == 0); \
    assert(f.runtime.redist_limit_penalty_q16 == 0); \
    ++subscribed; ++unsubscribed; \
    assert(redist_subscriptions == subscribed); \
    assert(redist_unsubscriptions == unsubscribed); \
    f.runtime.redist_limit_state = 2; \
    f.runtime.redist_limit_restart_cnt = 3; \
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit) \
           == EIGRP_RESULT_SUCCESS); \
    assert(f.runtime.redist_limit_state == 2); \
    assert(f.runtime.redist_limit_restart_cnt == 3); \
    assert(redist_subscriptions == subscribed); \
    assert(redist_unsubscriptions == unsubscribed); \
} while (0)
    limit.maximum = 2;
    CHECK_CHANGED_POLICY();
    limit.threshold = 75;
    CHECK_CHANGED_POLICY();
    limit.warning_only = true;
    CHECK_CHANGED_POLICY();
    limit.dampened = true;
    CHECK_CHANGED_POLICY();
    limit.reset_time_minutes = 10;
    CHECK_CHANGED_POLICY();
    limit.restart_minutes = 2;
    CHECK_CHANGED_POLICY();
    limit.restart_count = 4;
    CHECK_CHANGED_POLICY();
#undef CHECK_CHANGED_POLICY

    /* An automatic restart can leave admission active with prior incident
     * history and a pending reset deadline. Identical config keeps that
     * history; a real change clears it without unnecessary RIB replay. */
    f.runtime.redist_limit_state = 0;
    f.runtime.redist_limit_restart_cnt = 2;
    f.runtime.redist_limit_penalty_q16 = 131072;
    f.runtime.redist_limit_decay_msec = 42;
    f.runtime.redist_limit_reset_due_msec = 123456;
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_limit_restart_cnt == 2);
    assert(f.runtime.redist_limit_penalty_q16 == 131072);
    assert(f.runtime.redist_limit_reset_due_msec == 123456);
    limit.maximum = 5;
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_limit_state == 0);
    assert(f.runtime.redist_limit_restart_cnt == 0);
    assert(f.runtime.redist_limit_penalty_q16 == 0);
    assert(f.runtime.redist_limit_decay_msec == 0);
    assert(f.runtime.redist_limit_reset_due_msec == 0);
    assert(redist_subscriptions == subscribed);
    assert(redist_unsubscriptions == unsubscribed);
    /* Explicit manual recovery has the same reset-window semantics. */
    f.runtime.redist_limit_restart_cnt = 2;
    assert(eigrp_redist_limit_clear(&f.runtime) == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_limit_restart_cnt == 0);
    assert(redist_subscriptions == subscribed);
    assert(redist_unsubscriptions == unsubscribed);

    /* Removing the configured policy is recovery, but repeating its removal
     * is not an alternative lockout-clear command. */
    f.runtime.redist_limit_state = 2;
    f.runtime.redist_limit_restart_cnt = 3;
    assert(eigrp_redist_max_prefix_update(EIGRP_RESET, &context, NULL)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_limit_state == 0);
    assert(f.runtime.redist_limit_restart_cnt == 0);
    ++subscribed; ++unsubscribed;
    assert(redist_subscriptions == subscribed);
    assert(redist_unsubscriptions == unsubscribed);
    f.runtime.redist_limit_state = 2;
    f.runtime.redist_limit_restart_cnt = 3;
    assert(eigrp_redist_max_prefix_update(EIGRP_RESET, &context, NULL)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_limit_state == 2);
    assert(f.runtime.redist_limit_restart_cnt == 3);
    assert(redist_subscriptions == subscribed);
    assert(redist_unsubscriptions == unsubscribed);
    assert(eigrp_redist_limit_clear(&f.runtime) == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_limit_state == 0);
    assert(f.runtime.redist_limit_restart_cnt == 0);
    fixture_delete(&f);
    eigrp_redist_policy_delete_all(&f.af);
    eigrp_timer_cancel_all(&f.runtime);
}

/* Exercise real admission and topology ownership. In particular an existing
 * connected route owned by neighbor_self does not exempt new redistribution. */
static void test_redist_prefix_accounting(void)
{
    struct fixture f;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit = {.maximum = 1};
    eigrp_rib_route_t second;
    eigrp_prefix_descriptor_t *connected;
    eigrp_route_descriptor_t *connected_route;
    eigrp_intf_t connected_intf = {0};
    eigrp_prefix_descriptor_t *first;

    fixture_init(&f);
    fixture_drain_initial_update(&f);
    assert(f.runtime.redist_prefix_count == 1);
    context = (eigrp_instance_context_t){.config = &f.af,
        .runtime = &f.runtime, .topology_id = EIGRP_TOPOLOGY_ID_BASE};
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit)
           == EIGRP_RESULT_SUCCESS);

    /* Changing an already counted destination does not consume another slot. */
    f.candidate.tag++;
    assert(eigrp_rib_redist_add(&f.runtime, &f.candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_prefix_count == 1);
    second = f.candidate;
    set_ipv4(&second.prefix.address, 10, 77, 1, 0);
    assert(eigrp_rib_redist_add(&f.runtime, &second)
           == EIGRP_RESULT_CONFLICT);
    assert(f.runtime.redist_prefix_count == 1);

    /* Connected and redistributed routes share neighbor_self but are distinct
     * resources. A connected-only destination must not bypass the limit. */
    connected = eigrp_topology_prefix_create();
    connected_route = eigrp_topology_route_create(NULL);
    assert(connected && connected_route);
    connected->destination = second.prefix;
    connected_route->adv_router = &f.self;
    connected_route->type = EIGRP_TLV_IPv4_INT;
    connected_intf.eigrp = &f.runtime;
    connected_intf.ifindex = 3;
    connected_route->ei = &connected_intf;
    eigrp_prefix_descriptor_add(f.runtime.topology_table, connected);
    eigrp_route_descriptor_add(&f.runtime, connected, connected_route);
    assert(!eigrp_topology_redist_present(&f.runtime, connected));
    assert(eigrp_rib_redist_add(&f.runtime, &second)
           == EIGRP_RESULT_CONFLICT);
    assert(f.runtime.redist_prefix_count == 1);

    /* Withdrawal leaves the poisoned route retained until packetization.
     * Admission continues to account for that descriptor. */
    first = fixture_prefix(&f);
    assert(eigrp_rib_redist_del(&f.runtime, &f.candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_prefix_count == 1);
    assert(eigrp_rib_redist_add(&f.runtime, &second)
           == EIGRP_RESULT_CONFLICT);

    first->req_action = 0;
    eigrp_list_delete_data(f.runtime.topology_changes, first);
    eigrp_update_topology_table_prefix(&f.runtime, f.runtime.topology_table, first);
    assert(f.runtime.redist_prefix_count == 0);
    /* A retired descriptor frees accounting, not an active suspension. */
    assert(f.runtime.redist_limit_state != 0);
    assert(eigrp_rib_redist_add(&f.runtime, &second)
           == EIGRP_RESULT_CONFLICT);
    assert(eigrp_redist_max_prefix_update(EIGRP_RESET, &context, NULL)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_limit_state == 0);
    assert(eigrp_rib_redist_add(&f.runtime, &second)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_prefix_count == 1);
    assert(eigrp_topology_redist_present(&f.runtime, connected));

    limit.warning_only = true;
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit)
           == EIGRP_RESULT_SUCCESS);
    set_ipv4(&second.prefix.address, 10, 77, 2, 0);
    assert(eigrp_rib_redist_add(&f.runtime, &second)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_prefix_count == 2);
    fixture_delete(&f);
    assert(f.runtime.redist_prefix_count == 0);
    eigrp_redist_policy_delete_all(&f.af);
}

/* Exercise the real RIB admission and topology ownership over thousands of
 * unique destinations; no source scans or fake counter assignments. */
static void test_redist_prefix_volume(void)
{
    struct fixture f;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit = {.maximum = 100000};
    eigrp_rib_route_t candidate;
    eigrp_prefix_descriptor_t *prefix;
    unsigned i;

    fixture_init(&f);
    fixture_drain_initial_update(&f);
    context = (eigrp_instance_context_t){.config = &f.af,
        .runtime = &f.runtime, .topology_id = EIGRP_TOPOLOGY_ID_BASE};
    assert(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit)
           == EIGRP_RESULT_SUCCESS);
    candidate = f.candidate;
    candidate.prefix.prefix_length = 32;
    for (i = 1; i < 100000; ++i) {
        set_ipv4(&candidate.prefix.address, 198, 18 + ((i >> 16) & 1U),
                 (i >> 8) & 255U, i & 255U);
        assert(eigrp_rib_redist_add(&f.runtime, &candidate)
               == EIGRP_RESULT_SUCCESS);
        assert(f.runtime.redist_prefix_count == i + 1);
        prefix = eigrp_topology_table_lookup(f.runtime.topology_table,
                                             &candidate.prefix);
        assert(prefix);
        prefix->req_action = 0;
        eigrp_list_delete_data(f.runtime.topology_changes, prefix);
    }
    set_ipv4(&candidate.prefix.address, 198, 19, 134, 160);
    assert(eigrp_rib_redist_add(&f.runtime, &candidate)
           == EIGRP_RESULT_CONFLICT);
    assert(f.runtime.redist_prefix_count == 100000);
    fixture_delete(&f);
    assert(f.runtime.redist_prefix_count == 0);
    eigrp_redist_policy_delete_all(&f.af);
}

#ifdef EIGRP_TEST_PROCESS_VOLUME
/* Compare every semantic process-limit field even when padding differs.
 * Reapplying identical policy must not erase accumulated penalty history. */
static void test_process_policy_padding(void)
{
    struct fixture f;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t a, b;
    unsigned field;

    fixture_init(&f);
    fixture_drain_initial_update(&f);
    context = (eigrp_instance_context_t){.config = &f.af,
        .runtime = &f.runtime, .topology_id = EIGRP_TOPOLOGY_ID_BASE};
    memset(&a, 0xa5, sizeof(a));
    memset(&b, 0x5a, sizeof(b));
    a.maximum = b.maximum = 12;
    a.threshold = b.threshold = 75;
    a.warning_only = b.warning_only = false;
    a.dampened = b.dampened = false;
    a.reset_time_minutes = b.reset_time_minutes = 15;
    a.restart_minutes = b.restart_minutes = 5;
    a.restart_count = b.restart_count = 3;
    assert(memcmp(&a, &b, sizeof(a)) != 0);
    assert(eigrp_topology_max_prefix_update(EIGRP_SET, &context, &a)
           == EIGRP_RESULT_SUCCESS);
    f.runtime.eigrp_pfxlimits_af_restart_cnt = 2;
    f.runtime.eigrp_pfxlimits_af_penalty_q16 = 65536;
    assert(eigrp_topology_max_prefix_update(EIGRP_SET, &context, &b)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.eigrp_pfxlimits_af_restart_cnt == 2);
    assert(f.runtime.eigrp_pfxlimits_af_penalty_q16 == 65536);

    for (field = 0; field < 7; ++field) {
        b = a;
        switch (field) {
        case 0: ++b.maximum; break;
        case 1: ++b.threshold; break;
        case 2: b.warning_only = true; break;
        case 3: b.dampened = true; break;
        case 4: ++b.reset_time_minutes; break;
        case 5: ++b.restart_minutes; break;
        case 6: ++b.restart_count; break;
        }
        f.runtime.eigrp_pfxlimits_af_restart_cnt = 2;
        f.runtime.eigrp_pfxlimits_af_penalty_q16 = 65536;
        assert(eigrp_topology_max_prefix_update(EIGRP_SET, &context, &b)
               == EIGRP_RESULT_SUCCESS);
        assert(f.runtime.eigrp_pfxlimits_af_restart_cnt == 0);
        assert(f.runtime.eigrp_pfxlimits_af_penalty_q16 == 0);
        assert(eigrp_topology_max_prefix_update(EIGRP_SET, &context, &a)
               == EIGRP_RESULT_SUCCESS);
    }
    fixture_delete(&f);
}

/* Execute batch topology retirement using the actual DUAL FSM. Both a
 * retained path and an outstanding REPLY with no peer RDB are required. */
static void test_process_batch_dual(void)
{
    struct fixture f;
    eigrp_nbr_t a = {0}, b = {0}, unaffected = {0};
    eigrp_nbr_t *snapshot[2];
    eigrp_intf_t intf = {0};
    eigrp_prefix_descriptor_t *owned, *waiting;
    eigrp_route_descriptor_t *peer_path, *peer_path_b;

    fixture_init(&f);
    fixture_drain_initial_update(&f);
    intf.eigrp = &f.runtime;
    intf.params.bandwidth = 100000;
    intf.params.delay = 100;
    a.ei = b.ei = unaffected.ei = &intf;
    a.published = b.published = unaffected.published = true;
    a.eigrp_pfxlimits_af_suspended = b.eigrp_pfxlimits_af_suspended = true;

    owned = fixture_prefix(&f);
    peer_path = eigrp_topology_route_create(&intf);
    assert(peer_path);
    peer_path->adv_router = &a;
    peer_path->type = EIGRP_TLV_IPv4_INT;
    peer_path->dest = owned->destination;
    peer_path->metric.bandwidth = eigrp_bandwidth_to_scaled(100000);
    peer_path->metric.delay = 100;
    peer_path->metric.reliability = 255;
    peer_path->metric.load = 1;
    peer_path->reported_metric = peer_path->metric;
    peer_path->reported_distance = eigrp_metric_calculate(&f.runtime, peer_path->metric);
    peer_path->distance = eigrp_metric_total_calculate(&f.runtime, peer_path);
    eigrp_route_descriptor_add(&f.runtime, owned, peer_path);
    assert(eigrp_prefix_descriptor_lookup(owned, &a) == peer_path);
    /* Two neighbors share one DNDB. Both DRDBs must be retired by the
     * same pass without processing either neighbor twice. */
    peer_path_b = eigrp_topology_route_create(&intf);
    assert(peer_path_b);
    *peer_path_b = *peer_path;
    peer_path_b->adv_router = &b;
    eigrp_route_descriptor_add(&f.runtime, owned, peer_path_b);
    assert(eigrp_prefix_descriptor_lookup(owned, &b) == peer_path_b);

    waiting = eigrp_topology_prefix_create();
    assert(waiting);
    set_ipv4(&waiting->destination.address, 192, 0, 2, 254);
    waiting->destination.prefix_length = 32;
    waiting->state = EIGRP_FSM_STATE_ACTIVE_1;
    waiting->query_origin = &a;
    eigrp_prefix_descriptor_add(f.runtime.topology_table, waiting);
    eigrp_fsm_reply_status_add(waiting, &b);
    assert(waiting->rij->count == 1);

    eigrp_topology_suspended_neighbors_down(&f.runtime, snapshot, 2);
    assert(!a.topo_batch_seen && !b.topo_batch_seen);
    assert(peer_path->distance == EIGRP_MAX_METRIC);
    assert(peer_path_b->distance == EIGRP_MAX_METRIC);
    assert(owned->state == EIGRP_FSM_STATE_PASSIVE);
    assert(waiting->query_origin == NULL);
    assert(waiting->rij->count == 0);
    assert(waiting->state == EIGRP_FSM_STATE_PASSIVE);
    assert(!unaffected.eigrp_pfxlimits_af_suspended);
    fixture_delete(&f);
}

/* Full real topology/RIB/DUAL path with 100,000 retained redistributed
 * destinations, then an AF process hard-limit violation and withdrawal.
 * Verify actual descriptor ownership after simulated packetizer retirement. */
static void test_process_prefix_volume(void)
{
    struct fixture f;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit = {.maximum = 100000, .restart_count = 3};
    eigrp_rib_route_t candidate, overflow;
    eigrp_prefix_descriptor_t *prefix;
    eigrp_table_node_t *rn, *next;
    unsigned i;

    fixture_init(&f);
    fixture_drain_initial_update(&f);
    assert(pthread_mutex_init(&f.runtime.work_lock, NULL) == 0);
    assert(pthread_cond_init(&f.runtime.work_cond, NULL) == 0);
    f.runtime.thread_running = true;
    context = (eigrp_instance_context_t){.config = &f.af,
        .runtime = &f.runtime, .topology_id = EIGRP_TOPOLOGY_ID_BASE};
    assert(eigrp_topology_max_prefix_update(EIGRP_SET, &context, &limit)
           == EIGRP_RESULT_SUCCESS);
    candidate = f.candidate;
    candidate.prefix.prefix_length = 32;
    for (i = 1; i < 100000; ++i) {
        set_ipv4(&candidate.prefix.address, 198, 18 + ((i >> 16) & 1U),
                 (i >> 8) & 255U, i & 255U);
        assert(eigrp_rib_redist_add(&f.runtime, &candidate)
               == EIGRP_RESULT_SUCCESS);
        prefix = eigrp_topology_table_lookup(f.runtime.topology_table,
                                             &candidate.prefix);
        assert(prefix && prefix->external_routes->count == 1);
        prefix->req_action = 0;
        eigrp_list_delete_data(f.runtime.topology_changes, prefix);
    }
    assert(f.runtime.topology_table->size == 100000);
    assert(f.runtime.redist_prefix_count == 100000);
    overflow = candidate;
    set_ipv4(&overflow.prefix.address, 198, 19, 134, 160);
    assert(eigrp_rib_redist_add(&f.runtime, &overflow)
           == EIGRP_RESULT_CONFLICT);
    assert(f.runtime.eigrp_pfxlimits_af_state == 1);
    assert(f.runtime.eigrp_pfxlimits_af_restart_cnt == 1);
    assert(f.runtime.topology_table->size == 100000);
    assert(f.runtime.redist_prefix_count == 100000);
    assert(!eigrp_topology_table_lookup(f.runtime.topology_table,
                                        &overflow.prefix));
    /* Every previous external path must now be unreachable, but owned until
     * its withdrawal work is consumed. */
    for (rn = eigrp_table_first(f.runtime.topology_table); rn;
         rn = eigrp_table_next(rn)) {
        prefix = rn->info;
        assert(prefix && prefix->external_routes->count == 1);
        assert(only_route(&f.runtime, &prefix->destination)->distance
               == EIGRP_MAX_METRIC);
    }
    /* Consume every deferred withdrawal then prove real DNDB/DRDB
     * retirement, rather than relying on filtered operational output. */
    for (rn = eigrp_table_first(f.runtime.topology_table); rn; rn = next) {
        next = eigrp_table_next(rn);
        prefix = rn->info;
        assert(prefix && (prefix->req_action & EIGRP_FSM_NEED_UPDATE));
        prefix->req_action = 0;
        eigrp_list_delete_data(f.runtime.topology_changes, prefix);
        eigrp_update_topology_table_prefix(&f.runtime,
                                           f.runtime.topology_table, prefix);
    }
    assert(f.runtime.topology_table->size == 0);
    assert(f.runtime.redist_prefix_count == 0);
    assert(f.runtime.eigrp_pfxlimits_af_state == 1);
    assert(eigrp_rib_redist_add(&f.runtime, &candidate)
           == EIGRP_RESULT_CONFLICT);
    assert(eigrp_pfxlimits_state_clear(&f.runtime) == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.eigrp_pfxlimits_af_state == 0);
    assert(!f.runtime.t_pfxlimits_af_restart &&
           !f.runtime.t_pfxlimits_af_reset);
    assert(eigrp_rib_redist_add(&f.runtime, &candidate)
           == EIGRP_RESULT_SUCCESS);
    assert(f.runtime.redist_prefix_count == 1);
    fixture_delete(&f);
    assert(f.runtime.redist_prefix_count == 0);
    eigrp_timer_cancel_all(&f.runtime);
    f.runtime.thread_running = false;
    assert(pthread_cond_destroy(&f.runtime.work_cond) == 0);
    assert(pthread_mutex_destroy(&f.runtime.work_lock) == 0);
}
#endif

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (strcmp(argv[1], "no-neighbors") == 0)
        test_no_neighbors();
    else if (strcmp(argv[1], "query-route") == 0)
        test_query_with_poisoned_route();
    else if (strcmp(argv[1], "query-peer-reply") == 0)
        test_query_waits_for_peer_reply();
    else if (strcmp(argv[1], "query-empty") == 0)
        test_query_after_route_descriptor_cleanup();
    else if (strcmp(argv[1], "retirement") == 0)
        test_empty_destination_retirement();
    else if (strcmp(argv[1], "redist-limit-config") == 0)
        test_redist_limit_config_replay();
    else if (strcmp(argv[1], "redist-prefix") == 0)
        test_redist_prefix_accounting();
    else if (strcmp(argv[1], "redist-volume") == 0)
        test_redist_prefix_volume();
#ifdef EIGRP_TEST_PROCESS_VOLUME
    else if (strcmp(argv[1], "process-volume") == 0)
        test_process_prefix_volume();
    else if (strcmp(argv[1], "process-padding") == 0)
        test_process_policy_padding();
    else if (strcmp(argv[1], "process-batch") == 0)
        test_process_batch_dual();
#endif
    else
        return 2;
    return 0;
}
