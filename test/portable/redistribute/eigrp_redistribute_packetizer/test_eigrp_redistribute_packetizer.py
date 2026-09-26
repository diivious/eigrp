# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def read(path):
    return (ROOT / path).read_text()


def test_redistribution_stops_at_topology_not_packet_or_tlv():
    source = read("eigrpd/eigrp_redistribute.c")
    assert "eigrp_topology_redistributed_route_update" in source
    assert "eigrp_topology_redistributed_route_remove" in source
    assert "eigrp_packetizer" not in source
    assert "eigrp_tlv1" not in source
    assert "eigrp_tlv2" not in source
    assert "eigrp_packet_route_encode" not in source


def test_topology_drains_dual_actions_through_normal_packetizer_entry_points():
    source = read("eigrpd/eigrp_topology.c")
    notify = source[source.index("static void eigrp_topology_redistributed_notify") :]
    notify = notify[: notify.index("eigrp_result_t eigrp_topology_redistributed_route_update")]
    assert "EIGRP_FSM_NEED_QUERY" in notify
    assert "EIGRP_FSM_NEED_UPDATE" in notify
    assert "eigrp_query_send_all(eigrp)" in notify
    assert "eigrp_update_send_all(eigrp, NULL)" in notify
    assert "eigrp_packetizer_work_new" not in notify


def test_withdrawal_keeps_real_external_route_until_packetizer_consumes_update():
    topology = read("eigrpd/eigrp_topology.c")
    packetizer = read("eigrpd/eigrp_packetizer.c")
    assert "if (prefix->req_action & EIGRP_FSM_NEED_UPDATE)\n\t\treturn;" in topology
    assert "eigrp_update_topology_table_prefix(" in packetizer
    assert "action == EIGRP_FSM_NEED_UPDATE" in packetizer


def test_both_encoders_classify_external_before_internal_and_consume_extdata():
    tlv1 = read("eigrpd/eigrp_tlv1.c")
    tlv2 = read("eigrpd/eigrp_tlv2.c")
    assert "route->type == EIGRP_EXT" in tlv1
    assert "classic_external_tlv_type" in tlv1
    assert "eigrp_tlv1_external_encode(pkt, &route->extdata)" in tlv1
    assert "route->type == EIGRP_TLV_MP_EXT || route->type == EIGRP_EXT" in tlv2
    assert "classic_external_tlv_type" in tlv2
    assert "eigrp_tlv2_external_encode(eigrp, pkt, &route->extdata)" in tlv2


def test_classic_external_encoder_carries_topology_route_tag():
    topology = read("eigrpd/eigrp_topology.c")
    tlv1 = read("eigrpd/eigrp_tlv1.c")
    assert "extdata->tag = source_route->tag;" in topology
    assert "eigrp_stream_putl(pkt, extdata->tag);" in tlv1


def test_internal_route_classification_remains_internal():
    tlv1 = read("eigrpd/eigrp_tlv1.c")
    tlv2 = read("eigrpd/eigrp_tlv2.c")
    assert "route->type == EIGRP_INT" in tlv1
    assert "classic_internal_tlv_type" in tlv1
    assert "route->type == EIGRP_TLV_MP_INT || route->type == EIGRP_INT" in tlv2
    assert "classic_internal_tlv_type" in tlv2
