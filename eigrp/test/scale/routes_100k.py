#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""High-volume protocol scale tests using the portable Unix UUT and real EIGRP TLVs.

GitHub baseline: diivious/eigrp peer-scale.  Only test harness code is added.
No protocol shortcuts, host routing requirements, or thousands of UUT processes.
"""
from __future__ import annotations

import argparse
import collections
import ipaddress
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
NODE = ROOT / "eigrp/test/uut/eigrp-uut-node"
sys.path.insert(0, str(ROOT / "unix/test"))
from eigrp_wire import Broker, HDR, REGISTER, PACKET, MULTICAST, recv_frame

ASN = 4453
EIGRP_HDR = struct.Struct("!BBHIIIHH")
WATCHDOG = float(os.environ.get("EIGRP_SCALE_WATCHDOG_SEC", "1200"))
ROUTES = int(os.environ.get("EIGRP_SCALE_ROUTES", "100000"))
PEERS = int(os.environ.get("EIGRP_SCALE_PEERS", "2048"))
ROUTES_PER_PEER = int(os.environ.get("EIGRP_SCALE_ROUTES_PER_PEER", "50"))
BATCH_SIZE = int(os.environ.get("EIGRP_SCALE_BATCH", "24"))
# EIGRP header + classic route TLVs must fit a normal 1500-byte interface MTU.
assert 1 <= BATCH_SIZE <= 24


def checksum(data: bytes) -> int:
    if len(data) & 1:
        data += b"\0"
    value = sum(struct.unpack(f"!{len(data)//2}H", data))
    while value >> 16:
        value = (value & 0xffff) + (value >> 16)
    return (~value) & 0xffff


def packet(opcode: int, *, seq: int = 0, ack: int = 0, flags: int = 0, tlvs: bytes = b"") -> bytes:
    buf = bytearray(EIGRP_HDR.pack(2, opcode, 0, flags, seq, ack, 0, ASN) + tlvs)
    struct.pack_into("!H", buf, 2, checksum(buf))
    return bytes(buf)


PARAMETER = struct.pack("!HH6BH", 1, 12, 1, 0, 1, 0, 0, 0, 900)
# A classic TLV1-capable neighbor must advertise its software version; without
# it the UUT may keep the safe/no-op route decoder for an unnegotiated peer.
SW_VERSION = struct.pack("!HHBBBB", 4, 8, 1, 0, 1, 2)
HELLO = packet(5, tlvs=PARAMETER + SW_VERSION)


def route_tlv(route_number: int, external: bool) -> bytes:
    if route_number < 0 or route_number >= 2**24:
        raise ValueError("route ID outside dedicated 100/8 test prefix space")
    address = (100 << 24) + route_number
    # Classic EIGRP metrics: scaled delay, bandwidth, 24-bit MTU,
    # hop-count, reliability, load, tag, flags. One /32 destination per TLV.
    metric = struct.pack("!II", 2560, 2560) + (1500).to_bytes(3, "big") + bytes((0, 255, 1, 0, 0))
    ext = (struct.pack("!IIIIHBB", int(ipaddress.IPv4Address("10.250.0.2")),
                       ASN, 0, 1000, 0, 2, 0) if external else b"")
    body = b"\0" * 4 + ext + metric + bytes((32,)) + struct.pack("!I", address)
    return struct.pack("!HH", 0x0103 if external else 0x0102, 4 + len(body)) + body


def wire_frame(kind: int, payload: bytes, source: str, *, segment: str,
               interface: str = "eth0", ifindex: int = 999,
               destination: str = "224.0.0.10", multicast: bool = False) -> bytes:
    seg = segment.encode("ascii")
    iface = interface.encode("ascii")
    src = socket.inet_aton(source) + bytes(12)
    dst = socket.inet_aton(destination) + bytes(12)
    return (HDR.pack(1, kind, 4, MULTICAST if multicast else 0,
                     ifindex, len(seg), len(iface), len(payload), 0, src, dst)
            + seg + iface + payload)


class ScaleNode:
    """Streaming UUT control channel: never buffer 100K STATE records."""
    def __init__(self, name: str, config: Path, wire_path: Path):
        self.name = name
        env = dict(os.environ, EIGRP_UNIX_WIRE_SOCKET=str(wire_path))
        self.proc = subprocess.Popen([str(NODE), str(config)], env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1)
        self.responses: queue.Queue[str] = queue.Queue(maxsize=8192)
        self.errors = collections.deque(maxlen=80)
        self.control_lock = threading.Lock()
        threading.Thread(target=self._reader, args=(self.proc.stdout, self.responses), daemon=True).start()
        threading.Thread(target=self._error_reader, daemon=True).start()
        if self._response(min(WATCHDOG, 600)) != "READY":
            raise RuntimeError(f"{name}: UUT did not become READY: {list(self.errors)}")

    @staticmethod
    def _reader(stream, destination):
        for line in stream:
            destination.put(line.rstrip("\n"))

    def _error_reader(self):
        for line in self.proc.stderr:
            self.errors.append(line.rstrip("\n"))

    def _response(self, timeout):
        try:
            return self.responses.get(timeout=max(0.1, timeout))
        except queue.Empty:
            self.alive()
            raise AssertionError(f"{self.name}: UUT did not respond within {timeout:.1f}s")

    def alive(self):
        if self.proc.poll() is not None:
            raise AssertionError(f"{self.name}: UUT crashed (rc={self.proc.returncode}), stderr={list(self.errors)}")

    def state_counts(self, timeout=240):
        with self.control_lock:
            self.alive()
            self.proc.stdin.write("STATE\n")
            self.proc.stdin.flush()
            counts = collections.Counter()
            up = set()
            queue_counts = []
            deadline = time.monotonic() + timeout
            while True:
                if time.monotonic() >= deadline:
                    raise AssertionError(f"{self.name}: STATE streaming deadline exceeded")
                row = self._response(deadline - time.monotonic())
                if row == "END":
                    return counts, up, queue_counts
                kind, _, content = row.partition("|")
                counts[kind] += 1
                if kind == "NBR":
                    values = dict(part.split("=", 1) for part in content.split("|") if "=" in part)
                    if values.get("state", "").upper() == "UP":
                        up.add(values["address"])
                elif kind == "INTF":
                    values = dict(part.split("=", 1) for part in content.split("|") if "=" in part)
                    queue_counts.append((values.get("name"), int(values.get("output_queue", 0)),
                                         int(values.get("reliable_queue", 0))))

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write("STOP\n")
                self.proc.stdin.flush()
                self.proc.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.proc.kill()
                self.proc.wait(timeout=10)
        for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            stream.close()


class SyntheticLink:
    """Receives real UUT frames, ACKs its RTP traffic, and tracks our own ACKs.

    Direct mode uses one test-side socket listener (Test 1/2/5); broker mode
    registers one synthetic neighbor alongside real R1/R2/R3 UUT processes.
    """
    def __init__(self, path: Path, segment: str, peers: set[str], *, broker: bool):
        self.path, self.segment, self.peers, self.broker = path, segment, peers, broker
        self.sock = None
        self.listen_sock = None
        self.thread = None
        self.stop_event = threading.Event()
        self.ready = threading.Event()
        self.connected = threading.Event()
        self.write_lock = threading.Lock()
        self.acks = {}
        self.cv = threading.Condition()
        self.error = None
        self.received_wire = 0
        self.received_acks = 0
        self.sent_acks = 0
        self.seq_exclusions = set()
        self.hello_packets: dict[str, bytes] = {}
        self.held_peers: set[str] = set()
        self.held_acks: collections.deque[tuple[str, int]] = collections.deque()
        self.withheld_count = 0
        self.cr_hellos = 0
        self.max_frame_size = 0
        self.route_tlv_types = collections.Counter()
        self.route_wire_count = 0
        self.multicast_route_updates = 0
        self.ack_jobs: queue.Queue[tuple[str, int]] = queue.Queue(maxsize=100000)
        self.ack_thread = None
        self.hello_thread = None

    def start(self):
        if self.broker:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            # Retry until the broker has started listening.
            for _ in range(200):
                try:
                    self.sock.connect(str(self.path))
                    break
                except (OSError, ConnectionRefusedError):
                    time.sleep(0.02)
            else:
                raise RuntimeError("could not connect synthetic peer to broker")
            sole = next(iter(self.peers))
            self.sock.sendall(wire_frame(REGISTER, b"", sole, segment=self.segment))
            self.connected.set()
        else:
            self.listen_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.listen_sock.bind(str(self.path))
            self.listen_sock.listen(1)
            self.ready.set()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        self.ack_thread = threading.Thread(target=self._send_acks, daemon=True)
        self.ack_thread.start()

    def start_heartbeats(self):
        # Synthetic peers use a 900-second hold time. The stress watchdog may
        # exceed that, so keep existing adjacencies alive during route floods.
        def refresh():
            while not self.stop_event.wait(120):
                try:
                    for index, peer in enumerate(sorted(self.peers, key=ipaddress.IPv4Address)):
                        if self.stop_event.is_set():
                            return
                        self.transmit(self.hello_packets.get(peer, HELLO), peer)
                        if index % 64 == 63:
                            time.sleep(0.05)
                except Exception as exc:
                    if not self.stop_event.is_set():
                        self.error = exc
                    return
        self.hello_thread = threading.Thread(target=refresh, daemon=True)
        self.hello_thread.start()

    def hold_peer_acks(self, peers):
        with self.cv:
            self.held_peers.update(peers)

    def release_held_acks(self):
        # No protocol shortcut: release the actual ACK Hellos held by the test.
        with self.cv:
            self.held_peers.clear()
            pending = list(self.held_acks)
            self.held_acks.clear()
        for job in pending:
            self.ack_jobs.put(job, timeout=30)
        return len(pending)

    def wait_connect(self, timeout=15):
        if not self.connected.wait(timeout):
            raise AssertionError("synthetic wire did not connect")
        if self.error:
            raise RuntimeError("synthetic wire failed") from self.error

    def transmit(self, raw: bytes, peer: str):
        with self.write_lock:
            if self.error:
                raise RuntimeError("synthetic wire failed") from self.error
            if not self.sock:
                raise AssertionError("synthetic wire socket absent")
            self.sock.sendall(wire_frame(PACKET, raw, peer, segment=self.segment,
                                         ifindex=999 if self.broker else 101,
                                         destination="10.250.0.1" if self.broker else "10.0.0.1"))

    def _send_acks(self):
        while not self.stop_event.is_set():
            try:
                peer, seq = self.ack_jobs.get(timeout=0.25)
            except queue.Empty:
                continue
            try:
                with self.cv:
                    if peer in self.held_peers:
                        self.held_acks.append((peer, seq))
                        self.withheld_count += 1
                        self.cv.notify_all()
                        continue
                self.transmit(packet(5, ack=seq), peer)
                self.sent_acks += 1
            except Exception as exc:
                if not self.stop_event.is_set():
                    self.error = exc
                break

    @staticmethod
    def _excluded(payload: bytes):
        out = set()
        offset = 20
        while offset + 4 <= len(payload):
            typ, length = struct.unpack_from("!HH", payload, offset)
            if length < 4 or offset + length > len(payload):
                break
            if typ == 3:  # RFC 7868 SEQUENCE TLV
                p = offset + 4
                while p < offset + length:
                    n = payload[p]
                    p += 1
                    if n == 4 and p + n <= offset + length:
                        out.add(socket.inet_ntoa(payload[p:p+4]))
                    p += n
            offset += length
        return out

    def _run(self):
        try:
            if not self.broker:
                self.sock, _ = self.listen_sock.accept()
                self.connected.set()
            while not self.stop_event.is_set():
                frame = recv_frame(self.sock)
                if not frame:
                    if not self.stop_event.is_set():
                        raise ConnectionError("UUT or broker closed the virtual wire")
                    break
                _, typ, afi, flags, _, sl, il, pl, _, src, dst = HDR.unpack_from(frame)
                if typ != PACKET or afi != 4 or pl < 20:
                    continue
                payload = frame[HDR.size + sl + il:]
                self.received_wire += 1
                self.max_frame_size = max(self.max_frame_size, len(payload))
                if payload[1] == 1:
                    self.route_wire_count += 1
                    if flags & MULTICAST:
                        self.multicast_route_updates += 1
                    pos = 20
                    while pos + 4 <= len(payload):
                        kind, length = struct.unpack_from("!HH", payload, pos)
                        if length < 4 or pos + length > len(payload):
                            break
                        if kind in (0x0102, 0x0103, 0x0602, 0x0603):
                            self.route_tlv_types[kind] += 1
                        pos += length
                opcode = payload[1]
                seq = struct.unpack_from("!I", payload, 8)[0]
                ack = struct.unpack_from("!I", payload, 12)[0]
                recipient = socket.inet_ntoa(dst[:4])
                if opcode == 5 and not ack:
                    exclusions = self._excluded(payload)
                    if exclusions:
                        self.seq_exclusions = exclusions
                        self.cr_hellos += 1
                if ack and recipient in self.peers:
                    with self.cv:
                        self.acks[recipient] = ack
                        self.received_acks += 1
                        self.cv.notify_all()
                if seq:
                    # R1 may send multicast reliable UPDATEs to the whole LAN.
                    # Honor the preceding CR SEQUENCE Hello's exclusions.
                    if flags & MULTICAST:
                        recipients = self.peers - (self.seq_exclusions if struct.unpack_from("!I", payload, 4)[0] & 0x2 else set())
                        for peer in recipients:
                            self.ack_jobs.put((peer, seq), timeout=30)
                        self.seq_exclusions = set()
                    elif recipient in self.peers:
                        # Retransmitted packets must be ACKed again, even if
                        # our previous ACK was lost in the test-side wire.
                        self.ack_jobs.put((recipient, seq), timeout=30)
        except Exception as exc:
            if not self.stop_event.is_set():
                self.error = exc
                with self.cv:
                    self.cv.notify_all()

    def wait_ack(self, peer: str, seq: int, deadline: float):
        with self.cv:
            while self.acks.get(peer) != seq:
                if self.error:
                    raise RuntimeError(f"synthetic wire error: {self.error}") from self.error
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(f"{peer}: UPDATE seq={seq} unacknowledged")
                self.cv.wait(min(0.5, remaining))

    def close(self):
        self.stop_event.set()
        for sock in (self.sock, self.listen_sock):
            if sock:
                try:
                    sock.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                sock.close()
        if self.thread:
            self.thread.join(timeout=2)
        if self.ack_thread:
            self.ack_thread.join(timeout=2)
        if self.hello_thread:
            self.hello_thread.join(timeout=2)


def config_for(name, interfaces):
    lines = [f"router {name} {ASN}", f"router-id 10.255.0.{int(name[1:])}"]
    for index, (iface, segment, address) in enumerate(interfaces, 1):
        lines.extend((f"interface {iface} {100*int(name[1:])+index} 1000000 1500",
                      f"address {iface} {address}", f"segment {iface} {segment}",
                      f"network {ipaddress.ip_interface(address).network}", f"up {iface}"))
    return "\n".join(lines) + "\n"


def topology(test: int):
    if test in (1, 2, 5):
        return {"r1": [("eth0", "inject", "10.0.0.1/16")]}
    r1 = [("eth0", "inject", "10.250.0.1/24"),
          ("eth12", "r1-r2", "10.12.0.1/24")]
    r2 = [("eth21", "r1-r2", "10.12.0.2/24"),
          ("eth23", "r2-r3", "10.23.0.2/24")]
    r3 = [("eth32", "r2-r3", "10.23.0.3/24")]
    if test == 4:
        # Kite triangle: R1 tail/source, R1-R2-R3 form a loop.
        r1.append(("eth13", "r1-r3", "10.13.0.1/24"))
        r3.append(("eth31", "r1-r3", "10.13.0.3/24"))
    return {"r1": r1, "r2": r2, "r3": r3}


def state_until(node: ScaleNode, expected: int, deadline: float, label: str):
    last = None
    while time.monotonic() < deadline:
        node.alive()
        state = node.state_counts(timeout=min(240, max(5, deadline - time.monotonic())))
        counts, up, queues = state
        last = (counts, len(up), queues)
        if counts["TOPO"] >= expected:
            print(f"  {label}: topology={counts['TOPO']} rib={counts['RIB']} "
                  f"neighbors_up={len(up)} output_queues={queues}", flush=True)
            return state
        print(f"  {label}: awaiting routes, topology={counts['TOPO']}/{expected}", flush=True)
        time.sleep(2)
    raise AssertionError(f"{label}: route propagation stalled: last={last}")


def inject(wire: SyntheticLink, peers: list[str], *, route_count: int,
           pattern: str, deadline: float):
    total_sent = 0
    seqs = {peer: (i * 100000 + 1) for i, peer in enumerate(peers)}
    per_peer = (route_count // len(peers)) if pattern == "peer-mix" else route_count
    assert per_peer * len(peers) == route_count
    # Keep a single reliable packet outstanding per synthetic neighbor. This
    # is sender-side RTP discipline, not EIGRP TRANSMIT's queue-depth guard.
    for index in range(0, per_peer, BATCH_SIZE):
        n = min(BATCH_SIZE, per_peer-index)
        for pi, peer in enumerate(peers):
            start = pi*per_peer + index if pattern == "peer-mix" else index
            payload = b"".join(route_tlv(start + j, pattern == "external" or
                                         (pattern in ("mixed", "peer-mix") and (start+j) % 2 == 1))
                               for j in range(n))
            seq = seqs[peer]
            wire.transmit(packet(1, seq=seq, tlvs=payload), peer)
            wire.wait_ack(peer, seq, min(deadline, time.monotonic() + 120))
            seqs[peer] += 1
            total_sent += n
        if index % (BATCH_SIZE * 32) == 0:
            print(f"  inject {pattern}: {total_sent}/{route_count} routes", flush=True)
        if time.monotonic() > deadline:
            raise AssertionError("route-injection watchdog expired")
    return total_sent


def scenario(number: int):
    names = {1: "100000-internal", 2: "100000-external", 3: "line-mixed",
             4: "kite-mixed", 5: "2048-peers-50-routes"}
    label = names[number]
    needed = PEERS * ROUTES_PER_PEER if number == 5 else ROUTES
    if needed <= 0:
        raise ValueError("route count must be positive")
    if number == 5 and not (1 <= PEERS <= 2048 and ROUTES_PER_PEER > 0):
        raise ValueError("peer test requires 1..2048 peers and positive routes per peer")
    if needed >= 2**24:
        raise ValueError("too many unique /32 routes for 100/8 reserved test range")
    nodes = {}
    broker = None
    wire = None
    t0 = time.monotonic()
    deadline = t0 + WATCHDOG
    with tempfile.TemporaryDirectory(prefix=f"eigrp-scale-{number}-") as tmp:
        path = Path(tmp)
        cfgs = topology(number)
        single = number in (1, 2, 5)
        sock = path / "wire.sock"
        peers = ([str(ipaddress.IPv4Address(0x0a000002 + i)) for i in range(PEERS)]
                 if number == 5 else ["10.250.0.2" if not single else "10.0.0.2"])
        try:
            if not single:
                owners = {(4, ipaddress.ip_interface(addr).ip.packed): name
                          for name, ifs in cfgs.items() for _, _, addr in ifs}
                owners[(4, socket.inet_aton(peers[0]))] = "injection-peer"
                broker = Broker(str(sock), owners)
                # The regular scenario packet journal is intentionally not
                # unbounded in a high-volume stress test.
                broker.journal = collections.deque(maxlen=8192)
                broker.start()
                assert broker.ready.wait(10) and broker.error is None
            wire = SyntheticLink(sock, "inject", set(peers), broker=not single)
            wire.start()
            for name, ifs in cfgs.items():
                conf = path / f"{name}.conf"
                conf.write_text(config_for(name, ifs))
                nodes[name] = ScaleNode(name, conf, sock)
            wire.wait_connect()
            assert not broker or not broker.error
            # Bring up the complete real topology before route injection.
            # The AF input queue has a finite discovery capacity. Register
            # peers in batches, as the existing neighbor_2048 test does.
            for offset in range(0, len(peers), 64):
                group = peers[offset:offset + 64]
                for peer in group:
                    wire.transmit(HELLO, peer)
                target = offset + len(group)
                while time.monotonic() < deadline:
                    counts, up, _ = nodes["r1"].state_counts(80)
                    if len(up) >= target:
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError(f"R1 failed to establish synthetic peers {target}/{len(peers)}")
            wire.start_heartbeats()
            if not single:
                for name, expected in (("r1", 3 if number == 4 else 2),
                                       ("r2", 2 if number == 4 else 2),
                                       ("r3", 2 if number == 4 else 1)):
                    # R1 has injection-peer plus R2 (+R3 in the kite).
                    while time.monotonic() < deadline:
                        counts, up, _ = nodes[name].state_counts(80)
                        if len(up) >= expected:
                            break
                        time.sleep(0.2)
                    else:
                        raise AssertionError(f"{name}: missing EIGRP adjacencies")
            pattern = "internal" if number == 1 else "external" if number == 2 else "peer-mix" if number == 5 else "mixed"
            delivered = inject(wire, peers, route_count=needed, pattern=pattern, deadline=deadline)
            print(f"  received UPDATE acknowledgments={wire.received_acks} "
                  f"wire frames={wire.received_wire} response ACKs={wire.sent_acks}", flush=True)
            # An advertised route may take time to propagate through the R1/R2/R3
            # network. Count topology records via streaming, not state buffering.
            expected_peers = ({"r1": PEERS} if number == 5 else
                              {"r1": 1} if single else
                              {"r1": 3, "r2": 2, "r3": 2} if number == 4 else
                              {"r1": 2, "r2": 2, "r3": 1})
            for name, node in nodes.items():
                counts, up, queues = state_until(node, needed, deadline, name)
                if len(up) < expected_peers[name]:
                    raise AssertionError(f"{name}: adjacency loss under stress, UP={len(up)}, "
                                         f"expected={expected_peers[name]}")
            for name, node in nodes.items():
                node.alive()
            if wire.error or (broker and broker.error):
                raise AssertionError(f"wire failure: synthetic={wire.error}, broker={broker.error if broker else None}")
            print(f"PASS scale/{label}: injected={delivered}, verified_topology>= {needed} "
                  f"on {','.join(nodes)}, elapsed={time.monotonic()-t0:.3f}s "
                  "(informational, no performance threshold)", flush=True)
        finally:
            if wire:
                wire.close()
            for node in reversed(list(nodes.values())):
                node.stop()
            if broker:
                broker.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test", type=int, choices=range(1, 6),
                        help="1=internal, 2=external, 3=line, 4=kite, 5=2048 peers")
    opts = parser.parse_args()
    if not NODE.is_file():
        raise SystemExit(f"Missing {NODE}; run `make build && make -C eigrp/test/uut eigrp-uut-node`")
    print(f"START scale test {opts.test}: watchdog={WATCHDOG}s", flush=True)
    scenario(opts.test)


if __name__ == "__main__":
    main()
