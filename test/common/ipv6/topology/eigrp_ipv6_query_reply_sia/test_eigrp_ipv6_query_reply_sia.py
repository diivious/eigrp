# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import os
import subprocess
import textwrap

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd").is_dir())


def read(name):
    return (ROOT / name).read_text()


def test_multiple_neighbor_reply_status_bookkeeping_executes(tmp_path):
    source = tmp_path / "reply_status.c"
    binary = tmp_path / "reply_status"
    source.write_text(textwrap.dedent(r'''
        #include <assert.h>
        #include <stdlib.h>
        #include "eigrpd/eigrpd.h"
        #include "eigrpd/eigrp_structs.h"
        #include "eigrpd/eigrp_neighbor.h"
        #include "eigrpd/eigrp_fsm.h"

        int main(void)
        {
            eigrp_prefix_descriptor_t prefix = {0};
            eigrp_nbr_t a = {0}, b = {0}, c = {0};

            prefix.rij = eigrp_list_create();
            assert(prefix.rij != NULL);
            prefix.rij->del = free;

            eigrp_fsm_reply_status_add(&prefix, &a);
            eigrp_fsm_reply_status_add(&prefix, &b);
            eigrp_fsm_reply_status_add(&prefix, &c);
            eigrp_fsm_reply_status_add(&prefix, &b);
            assert(prefix.rij->count == 3);
            assert(eigrp_fsm_reply_status_pending(&prefix, &a));
            assert(eigrp_fsm_reply_status_pending(&prefix, &b));
            assert(eigrp_fsm_reply_status_pending(&prefix, &c));

            assert(eigrp_fsm_reply_status_remove(&prefix, &b));
            assert(prefix.rij->count == 2);
            assert(!eigrp_fsm_reply_status_pending(&prefix, &b));
            assert(eigrp_fsm_reply_status_remove(&prefix, &a));
            assert(prefix.rij->count == 1);
            assert(!eigrp_fsm_reply_status_remove(&prefix, &c));
            assert(prefix.rij->count == 0);

            eigrp_list_delete(&prefix.rij);
            return 0;
        }
    '''))
    cc = os.environ.get("CC", "cc")
    result = subprocess.run([
        cc, "-std=gnu11", "-O0", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-function", "-Wno-unused-parameter",
        "-ffunction-sections", "-fdata-sections",
        f"-I{ROOT}", f"-I{ROOT / 'test' / 'build' / 'include'}",
        str(source), str(ROOT / "eigrpd" / "eigrp_fsm.c"),
        str(ROOT / "eigrpd" / "eigrp_list.c"),
        "-Wl,--gc-sections", "-o", str(binary),
    ], cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert result.returncode == 0, result.stderr
    run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert run.returncode == 0, run.stderr


def test_query_packetization_owns_reply_status_and_starts_active_timer():
    packetizer = read("eigrpd/eigrp_packetizer.c")
    assert "eigrp_fsm_reply_status_add(prefix, nbr);" in packetizer
    assert "eigrp_fsm_query_sent(eigrp, prefix);" in packetizer
    assert "EIGRP_FSM_NEED_QUERY" in packetizer


def test_neighbor_loss_is_a_reply_during_active_computation():
    topology = read("eigrpd/eigrp_topology.c")
    assert "pe->state == EIGRP_FSM_STATE_PASSIVE" in topology
    assert "? EIGRP_OPC_UPDATE : EIGRP_OPC_REPLY" in topology


def test_sia_timer_probes_pending_neighbors_and_bounds_busy_intervals():
    fsm = read("eigrpd/eigrp_fsm.c")
    assert "status->sia_queries >= 3" in fsm
    assert "eigrp_siaquery_send(eigrp, status->neighbor, prefix);" in fsm
    assert "(uint32_t)active_time * 500U" in fsm
    assert "eigrp_nbr_delete(stuck);" in fsm


def test_sia_reply_active_bit_and_ipv6_route_types_are_supported():
    siareply = read("eigrpd/eigrp_siareply.c")
    packetizer = read("eigrpd/eigrp_packetizer.c")
    query = read("eigrpd/eigrp_query.c")
    reply = read("eigrpd/eigrp_reply.c")

    assert "route->metric.flags & EIGRP_OPAQUE_ACTIVE" in siareply
    assert "active_route.metric.flags |= EIGRP_OPAQUE_ACTIVE" in packetizer
    assert "EIGRP_TLV_IPv6_EXT" in query
    assert "EIGRP_TLV_IPv6_EXT" in reply


def test_reply_for_passive_destination_is_discarded():
    reply = read("eigrpd/eigrp_reply.c")
    assert "prefix->state == EIGRP_FSM_STATE_PASSIVE" in reply
    assert "eigrp_topology_route_free(route);" in reply
