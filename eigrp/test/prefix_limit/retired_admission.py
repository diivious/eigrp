# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
#
# Defect 3: topology maximum-prefix counting and re-admission regressions.
# Real topology and metric units, mocked host/RIB/packetizer services.

from pathlib import Path
import os
import subprocess
import sys
import textwrap
import pytest

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())

@pytest.fixture(scope="module")
def prefix_admission_executable(tmp_path_factory):
    work = tmp_path_factory.mktemp("prefix-admission")
    source = work / "prefix_admission.c"
    binary = work / "prefix_admission"
    update_source = (ROOT / "eigrp/code/eigrp_update.c").read_text()
    start = (update_source.index("bool free_received_route = false;")
             + len("bool free_received_route = false;"))
    end = update_source.index("if (topology_route) {", start)
    # Exercise the actual receive admission statements, not a copied
    # approximation. Compile them with the real topology admission function.
    ingress_gate = update_source[start:end]
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


            static void max_prefix(eigrp_af_config_t *af, uint32_t maximum,
                                   bool warning_only)
            {
                memset(af, 0, sizeof(*af));
                af->topology_maximum_prefix_configured = true;
                af->topology_maximum_prefix.maximum = maximum;
                af->topology_maximum_prefix.warning_only = warning_only;
                test_runtime_af = af;
            }

            static eigrp_prefix_t dest(unsigned id)
            {
                eigrp_prefix_t result = {0};
                set_ipv4(&result.address, id);
                result.prefix_length = 32;
                return result;
            }

            static void empty_prefix(eigrp_instance_t *runtime, unsigned id)
            {
                eigrp_prefix_descriptor_t *prefix =
                    eigrp_topology_prefix_create();
                assert(prefix);
                prefix->state = EIGRP_FSM_STATE_PASSIVE;
                prefix->destination = dest(id);
                eigrp_prefix_descriptor_add(runtime->topology_table, prefix);
            }

            static void mark_poison(eigrp_route_descriptor_t *route)
            {
                route->distance = EIGRP_MAX_METRIC;
                route->metric.delay = EIGRP_MAX_METRIC;
                route->reported_metric.delay = EIGRP_MAX_METRIC;
                route->total_metric.delay = EIGRP_MAX_METRIC;
                route->flags &= ~EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
            }

            static void test_empty_shell_does_not_consume_capacity(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                empty_prefix(&runtime, 1);
                eigrp_prefix_t target = dest(2);
                assert(eigrp_topology_prefix_admit(&runtime, &target));
                runtime_destroy(&runtime);
            }

            static void test_poisoned_route_does_not_consume_capacity(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                eigrp_route_descriptor_t *r = add_route(&runtime, &self, NULL,
                    seed_metric(), true, 1);
                mark_poison(r);
                r->prefix->req_action = EIGRP_FSM_NEED_UPDATE;
                eigrp_prefix_t target = dest(2);
                assert(eigrp_topology_prefix_admit(&runtime, &target));
                runtime_destroy(&runtime);
            }

            static void test_readvertisement_must_check_new_capacity(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                add_route(&runtime, &self, NULL, seed_metric(), true, 1);
                empty_prefix(&runtime, 2);
                eigrp_prefix_t stale = dest(2), live = dest(1);
                assert(eigrp_topology_prefix_admit(&runtime, &live));
                assert(!eigrp_topology_prefix_admit(&runtime, &stale));
                runtime_destroy(&runtime);
            }

            static void test_poisoned_readvertisement_respects_capacity(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                add_route(&runtime, &self, NULL, seed_metric(), true, 1);
                eigrp_route_descriptor_t *stale = add_route(&runtime, &self,
                    NULL, seed_metric(), true, 2);
                mark_poison(stale);
                eigrp_prefix_t target = dest(2);
                assert(!eigrp_topology_prefix_admit(&runtime, &target));
                runtime_destroy(&runtime);
            }

            static void test_live_external_path_counts_despite_poisoned_internal(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                eigrp_nbr_t neighbor = {0};
                eigrp_intf_t link;
                link_init(&link);
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                eigrp_route_descriptor_t *first = add_route(&runtime, &self,
                    &link, connected_metric(&link), false, 1);
                eigrp_route_descriptor_t *second = eigrp_topology_route_create(NULL);
                assert(second);
                second->type = EIGRP_TLV_IPv4_EXT;
                second->adv_router = &neighbor;
                second->dest = first->dest;
                second->metric = second->reported_metric = second->total_metric = seed_metric();
                second->distance = 10000;
                second->reported_distance = 200;
                eigrp_route_descriptor_add(&runtime, first->prefix, second);
                mark_poison(first);
                eigrp_prefix_t target = dest(2);
                assert(!eigrp_topology_prefix_admit(&runtime, &target));
                mark_poison(second);
                assert(eigrp_topology_prefix_admit(&runtime, &target));
                runtime_destroy(&runtime);
            }

            static void test_warning_only_and_capacity_recovery(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                eigrp_route_descriptor_t *r = add_route(&runtime, &self, NULL,
                    seed_metric(), true, 1);
                eigrp_prefix_t target = dest(2);
                assert(!eigrp_topology_prefix_admit(&runtime, &target));
                af.topology_maximum_prefix.warning_only = true;
                assert(eigrp_topology_prefix_admit(&runtime, &target));
                af.topology_maximum_prefix.warning_only = false;
                mark_poison(r);
                assert(eigrp_topology_prefix_admit(&runtime, &target));
                /* Even with a stale finite distance, a poisoned metric
                 * vector must not count as a reachable prefix. */
                r->distance = 123456;
                assert(eigrp_topology_prefix_admit(&runtime, &target));
                r->metric.delay = 100;
                assert(!eigrp_topology_prefix_admit(&runtime, &target));
                runtime_destroy(&runtime);
            }


            static bool neighbor_gate_allowed = true;

            bool eigrp_nbr_prefix_admit(eigrp_instance_t *runtime,
                                       eigrp_nbr_t *nbr,
                                       const eigrp_prefix_t *prefix)
            {
                (void)runtime; (void)nbr; (void)prefix;
                return neighbor_gate_allowed;
            }

            static bool ingress_check(eigrp_instance_t *eigrp,
                                      unsigned id, bool poisoned,
                                      eigrp_route_descriptor_t *topology_route)
            {
                eigrp_nbr_t neighbor = {0};
                eigrp_nbr_t *nbr = &neighbor;
                eigrp_route_descriptor_t *received_route =
                    eigrp_topology_route_create(NULL);
                bool accepted = false;

                assert(received_route);
                received_route->dest = dest(id);
                received_route->metric = seed_metric();
                if (poisoned)
                    received_route->metric.delay = EIGRP_MAX_METRIC;
                for (int once = 0; once < 1; once++) {
                    /* INGRESS_GATE_PRODUCTION */
                    accepted = true;
                }
                if (accepted)
                    eigrp_topology_route_free(received_route);
                return accepted;
            }

            static void test_receive_gate_rejects_stale_at_capacity(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                eigrp_route_descriptor_t *live = add_route(&runtime, &self,
                    NULL, seed_metric(), true, 1);
                empty_prefix(&runtime, 2);
                neighbor_gate_allowed = true;
                assert(!ingress_check(&runtime, 2, false, NULL));
                mark_poison(live);
                assert(ingress_check(&runtime, 2, false, NULL));
                runtime_destroy(&runtime);
            }

            static void test_receive_gate_poison_and_duplicate(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                eigrp_route_descriptor_t *live = add_route(&runtime, &self,
                    NULL, seed_metric(), true, 1);
                empty_prefix(&runtime, 2);
                neighbor_gate_allowed = true;
                /* A new poisoned path must not re-populate an empty shell. */
                assert(!ingress_check(&runtime, 2, true, NULL));
                /* A known path update consumes no new prefix capacity. */
                assert(ingress_check(&runtime, 1, false, live));
                neighbor_gate_allowed = false;
                /* Neighbor limits apply to a new path even to a live prefix. */
                assert(!ingress_check(&runtime, 1, false, NULL));
                /* But neither admission check may reject a path already owned. */
                assert(ingress_check(&runtime, 1, false, live));
                neighbor_gate_allowed = true;
                runtime_destroy(&runtime);
            }


            static void test_receive_gate_revived_owned_poison(void)
            {
                eigrp_instance_t runtime;
                eigrp_af_config_t af;
                eigrp_nbr_t self = {0};
                runtime_init(&runtime, &self);
                max_prefix(&af, 1, false);
                add_route(&runtime, &self, NULL, seed_metric(), true, 1);
                eigrp_route_descriptor_t *stale = add_route(&runtime, &self,
                    NULL, seed_metric(), true, 2);
                mark_poison(stale);
                neighbor_gate_allowed = true;
                /* An existing RDB is not a free slot when it is poisoned. */
                assert(!ingress_check(&runtime, 2, false, stale));
                /* Withdrawal of the same RDB must reach DUAL unconditionally. */
                assert(ingress_check(&runtime, 2, true, stale));
                /* Still enforce topology even if neighbor admission is skipped. */
                neighbor_gate_allowed = false;
                assert(!ingress_check(&runtime, 2, false, stale));
                neighbor_gate_allowed = true;
                runtime_destroy(&runtime);
            }

            int main(int argc, char **argv)
            {
                if (argc != 2) return 2;
                if (!strcmp(argv[1], "empty"))
                    test_empty_shell_does_not_consume_capacity();
                else if (!strcmp(argv[1], "poison"))
                    test_poisoned_route_does_not_consume_capacity();
                else if (!strcmp(argv[1], "readvert-shell"))
                    test_readvertisement_must_check_new_capacity();
                else if (!strcmp(argv[1], "readvert-poison"))
                    test_poisoned_readvertisement_respects_capacity();
                else if (!strcmp(argv[1], "mixed"))
                    test_live_external_path_counts_despite_poisoned_internal();
                else if (!strcmp(argv[1], "recover"))
                    test_warning_only_and_capacity_recovery();
                else if (!strcmp(argv[1], "ingress-shell"))
                    test_receive_gate_rejects_stale_at_capacity();
                else if (!strcmp(argv[1], "ingress-poison"))
                    test_receive_gate_poison_and_duplicate();
                else if (!strcmp(argv[1], "ingress-owned-poison"))
                    test_receive_gate_revived_owned_poison();
                else return 2;
                return 0;
            }
''').replace("/* INGRESS_GATE_PRODUCTION */", ingress_gate))
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


def run_case(binary, name):
    result = subprocess.run([str(binary), name], cwd=ROOT,
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr

@pytest.mark.parametrize("scenario", [
    "empty", "poison", "readvert-shell", "readvert-poison", "mixed", "recover",
    "ingress-shell", "ingress-poison", "ingress-owned-poison"
])
def test_topology_maximum_prefix_admission(prefix_admission_executable, scenario):
    run_case(prefix_admission_executable, scenario)
