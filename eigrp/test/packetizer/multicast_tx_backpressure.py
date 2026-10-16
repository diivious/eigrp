# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Source boundary regressions; RTP/CR behavior is exercised by the UUT."""
from pathlib import Path

CODE = Path(__file__).resolve().parents[2] / "code"

def source(name):
    return (CODE / name).read_text()


def test_multicast_handoff_retains_packet_without_eager_peer_fanout():
    packet = source("eigrp_packet.c")
    start = packet.index("bool eigrp_packet_multicast_reliable_enqueue(")
    end = packet.index("void eigrp_packet_multicast_resume(", start)
    handoff = packet[start:end]
    assert "eigrp_packet_enqueue(&ei->multicast_pending, packet);" in handoff
    assert "eigrp_packet_dup" not in handoff
    assert "eigrp_packet_multicast_resume(ei);" in handoff


def test_multicast_rtp_admission_is_transactional_and_bounded():
    packet = source("eigrp_packet.c")
    admit = packet[packet.index("static bool eigrp_packet_multicast_admit(eigrp_intf_t *ei)\n{"):]
    assert "EIGRP_MULTICAST_RELIABLE_MAX 4U" in packet
    assert "eigrp_hello_sequence_fits(ei," in admit
    assert "eigrp_packet_dup(packet, nbr)" in admit
    assert admit.index("copies[i] = eigrp_packet_dup") < admit.index("eigrp_packet_reliable_ordered_enqueue(receivers[i]->retrans_queue")
    assert admit.index("eigrp_hello_send_sequence(ei, packet->sequence_number,") < admit.index("eigrp_packet_output_enqueue(eigrp, ei, packet)")
    assert admit.rindex("(void)eigrp_packet_dequeue(&ei->multicast_pending);") > admit.index("eigrp_packet_reliable_ordered_enqueue(receivers[i]->retrans_queue")


def test_backoff_drains_or_waits_for_ack_and_hellos_remain_independent():
    packet = source("eigrp_packet.c")
    hello = source("eigrp_hello.c")
    neighbor = source("eigrp_neighbor.c")
    interface = source("eigrp_interface.c")
    assert "ei->multicast_backoff = true;" in packet
    assert "ei->multicast_waiting = true;" in packet
    assert "eigrp_packet_multicast_resume(nbr->ei);" in packet
    assert "eigrp_packet_multicast_resume(nbr->ei);" in neighbor
    assert "eigrp_timer_add(eigrp, &ei->t_multicast_retry" in packet
    assert "eigrp_packet_queue_clear(&ei->multicast_pending);" in interface
    assert "eigrp_packet_output_enqueue(ei->eigrp, ei, packet);" in hello
    assert "eigrp_packet_output_enqueue(nbr->ei->eigrp, nbr->ei, packet);" in hello


def test_cr_hello_capacity_checks_all_busy_receivers():
    hello = source("eigrp_hello.c")
    start = hello.index("bool eigrp_hello_sequence_fits(")
    end = hello.index("static uint16_t eigrp_sequence_encode(", start)
    capacity = hello[start:end]
    assert "EIGRP_AUTH_MD5_TLV_SIZE" in capacity
    assert "EIGRP_AUTH_SHA256_TLV_SIZE" in capacity
    assert "eigrp_packet_reliable_prior_count(nbr, sequence)" in capacity
    assert "eigrp_packet_payload_limit(ei->curr_mtu)" in capacity
    assert "EIGRP_NEXT_SEQUENCE_TLV_SIZE" in capacity


def test_snapshot_adjacency_epoch_and_ordered_rtp_handoff():
    packet = source("eigrp_packet.c")
    nbr = source("eigrp_neighbor.c")
    hello = source("eigrp_hello.c")
    assert "packet->multicast_epoch = ei->multicast_epoch" in packet
    assert "nbr->multicast_epoch = nbr->ei->multicast_epoch" in nbr
    assert "eigrp_packet_multicast_peer_eligible(nbr," in packet
    assert "eigrp_packet_reliable_ordered_enqueue(receivers[i]->retrans_queue," in packet
    assert "eigrp_packet_unicast_behind_multicast(nbr," in packet
    assert "eigrp_packet_reliable_prior_count(nbr, sequence)" in hello


def test_allocation_failure_does_not_strand_reliable_head():
    packet = source("eigrp_packet.c")
    start = packet.index("void eigrp_packet_unack_retrans(void *arg)\n{")
    part = packet[start:]
    assert "eigrp_packet_allocation_retry(nbr);" in part
    assert part.index("eigrp_packet_dup(packet, nbr)") < part.index("eigrp_nbr_rto_backoff(nbr)")
