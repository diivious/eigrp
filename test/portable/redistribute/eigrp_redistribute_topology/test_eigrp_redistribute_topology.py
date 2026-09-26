# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Portable behavior coverage for redistribution candidate topology lifecycle.

from pathlib import Path
import os
import subprocess
import textwrap


ROOT = Path(__file__).resolve().parents[4]


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

            #include "eigrpd/eigrpd.h"
            #include "eigrpd/eigrp_structs.h"
            #include "eigrpd/eigrp_debug.h"
            #include "eigrpd/eigrp_fsm.h"
            #include "eigrpd/eigrp_neighbor.h"
            #include "eigrpd/eigrp_redistribute.h"
            #include "eigrpd/eigrp_rib.h"
            #include "eigrpd/eigrp_topology.h"

            static eigrp_address_family_config_t *test_runtime_af;
            static unsigned query_work;
            static unsigned update_work;

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

            bool eigrp_debug_address_family_enabled(
                eigrp_instance_t *eigrp,
                eigrp_debug_address_family_category_t category,
                const eigrp_addr_t *neighbor)
            {
                (void)eigrp;
                (void)category;
                (void)neighbor;
                return false;
            }

            bool eigrp_instance_data_path_ready(const eigrp_instance_t *runtime)
            {
                return runtime != NULL;
            }

            eigrp_address_family_t
            eigrp_instance_address_family(const eigrp_instance_t *runtime)
            {
                (void)runtime;
                return EIGRP_ADDRESS_FAMILY_IPV4;
            }

            eigrp_address_family_config_t *
            eigrp_instance_runtime_config(eigrp_instance_t *runtime)
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
                const eigrp_redistribute_source_t *source)
            {
                (void)eigrp;
                (void)source;
                return EIGRP_RESULT_SUCCESS;
            }

            eigrp_result_t eigrp_rib_redistribute_remove(
                eigrp_instance_t *eigrp,
                const eigrp_redistribute_source_t *source)
            {
                (void)eigrp;
                (void)source;
                return EIGRP_RESULT_SUCCESS;
            }

            eigrp_result_t eigrp_rib_route_install(
                eigrp_instance_t *eigrp, const eigrp_rib_route_t *route)
            {
                (void)eigrp;
                (void)route;
                return EIGRP_RESULT_SUCCESS;
            }

            eigrp_result_t eigrp_rib_route_remove(
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
                                       eigrp_interface_t *exception)
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

                head = eigrp_topology_route_head(msg->prefix);
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
                return eigrp_list_node_data(eigrp_list_head(pd->external_routes));
            }

            static void set_ipv4(eigrp_address_t *address,
                                 unsigned a, unsigned b,
                                 unsigned c, unsigned d)
            {
                address->afi = EIGRP_ADDRESS_FAMILY_IPV4;
                address->bytes[0] = (uint8_t)a;
                address->bytes[1] = (uint8_t)b;
                address->bytes[2] = (uint8_t)c;
                address->bytes[3] = (uint8_t)d;
            }

            int main(void)
            {
                eigrp_instance_t runtime = {0};
                eigrp_neighbor_t self = {0};
                eigrp_address_family_config_t af = {
                    .afi = EIGRP_ADDRESS_FAMILY_IPV4,
                    .asn = 4453,
                };
                eigrp_instance_context_t context = {
                    .config = &af,
                    .runtime = &runtime,
                    .topology_id = EIGRP_TOPOLOGY_ID_BASE,
                };
                eigrp_redistribute_source_t source_id = {
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
                eigrp_rib_source_route_t candidate = {0};
                eigrp_prefix_descriptor_t *pd;
                eigrp_route_descriptor_t *route;
                eigrp_route_descriptor_t *same_route;
                uint32_t frozen_fd;
                uint32_t frozen_distance;
                eigrp_metrics_t frozen_reported_metric;

                runtime.AS = 4453;
                runtime.vrid = 1;
                runtime.variance = 1;
                runtime.max_paths = 4;
                runtime.k_values[0] = 1;
                runtime.k_values[2] = 1;
                runtime.af_vectors.afi = EIGRP_ADDRESS_FAMILY_IPV4;
                runtime.af_vectors.classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
                runtime.topology_table = eigrp_topology_table_create();
                runtime.topology_changes = eigrp_list_new();
                runtime.neighbor_self = &self;
                assert(inet_pton(AF_INET, "192.0.2.45", &runtime.router_id) == 1);
                test_runtime_af = &af;

                assert(eigrp_redistribute_add(&context, &source_id, &seed, NULL)
                       == EIGRP_RESULT_SUCCESS);

                set_ipv4(&candidate.prefix.address, 10, 77, 0, 0);
                candidate.prefix.prefix_length = 24;
                candidate.source = source_id;
                candidate.metric = 900;
                candidate.tag = 0x11223344U;
                candidate.gateway_present = true;
                set_ipv4(&candidate.gateway, 198, 51, 100, 1);

                /* First appearance: one external topology path with complete
                 * RFC external metadata.  An EIGRP-origin candidate remains
                 * EXTERNAL rather than becoming an internal route.
                 */
                assert(eigrp_rib_source_route_add(&runtime, &candidate)
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
                assert(eigrp_redistribute_add(&context, &source_id, &seed, NULL)
                       == EIGRP_RESULT_SUCCESS);
                candidate.metric = 1234;
                candidate.tag = 0xaabbccddU;
                set_ipv4(&candidate.gateway, 203, 0, 113, 9);
                assert(eigrp_rib_source_route_add(&runtime, &candidate)
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
                assert(eigrp_redistribute_add(&context, &source_id, &seed, NULL)
                       == EIGRP_RESULT_SUCCESS);
                candidate.tag = 77;
                set_ipv4(&candidate.gateway, 192, 0, 2, 99);
                assert(eigrp_rib_source_route_add(&runtime, &candidate)
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
                assert(eigrp_rib_source_route_remove(&runtime, &candidate)
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
                assert(eigrp_rib_source_route_remove(&runtime, &candidate)
                       == EIGRP_RESULT_SUCCESS);
                pd = eigrp_topology_table_lookup(runtime.topology_table,
                                                 &candidate.prefix);
                assert(pd != NULL);
                assert(pd->req_action & EIGRP_FSM_NEED_UPDATE);
                route = only_route(&runtime, &candidate.prefix);
                assert(route->type == EIGRP_TLV_IPv4_EXT);
                assert(route->distance == EIGRP_MAX_METRIC);
                assert(route->extdata.tag == 77U);

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
            str(ROOT / "eigrpd" / "eigrp_redistribute.c"),
            str(ROOT / "eigrpd" / "eigrp_topology.c"),
            str(ROOT / "eigrpd" / "eigrp_metric.c"),
            str(ROOT / "eigrpd" / "eigrp_list.c"),
            str(ROOT / "eigrpd" / "eigrp_table.c"),
            str(ROOT / "eigrpd" / "eigrp_prefix.c"),
            "-Wl,--gc-sections",
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
