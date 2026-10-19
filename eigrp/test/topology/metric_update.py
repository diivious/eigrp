# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# RFC 7868 runtime metric recomputation regression tests (Andy Defect 2).
# Compiles and runs the real topology and metric modules, with mocked
# southbound, packetizer, and DUAL event boundaries.

from pathlib import Path
import os
import subprocess
import sys
import textwrap

import pytest

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())


@pytest.fixture(scope="module")
def metric_update_executable(tmp_path_factory):
    work = tmp_path_factory.mktemp("topology-metric-update")
    source = work / "metric_update.c"
    binary = work / "metric_update"
    source.write_text(textwrap.dedent(r'''
            #include <assert.h>
            #include <stdio.h>
            #include <stdlib.h>
            #include <arpa/inet.h>
            #include <stdarg.h>
            #include <stdint.h>
            #include <string.h>

            #include "eigrp/code/eigrp.h"
            #include "eigrp/code/eigrp_log.h"
            #include "eigrp/code/eigrp_structs.h"
            #include "eigrp/code/eigrp_debug.h"
            #include "eigrp/code/eigrp_fsm.h"
            #include "eigrp/code/eigrp_neighbor.h"
            #include "eigrp/code/eigrp_metric.h"
            #include "eigrp/code/eigrp_redistribute.h"
            #include "eigrp/code/eigrp_rib.h"
            #include "eigrp/code/eigrp_topology.h"

            static eigrp_af_config_t *test_runtime_af;
            static unsigned query_work;
            static unsigned update_work;

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

            /* Focus this test on candidate/topology ownership.  The real DUAL
             * Active-state FSM has separate direct coverage; this shim applies
             * the same path-distance update and only refreshes destination
             * state while Passive.
             */
            int eigrp_fsm_event(eigrp_fsm_action_message_t *msg)
            {
                eigrp_route_descriptor_t *head;

                msg->change = eigrp_topology_update_distance(msg);
                if (msg->prefix->state != EIGRP_FSM_STATE_PASSIVE)
                    return 1;

                head = eigrp_topology_route_read(msg->prefix);
                msg->prefix->fdistance = head->distance;
                msg->prefix->distance = head->distance;
                msg->prefix->rdistance = head->distance;
                msg->prefix->reported_metric = head->total_metric;
                eigrp_topology_update_node_flags(msg->eigrp, msg->prefix);
                return 1;
            }


            /* This executable deliberately calls the real topology/metric
             * code: only southbound and packetization dependencies are mocked.
             * The test cases are selected by name for independent pytest IDs.
             */
            static void set_ipv4(eigrp_address_t *address, unsigned final)
            {
                memset(address, 0, sizeof(*address));
                address->afi = EIGRP_AFI_IPV4;
                address->bytes[0] = 10;
                address->bytes[1] = 44;
                address->bytes[2] = 0;
                address->bytes[3] = (uint8_t)final;
            }

            static void runtime_init(eigrp_instance_t *runtime, eigrp_nbr_t *self)
            {
                memset(runtime, 0, sizeof(*runtime));
                runtime->AS = 4453;
                runtime->k_values[0] = 1;
                runtime->k_values[2] = 1;
                runtime->metric_version = EIGRP_TLV_32B_VERSION;
                runtime->variance = 1;
                runtime->max_hops = 100;
                runtime->max_paths = 4;
                runtime->neighbor_self = self;
                runtime->af_vectors.afi = EIGRP_AFI_IPV4;
                runtime->af_vectors.classic_internal_tlv_type = EIGRP_TLV_IPv4_INT;
                runtime->af_vectors.classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
                runtime->topology_table = eigrp_topology_table_create();
                runtime->topology_changes = eigrp_list_create();
                assert(runtime->topology_table && runtime->topology_changes);
            }

            static void runtime_destroy(eigrp_instance_t *runtime)
            {
                eigrp_topology_table_delete(runtime, runtime->topology_table);
                eigrp_list_delete(&runtime->topology_changes);
            }

            static eigrp_metrics_t seed_metric(void)
            {
                eigrp_metrics_t metric = {0};
                metric.bandwidth = eigrp_bandwidth_to_scaled(100000);
                metric.delay = 400; /* native redistribution seed, 10-us units */
                metric.reliability = 255;
                metric.load = 1;
                metric.mtu[0] = 0xdc;
                metric.mtu[1] = 0x05;
                return metric;
            }

            static eigrp_metrics_t connected_metric(const eigrp_intf_t *link)
            {
                eigrp_metrics_t metric = seed_metric();
                metric.bandwidth = eigrp_bandwidth_to_scaled(link->params.bandwidth);
                metric.delay = eigrp_delay_to_scaled(link->params.delay);
                metric.reliability = link->params.reliability;
                metric.load = link->params.load;
                metric.hop_count = 0;
                return metric;
            }

            static void link_init(eigrp_intf_t *link)
            {
                memset(link, 0, sizeof(*link));
                link->params.bandwidth = 100000;
                link->params.delay = 100;
                link->params.reliability = 255;
                link->params.load = 1;
            }

            /* Build the same destination/path relationship as the live ingress
             * paths: local connected vector already has scaled delay; local
             * redistribution vector retains native delay; learned metric is
             * received from a neighbor and accumulated with the local link.
             */
            static eigrp_route_descriptor_t *add_route(
                eigrp_instance_t *runtime, eigrp_nbr_t *advertiser,
                eigrp_intf_t *link, eigrp_metrics_t metric,
                bool external, unsigned id)
            {
                eigrp_prefix_descriptor_t *prefix = eigrp_topology_prefix_create();
                eigrp_route_descriptor_t *route = eigrp_topology_route_create(link);
                bool self = advertiser == runtime->neighbor_self;
                eigrp_metrics_t calculation;

                assert(prefix && route);
                set_ipv4(&prefix->destination.address, id);
                prefix->destination.prefix_length = 32;
                prefix->state = EIGRP_FSM_STATE_PASSIVE;
                prefix->nt = external ? EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL
                                      : (self ? EIGRP_TOPOLOGY_TYPE_CONNECTED
                                              : EIGRP_TOPOLOGY_TYPE_REMOTE);
                route->type = external ? EIGRP_TLV_IPv4_EXT : EIGRP_TLV_IPv4_INT;
                route->dest = prefix->destination;
                route->adv_router = advertiser;
                route->metric = metric;
                route->reported_metric = metric;
                route->total_metric = metric;
                route->reported_distance = self ? 0
                    : eigrp_metric_calculate(runtime, metric);
                if (self && !link) {
                    calculation = metric;
                    calculation.delay = eigrp_delay_to_scaled(metric.delay);
                    route->distance = eigrp_metric_calculate(runtime, calculation);
                } else if (self) {
                    route->distance = eigrp_metric_calculate(runtime, metric);
                } else {
                    assert(link);
                    route->distance = eigrp_metric_total_calculate(runtime, route);
                }
                assert(route->distance && route->distance != EIGRP_MAX_METRIC);
                route->flags = EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
                prefix->distance = prefix->fdistance = prefix->rdistance =
                    route->distance;
                prefix->reported_metric = route->total_metric;
                eigrp_prefix_descriptor_add(runtime->topology_table, prefix);
                eigrp_route_descriptor_add(runtime, prefix, route);
                assert(route->prefix == prefix);
                return route;
            }

            /* Defect 1: both forms of self-originated route keep RD=0, and
             * recomputing FD must not strip the successor bit. */
            static void test_local_reported_distance_and_successors(void)
            {
                eigrp_instance_t runtime;
                eigrp_nbr_t self = {0};
                eigrp_intf_t link;
                eigrp_route_descriptor_t *connected, *redist;
                link_init(&link);
                runtime_init(&runtime, &self);
                connected = add_route(&runtime, &self, &link,
                                      connected_metric(&link), false, 1);
                redist = add_route(&runtime, &self, NULL,
                                   seed_metric(), true, 2);
                for (unsigned pass = 0; pass < 3; pass++) {
                    runtime.k_values[0] = pass + 1;
                    runtime.k_values[2] = pass + 1;
                    runtime.metric_version = pass == 0 ? EIGRP_TLV_32B_VERSION
                                                       : EIGRP_TLV_64B_VERSION;
                    eigrp_topology_metric_update(&runtime);
                    for (unsigned n = 0; n < 2; n++) {
                        eigrp_route_descriptor_t *route = n ? redist : connected;
                        assert(route->reported_distance == 0);
                        assert(route->distance != EIGRP_MAX_METRIC);
                        assert(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
                        assert(!(route->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG));
                        assert(route->prefix->fdistance == route->distance);
                    }
                }
                runtime_destroy(&runtime);
            }

            /* Defect 2: a connected prefix has no upstream link, so the
             * current interface vector is its total vector and hop remains 0. */
            static void test_connected_vector_rebuild_without_double_add(void)
            {
                eigrp_instance_t runtime;
                eigrp_nbr_t self = {0};
                eigrp_intf_t link;
                eigrp_route_descriptor_t *route;
                eigrp_metrics_t expected;
                eigrp_metric_t previous;
                link_init(&link);
                runtime_init(&runtime, &self);
                route = add_route(&runtime, &self, &link,
                                  connected_metric(&link), false, 3);
                previous = route->distance;
                link.params.bandwidth = 50000;
                link.params.delay = 200;
                link.params.reliability = 250;
                link.params.load = 10;
                runtime.k_values[2] = 2;
                expected = connected_metric(&link);
                for (unsigned pass = 0; pass < 3; pass++) {
                    eigrp_topology_metric_update(&runtime);
                    assert(route->total_metric.hop_count == 0);
                    assert(route->total_metric.delay == eigrp_delay_to_scaled(200));
                    assert(eigrp_metrics_match(route->metric, expected));
                    assert(eigrp_metrics_match(route->reported_metric, expected));
                    assert(eigrp_metrics_match(route->total_metric, expected));
                    assert(route->reported_distance == 0);
                    assert(route->distance == eigrp_metric_calculate(&runtime, expected));
                    assert(route->distance != previous);
                    assert(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
                }
                link.params.delay = 100;
                expected = connected_metric(&link);
                eigrp_topology_metric_update(&runtime);
                assert(eigrp_metrics_match(route->total_metric, expected));
                assert(route->total_metric.hop_count == 0);
                runtime_destroy(&runtime);
            }

            /* Defect 3: redistribution seed delay is native and must be scaled
             * for distance calculations, without modifying the advertised seed. */
            static void test_redistributed_seed_scaled_without_mutating_vector(void)
            {
                eigrp_instance_t runtime;
                eigrp_nbr_t self = {0};
                eigrp_route_descriptor_t *route;
                eigrp_metrics_t seed = seed_metric();
                eigrp_metrics_t calculation = seed;
                runtime_init(&runtime, &self);
                route = add_route(&runtime, &self, NULL, seed, true, 4);
                calculation.delay = eigrp_delay_to_scaled(seed.delay);
                for (unsigned pass = 0; pass < 3; pass++) {
                    runtime.k_values[0] = pass + 1;
                    runtime.metric_version = pass ? EIGRP_TLV_64B_VERSION
                                                  : EIGRP_TLV_32B_VERSION;
                    eigrp_topology_metric_update(&runtime);
                    assert(route->distance == eigrp_metric_calculate(&runtime,
                                                                     calculation));
                    assert(route->reported_distance == 0);
                    assert(route->distance != eigrp_metric_calculate(&runtime, seed));
                    assert(eigrp_metrics_match(route->metric, seed));
                    assert(eigrp_metrics_match(route->total_metric, seed));
                    assert(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
                }
                /* A withdrawn local external remains unreachable, even after
                 * a K-value or version change. */
                seed.delay = EIGRP_MAX_METRIC;
                route->metric = route->reported_metric = route->total_metric = seed;
                eigrp_topology_metric_update(&runtime);
                assert(route->reported_distance == 0);
                assert(route->distance == EIGRP_MAX_METRIC);
                assert(!(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG));
                runtime_destroy(&runtime);
            }

            /* Defect 4: destination RD and the metric advertised to peers are
             * successor's complete composite/vector, not its predecessor RD. */
            static void test_prefix_advertisement_is_total_distance_and_vector(void)
            {
                eigrp_instance_t runtime;
                eigrp_nbr_t self = {0}, peer = {0};
                eigrp_intf_t link;
                eigrp_route_descriptor_t *route;
                eigrp_metrics_t incoming = seed_metric();
                eigrp_metrics_t expected_total;
                link_init(&link);
                runtime_init(&runtime, &self);
                incoming.delay = eigrp_delay_to_scaled(250);
                route = add_route(&runtime, &peer, &link, incoming, false, 5);
                for (unsigned pass = 0; pass < 2; pass++) {
                    runtime.metric_version = pass ? EIGRP_TLV_64B_VERSION
                                                  : EIGRP_TLV_32B_VERSION;
                    runtime.k_values[2] = pass + 1;
                    link.params.delay = 100 + (pass * 100);
                    eigrp_topology_metric_update(&runtime);
                    expected_total = incoming;
                    expected_total.delay += eigrp_delay_to_scaled(link.params.delay);
                    expected_total.hop_count++;
                    assert(route->reported_distance == eigrp_metric_calculate(
                        &runtime, incoming));
                    assert(route->distance == eigrp_metric_calculate(
                        &runtime, expected_total));
                    assert(route->reported_distance < route->distance);
                    assert(route->prefix->rdistance == route->distance);
                    assert(route->prefix->distance == route->distance);
                    assert(route->prefix->fdistance == route->distance);
                    assert(eigrp_metrics_match(route->total_metric, expected_total));
                    assert(eigrp_metrics_match(route->prefix->reported_metric,
                                               expected_total));
                    assert(!eigrp_metrics_match(route->prefix->reported_metric,
                                                route->reported_metric));
                    assert(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
                }
                runtime_destroy(&runtime);
            }

            /* Existing DUAL rule: recomputation must not mutate Active frozen
             * distance or advertised state. */
            static void test_active_destination_remains_frozen(void)
            {
                eigrp_instance_t runtime;
                eigrp_nbr_t self = {0};
                eigrp_intf_t link;
                eigrp_route_descriptor_t *route;
                eigrp_metric_t before;
                eigrp_metrics_t before_vector;
                link_init(&link);
                runtime_init(&runtime, &self);
                route = add_route(&runtime, &self, &link,
                                  connected_metric(&link), false, 6);
                before = route->distance;
                before_vector = route->prefix->reported_metric;
                route->prefix->state = EIGRP_FSM_STATE_ACTIVE_1;
                link.params.delay = 900;
                runtime.k_values[0] = 3;
                eigrp_topology_metric_update(&runtime);
                assert(route->distance == before);
                assert(route->prefix->distance == before);
                assert(route->prefix->fdistance == before);
                assert(route->prefix->rdistance == before);
                assert(eigrp_metrics_match(route->prefix->reported_metric,
                                            before_vector));
                runtime_destroy(&runtime);
            }

            /* Compare every field, including flags/tag which the metric
             * equality helper intentionally does not compare. */
            static void assert_vector_equal(eigrp_metrics_t actual,
                                            eigrp_metrics_t expected)
            {
                assert(actual.delay == expected.delay);
                assert(actual.bandwidth == expected.bandwidth);
                assert(!memcmp(actual.mtu, expected.mtu, sizeof(actual.mtu)));
                assert(actual.hop_count == expected.hop_count);
                assert(actual.reliability == expected.reliability);
                assert(actual.load == expected.load);
                assert(actual.tag == expected.tag);
                assert(actual.flags == expected.flags);
            }

            /* Complete set of EIGRP self-originated invariants:
             * - path RD is zero (not the locally computed cost)
             * - CD is computed from the local interface or redistribution seed
             * - path total vector is the entire local path, not predecessor data
             * - path reported vector is local/nonzero, not an empty vector
             * - destination rdistance is the locally advertised distance
             * - destination reported vector is the selected path total vector
             * These apply equally to internal connected and external seeds.
             */
            static void assert_local_six_invariants(
                eigrp_instance_t *runtime, eigrp_route_descriptor_t *route,
                eigrp_metrics_t expected, eigrp_metric_t local_distance)
            {
                eigrp_prefix_descriptor_t *dest = route->prefix;
                assert(route->adv_router == runtime->neighbor_self);
                assert(route->reported_distance == 0);
                assert(route->distance == local_distance);
                assert(route->metric.bandwidth > 0);
                assert(route->reported_metric.bandwidth > 0);
                assert(route->reported_metric.delay > 0);
                assert_vector_equal(route->metric, expected);
                assert_vector_equal(route->reported_metric, expected);
                assert_vector_equal(route->total_metric, expected);
                assert(dest->rdistance == local_distance);
                assert(dest->distance == local_distance);
                assert(dest->fdistance == local_distance);
                assert_vector_equal(dest->reported_metric, expected);
                assert(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
                assert(!(route->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG));
            }

            /* Cisco's local Connected (CD/0) has no predecessor: the actual
             * interface parameters form all three path metric vectors and
             * the advertised destination vector.  Vary link/K/metric version
             * and repeat without allowing cumulative delay/hop increments.
             */
            static void test_connected_all_six_invariants(void)
            {
                eigrp_instance_t runtime;
                eigrp_nbr_t self = {0};
                eigrp_intf_t link;
                eigrp_route_descriptor_t *route;
                link_init(&link);
                runtime_init(&runtime, &self);
                route = add_route(&runtime, &self, &link,
                                  connected_metric(&link), false, 10);
                for (unsigned pass = 0; pass < 4; pass++) {
                    eigrp_metrics_t expected;
                    if (pass == 1) {
                        link.params.delay = 320;
                        link.params.bandwidth = 50000;
                        link.params.load = 25;
                        link.params.reliability = 240;
                    }
                    if (pass == 2) {
                        runtime.metric_version = EIGRP_TLV_64B_VERSION;
                        runtime.k_values[0] = 2;
                        runtime.k_values[2] = 3;
                    }
                    expected = connected_metric(&link);
                    eigrp_topology_metric_update(&runtime);
                    assert(expected.hop_count == 0);
                    assert(expected.delay == eigrp_delay_to_scaled(link.params.delay));
                    assert_local_six_invariants(&runtime, route, expected,
                        eigrp_metric_calculate(&runtime, expected));
                }
                /* A failed/withdrawn connected candidate must not retain
                 * the old reachable vector in the destination advertisement. */
                route->metric.delay = EIGRP_MAX_METRIC;
                eigrp_topology_metric_update(&runtime);
                assert(route->reported_distance == 0);
                assert(route->distance == EIGRP_MAX_METRIC);
                assert(route->reported_metric.delay == EIGRP_MAX_METRIC);
                assert(route->total_metric.delay == EIGRP_MAX_METRIC);
                assert(route->prefix->reported_metric.delay == EIGRP_MAX_METRIC);
                assert(route->prefix->rdistance == EIGRP_MAX_METRIC);
                assert(!(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG));
                runtime_destroy(&runtime);
            }

            /* Cisco's local Redistributed (CD/0) stores the entire seed
             * unchanged in its vectors; only the scalar CD calculation scales
             * seed delay by 256. Changes to the seed and withdrawn metrics
             * cannot leave stale reported/total/advertised vectors behind.
             */
            static void test_redistributed_all_six_invariants(void)
            {
                eigrp_instance_t runtime;
                eigrp_nbr_t self = {0};
                eigrp_route_descriptor_t *route;
                eigrp_metrics_t seed = seed_metric();
                eigrp_metrics_t calculation;
                runtime_init(&runtime, &self);
                seed.tag = 12;
                seed.flags = 4;
                seed.hop_count = 2; /* e.g., seed from a different AS */
                route = add_route(&runtime, &self, NULL, seed, true, 11);
                for (unsigned pass = 0; pass < 3; pass++) {
                    if (pass == 1) {
                        /* Simulate a new locally selected seed.  The route's
                         * canonical seed must replace any earlier vectors. */
                        seed.delay = 700;
                        seed.bandwidth = eigrp_bandwidth_to_scaled(200000);
                        seed.tag = 55;
                        route->metric = seed;
                        runtime.k_values[2] = 2;
                    }
                    if (pass == 2)
                        runtime.metric_version = EIGRP_TLV_64B_VERSION;
                    calculation = seed;
                    calculation.delay = eigrp_delay_to_scaled(seed.delay);
                    eigrp_topology_metric_update(&runtime);
                    assert(route->total_metric.delay == seed.delay);
                    assert(route->total_metric.hop_count == seed.hop_count);
                    assert_local_six_invariants(&runtime, route, seed,
                        eigrp_metric_calculate(&runtime, calculation));
                }
                /* Withdrawal is not allowed to retain the old seed as a
                 * usable destination advertisement after recalculation. */
                seed.delay = EIGRP_MAX_METRIC;
                route->metric = seed;
                eigrp_topology_metric_update(&runtime);
                assert(route->reported_distance == 0);
                assert(route->distance == EIGRP_MAX_METRIC);
                assert(route->reported_metric.delay == EIGRP_MAX_METRIC);
                assert(route->total_metric.delay == EIGRP_MAX_METRIC);
                assert(route->prefix->reported_metric.delay == EIGRP_MAX_METRIC);
                assert(route->prefix->rdistance == EIGRP_MAX_METRIC);
                assert(!(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG));
                /* A later fresh redistribution can become a valid successor. */
                seed.delay = 550;
                route->metric = seed;
                calculation = seed;
                calculation.delay = eigrp_delay_to_scaled(seed.delay);
                eigrp_topology_metric_update(&runtime);
                assert_local_six_invariants(&runtime, route, seed,
                    eigrp_metric_calculate(&runtime, calculation));
                runtime_destroy(&runtime);
            }

            /* Received path RD is the NEIGHBOR's computed distance, unlike
             * the local route's zero RD; the advertised vector must instead
             * include the receiving interface's bandwidth and delay. */
            static void test_learned_path_predecessor_vs_advertisement(void)
            {
                eigrp_instance_t runtime;
                eigrp_nbr_t self = {0}, peer = {0};
                eigrp_intf_t link;
                eigrp_metrics_t incoming = seed_metric(), expected;
                eigrp_route_descriptor_t *route;
                link_init(&link);
                runtime_init(&runtime, &self);
                incoming.delay = eigrp_delay_to_scaled(200);
                incoming.hop_count = 3;
                route = add_route(&runtime, &peer, &link, incoming, false, 12);
                for (unsigned pass = 0; pass < 3; pass++) {
                    if (pass == 1) {
                        link.params.delay = 500;
                        link.params.bandwidth = 25000;
                        runtime.k_values[2] = 2;
                    } else if (pass == 2) {
                        runtime.metric_version = EIGRP_TLV_64B_VERSION;
                    }
                    eigrp_topology_metric_update(&runtime);
                    expected = incoming;
                    expected.delay += eigrp_delay_to_scaled(link.params.delay);
                    expected.hop_count++;
                    expected.bandwidth = eigrp_bandwidth_to_scaled(link.params.bandwidth)
                        > incoming.bandwidth
                        ? eigrp_bandwidth_to_scaled(link.params.bandwidth)
                        : incoming.bandwidth;
                    assert(route->adv_router == &peer);
                    assert(route->reported_distance ==
                           eigrp_metric_calculate(&runtime, incoming));
                    assert(route->reported_distance != 0);
                    assert_vector_equal(route->reported_metric, incoming);
                    assert_vector_equal(route->total_metric, expected);
                    assert(route->distance ==
                           eigrp_metric_calculate(&runtime, expected));
                    assert(route->prefix->rdistance == route->distance);
                    assert_vector_equal(route->prefix->reported_metric, expected);
                    assert(route->reported_distance != route->prefix->rdistance);
                    assert(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
                }
                runtime_destroy(&runtime);
            }

            int main(int argc, char **argv)
            {
                if (argc != 2)
                    return 2;
                if (!strcmp(argv[1], "local-rd"))
                    test_local_reported_distance_and_successors();
                else if (!strcmp(argv[1], "connected-vector"))
                    test_connected_vector_rebuild_without_double_add();
                else if (!strcmp(argv[1], "redist-seed"))
                    test_redistributed_seed_scaled_without_mutating_vector();
                else if (!strcmp(argv[1], "advertised-vector"))
                    test_prefix_advertisement_is_total_distance_and_vector();
                else if (!strcmp(argv[1], "active-frozen"))
                    test_active_destination_remains_frozen();
                else if (!strcmp(argv[1], "connected-invariants"))
                    test_connected_all_six_invariants();
                else if (!strcmp(argv[1], "redist-invariants"))
                    test_redistributed_all_six_invariants();
                else if (!strcmp(argv[1], "learned-predecessor"))
                    test_learned_path_predecessor_vs_advertisement();
                else
                    return 2;
                return 0;
            }
'''))
    compiler = os.environ.get("CC", "cc")
    result = subprocess.run(
        [compiler, "-std=gnu11", "-O0", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-function", "-Wno-unused-parameter",
         "-ffunction-sections", "-fdata-sections",
         f"-I{ROOT}", f"-I{ROOT / 'test' / 'build' / 'include'}",
         str(source),
         *[str(ROOT / "eigrp" / "code" / f) for f in (
             "eigrp_topology.c", "eigrp_timer.c", "eigrp_metric.c", "eigrp_list.c",
             "eigrp_table.c", "eigrp_prefix.c")],
         *(["-Wl,-dead_strip"] if sys.platform == "darwin" else
           ["-Wl,--gc-sections"]), "-o", str(binary)],
        cwd=ROOT, text=True, capture_output=True,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return binary


def _run_case(binary, name):
    result = subprocess.run([str(binary), name], cwd=ROOT,
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_self_originated_rd_and_successor_survive_recalculation(metric_update_executable):
    _run_case(metric_update_executable, "local-rd")


def test_connected_route_interface_vector_is_not_added_twice(metric_update_executable):
    _run_case(metric_update_executable, "connected-vector")


def test_redistributed_seed_delay_is_scaled_and_poison_preserved(metric_update_executable):
    _run_case(metric_update_executable, "redist-seed")


def test_destination_advertises_successor_total_metric(metric_update_executable):
    _run_case(metric_update_executable, "advertised-vector")


def test_active_destination_not_recomputed(metric_update_executable):
    _run_case(metric_update_executable, "active-frozen")


def test_connected_all_six_self_originated_invariants(metric_update_executable):
    _run_case(metric_update_executable, "connected-invariants")


def test_redistributed_all_six_self_originated_invariants(metric_update_executable):
    _run_case(metric_update_executable, "redist-invariants")


def test_learned_neighbor_reported_vs_advertised_metrics(metric_update_executable):
    _run_case(metric_update_executable, "learned-predecessor")
