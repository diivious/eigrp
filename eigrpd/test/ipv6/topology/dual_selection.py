# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Task 11: IPv6 DUAL successor/feasibility selection while Passive.

from pathlib import Path
import os
import subprocess
import sys
import textwrap

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def test_ipv6_dual_successor_selection_executes(tmp_path):
    source = tmp_path / "ipv6_dual_selection.c"
    binary = tmp_path / "ipv6_dual_selection"
    source.write_text(textwrap.dedent(r'''
        #include <assert.h>
        #include <stdarg.h>
        #include <stdint.h>

        #include "eigrpd/code/eigrpd.h"
        #include "eigrpd/code/eigrp_structs.h"
        #include "eigrpd/code/eigrp_debug.h"
        #include "eigrpd/code/eigrp_rib.h"
        #include "eigrpd/code/eigrp_topology.h"

        unsigned long term_debug_eigrp;
        unsigned long term_debug_eigrp_nei;
        unsigned long term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX];
        unsigned long term_debug_eigrp_transmit;
        unsigned long term_debug_eigrp_notifications;
        static unsigned rib_installs;
        static unsigned rib_removes;

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

        eigrp_afi_t
        eigrp_instance_afi(const eigrp_instance_t *runtime)
        {
            return runtime ? runtime->af_vectors.afi : 0;
        }

        eigrp_result_t eigrp_rib_route_install(
            eigrp_instance_t *eigrp, const eigrp_rib_route_t *route)
        {
            (void)eigrp;
            (void)route;
            rib_installs++;
            return EIGRP_RESULT_SUCCESS;
        }

        eigrp_result_t eigrp_rib_route_remove(
            eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix)
        {
            (void)eigrp;
            (void)prefix;
            rib_removes++;
            return EIGRP_RESULT_SUCCESS;
        }

        static eigrp_route_descriptor_t *add_route(
            eigrp_instance_t *eigrp, eigrp_prefix_descriptor_t *prefix,
            uint32_t distance, uint32_t rd, uint16_t type)
        {
            eigrp_route_descriptor_t *route = eigrp_topology_route_create(NULL);
            assert(route != NULL);
            route->distance = distance;
            route->reported_distance = rd;
            route->type = type;
            route->prefix = prefix;
            eigrp_route_descriptor_add(eigrp, prefix, route);
            return route;
        }

        int main(void)
        {
            eigrp_instance_t eigrp = {0};
            eigrp_prefix_descriptor_t *prefix = eigrp_topology_prefix_create();
            eigrp_route_descriptor_t *a, *b, *c, *d, *poison;

            assert(prefix != NULL);
            eigrp.af_vectors.afi = EIGRP_AFI_IPV6;
            eigrp.variance = 2;
            eigrp.max_paths = 2;
            prefix->state = EIGRP_FSM_STATE_PASSIVE;
            prefix->fdistance = 100;
            prefix->distance = 50;

            /* Sorted CD order: equal-cost A/B, unequal C within variance,
             * D outside variance, then unreachable poison.  All reachable
             * routes satisfy RD < FD.
             */
            a = add_route(&eigrp, prefix, 50, 20, EIGRP_TLV_IPv6_INT);
            b = add_route(&eigrp, prefix, 50, 30, EIGRP_TLV_IPv6_INT);
            c = add_route(&eigrp, prefix, 80, 40, EIGRP_TLV_IPv6_INT);
            d = add_route(&eigrp, prefix, 120, 45, EIGRP_TLV_IPv6_INT);
            poison = add_route(&eigrp, prefix, EIGRP_MAX_METRIC,
                               EIGRP_MAX_METRIC, EIGRP_TLV_IPv6_INT);

            eigrp_topology_update_node_flags(&eigrp, prefix);
            assert(a->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
            assert(b->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
            assert(c->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG);
            assert(d->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG);
            assert(!(poison->flags & (EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG |
                                      EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG)));

            /* max-paths expands the selected variance set. */
            eigrp.max_paths = 3;
            eigrp_topology_update_node_flags(&eigrp, prefix);
            assert(c->flags & EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG);
            assert(!(c->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG));

            /* Tightening variance retains the unequal path only as an FS. */
            eigrp.variance = 1;
            eigrp_topology_update_node_flags(&eigrp, prefix);
            assert(c->flags & EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG);

            /* Strict FC: RD == FD is not feasible. */
            b->reported_distance = prefix->fdistance;
            eigrp_topology_update_node_flags(&eigrp, prefix);
            assert(!(b->flags & (EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG |
                                 EIGRP_ROUTE_DESCRIPTOR_FSUCCESSOR_FLAG)));

            /* Task 11 must not install/remove IPv6 routes in the host RIB. */
            eigrp_update_routing_table(&eigrp, prefix);
            assert(rib_installs == 0);
            assert(rib_removes == 0);

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
        str(source), str(ROOT / "eigrpd" / "code" / "eigrp_topology.c"),
        str(ROOT / "eigrpd" / "code" / "eigrp_list.c"),
        str(ROOT / "eigrpd" / "code" / "eigrp_table.c"),
        str(ROOT / "eigrpd" / "code" / "eigrp_prefix.c"),
        *( ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"] ), "-o", str(binary),
    ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr

    result = subprocess.run([str(binary)], cwd=ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr


def test_passive_fd_and_active_freeze_rules_are_preserved():
    fsm = (ROOT / "eigrpd" / "code" / "eigrp_fsm.c").read_text()
    topology = (ROOT / "eigrpd" / "code" / "eigrp_topology.c").read_text()

    keep_start = fsm.index("int eigrp_fsm_event_keep_state(eigrp_fsm_action_message_t *msg)")
    keep = fsm[keep_start:fsm.index("int eigrp_fsm_event_lr(", keep_start)]
    assert "if (route->distance < prefix->fdistance)" in keep
    assert "prefix->fdistance = route->distance;" in keep
    assert "prefix->fdistance =\n\t\t\t\tprefix->distance = route->distance" not in keep

    # Passive selection must walk CD order until it finds a path satisfying FC.
    # A lower-CD infeasible path must not hide a higher-CD feasible successor.
    event = fsm[fsm.index("static enum eigrp_fsm_events"):
                fsm.index("int eigrp_fsm_event(")]
    assert "eigrp_topology_route_select(prefix)" in event
    assert "if (selected)" in event

    flags_start = topology.index("void eigrp_topology_update_node_flags")
    flags = topology[
        flags_start:topology.index("void eigrp_update_routing_table", flags_start)
    ]
    assert "eigrp_topology_route_select(dest)" in flags

    # Task 13 carries the selected IPv6 successor set through the common RIB path.
    rib = topology[topology.index("void eigrp_update_routing_table"):
                   topology.index("void eigrp_topology_neighbor_down")]
    assert "Task 11 stops at IPv6 DUAL/topology selection" not in rib
    assert "eigrp_rib_route_install" in rib
    assert "eigrp_topology_southbound_nexthops" in rib
