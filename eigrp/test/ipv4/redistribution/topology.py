# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Portable behavior coverage for redistribution candidate topology lifecycle.

from pathlib import Path
import os
import subprocess
import sys
import textwrap


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())


def test_redistributed_candidate_external_topology_lifecycle_executes(tmp_path):
    source = tmp_path / "redistribute_topology.c"
    binary = tmp_path / "redistribute_topology"
    source.write_text(
        textwrap.dedent(
            r'''
            #include <assert.h>
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
            #include "eigrp/code/eigrp_redistribute.h"
            #include "eigrp/code/eigrp_rib.h"
            #include "eigrp/code/eigrp_topology.h"

            uint64_t eigrp_sys_monotime_msec(void) { return 1; }

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

            int main(void)
            {
                eigrp_virt_router_t virt_router = {0};
                eigrp_instance_t runtime = {0};
                eigrp_nbr_t self = {0};
                eigrp_af_config_t af = {
                    .afi = EIGRP_AFI_IPV4,
                    .asn = 4453,
                };
                eigrp_instance_context_t context = {
                    .config = &af,
                    .runtime = &runtime,
                    .topology_id = EIGRP_TOPOLOGY_ID_BASE,
                };
                eigrp_redist_source_t source_id = {
                    .protocol = EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP,
                    .route_instance = 100,
                };
                eigrp_metric_values_t seed = {
                    .bandwidth = 100000,
                    .delay = 100,
                    .reliability = 255,
                    .load = 1,
                    .mtu = 1500,
                };
                eigrp_rib_route_t candidate = {0};
                eigrp_rib_nexthop_t candidate_nexthop = {0};
                eigrp_prefix_descriptor_t *pd;
                eigrp_route_descriptor_t *route;
                eigrp_route_descriptor_t *same_route;
                uint32_t frozen_fd;
                uint32_t frozen_distance;
                eigrp_metrics_t frozen_reported_metric;

                virt_router.vrid = 1;
                runtime.virt_router = &virt_router;
                runtime.AS = 4453;
                runtime.variance = 1;
                runtime.max_paths = 4;
                runtime.k_values[0] = 1;
                runtime.k_values[2] = 1;
                runtime.af_vectors.afi = EIGRP_AFI_IPV4;
                runtime.af_vectors.classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
                runtime.topology_table = eigrp_topology_table_create();
                runtime.topology_changes = eigrp_list_create();
                runtime.neighbor_self = &self;
                assert(inet_pton(AF_INET, "192.0.2.45", &runtime.router_id) == 1);
                test_runtime_af = &af;

                assert(eigrp_redist_add(&context, &source_id, &seed, NULL)
                       == EIGRP_RESULT_SUCCESS);

                set_ipv4(&candidate.prefix.address, 10, 77, 0, 0);
                candidate.prefix.prefix_length = 24;
                candidate.redist.source = source_id;
                candidate.metric = 900;
                candidate.tag = 0x11223344U;
                candidate.nexthops = &candidate_nexthop;
                candidate.nexthop_count = 1;
                candidate_nexthop.gateway_present = true;
                set_ipv4(&candidate_nexthop.gateway, 198, 51, 100, 1);

                /* First appearance: one external topology path with complete
                 * RFC external metadata.  An EIGRP-origin candidate remains
                 * EXTERNAL rather than becoming an internal route.
                 */
                assert(eigrp_rib_redist_add(&runtime, &candidate)
                       == EIGRP_RESULT_SUCCESS);
                assert(update_work == 1);
                pd = eigrp_topology_table_lookup(runtime.topology_table,
                                                 &candidate.prefix);
                assert(pd != NULL);
                assert(pd->nt == EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL);
                route = only_route(&runtime, &candidate.prefix);
                assert(route->type == EIGRP_TLV_IPv4_EXT);
                assert(route->adv_router == runtime.neighbor_self);
                assert(route->ei == NULL);
                assert(route->extdata.orig == 0xc000022dU);
                assert(route->extdata.as == 0x00010064U);
                assert(route->extdata.metric == 900U);
                assert(route->extdata.protocol == 2U);
                assert(route->extdata.flags == 0U);
                assert(route->extdata.reserved == 0U);
                assert(route->extdata.tag == 0x11223344U);
                assert(route->nexthop.afi == AF_INET);
                assert(((const uint8_t *)&route->nexthop.ip.v4)[3] == 1U);
                assert(route->reported_distance == 0U);
                assert(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);

                /* ADD is also replace/update.  Change metric, tag and next hop;
                 * the same route_descriptor is updated and no duplicate path
                 * is created.
                 */
                same_route = route;
                seed.delay = 250;
                assert(eigrp_redist_add(&context, &source_id, &seed, NULL)
                       == EIGRP_RESULT_SUCCESS);
                candidate.metric = 1234;
                candidate.tag = 0xaabbccddU;
                set_ipv4(&candidate_nexthop.gateway, 203, 0, 113, 9);
                assert(eigrp_rib_redist_add(&runtime, &candidate)
                       == EIGRP_RESULT_SUCCESS);
                assert(update_work == 2);
                pd = eigrp_topology_table_lookup(runtime.topology_table,
                                                 &candidate.prefix);
                assert(pd->external_routes->count == 1);
                assert(pd->internal_routes->count == 0);
                route = only_route(&runtime, &candidate.prefix);
                assert(route == same_route);
                assert(route->extdata.metric == 1234U);
                assert(route->extdata.tag == 0xaabbccddU);
                assert(((const uint8_t *)&route->nexthop.ip.v4)[0] == 203U);
                assert(((const uint8_t *)&route->nexthop.ip.v4)[3] == 9U);
                assert(route->metric.delay == 250U);

                /* While Active, a redistribution change may update path-level
                 * observations, but the destination FD/current/reported state
                 * remains frozen.
                 */
                pd->state = EIGRP_FSM_STATE_ACTIVE_1;
                frozen_fd = pd->fdistance;
                frozen_distance = pd->distance;
                frozen_reported_metric = pd->reported_metric;
                seed.delay = 400;
                assert(eigrp_redist_add(&context, &source_id, &seed, NULL)
                       == EIGRP_RESULT_SUCCESS);
                candidate.tag = 77;
                set_ipv4(&candidate_nexthop.gateway, 192, 0, 2, 99);
                assert(eigrp_rib_redist_add(&runtime, &candidate)
                       == EIGRP_RESULT_SUCCESS);
                assert(pd->fdistance == frozen_fd);
                assert(pd->distance == frozen_distance);
                assert(memcmp(&pd->reported_metric, &frozen_reported_metric,
                              sizeof(frozen_reported_metric)) == 0);
                route = only_route(&runtime, &candidate.prefix);
                assert(route->metric.delay == 400U);
                assert(route->extdata.tag == 77U);
                assert(((const uint8_t *)&route->nexthop.ip.v4)[3] == 99U);

                /* Withdrawal while Active is also path-only: poison the local
                 * external observation without rewriting frozen destination
                 * successor/FD/current/reported state.
                 */
                assert(eigrp_rib_redist_del(&runtime, &candidate)
                       == EIGRP_RESULT_SUCCESS);
                assert(pd->fdistance == frozen_fd);
                assert(pd->distance == frozen_distance);
                assert(memcmp(&pd->reported_metric, &frozen_reported_metric,
                              sizeof(frozen_reported_metric)) == 0);
                route = only_route(&runtime, &candidate.prefix);
                assert(route->distance == EIGRP_MAX_METRIC);

                /* Return Passive.  The unreachable external path remains
                 * topology-owned until packetizer consumes NEED_UPDATE; this
                 * preserves its external type and metadata for withdrawal.
                 */
                pd->state = EIGRP_FSM_STATE_PASSIVE;
                assert(eigrp_rib_redist_del(&runtime, &candidate)
                       == EIGRP_RESULT_SUCCESS);
                pd = eigrp_topology_table_lookup(runtime.topology_table,
                                                 &candidate.prefix);
                assert(pd != NULL);
                assert(pd->req_action & EIGRP_FSM_NEED_UPDATE);
                route = only_route(&runtime, &candidate.prefix);
                assert(route->type == EIGRP_TLV_IPv4_EXT);
                assert(route->distance == EIGRP_MAX_METRIC);
                assert(route->extdata.tag == 77U);

                /* Runtime metric updates must retain local-route RD=0 and
                 * must not turn a poisoned redistribution path reachable.
                 */
                runtime.k_values[0] = 2;
                runtime.metric_version = EIGRP_TLV_64B_VERSION;
                eigrp_topology_metric_update(&runtime);
                assert(route->reported_distance == 0);
                assert(route->distance == EIGRP_MAX_METRIC);

                /* Reintroduce the local redistribution candidate. Its seed
                 * delay is unscaled (10-us units), unlike its DUAL distance.
                 */
                assert(eigrp_rib_redist_add(&runtime, &candidate)
                       == EIGRP_RESULT_SUCCESS);
                pd = eigrp_topology_table_lookup(runtime.topology_table,
                                                 &candidate.prefix);
                route = only_route(&runtime, &candidate.prefix);
                assert(route->metric.delay == 400);
                assert(route->reported_distance == 0);
                assert(route->distance != EIGRP_MAX_METRIC);
                assert(eigrp_metrics_match(route->reported_metric, route->metric));
                assert(eigrp_metrics_match(route->total_metric, route->metric));
                assert(pd->rdistance == route->distance);
                assert(eigrp_metrics_match(pd->reported_metric,
                                           route->total_metric));

                /* Build one self-originated connected route, independently
                 * from the redistribution route above. */
                {
                    eigrp_intf_t link = {0};
                    eigrp_prefix_descriptor_t *local_pd;
                    eigrp_route_descriptor_t *local_route;
                    eigrp_metrics_t local_metric = {0};
                    eigrp_prefix_t local_destination = {0};
                    eigrp_metric_t old_local_distance;
                    eigrp_metric_t old_redist_distance = route->distance;
                    eigrp_metrics_t redist_calculation = route->metric;

                    link.params.bandwidth = 100000;
                    link.params.delay = 100;
                    link.params.load = 1;
                    link.params.reliability = 255;
                    local_metric.bandwidth = eigrp_bandwidth_to_scaled(
                        link.params.bandwidth);
                    local_metric.delay = eigrp_delay_to_scaled(
                        link.params.delay);
                    local_metric.load = link.params.load;
                    local_metric.reliability = link.params.reliability;
                    set_ipv4(&local_destination.address, 10, 88, 0, 0);
                    local_destination.prefix_length = 24;
                    local_pd = eigrp_topology_prefix_create();
                    local_route = eigrp_topology_route_create(&link);
                    assert(local_pd && local_route);
                    local_pd->destination = local_destination;
                    local_pd->nt = EIGRP_TOPOLOGY_TYPE_CONNECTED;
                    local_pd->state = EIGRP_FSM_STATE_PASSIVE;
                    local_pd->fdistance = local_pd->distance =
                        local_pd->rdistance = eigrp_metric_calculate(
                            &runtime, local_metric);
                    local_pd->reported_metric = local_metric;
                    local_route->type = EIGRP_TLV_IPv4_INT;
                    local_route->dest = local_destination;
                    local_route->adv_router = &self;
                    local_route->metric = local_metric;
                    local_route->reported_metric = local_metric;
                    local_route->total_metric = local_metric;
                    local_route->reported_distance = 0;
                    local_route->distance = local_pd->distance;
                    local_route->flags = EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG;
                    eigrp_prefix_descriptor_add(runtime.topology_table, local_pd);
                    eigrp_route_descriptor_add(&runtime, local_pd, local_route);
                    old_local_distance = local_route->distance;

                    /* Simulate an interface delay update and a K-value
                     * recomputation in the same topology refresh. */
                    link.params.delay = 200;
                    runtime.k_values[2] = 2;
                    eigrp_topology_metric_update(&runtime);
                    local_metric.delay = eigrp_delay_to_scaled(200);
                    assert(local_route->reported_distance == 0);
                    assert(local_route->metric.delay == local_metric.delay);
                    assert(eigrp_metrics_match(local_route->reported_metric,
                                               local_metric));
                    assert(eigrp_metrics_match(local_route->total_metric,
                                               local_metric));
                    assert(local_route->total_metric.delay == local_metric.delay);
                    assert(local_route->total_metric.hop_count == 0);
                    assert(local_route->distance ==
                           eigrp_metric_calculate(&runtime, local_metric));
                    assert(local_route->distance != old_local_distance);
                    assert(local_route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
                    assert(local_pd->fdistance == local_route->distance);
                    assert(local_pd->rdistance == local_route->distance);
                    assert(eigrp_metrics_match(local_pd->reported_metric,
                                               local_route->total_metric));

                    /* No second link-delay addition for redistributed seeds. */
                    redist_calculation.delay = eigrp_delay_to_scaled(400);
                    assert(route->reported_distance == 0);
                    assert(route->distance != old_redist_distance);
                    assert(route->distance == eigrp_metric_calculate(
                        &runtime, redist_calculation));
                    assert(eigrp_metrics_match(route->reported_metric,
                                               route->metric));
                    assert(eigrp_metrics_match(route->total_metric,
                                               route->metric));
                    assert(route->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
                    assert(pd->rdistance == route->distance);
                    assert(eigrp_metrics_match(pd->reported_metric,
                                               route->total_metric));

                    /* Reapply with identical inputs: no cumulative hop or
                     * delay growth and no change of the selected metrics. */
                    old_local_distance = local_route->distance;
                    old_redist_distance = route->distance;
                    eigrp_topology_metric_update(&runtime);
                    assert(local_route->distance == old_local_distance);
                    assert(route->distance == old_redist_distance);
                    assert(local_route->total_metric.hop_count == 0);
                    assert(route->reported_distance == 0);
                    assert(local_route->reported_distance == 0);
                }

                eigrp_topology_table_delete(&runtime, runtime.topology_table);
                eigrp_list_delete(&runtime.topology_changes);
                return 0;
            }
            '''
        )
    )

    compiler = os.environ.get("CC", "cc")
    result = subprocess.run(
        [
            compiler,
            "-std=gnu11",
            "-O0",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-function",
            "-Wno-unused-parameter",
            "-ffunction-sections",
            "-fdata-sections",
            f"-I{ROOT}",
            f"-I{ROOT / 'test' / 'build' / 'include'}",
            str(source),
            str(ROOT / "eigrp" / "code" / "eigrp_redistribute.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_topology.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_timer.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_metric.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_list.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_table.c"),
            str(ROOT / "eigrp" / "code" / "eigrp_prefix.c"),
            *( ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"] ),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr

    result = subprocess.run(
        [str(binary)],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr
