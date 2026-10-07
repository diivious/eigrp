from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CODE = ROOT / "code"


def read(name):
    return (CODE / name).read_text()


def test_p0_unknown_ack_is_visible():
    packet = read("eigrp_packet.c")
    hello_ack = packet[packet.index("if (opcode == EIGRP_OPC_HELLO)"):]
    assert "EIGRP_EVENTLOG_PACKET_REJECT_NO_NEIGHBOR" in hello_ack
    assert "ntohl(eigrph->ack)" in hello_ack


def test_policy_denials_and_metric_offsets_are_logged_at_decision_sites():
    filt = read("eigrp_filter.c")
    for reason in (
        "EIGRP_EVENTLOG_FILTER_REASON_DEFAULT_INFORMATION",
        "EIGRP_EVENTLOG_FILTER_REASON_PROCESS",
        "EIGRP_EVENTLOG_FILTER_REASON_INTERFACE",
    ):
        assert reason in filt
    assert "EIGRP_EVENTLOG_OPCODE_ROUTE_FILTERED" in filt
    assert "EIGRP_EVENTLOG_OPCODE_OFFSET_APPLIED" in filt
    assert "old_delay" in filt


def test_prefix_limit_rejections_cover_all_runtime_scopes():
    neighbor = read("eigrp_neighbor.c")
    topology = read("eigrp_topology.c")
    redist = read("eigrp_redistribute.c")
    assert "EIGRP_EVENTLOG_PREFIX_LIMIT_NEIGHBOR" in neighbor
    assert "EIGRP_EVENTLOG_OPCODE_PREFIX_LIMIT_PEER" in neighbor
    assert "EIGRP_EVENTLOG_PREFIX_LIMIT_TOPOLOGY" in topology
    assert "EIGRP_EVENTLOG_PREFIX_LIMIT_REDISTRIBUTION" in redist


def test_rib_install_and_withdraw_outcomes_are_logged():
    topology = read("eigrp_topology.c")
    assert topology.count("EIGRP_EVENTLOG_OPCODE_RIB_INSTALL") >= 2
    assert "EIGRP_EVENTLOG_OPCODE_RIB_WITHDRAW" in topology
    assert "rib_result" in topology


def test_only_terminal_route_encode_failure_is_logged():
    packetizer = read("eigrp_packetizer.c")
    update = read("eigrp_update.c")
    assert "EIGRP_EVENTLOG_OPCODE_ROUTE_ENCODE_FAILURE" in packetizer
    assert update.count("EIGRP_EVENTLOG_OPCODE_ROUTE_ENCODE_FAILURE") >= 3
    assert "builder->route_count" in packetizer


def test_packet_send_failure_and_unexpected_ack_are_logged():
    packet = read("eigrp_packet.c")
    assert "EIGRP_EVENTLOG_OPCODE_PACKET_TX_FAILURE" in packet
    assert "EIGRP_EVENTLOG_OPCODE_RTP_ACK_UNEXPECTED" in packet
    assert "packet ? packet->sequence_number : 0" in packet
