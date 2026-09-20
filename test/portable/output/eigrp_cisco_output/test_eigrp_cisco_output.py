# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for Cisco-style formatting of implemented EIGRP output.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
DUMP = ROOT / "eigrpd" / "eigrp_dump.c"
HELLO = ROOT / "eigrpd" / "eigrp_hello.c"
UPDATE = ROOT / "eigrpd" / "eigrp_update.c"
PACKET = ROOT / "eigrpd" / "eigrp_packet.c"
NAMED = ROOT / "frr" / "eigrp_cli_named.c"
CLASSIC_VTY = ROOT / "frr" / "eigrp_vty.c"
SPEC = ROOT / "specs" / "cli-spec.md"


def read(path: Path) -> str:
    return path.read_text()


def test_classic_neighbor_and_topology_headings_follow_eigrp_reference_format():
    dump = read(DUMP)

    assert "IP-EIGRP neighbors for process %u" in dump
    assert "H   Address                 Interface       Hold Uptime   SRTT   RTO  Q  Seq" in dump
    assert "(sec)          (ms)       Cnt Num" in dump
    assert "IP-EIGRP Topology Table for AS(%d)/ID(%s)" in dump
    assert "Codes: P - Passive, A - Active, U - Update, Q - Query, " in dump
    assert '"R - Reply,\\n       r - reply Status, s - sia Status\\n\\n"' in dump


def test_event_log_heading_already_matches_reference_output():
    named = read(NAMED)
    classic = read(CLASSIC_VTY)

    assert '"Event information for AS %u:\\n"' in named
    assert '"Event information for AS %u:\\n"' in classic


def test_topology_uses_inaccessible_and_all_links_serial_number_formatting():
    dump = read(DUMP)
    named = read(NAMED)

    for source in (dump, named):
        assert 'vty_out(' in source
        assert '"Inaccessible"' in source
        assert '", serno %" PRIu64' in source
        assert "serno:" not in source

    assert "include_serial" in dump
    assert ".include_serial = options->all_links" in named
    assert "route flags identify successors and feasible successors" not in named
    assert '"        via Connected, %s\\n"' in named
    assert "[successor]" not in named
    assert "[feasible-successor]" not in named


def test_packet_debug_summary_uses_cisco_eigrp_wording_without_invented_queue_state():
    dump = read(DUMP)

    assert '"EIGRP: Sending %s on %s nbr %s, retry %u, RTO %u"' in dump
    assert '"EIGRP: Received %s on %s nbr %s"' in dump
    assert '"  AS %u, Flags 0x%x, Seq %u/%u"' in dump
    assert '"EIGRP: Retransmitting %s' not in dump
    assert "idbQ" not in dump
    assert "iidbQ" not in dump
    assert "peerQ" not in dump
    assert '"  packet length %u' in dump
    assert "EIGRP_DEBUG_PACKET_DETAIL" in dump


def test_neighbor_change_messages_use_established_transition_reasons():
    hello = read(HELLO)
    update = read(UPDATE)
    packet = read(PACKET)

    assert "is down: K-value mismatch" in hello
    assert "is down: Interface Goodbye received" in hello
    assert "Kvalue mismatch" not in hello
    assert "Interface PEER-TERMINATION received" not in hello

    assert "is down: peer restarted" in update
    assert "is resync: peer graceful-restart" in update
    assert "is pending: new adjacency" not in update

    assert "is up: new adjacency" in packet
    assert "adjacency became full" not in packet


def test_neighbor_output_uses_real_available_transport_state_without_synthesizing_srtt():
    dump = read(DUMP)
    named = read(NAMED)

    assert "nbr->up_since_msec" in dump
    assert "nbr->retransmissions" in dump
    assert "nbr->retrans_queue->tail->retrans_counter" in dump
    assert "EIGRP_PACKET_RETRANS_TIME * 1000U" in dump
    assert '"n/a"' in dump
    assert '"%-3s %-23s' in dump

    assert "state->uptime_seconds" in named
    assert "state->retransmit_count" in named
    assert "state->retry_count" in named
    assert "state->prefix_count" in named
    assert 'snprintf(srtt, sizeof(srtt), "n/a")' in named
    assert "SRTT: n/a; transport uses the fixed" not in named


def test_output_spec_forbids_capability_work_for_presentation_parity():
    spec = read(SPEC)

    assert "Operational output conventions" in spec
    assert "118974-technote-eigrp-00.html" in spec
    assert "Do not create protocol state or implement an incomplete capability solely" in spec
    assert "detailed topology/vector-metric output is not synthesized" in spec
