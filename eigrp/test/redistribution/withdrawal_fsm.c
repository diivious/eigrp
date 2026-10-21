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

static eigrp_af_config_t *test_runtime_af;
static unsigned query_work;
static unsigned update_work;
static eigrp_prefix_descriptor_t *deferred_prefix;

/* This FSM-only harness has no packetizer worker. Capture the retired
 * descriptor to verify detachment occurs before deferred reclamation. */
void eigrp_packetizer_prefix_defer_free(eigrp_instance_t *eigrp,
                                       eigrp_prefix_descriptor_t *prefix)
{
    (void)eigrp;
    assert(prefix && deferred_prefix == NULL);
    deferred_prefix = prefix;
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
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_redistribute_remove(
    eigrp_instance_t *eigrp,
    const eigrp_redist_source_t *source)
{
    (void)eigrp;
    (void)source;
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
    else if (strcmp(argv[1], "redist-prefix") == 0)
        test_redist_prefix_accounting();
    else if (strcmp(argv[1], "redist-volume") == 0)
        test_redist_prefix_volume();
    else
        return 2;
    return 0;
}
