# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level closure guards for Post Command Audit item 6.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
PACKET = ROOT / "eigrpd" / "eigrp_packet.c"
STRUCTS = ROOT / "eigrpd" / "eigrp_structs.h"
INTERFACE_C = ROOT / "eigrpd" / "eigrp_interface.c"
INTERFACE_H = ROOT / "eigrpd" / "eigrp_interface.h"
NEIGHBOR_C = ROOT / "eigrpd" / "eigrp_neighbor.c"
NEIGHBOR_H = ROOT / "eigrpd" / "eigrp_neighbor.h"
STATISTICS = ROOT / "eigrpd" / "eigrp_statistics.c"
TIMER_C = ROOT / "eigrpd" / "eigrp_timer.c"
TIMER_H = ROOT / "eigrpd" / "eigrp_timer.h"
TYPES = ROOT / "eigrpd" / "eigrp_types.h"
INSTANCE = ROOT / "eigrpd" / "eigrp_instance.c"
SOUTHBOUND_H = ROOT / "eigrpd" / "eigrp_southbound.h"
FRR_SOUTHBOUND = ROOT / "frr" / "eigrp_southbound.c"
VTY = ROOT / "frr" / "eigrp_cli_named.c"
CLI_SPEC = ROOT / "specs" / "cli-spec.md"


def read(path: Path) -> str:
    return path.read_text()


def test_traffic_counters_are_owned_by_validated_packet_io():
    packet = read(PACKET)
    statistics = read(STATISTICS)

    assert "eigrp_packet_header_is_ack" in packet
    classifier = packet[
        packet.index("static bool eigrp_packet_header_is_ack"):
        packet.index("static void eigrp_packet_opcode_counter_increment")
    ]
    assert "header->opcode == EIGRP_OPC_HELLO" in classifier
    assert "ntohl(header->sequence) == 0" in classifier
    assert "ntohl(header->ack) != 0" in classifier

    assert "if (ret >= 0)\n\t\teigrp_packet_send_stats_record" in packet
    assert packet.count("eigrp_packet_receive_stats_record(ei, eigrph);") == 2
    assert "Validated packet I/O owns all IPv4 traffic counters." in statistics
    assert "state->received_valid = state->sent_valid;" in statistics

    # Per-opcode modules and the packetizer must not double count packets.
    for path in (
        ROOT / "eigrpd" / "eigrp_hello.c",
        ROOT / "eigrpd" / "eigrp_update.c",
        ROOT / "eigrpd" / "eigrp_query.c",
        ROOT / "eigrpd" / "eigrp_reply.c",
        ROOT / "eigrpd" / "eigrp_siaquery.c",
        ROOT / "eigrpd" / "eigrp_siareply.c",
        ROOT / "eigrpd" / "eigrp_packetizer.c",
    ):
        source = read(path)
        assert "stats.sent." not in source
        assert "stats.rcvd." not in source


def test_interface_detail_uses_real_transport_counters():
    structs = read(STRUCTS)
    interface_c = read(INTERFACE_C)
    interface_h = read(INTERFACE_H)
    packet = read(PACKET)
    vty = read(VTY)

    for field in (
        "unreliable_multicast_sent",
        "reliable_multicast_sent",
        "unreliable_unicast_sent",
        "reliable_unicast_sent",
        "multicast_exceptions",
        "cr_packets_sent",
        "retransmissions_sent",
    ):
        assert field in structs
        assert field in interface_h
        assert f"state.{field}" in interface_c or f"state->{field}" in interface_c
        assert f"stats.{field}" in packet

    assert "eigrp_southbound_timer_remaining_seconds(ei->t_hello)" in interface_c
    assert "Un/reliable mcasts:" in vty
    assert "Mcast exceptions:" in vty
    assert "Retransmissions sent:" in vty
    assert "Next hello:" in vty
    assert "not exposed by current backend" not in vty
    assert "not exposed by the current backend" not in vty


