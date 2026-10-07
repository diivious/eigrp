from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CODE = ROOT / "code"
FRR = ROOT.parents[0] / "frr" / "code"


def read(path):
    return path.read_text()


def test_p2_event_families_are_defined_and_formatted():
    hdr = read(CODE / "eigrp_eventlog.h")
    src = read(CODE / "eigrp_eventlog.c")

    for name in (
        "EIGRP_EVENTLOG_OPCODE_PEER_CAPABILITY",
        "EIGRP_EVENTLOG_OPCODE_INTERFACE_STATE",
        "EIGRP_EVENTLOG_OPCODE_SUCCESSOR_CHANGE",
    ):
        assert name in hdr
        assert name in src

    assert "Peer capability %s TLV %lu -> %lu advertised %lu K6 %lu" in src
    assert "Successor change %s old %lu new %lu metric %lu FD %lu" in src


def test_peer_capability_event_is_emitted_at_codec_selection():
    src = read(CODE / "eigrp_neighbor.c")
    start = src.index("void eigrp_nbr_codec_select")
    body = src[start : src.index("void eigrp_nbr_codec_update", start)]

    assert "old_version = nbr->tlv_version" in body
    assert "nbr->tlv_version = tlv_version" in body
    assert "if (old_version != tlv_version)" in body
    assert "EIGRP_EVENTLOG_OPCODE_PEER_CAPABILITY" in body


def test_interface_lifecycle_events_cover_failure_and_state_changes():
    src = read(CODE / "eigrp_interface.c")

    assert "EIGRP_EVENTLOG_INTERFACE_STATE_UP" in src
    assert "EIGRP_EVENTLOG_INTERFACE_STATE_DOWN" in src
    assert "EIGRP_EVENTLOG_INTERFACE_STATE_PASSIVE" in src
    assert "EIGRP_EVENTLOG_INTERFACE_STATE_ACTIVE" in src
    assert "EIGRP_EVENTLOG_INTERFACE_STATE_MULTICAST_JOIN_FAILED" in src
    assert "result = eigrp_sys_multicast_join" in src


def test_successor_change_compares_selected_set_to_installed_set():
    src = read(CODE / "eigrp_topology.c")
    start = src.index("void eigrp_update_routing_table")
    body = src[start : src.index("void eigrp_topology_connected_interface_down", start)]

    assert "EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG" in body
    assert "EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG" in body
    assert "old_successor != new_successor" in body
    assert "EIGRP_EVENTLOG_OPCODE_SUCCESSOR_CHANGE" in body


def test_eventlog_reports_history_loss():
    mgnt = read(CODE / "eigrp_mgnt.h")
    src = read(CODE / "eigrp_eventlog.c")

    assert "uint64_t written;" in mgnt
    assert "uint64_t overwritten;" in mgnt
    assert "log->overwritten++" in src
    assert "state->written = log->written" in src
    assert "state->overwritten = log->overwritten" in src


def test_p0_no_interface_reject_and_all_rib_deletes_are_logged():
    packet = read(CODE / "eigrp_packet.c")
    hdr = read(CODE / "eigrp_eventlog.h")
    topology = read(CODE / "eigrp_topology.c")

    assert "EIGRP_EVENTLOG_PACKET_REJECT_NO_INTERFACE" in hdr
    assert "EIGRP_EVENTLOG_PACKET_REJECT_NO_INTERFACE" in packet

    # Every portable southbound delete in topology must capture and log result.
    assert topology.count("eigrp_rib_route_del(") == topology.count(
        "EIGRP_EVENTLOG_OPCODE_RIB_WITHDRAW"
    )
