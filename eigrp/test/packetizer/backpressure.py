# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Regression guards for RTP-driven, bounded initial-sync packetization.

The protocol UUT scenarios remain responsible for wire reliability and DUAL.
The guards pin the cross-module backpressure/ACK/cancellation wiring so a
future refactor cannot silently turn EOT back into an eager table walk.
"""
from pathlib import Path

ROOT = next(parent for parent in Path(__file__).resolve().parents
            if (parent / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"


def read(name):
    return (CODE / name).read_text()


def test_initial_sync_has_bounded_queue_and_yields_without_idle_pauses():
    source = read("eigrp_update.c")
    assert "EIGRP_UPDATE_SYNC_RELIABLE_MAX 4U" in source
    assert "EIGRP_UPDATE_SYNC_PREFIX_BATCH 128U" in source
    assert "nbr->retrans_queue->count >= EIGRP_UPDATE_SYNC_RELIABLE_MAX" in source
    assert "nbr->sync_cursor = rn;" in source
    assert "eigrp_update_sync_timer, nbr, 0" in source
    assert "sync_packet = packet" in source
    assert "EIGRP_EOT_FLAG" in source
    assert source.index("nbr->sync_active = false;") < source.index(
        "htonl(EIGRP_EOT_FLAG);", source.index("void eigrp_update_sync_continue"))


def test_ack_replenishes_only_after_rtp_head_advances():
    source = read("eigrp_packet.c")
    start = source.index("static void eigrp_packet_ack(")
    end = source.index("static void eigrp_packet_reliable_neighbor_send_update", start)
    ack = source[start:end]
    assert "packet = eigrp_packet_dequeue(nbr->retrans_queue)" in ack
    assert "eigrp_packet_send_reliably(eigrp, nbr);" in ack
    assert "eigrp_update_sync_continue(nbr);" in ack
    assert ack.index("packet = eigrp_packet_dequeue") < ack.index(
        "eigrp_update_sync_continue(nbr);")
    assert ack.index("eigrp_packet_send_reliably(eigrp, nbr);") < ack.index(
        "eigrp_update_sync_continue(nbr);")


def test_neighbor_down_and_teardown_cancel_sync_producer():
    neighbor = read("eigrp_neighbor.c")
    assert neighbor.count("eigrp_update_sync_cancel(nbr);") >= 1
    assert "old_state == EIGRP_NEIGHBOR_UP && state != EIGRP_NEIGHBOR_UP" in neighbor
    sync = read("eigrp_update.c")
    assert "eigrp_timer_cancel(nbr->ei ? nbr->ei->eigrp : NULL, &nbr->t_sync)" in sync
    assert "eigrp_packet_free(nbr->sync_packet)" in sync
    assert "free(nbr->sync_summary_seen)" in sync


def test_persistent_sync_state_and_allocation_failure_are_recoverable():
    nbr = read("eigrp_neighbor.h")
    assert "eigrp_table_node_t *sync_cursor;" in nbr
    assert "eigrp_packet_t *sync_packet;" in nbr
    assert "eigrp_prefix_t *sync_summary_seen;" in nbr
    source = read("eigrp_update.c")
    assert "goto blocked_alloc;" in source
    assert "nbr->sync_summary_count = summary_before;" in source
    assert "EIGRP_UPDATE_SYNC_ALLOC_RETRY_MSEC" in source


def test_multicast_hellos_and_ack_bypass_data_pacing():
    packet = read("eigrp_packet.c")
    hello = read("eigrp_hello.c")
    writer = packet[packet.index("void eigrp_packet_write(void *arg)"):
                    packet.index("/* Starting point of packet process function. */")]
    assert "pending->length, now);" in writer
    assert "EIGRP_OPC_HELLO" in writer
    assert "wait = 0;" in writer
    assert writer.index("EIGRP_OPC_HELLO") < writer.index("eigrp_packet_pacing_delay_msec(pending_ei,")
    wire = packet[packet.index("static void eigrp_packet_wire_send("):
                  packet.index("void eigrp_packet_write(void *arg)")]
    assert "eigrp_packet_pacing_charge(ei, packet->length);" in wire
    assert "eigrp_packet_wire_send(ei, packet);" in writer
    assert "eigrp_packet_output_enqueue(nbr->ei->eigrp, nbr->ei, packet);" in hello
    assert "eigrp_packet_output_enqueue(ei->eigrp, ei, packet);" in hello


def test_cr_sequence_hello_keeps_transport_fifo_order():
    hello = read("eigrp_hello.c")
    fn = hello[hello.index("bool eigrp_hello_send_sequence("):]
    assert "eigrp_packet_enqueue(ei->obuf, packet);" in fn
    assert "eigrp_packet_write_schedule(ei->eigrp);" in fn
