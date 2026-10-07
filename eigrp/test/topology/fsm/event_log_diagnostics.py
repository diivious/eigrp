# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"


def read(name):
    return (CODE / name).read_text()


def test_diagnostic_event_opcodes_and_cisco_overlap_text_exist():
    header = read("eigrp_eventlog.h")
    source = read("eigrp_eventlog.c")
    for opcode in (
        "DUAL_METRIC_SET", "DUAL_FIND_FS", "DUAL_FC_SAT", "DUAL_FC_NOT_SAT",
        "DUAL_ACTIVE_PEERS", "QUERY_RX", "REPLY_RX", "REPLY_TX",
        "REPLY_STATUS", "TOPOLOGY_ROUTE_DELETE", "UPDATE_PACKETIZED",
        "POISON_REVERSE", "NSF_EVENT", "SIA_QUERY_TX", "SIA_QUERY_RX",
        "SIA_REPLY_TX", "SIA_REPLY_RX", "SIA_EXPIRE",
    ):
        assert f"EIGRP_EVENTLOG_OPCODE_{opcode}" in header
        assert f"EIGRP_EVENTLOG_OPCODE_{opcode}" in source

    for text in (
        "Metric set:", "Find FS:", "FC sat rdbmet/succmet:",
        "FC not sat Dmin/met:", "Active net/peers:", "Send reply:",
        "RDB delete:", "Update packetized:",
    ):
        assert text in source


def test_dual_diagnostics_are_at_selection_and_commit_sites():
    fsm = read("eigrp_fsm.c")
    assert "EIGRP_EVENTLOG_OPCODE_DUAL_FIND_FS" in fsm
    assert "EIGRP_EVENTLOG_OPCODE_DUAL_FC_SAT" in fsm
    assert "EIGRP_EVENTLOG_OPCODE_DUAL_FC_NOT_SAT" in fsm
    assert "EIGRP_EVENTLOG_OPCODE_DUAL_METRIC_SET" in fsm
    assert "EIGRP_EVENTLOG_OPCODE_DUAL_ACTIVE_PEERS" in fsm
    # Exact query targets are known only after packetization populated RIJ.
    query_sent = fsm[fsm.index("void eigrp_fsm_query_sent") : fsm.index("/*\n * NSM", fsm.index("void eigrp_fsm_query_sent"))]
    assert "prefix->rij ? prefix->rij->count : 0" in query_sent


def test_query_reply_events_are_prefix_level_not_just_packet_level():
    query = read("eigrp_query.c")
    reply = read("eigrp_reply.c")
    assert "EIGRP_EVENTLOG_OPCODE_QUERY_RX" in query
    assert "&route->dest" in query
    assert "EIGRP_EVENTLOG_OPCODE_REPLY_RX" in reply
    assert "EIGRP_EVENTLOG_OPCODE_REPLY_TX" in reply
    assert "&prefix->destination" in reply


def test_topology_packetizer_and_nsf_decisions_are_logged_at_operation_sites():
    topology = read("eigrp_topology.c")
    packetizer = read("eigrp_packetizer.c")
    update = read("eigrp_update.c")

    delete = topology[topology.index("void eigrp_route_descriptor_delete") :
                      topology.index("eigrp_table_t *eigrp_topology_table_create")]
    assert delete.index("EIGRP_EVENTLOG_OPCODE_TOPOLOGY_ROUTE_DELETE") < delete.index("eigrp_list_delete_data")

    add = packetizer[packetizer.index("static int eigrp_packetizer_builder_route_add") :
                     packetizer.index("static void eigrp_packetizer_builder_init")]
    assert "encoded > 0" in add
    assert "EIGRP_EVENTLOG_OPCODE_UPDATE_PACKETIZED" in add

    poison = packetizer[packetizer.index("static eigrp_route_descriptor_t *eigrp_packetizer_poison_reverse") :
                         packetizer.index("static void eigrp_packetizer_query_rij_add")]
    assert "eigrp_nbr_split_horizon" in poison
    assert "EIGRP_EVENTLOG_OPCODE_POISON_REVERSE" in poison

    assert "eigrp_update_nsf_eventlog" in update
    for reason in ("1, flags", "2, flags", "3, flags", "4, flags"):
        assert reason in update

    siaquery = read("eigrp_siaquery.c")
    siareply = read("eigrp_siareply.c")
    fsm = read("eigrp_fsm.c")
    assert "EIGRP_EVENTLOG_OPCODE_SIA_QUERY_TX" in siaquery
    assert "EIGRP_EVENTLOG_OPCODE_SIA_QUERY_RX" in siaquery
    assert "EIGRP_EVENTLOG_OPCODE_SIA_REPLY_TX" in siareply
    assert "EIGRP_EVENTLOG_OPCODE_SIA_REPLY_RX" in siareply
    assert "EIGRP_EVENTLOG_OPCODE_SIA_EXPIRE" in fsm
