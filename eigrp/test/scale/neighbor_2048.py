#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""One real R1, 2048 synthetic EIGRP neighbors over the Unix virtual wire.

Assertions are protocol/state based; elapsed time is printed for comparison on
any developer machine and is NOT compared against a performance threshold.
The watchdog is only a deadlock/unbounded-test safeguard (env-configurable).
"""
from __future__ import annotations

import ipaddress
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[3]
NODE = ROOT / "eigrp" / "test" / "uut" / "eigrp-uut-node"
PEERS = 2048
AS = 4453
IFINDEX = 101
INTERFACE = "eth0"
SEGMENT = "scale-lan"
FRAME = struct.Struct("!BBBBIHHHH16s16s")


def checksum(payload: bytes) -> int:
    if len(payload) & 1:
        payload += b"\0"
    value = sum(struct.unpack(f"!{len(payload)//2}H", payload))
    while value >> 16:
        value = (value & 0xFFFF) + (value >> 16)
    return (~value) & 0xFFFF


def eigrp_packet(opcode: int, *, ack: int = 0, parameter: bool = False) -> bytes:
    data = struct.pack("!BBHIIIHH", 2, opcode, 0, 0, 0, ack, 0, AS)
    if parameter:
        # K1=1, K3=1, K2/K4/K5/K6=0, 900-second test hold time.
        data += struct.pack("!HH6BH", 1, 12, 1, 0, 1, 0, 0, 0, 900)
    result = bytearray(data)
    result[2:4] = struct.pack("!H", checksum(result))
    return bytes(result)


HELLO = eigrp_packet(5, parameter=True)


def frame(payload: bytes, source: str, destination: str = "10.0.0.1") -> bytes:
    sn = SEGMENT.encode("ascii")
    iface = INTERFACE.encode("ascii")
    src = socket.inet_aton(source) + bytes(12)
    dst = socket.inet_aton(destination) + bytes(12)
    h = FRAME.pack(1, 2, 4, 0, IFINDEX, len(sn), len(iface), len(payload),
                   0, src, dst)
    return h + sn + iface + payload


def receive_exact(sock: socket.socket, count: int) -> bytes:
    buf = bytearray()
    while len(buf) < count:
        chunk = sock.recv(count - len(buf))
        if not chunk:
            raise ConnectionError("R1 closed the virtual wire")
        buf.extend(chunk)
    return bytes(buf)


class ScaleWire(threading.Thread):
    def __init__(self, path: Path, peers: set[str]):
        super().__init__(name="scale-wire", daemon=True)
        self.path = path
        self.peers = peers
        self.ready = threading.Event()
        self.connected = threading.Event()
        self.stopped = threading.Event()
        self.error: Exception | None = None
        self.conn: socket.socket | None = None
        self.listener: socket.socket | None = None
        self.send_lock = threading.Lock()
        self.acks = 0
        self.hello_tx = 0

    def transmit(self, payload: bytes, source: str):
        with self.send_lock:
            if self.error:
                raise RuntimeError("virtual wire failed") from self.error
            if not self.conn:
                raise RuntimeError("R1 virtual wire not connected")
            self.conn.sendall(frame(payload, source))

    def run(self):
        try:
            self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.listener.bind(str(self.path))
            self.listener.listen(1)
            self.ready.set()
            self.conn, _ = self.listener.accept()
            self.connected.set()
            while not self.stopped.is_set():
                header = receive_exact(self.conn, FRAME.size)
                ver, typ, afi, flags, ifindex, sl, il, pl, _, src, dst = FRAME.unpack(header)
                packet = receive_exact(self.conn, sl + il + pl)
                if (ver, afi) != (1, 4) or typ != 2 or pl < 20:
                    continue
                data = packet[sl + il:]
                opcode = data[1]
                seq = struct.unpack_from("!I", data, 8)[0]
                destination = socket.inet_ntoa(dst[:4])
                if opcode == 5:
                    self.hello_tx += 1
                if seq and destination in self.peers:
                    # Each virtual peer ACKs only the reliable packet it received.
                    # On-wire ACK is an EIGRP Hello with the acknowledgement field.
                    self.transmit(eigrp_packet(5, ack=seq), destination)
                    self.acks += 1
        except (ConnectionError, OSError) as exc:
            if not self.stopped.is_set():
                self.error = exc
        except Exception as exc:
            self.error = exc
        finally:
            self.ready.set()
            self.connected.set()

    def close(self):
        self.stopped.set()
        if self.conn:
            try:
                self.conn.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.conn.close()
        if self.listener:
            self.listener.close()
        self.join(timeout=2)


class R1:
    def __init__(self, cfg: Path, sock: Path):
        env = dict(os.environ, EIGRP_UNIX_WIRE_SOCKET=str(sock))
        self.proc = subprocess.Popen([str(NODE), str(cfg)], env=env,
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, bufsize=1)
        self.responses: queue.Queue[str] = queue.Queue()
        self.errors: list[str] = []
        threading.Thread(target=lambda: self._reader(self.proc.stdout,
                                                     self.responses), daemon=True).start()
        threading.Thread(target=lambda: self._error_reader(), daemon=True).start()
        assert self.responses.get(timeout=20) == "READY", "R1 startup failed"

    @staticmethod
    def _reader(stream, destination):
        for line in stream:
            destination.put(line.rstrip("\n"))

    def _error_reader(self):
        for line in self.proc.stderr:
            if len(self.errors) < 200:
                self.errors.append(line.strip())

    def snapshot(self, timeout=45):
        assert self.proc.poll() is None, f"R1 crashed (code {self.proc.poll()}): {self.errors[-8:]}"
        self.proc.stdin.write("STATE\n")
        self.proc.stdin.flush()
        peers = set()
        up_peers = set()
        queue_counts = None
        for _ in range(PEERS + 200):
            line = self.responses.get(timeout=timeout)
            if line == "END":
                return peers, up_peers, queue_counts
            if line.startswith("NBR|"):
                fields = dict(item.split("=", 1) for item in line.split("|")[1:] if "=" in item)
                peers.add(fields["address"])
                if fields["state"].upper() == "UP":
                    up_peers.add(fields["address"])
            if line.startswith("INTF|name=eth0|"):
                fields = dict(item.split("=", 1) for item in line.split("|")[1:] if "=" in item)
                queue_counts = (int(fields["peer_count"]),
                                int(fields["output_queue"]),
                                int(fields["reliable_queue"]))
        raise AssertionError("STATE did not terminate or emitted excessive rows")

    def stop(self):
        if self.proc.poll() is None:
            try:
                self.proc.stdin.write("STOP\n")
                self.proc.stdin.flush()
                self.proc.wait(timeout=4)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.proc.kill()
                self.proc.wait(timeout=5)
        for stream in (self.proc.stdin, self.proc.stdout, self.proc.stderr):
            stream.close()


def main():
    if not NODE.is_file():
        raise SystemExit(f"Missing UUT node: {NODE} (run 'make -C eigrp/test/uut' first)")
    # A large /16 subnet is needed: 2048 addresses must share a single R1 link.
    peers = {str(ipaddress.IPv4Address(0x0A000002 + i)) for i in range(PEERS)}
    watchdog = float(os.environ.get("EIGRP_SCALE_WATCHDOG_SEC", "300"))
    with tempfile.TemporaryDirectory(prefix="eigrp-scale-") as temp:
        work = Path(temp)
        config = work / "r1.conf"
        config.write_text("\n".join(("router r1 4453", "router-id 10.255.0.1",
                            "interface eth0 101 1000000 1500", "address eth0 10.0.0.1/16",
                            "segment eth0 scale-lan", "network 10.0.0.0/16",
                            "up eth0", "")))
        wire = ScaleWire(work / "wire.sock", peers)
        wire.start()
        assert wire.ready.wait(5), "virtual wire failed to bind"
        assert wire.error is None, f"virtual wire failed: {wire.error}"
        node = None
        begin = time.monotonic()
        try:
            node = R1(config, wire.path)
            assert wire.connected.wait(10), "R1 did not connect to virtual wire"
            first_hello = time.monotonic()
            # Inject actual, independently checksummed Hello packets on the
            # wire.  Read back show-neighbor management state after each batch
            # to avoid turning ingress queue saturation into accidental loss.
            ordered = sorted(peers, key=ipaddress.IPv4Address)
            for index in range(0, PEERS, 64):
                for address in ordered[index:index + 64]:
                    wire.transmit(HELLO, address)
                target = index + min(64, PEERS - index)
                while True:
                    if wire.error:
                        raise RuntimeError("virtual wire failure") from wire.error
                    visible, _, counters = node.snapshot()
                    if counters and counters[0] >= target:
                        break
                    if time.monotonic() - first_hello > watchdog:
                        raise AssertionError(f"R1 failed to admit Hello batch {target}; visible={len(visible)}")
                    time.sleep(0.02)

            stable = 0
            while True:
                visible, up_peers, counters = node.snapshot()
                if visible != peers:
                    raise AssertionError(f"show neighbors mismatch: expected 2048, got {len(visible)}; missing={list(peers-visible)[:5]}")
                if (up_peers == peers and counters and counters[0] == PEERS
                        and counters[1:] == (0, 0)):
                    stable += 1
                    if stable >= 2:
                        elapsed = time.monotonic() - first_hello
                        print(f"PASS scale/neighbor_2048: {len(visible)} UP neighbors visible on R1")
                        print(f"INFORMATIONAL time_to_quiescence={elapsed:.3f}s "
                              f"(not a performance pass/fail threshold); "
                              f"acknowledgements={wire.acks} sent_hellos={wire.hello_tx}")
                        break
                else:
                    stable = 0
                if time.monotonic() - first_hello > watchdog:
                    raise AssertionError(f"R1 did not drain, peers={len(visible)}, UP={len(up_peers)}, queues={counters} "
                                         f"ACKs={wire.acks} (watchdog={watchdog}s)")
                time.sleep(0.25)
            if node.proc.poll() is not None:
                raise AssertionError(f"R1 exited unexpectedly: {node.proc.poll()}")
        finally:
            if node:
                node.stop()
            wire.close()


if __name__ == "__main__":
    main()
