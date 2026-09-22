# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for the packetizer/RTP pipeline in rtp-spec.md.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


def read(relative: str) -> str:
    return (ROOT / relative).read_text()


def test_rtp_uses_one_ordered_reliable_queue_per_neighbor():
    neighbor = read("eigrpd/eigrp_neighbor.h")
    packet = read("eigrpd/eigrp_packet.c")

    assert "eigrp_packet_queue_t *retrans_queue;" in neighbor
    assert "multicast_queue" not in neighbor
    assert "eigrp_packet_unack_multicast_retrans" not in packet
    assert "eigrp_packet_multicast_reliable_enqueue" in packet


def test_hello_opcode_ack_is_consumed_by_rtp_without_creating_neighbor():
    packet = read("eigrpd/eigrp_packet.c")
    hello_path = packet[
        packet.index("if (opcode == EIGRP_OPC_HELLO)") :
        packet.index("/* A neighbor must exist before accepting non-Hello packets. */")
    ]

    assert "if (ntohl(eigrph->ack))" in hello_path
    assert "eigrp_packet_ack(eigrp, eigrph, nbr);" in hello_path
    assert hello_path.index("eigrp_packet_ack(eigrp, eigrph, nbr);") < hello_path.index(
        "eigrp_hello_receive(eigrp, eigrph"
    )


def test_reliable_wire_image_is_not_reencoded_or_rewritten_on_send():
    packet = read("eigrpd/eigrp_packet.c")
    write = packet[
        packet.index("void eigrp_packet_write(void *arg)") :
        packet.index("/* Starting point of packet process function. */")
    ]

    assert "eigrph->ack =" not in write
    assert "eigrph->checksum =" not in write
    assert "eigrp_make_md5_digest" not in write
    assert "stream_copy(new->s, old->s);" in packet


def test_packetizer_batches_complete_route_tlvs_and_owns_topology_update_path():
    packetizer = read("eigrpd/eigrp_packetizer.c")
    update = read("eigrpd/eigrp_update.c")
    packet = read("eigrpd/eigrp_packet.c")

    assert "eigrp_packet_route_encode_append" in packetizer
    assert "The next complete TLV does not fit" in packetizer
    assert "eigrp_packet_multicast_reliable_enqueue" in packetizer
    assert "eigrp_update_packetize_all" not in packetizer
    assert "eigrp_update_packetize_all" not in update
    assert "stream_get_endp(pkt) + encoded > packet_limit" in packet


def test_query_all_queues_one_batchable_work_bead():
    query = read("eigrpd/eigrp_query.c")

    start = query.index("uint32_t eigrp_query_send_all")
    end = query.index("/*EIGRP QUERY read function*/", start)
    send_all = query[start:end]
    assert send_all.count("eigrp_packetizer_work_new(EIGRP_OPC_QUERY)") == 1
    assert "work->prefix = prefix" not in send_all


def test_conditional_receive_is_transport_state_tied_to_multicast_sequence():
    neighbor = read("eigrpd/eigrp_neighbor.h")
    hello = read("eigrpd/eigrp_hello.c")
    packet = read("eigrpd/eigrp_packet.c")
    update = read("eigrpd/eigrp_update.c")

    assert "bool cr_mode;" in neighbor
    assert "uint32_t cr_sequence;" in neighbor
    assert "EIGRP_TLV_SEQ" in hello
    assert "EIGRP_TLV_NEXT_MCAST_SEQ" in hello
    assert "meta.destination_multicast" in packet
    assert "nbr->cr_sequence != sequence" in packet
    assert "if (flags & EIGRP_CR_FLAG)" not in update
