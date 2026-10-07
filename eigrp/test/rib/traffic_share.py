# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import os
import subprocess
import sys
import textwrap

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())


def test_balanced_traffic_share_weights_selected_successors_only(tmp_path):
    source = tmp_path / "traffic_share.c"
    binary = tmp_path / "traffic_share"
    source.write_text(textwrap.dedent(r'''
        #include <assert.h>
        #include <stdarg.h>
        #include <stdint.h>

        #include "eigrp/code/eigrp.h"
            #include "eigrp/code/eigrp_log.h"
        #include "eigrp/code/eigrp_structs.h"
        #include "eigrp/code/eigrp_debug.h"
        #include "eigrp/code/eigrp_rib.h"
        #include "eigrp/code/eigrp_topology.h"

        unsigned long term_debug_eigrp;
        unsigned long term_debug_eigrp_nei;
        unsigned long term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX];
        unsigned long term_debug_eigrp_transmit;
        unsigned long term_debug_eigrp_notifications;
        static unsigned installs;
        static size_t last_count;
        static uint64_t weights[3];

        void eigrp_sys_event_cancel(eigrp_event_t **event) { if (event) *event = NULL; }
        void eigrp_log(eigrp_log_level_t level, const char *format, ...) { (void)level; (void)format; }
        bool eigrp_debug_af_enabled(eigrp_instance_t *eigrp,
            eigrp_debug_af_category_t category, const eigrp_addr_t *neighbor)
        { (void)eigrp; (void)category; (void)neighbor; return false; }
        eigrp_afi_t eigrp_instance_afi(const eigrp_instance_t *runtime)
        { return runtime ? runtime->af_vectors.afi : 0; }
        eigrp_result_t eigrp_eventlog_msg_add(
            eigrp_instance_t *eigrp, uint16_t opcode,
            const eigrp_prefix_t *addr, eventmsg_arg_t arg1,
            eventmsg_arg_t arg2, eventmsg_arg_t arg3, eventmsg_arg_t arg4)
        {
            (void)eigrp; (void)opcode; (void)addr;
            (void)arg1; (void)arg2; (void)arg3; (void)arg4;
            return EIGRP_RESULT_SUCCESS;
        }

        eigrp_result_t eigrp_rib_route_del(eigrp_instance_t *eigrp,
            const eigrp_prefix_t *prefix)
        { (void)eigrp; (void)prefix; return EIGRP_RESULT_SUCCESS; }
        eigrp_result_t eigrp_rib_route_add(eigrp_instance_t *eigrp,
            const eigrp_rib_route_t *route)
        {
            size_t i;
            (void)eigrp;
            installs++;
            last_count = route->nexthop_count;
            for (i = 0; i < last_count && i < 3; i++)
                weights[i] = route->nexthops[i].weight;
            return EIGRP_RESULT_SUCCESS;
        }

        static eigrp_route_descriptor_t *add_route(eigrp_instance_t *eigrp,
            eigrp_prefix_descriptor_t *prefix, eigrp_intf_t *ei,
            uint32_t distance, uint32_t rd)
        {
            eigrp_route_descriptor_t *route = eigrp_topology_route_create(NULL);
            assert(route);
            route->distance = distance;
            route->reported_distance = rd;
            route->type = EIGRP_TLV_IPv4_INT;
            route->prefix = prefix;
            route->ei = ei;
            eigrp_route_descriptor_add(eigrp, prefix, route);
            return route;
        }

        int main(void)
        {
            eigrp_instance_t eigrp = {0};
            eigrp_intf_t ei = {0};
            eigrp_prefix_descriptor_t *prefix = eigrp_topology_prefix_create();
            eigrp_route_descriptor_t *best, *unequal, *infeasible;

            assert(prefix);
            eigrp.af_vectors.afi = EIGRP_AFI_IPV4;
            eigrp.variance = 3;
            eigrp.max_paths = 3;
            eigrp.distance_internal = EIGRP_DISTANCE_INTERNAL_DEFAULT;
            eigrp.traffic_share_balanced = true;
            prefix->state = EIGRP_FSM_STATE_PASSIVE;
            prefix->distance = 20;
            prefix->fdistance = 20;

            best = add_route(&eigrp, prefix, &ei, 20, 10);
            unequal = add_route(&eigrp, prefix, &ei, 40, 15);
            infeasible = add_route(&eigrp, prefix, &ei, 50, 20);
            eigrp_topology_update_node_flags(&eigrp, prefix);

            assert(best->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
            assert(unequal->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
            assert(!(infeasible->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG));

            installs = 0;
            eigrp_update_routing_table(&eigrp, prefix);
            assert(installs == 1 && last_count == 2);
            assert(weights[0] == 2 && weights[1] == 1);

            /* Disabling balanced sharing changes only the host forwarding hint. */
            eigrp.traffic_share_balanced = false;
            eigrp_update_routing_table(&eigrp, prefix);
            assert(installs == 2 && last_count == 2);
            assert(weights[0] == 0 && weights[1] == 0);
            assert(best->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
            assert(unequal->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
            assert(!(infeasible->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG));

            /* Tightening variance removes the unequal successor before weighting. */
            eigrp.variance = 1;
            eigrp_topology_update_node_flags(&eigrp, prefix);
            eigrp.traffic_share_balanced = true;
            eigrp_update_routing_table(&eigrp, prefix);
            assert(last_count == 1 && weights[0] == 1);

            eigrp_topology_prefix_free(prefix);
            return 0;
        }
    '''))

    compiler = os.environ.get("CC", "cc")
    result = subprocess.run([
        compiler, "-std=gnu11", "-O0", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-Wno-unused-parameter",
        "-ffunction-sections", "-fdata-sections",
        f"-I{ROOT}", f"-I{ROOT / 'test' / 'build' / 'include'}",
        str(source), str(ROOT / "eigrp" / "code" / "eigrp_topology.c"),
        str(ROOT / "eigrp" / "code" / "eigrp_timer.c"),
        str(ROOT / "eigrp" / "code" / "eigrp_list.c"),
        str(ROOT / "eigrp" / "code" / "eigrp_table.c"),
        str(ROOT / "eigrp" / "code" / "eigrp_prefix.c"),
        *(["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]),
        "-o", str(binary),
    ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr

    result = subprocess.run([str(binary)], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr


def test_traffic_share_refresh_skips_active_destinations_and_target_is_live():
    topology = (ROOT / "eigrp" / "code" / "eigrp_topology.c").read_text()
    metric = (ROOT / "eigrp" / "code" / "eigrp_metric.c").read_text()
    rib = (ROOT / "eigrp" / "code" / "eigrp_rib.h").read_text()

    refresh = topology[topology.index("void eigrp_topology_traffic_share_update"):
                       topology.index("static void eigrp_topology_route_queue_resort")]
    assert "prefix->state != EIGRP_FSM_STATE_PASSIVE" in refresh
    assert "eigrp_update_routing_table(eigrp, prefix);" in refresh
    assert "eigrp_topology_update_node_flags" not in refresh

    target = metric[metric.index("eigrp_result_t eigrp_traffic_share_balanced_update"):
                    metric.index("metric maximum-hops")]
    assert "context->runtime->traffic_share_balanced = enabled;" in target
    assert "eigrp_topology_traffic_share_update(context->runtime);" in target
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in target
    assert "uint64_t weight;" in rib
