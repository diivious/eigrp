# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for EIGRP per-neighbor RTP SRTT/RTO behavior.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
NEIGHBOR_C = ROOT / "eigrpd" / "eigrp_neighbor.c"
PACKET = ROOT / "eigrpd" / "eigrp_packet.c"
NEIGHBOR = ROOT / "eigrpd" / "eigrp_neighbor.h"
CONSTANTS = ROOT / "eigrpd" / "eigrp_const.h"


def read(path: Path) -> str:
    return path.read_text()


def test_neighbor_owns_independent_srtt_variance_and_rto_state():
    neighbor = read(NEIGHBOR)

    assert "bool srtt_valid;" in neighbor
    assert "uint32_t srtt_msec;" in neighbor
    assert "uint32_t rttvar_msec;" in neighbor
    assert "uint32_t rto_msec;" in neighbor


def test_transport_uses_eigrp_rto_bounds_and_six_times_srtt():
    constants = read(CONSTANTS)
    neighbor_c = read(NEIGHBOR_C)

    assert "EIGRP_TRANSPORT_RTO_INITIAL_MSEC 2000U" in constants
    assert "EIGRP_TRANSPORT_RTO_MIN_MSEC 200U" in constants
    assert "EIGRP_TRANSPORT_RTO_MAX_MSEC 5000U" in constants
    assert "EIGRP_TRANSPORT_RTO_SRTT_MULTIPLIER 6U" in constants
    assert "srtt_msec * EIGRP_TRANSPORT_RTO_SRTT_MULTIPLIER" in neighbor_c
    assert "EIGRP_TRANSPORT_RTO_MIN_MSEC" in neighbor_c
    assert "EIGRP_TRANSPORT_RTO_MAX_MSEC" in neighbor_c


def test_first_and_later_samples_follow_jacobson_smoothing_order():
    neighbor_c = read(NEIGHBOR_C)

    assert "nbr->srtt_msec = sample_msec;" in neighbor_c
    assert "nbr->rttvar_msec = sample_msec / 2U;" in neighbor_c
    assert "EIGRP_NEIGHBOR_SRTT_ALPHA_SHIFT 3U" in neighbor_c
    assert "EIGRP_NEIGHBOR_RTTVAR_BETA_SHIFT 2U" in neighbor_c

    variance_update = neighbor_c.index("nbr->rttvar_msec = (uint32_t)rttvar;")
    srtt_update = neighbor_c.index("nbr->srtt_msec = (uint32_t)srtt;")
    assert variance_update < srtt_update


def test_karn_rule_rejects_retransmitted_packets_and_samples_successful_send_time():
    neighbor_c = read(NEIGHBOR_C)
    packet = read(PACKET)

    assert "packet->retrans_counter != 0" in neighbor_c
    assert "packet->sent_msec == 0" in neighbor_c
    assert "now_msec - packet->sent_msec" in neighbor_c
    assert "if (ret >= 0)" in packet
    assert "queued->sent_msec = now_msec;" in packet
    assert "eigrp_packet_reliable_send_record(ei, packet);" in packet
    assert "for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, node, nbr))" in packet
    assert "packet->sequence_number, now_msec" in packet
    assert "packet->sequence_number == 0 || packet->retransmission" in packet
    assert "eigrp_packet_retransmit_timer_start(nbr);" in packet


def test_retransmission_timer_uses_neighbor_rto_and_backs_off_to_protocol_ceiling():
    neighbor_c = read(NEIGHBOR_C)
    packet = read(PACKET)

    assert "eigrp_neighbor_rto_backoff(nbr);" in packet
    assert "eigrp_southbound_timer_msec_add" in packet
    assert "eigrp_neighbor_rto_get(nbr)" in packet
    assert "rto *= 2U;" in neighbor_c
    assert "EIGRP_TRANSPORT_RTO_MAX_MSEC" in neighbor_c


def test_retry_limit_allows_sixteen_retransmissions_then_drops_neighbor():
    packet = read(PACKET)

    assert "packet->retrans_counter >= EIGRP_TRANSPORT_RETRANS_MAX" in packet
    assert "is down: retry limit exceeded" in packet
    assert "eigrp_nbr_delete(nbr);" in packet

    unicast = packet[
        packet.index("void eigrp_packet_unack_retrans(void *arg)") :
        packet.index("eigrp_packet_t *eigrp_packet_dequeue", packet.index("void eigrp_packet_unack_retrans(void *arg)"))
    ]
    assert unicast.index("packet->retrans_counter >= EIGRP_TRANSPORT_RETRANS_MAX") < unicast.index(
        "duplicate = eigrp_packet_duplicate(packet, nbr);"
    )
    assert "eigrp_packet_unack_multicast_retrans" not in packet
