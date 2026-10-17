# SPDX-License-Identifier: ISC
"""Fast generator checks. Heavy scenarios run explicitly via scale/Makefile."""
import importlib.util
import collections
import queue
import threading
from pathlib import Path
import socket
import struct

path = Path(__file__).with_name("routes_100k.py")
spec = importlib.util.spec_from_file_location("eigrp_routes_100k", path)
scale = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scale)


def test_internal_wire_tlv():
    tlv = scale.route_tlv(0x12345, False)
    typ, length = struct.unpack_from("!HH", tlv)
    assert typ == 0x0102 and length == len(tlv) == 29
    assert tlv[-5] == 32
    assert socket.inet_ntoa(tlv[-4:]) == "100.1.35.69"


def test_external_wire_tlv():
    tlv = scale.route_tlv(12345, True)
    typ, length = struct.unpack_from("!HH", tlv)
    assert typ == 0x0103 and length == len(tlv) == 49
    assert struct.unpack_from("!I", tlv, 12)[0] == scale.ASN
    assert tlv[-5] == 32


def test_update_packet_checksum_mtu():
    encoded = b"".join(scale.route_tlv(i, i % 2 == 0) for i in range(24))
    pkt = scale.packet(1, seq=123, tlvs=encoded)
    assert len(pkt) <= 1500
    assert scale.checksum(pkt) == 0
    assert struct.unpack_from("!I", pkt, 8)[0] == 123


def test_discovery_hello_advertises_tlv1():
    assert scale.checksum(scale.HELLO) == 0
    assert (1).to_bytes(2, "big") in scale.HELLO
    assert scale.SW_VERSION in scale.HELLO


def test_kite_has_two_paths_from_r1_to_r3():
    link = scale.topology(4)
    assert len(link["r1"]) == 3
    assert {i[1] for i in link["r3"]} == {"r1-r3", "r2-r3"}
    assert {i[1] for i in scale.topology(3)["r3"]} == {"r2-r3"}


def test_peer_route_space_is_disjoint():
    # Same 2048 x 50 dimensions as the test. No overlapping /32 routes.
    boundaries = {(i * 50, i * 50 + 49) for i in range(2048)}
    assert len(boundaries) == 2048
    assert max(end for _, end in boundaries) == 102399


def test_cr_sequence_exclusions():
    peer = socket.inet_aton("10.0.0.7")
    exclusion = struct.pack("!HHB", 3, 9, 4) + peer
    hello = scale.packet(5, tlvs=scale.PARAMETER + exclusion)
    assert scale.SyntheticLink._excluded(hello) == {"10.0.0.7"}


def test_odd_length_update_checksum_preserves_tlv_boundary():
    tlv = scale.route_tlv(1, False)  # 29 bytes, odd TLV and packet length
    pkt = scale.packet(1, seq=37, tlvs=tlv)
    assert len(pkt) == scale.EIGRP_HDR.size + len(tlv)
    assert scale.checksum(pkt) == 0
    payload = bytearray(pkt)
    assert scale.checksum(payload) == 0
    assert bytes(payload) == pkt  # checksum verification cannot alter bytes


def test_synthetic_rtp_sender_retransmits_same_sequence():
    # Exercise the RTP discipline without a UUT or a real-time wait.
    link = scale.SyntheticLink.__new__(scale.SyntheticLink)
    sent = []
    link.retransmitted_updates = 0
    link.transmit = lambda raw, peer: sent.append((raw, peer))
    link.wait_ack = lambda peer, seq, deadline: len(sent) == 2
    link.send_reliable_update(b"same-wire-packet", "10.0.0.2", 101,
                              scale.time.monotonic() + 30)
    assert sent == [(b"same-wire-packet", "10.0.0.2")] * 2
    assert link.retransmitted_updates == 1


def test_synthetic_ack_pacing_is_opt_in(monkeypatch):
    link = scale.SyntheticLink.__new__(scale.SyntheticLink)
    link.stop_event = threading.Event()
    link.ack_jobs = queue.Queue()
    link.cv = threading.Condition()
    link.held_peers = set()
    link.held_acks = collections.deque()
    link.withheld_count = 0
    link.error = None
    link.sent_acks = 0
    link.ack_spacing_seconds = 0.001
    sends = []
    delays = []

    def send(raw, peer):
        sends.append((raw, peer))
        link.stop_event.set()

    link.transmit = send
    monkeypatch.setattr(scale.time, "sleep", delays.append)
    link.ack_jobs.put(("10.0.0.2", 47))
    link._send_acks()

    assert len(sends) == 1
    assert sends[0][1] == "10.0.0.2"
    assert struct.unpack_from("!I", sends[0][0], 12)[0] == 47
    assert scale.checksum(sends[0][0]) == 0
    assert link.sent_acks == 1
    assert delays == [0.001]
