from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CODE = ROOT / "code"


def read(name):
    return (CODE / name).read_text()


def test_neighbor_down_reasons_are_logged_at_cause_sites():
    eventlog = read("eigrp_eventlog.h")
    neighbor = read("eigrp_neighbor.c")
    hello = read("eigrp_hello.c")
    packet = read("eigrp_packet.c")
    update = read("eigrp_update.c")
    fsm = read("eigrp_fsm.c")
    interface = read("eigrp_interface.c")

    for reason in (
        "HOLD_EXPIRED", "K_MISMATCH", "GOODBYE", "PEER_RESTART",
        "RTP_RETRY_LIMIT", "ADMIN_CLEAR", "INTERFACE_DOWN",
        "PEER_TERMINATION", "SIA",
    ):
        assert f"EIGRP_EVENTLOG_NEIGHBOR_REASON_{reason}" in eventlog

    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_HOLD_EXPIRED" in neighbor
    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_ADMIN_CLEAR" in neighbor
    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_K_MISMATCH" in hello
    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_GOODBYE" in hello
    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_PEER_TERMINATION" in hello
    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_RTP_RETRY_LIMIT" in packet
    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_PEER_RESTART" in update
    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_SIA" in fsm
    assert "EIGRP_EVENTLOG_NEIGHBOR_REASON_INTERFACE_DOWN" in interface


def test_packet_reject_reasons_cover_pre_protocol_drop_paths():
    eventlog = read("eigrp_eventlog.h")
    packet = read("eigrp_packet.c")
    hello = read("eigrp_hello.c")

    reasons = (
        "BOUNDS", "PASSIVE_INTERFACE", "SHORT_HEADER", "VERSION",
        "AS_MISMATCH", "VRID_MISMATCH", "CHECKSUM", "SOURCE_OFFLINK",
        "AUTH_TLV_FRAMING", "AUTH_UNEXPECTED", "AUTH_NO_KEY",
        "AUTH_MISSING", "AUTH_NOT_FIRST", "AUTH_TYPE_MISMATCH",
        "AUTH_LENGTH", "AUTH_UNSUPPORTED", "AUTH_DIGEST", "NO_NEIGHBOR",
        "CR_NOT_ELIGIBLE", "HELLO_TLV", "STATIC_NEIGHBOR",
        "HELLO_K_MISMATCH", "UNSUPPORTED_OPCODE",
    )
    for reason in reasons:
        enum_name = f"EIGRP_EVENTLOG_PACKET_REJECT_{reason}"
        assert enum_name in eventlog
        assert enum_name in packet or enum_name in hello

    assert "EIGRP_EVENTLOG_OPCODE_PACKET_REJECT" in packet
    assert "eigrp_packet_reject_event" in packet
    assert "eigrp_hello_reject_event" in hello


def test_neighbor_transition_event_carries_reason_in_arg4():
    neighbor = read("eigrp_neighbor.c")
    eventlog = read("eigrp_eventlog.c")

    assert "old_state, nbr->state, nbr->ei->ifindex, reason" in neighbor
    assert "eigrp_eventlog_neighbor_reason_name(entry->arg4)" in eventlog
    assert "reason %s" in eventlog


def test_packet_reject_event_keeps_compact_event_shape():
    eventlog_h = read("eigrp_eventlog.h")
    eventlog_c = read("eigrp_eventlog.c")

    assert "EIGRP_EVENTLOG_OPCODE_PACKET_REJECT" in eventlog_h
    assert "eventmsg_arg_t arg1" in eventlog_h
    assert "eventmsg_arg_t arg4" in eventlog_h
    assert "Packet rejected from %s %s reason %s detail %lu/%lu" in eventlog_c