def test_neighbor_detail_uses_live_hold_uptime_retry_and_prefix_state():
    neighbor_c = read(NEIGHBOR_C)
    neighbor_h = read(NEIGHBOR_H)
    southbound_h = read(SOUTHBOUND_H)
    frr_southbound = read(FRR_SOUTHBOUND)
    vty = read(VTY)

    assert "eigrp_southbound_timer_remaining_seconds(" in neighbor_c
    assert "nbr->t_holddown" in neighbor_c
    assert "eigrp_southbound_monotime_msec" in neighbor_c
    assert "uint64_t up_since_msec;" in neighbor_h
    assert "uint64_t uptime_seconds;" in neighbor_h
    assert "uint32_t prefix_count;" in neighbor_h
    assert "uint64_t retransmit_count;" in neighbor_h
    assert "uint8_t retry_count;" in neighbor_h
    assert "EIGRP_PACKET_RETRANS_TIME * 1000U" in neighbor_c
    assert "eigrp_neighbor_prefix_count(runtime, nbr)" in neighbor_c
    assert "nbr->retrans_queue->tail->retrans_counter" in neighbor_c
    assert "uint64_t eigrp_southbound_monotime_msec(void);" in southbound_h
    assert "uint64_t eigrp_southbound_monotime_msec(void)" in frr_southbound

    for label in ("Hold", "Uptime", "SRTT", "RTO", "Retrans:", "Retries:", "Prefixes:"):
        assert label in vty
    assert "TLV version %u, State: %s" not in vty


def test_timer_show_walks_actual_runtime_expirations():
    timer_c = read(TIMER_C)
    timer_h = read(TIMER_H)
    vty = read(VTY)

    assert "EIGRP_TIMER_STATE_HELLO" in timer_h
    assert "EIGRP_TIMER_STATE_PEER_HOLD" in timer_h
    assert "interface->t_hello" in timer_c
    assert "neighbor->t_holddown" in timer_c
    assert timer_c.count("eigrp_southbound_timer_remaining_seconds") >= 2
    assert "Hello interval" not in timer_c
    assert "hold_time_configured" not in timer_c
    assert "Peer holding" in vty
    assert "SIA process expiration: n/a until ACTIVE-time enforcement is implemented" in vty


def test_accounting_and_traffic_have_real_ipv4_backend_data():
    statistics = read(STATISTICS)
    vty = read(VTY)

    assert "eigrp_statistics_neighbor_prefix_count" in statistics
    assert "state.prefix_count = eigrp_statistics_neighbor_prefix_count" in statistics
    assert "Total Prefix Count:" in vty
    assert "Restart fields are n/a until neighbor maximum-prefix enforcement is implemented." in vty

    for field in (
        "sent_hello", "received_hello", "sent_update", "received_update",
        "sent_query", "received_query", "sent_reply", "received_reply",
        "sent_ack", "received_ack", "sent_sia_query", "received_sia_query",
        "sent_sia_reply", "received_sia_reply",
    ):
        assert f"state->{field}" in statistics


def test_multicast_exec_selector_is_retained_as_maf_not_transport_multicast():
    types = read(TYPES)
    instance = read(INSTANCE)
    vty = read(VTY)
    cli_spec = read(CLI_SPEC)

    # Cisco named EXEC grammar keeps the selector on the operational views.
    assert vty.count("[multicast]") >= 8
    assert "bool multicast;" in types
    assert vty.count("request.multicast = eigrp_vty_multicast_requested") >= 8

    # The portable instance target owns the current incomplete capability.
    multicast_guard = instance[
        instance.index("The CLI token selects EIGRP's Multicast Address Family"):
        instance.index("/* A normal show request", instance.index("The CLI token selects EIGRP's Multicast Address Family"))
    ]
    assert "VRID 0x0001" in multicast_guard
    assert "if (request->multicast)" in multicast_guard
    assert "return EIGRP_RESULT_NOT_IMPLEMENTED;" in multicast_guard

    # It must not be confused with normal EIGRP multicast packet transport.
    show_region = vty[vty.index("DEFPY(show_eigrp_interface,"):vty.index("struct eigrp_vty_protocol_show")]
    assert "multicast topology state" not in show_region
    assert "EIGRP_RESULT_UNSUPPORTED" not in show_region
    assert "Multicast Address Family (MAF)" in cli_spec
    assert "VRID `0x0001`" in cli_spec
    assert "project configuration/runtime model is unicast" in cli_spec
    assert "unsupported/not implemented" in cli_spec
