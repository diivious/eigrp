#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Fault-oriented scale qualification against the real portable EIGRP UUT.

No DUAL/packetizer shortcuts: static sources use the Unix host-RIB adapter;
synthetic peer updates use RFC 7868 packets over the Unix virtual wire.
All elapsed times and RSS measurements are informational, never performance gates.
"""
from __future__ import annotations

import argparse
import collections
import contextlib
import ipaddress
import importlib.util
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import threading
import time

# pytest uses --import-mode=importlib and imports every *.py in eigrp/test.
# Resolve our sibling directly by path so both script execution and pytest
# collection work without relying on the current working directory/sys.path.
_base_spec = importlib.util.spec_from_file_location(
    "eigrp_scale_routes_100k", Path(__file__).with_name("routes_100k.py"))
if _base_spec is None or _base_spec.loader is None:
    raise ImportError("unable to load scale route generator")
base = importlib.util.module_from_spec(_base_spec)
_base_spec.loader.exec_module(base)

ROOT, NODE = base.ROOT, base.NODE
WATCHDOG = base.WATCHDOG
ROUTES = base.ROUTES
CHURN_ROUTES = int(os.environ.get("EIGRP_SCALE_CHURN_ROUTES", "10000"))
CHURN_CYCLES = int(os.environ.get("EIGRP_SCALE_CHURN_CYCLES", "3"))
MCAST_ROUTES = int(os.environ.get("EIGRP_SCALE_MCAST_ROUTES", "256"))
MCAST_PEERS = int(os.environ.get("EIGRP_SCALE_PEERS", "2048"))
MCAST_SLOW = int(os.environ.get("EIGRP_SCALE_MCAST_SLOW_PEERS", "300"))


def prefix(n: int) -> str:
    if not 0 <= n < 1 << 24:
        raise ValueError(f"test prefix index {n} exceeds 100/8 /32 space")
    return f"{ipaddress.IPv4Address((100 << 24) | n)}/32"


def command(node: base.ScaleNode, cmd: str, *, timeout: float = 120) -> None:
    """One stdin transaction. Exercises the real UUT host/management handler."""
    with node.control_lock:
        node.alive()
        node.proc.stdin.write(cmd + "\n")
        node.proc.stdin.flush()
        response = node._response(timeout)
        if response != "RESULT|0":
            raise AssertionError(f"{node.name}: {cmd} returned {response}")


def check_live(nodes, wire=None, broker=None):
    for node in nodes.values():
        node.alive()
    if wire is not None and wire.error:
        raise AssertionError(f"synthetic wire failed: {wire.error}")
    if broker is not None and broker.error:
        raise AssertionError(f"Unix wire broker failed: {broker.error}")


def wait_for(node, minimum: int, deadline: float, description: str,
             *, maximum: int | None = None, min_peers: int | None = None,
             min_rib: int | None = None, max_rib: int | None = None):
    last = None
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        counts, up, outputs = node.state_counts(timeout=min(240, max(5, remaining)))
        last = (counts["TOPO"], counts["RIB"], len(up), outputs)
        if counts["TOPO"] >= minimum and (maximum is None or counts["TOPO"] <= maximum) \
                and (min_peers is None or len(up) >= min_peers) \
                and (min_rib is None or counts["RIB"] >= min_rib) \
                and (max_rib is None or counts["RIB"] <= max_rib):
            print(f"  {description}: TOPO={counts['TOPO']} RIB={counts['RIB']} "
                  f"UP={len(up)} queues={outputs}", flush=True)
            return counts, up, outputs
        print(f"  {description}: waiting, topology={counts['TOPO']}, peers={len(up)}", flush=True)
        time.sleep(1)
    raise AssertionError(f"{description}: route/peer state stalled; last={last}")


def rss_kib(pid: int) -> int | None:
    try:
        if os.path.exists(f"/proc/{pid}/status"):
            for row in Path(f"/proc/{pid}/status").read_text().splitlines():
                if row.startswith("VmRSS:"):
                    return int(row.split()[1])
        out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True)
        return int(out.strip())
    except (OSError, ValueError, subprocess.CalledProcessError):
        return None


def router_config(name, address: str, *, source_count=0, md5=False):
    """Populate host RIB with real static routes, not DUAL-only descriptors."""
    iface = "eth0"
    lines = [f"router {name} {base.ASN}", f"router-id 10.255.0.{name[1:]}",
             f"interface {iface} {100 + int(name[1:])} 1000000 1500",
             f"address {iface} {address}/24", f"segment {iface} shared",
             f"network {ipaddress.ip_network(address + '/24', strict=False)}"]
    if md5:
        lines.append("auth-md5 eth0 scale-test-keychain scale-test-only-secret")
    if source_count:
        lines.extend(f"source {prefix(i)} 1" for i in range(source_count))
        lines.append("redistribute static")
    lines.append("up eth0")
    return "\n".join(lines) + "\n"


@contextlib.contextmanager
def paired_nodes(path: Path, count: int, *, md5=False):
    """Two real EIGRP processes on the existing Unix segment broker."""
    sock = path / "wire.sock"
    owners = {(4, socket.inet_aton(f"10.41.0.{i}")): f"r{i}" for i in (1, 2)}
    broker = base.Broker(str(sock), owners)
    broker.journal = collections.deque(maxlen=4096)
    nodes = {}
    broker.start()
    assert broker.ready.wait(10) and broker.error is None
    try:
        for name, address, sources in (("r1", "10.41.0.1", count),
                                       ("r2", "10.41.0.2", 0)):
            cfg = path / f"{name}.conf"
            cfg.write_text(router_config(name, address, source_count=sources, md5=md5))
            nodes[name] = base.ScaleNode(name, cfg, sock)
        yield nodes, broker
    finally:
        for node in reversed(list(nodes.values())):
            node.stop()
        broker.close()


def withdraw():
    deadline = time.monotonic() + WATCHDOG
    with base.tempfile.TemporaryDirectory(prefix="eigrp-withdraw-") as tmp:
        with paired_nodes(Path(tmp), ROUTES) as (nodes, broker):
            wait_for(nodes["r2"], ROUTES + 1, deadline, "initial redistribution",
                     maximum=ROUTES + 1, min_peers=1,
                     min_rib=ROUTES + 1, max_rib=ROUTES + 1)
            peak = rss_kib(nodes["r2"].proc.pid)
            for i in range(ROUTES):
                command(nodes["r1"], "SOURCE-DEL " + prefix(i))
                if i % 10000 == 9999:
                    print(f"  withdrawn {i+1}/{ROUTES}", flush=True)
                    check_live(nodes, broker=broker)
                if time.monotonic() >= deadline:
                    raise AssertionError("withdraw watchdog expired")
            # Allow connected prefixes; none of the 100/8 source routes may remain.
            wait_for(nodes["r2"], 1, deadline, "withdrawn topology",
                     maximum=1, min_peers=1, min_rib=1, max_rib=1)
            for i in range(ROUTES):
                command(nodes["r1"], f"SOURCE-ADD {prefix(i)} 1")
                if i % 10000 == 9999:
                    print(f"  reinjected {i+1}/{ROUTES}", flush=True)
                if time.monotonic() >= deadline:
                    raise AssertionError("reinject watchdog expired")
            wait_for(nodes["r2"], ROUTES + 1, deadline, "reinstall after withdrawal",
                     maximum=ROUTES + 1, min_peers=1,
                     min_rib=ROUTES + 1, max_rib=ROUTES + 1)
            check_live(nodes, broker=broker)
            print(f"PASS scale/withdraw: {ROUTES} routes withdrawn and reinstalled; "
                  f"RSS_before={peak}KiB RSS_after={rss_kib(nodes['r2'].proc.pid)}KiB")


def churn():
    # WATCHDOG is a per-phase progress bound, not a cumulative timeout shared
    # by initial convergence and all churn cycles.  At 100K destinations a
    # later phase can still be progressing after earlier phases spent time.
    if CHURN_ROUTES <= 0 or CHURN_ROUTES >= ROUTES or CHURN_CYCLES <= 0:
        raise ValueError("churn routes and cycles must be positive")
    with base.tempfile.TemporaryDirectory(prefix="eigrp-churn-") as tmp:
        with paired_nodes(Path(tmp), ROUTES) as (nodes, broker):
            wait_for(nodes["r2"], ROUTES + 1, time.monotonic() + WATCHDOG,
                     "churn initial routes", maximum=ROUTES + 1, min_peers=1,
                     min_rib=ROUTES + 1, max_rib=ROUTES + 1)
            rss_before = rss_kib(nodes["r1"].proc.pid)
            for cycle in range(CHURN_CYCLES):
                phase_start = time.monotonic()
                phase_deadline = phase_start + WATCHDOG
                for i in range(CHURN_ROUTES):
                    command(nodes["r1"], f"SOURCE-ADD {prefix(i)} {cycle+2}")
                    if time.monotonic() >= phase_deadline:
                        raise AssertionError(f"churn cycle {cycle+1}: change phase exceeded watchdog")
                phase_deadline = time.monotonic() + WATCHDOG
                for i in range(CHURN_ROUTES):
                    command(nodes["r1"], "SOURCE-DEL " + prefix(i))
                    if time.monotonic() >= phase_deadline:
                        raise AssertionError(f"churn cycle {cycle+1}: delete phase exceeded watchdog")
                # With no remaining path, both topology and RIB must remove
                # the destination after DUAL and packetization finish.
                expected = ROUTES - CHURN_ROUTES + 1
                wait_for(nodes["r2"], expected, time.monotonic() + WATCHDOG,
                         f"churn cycle {cycle+1} withdraw", maximum=expected,
                         min_peers=1, min_rib=expected, max_rib=expected)
                phase_deadline = time.monotonic() + WATCHDOG
                for i in range(CHURN_ROUTES):
                    command(nodes["r1"], f"SOURCE-ADD {prefix(i)} {cycle+2}")
                    if time.monotonic() >= phase_deadline:
                        raise AssertionError(f"churn cycle {cycle+1}: restore injection exceeded watchdog")
                wait_for(nodes["r2"], ROUTES + 1, time.monotonic() + WATCHDOG,
                         f"churn cycle {cycle+1} restore", maximum=ROUTES + 1,
                         min_peers=1, min_rib=ROUTES + 1, max_rib=ROUTES + 1)
                check_live(nodes, broker=broker)
                print(f"  cycle={cycle+1}/{CHURN_CYCLES} "
                      f"elapsed={time.monotonic()-phase_start:.3f}s "
                      f"rss_kib={rss_kib(nodes['r1'].proc.pid)}", flush=True)
            print(f"PASS scale/churn: base={ROUTES} changed={CHURN_ROUTES} "
                  f"cycles={CHURN_CYCLES} RSS_before={rss_before}KiB "
                  f"RSS_after={rss_kib(nodes['r1'].proc.pid)}KiB")


def auth_md5():
    deadline = time.monotonic() + WATCHDOG
    with base.tempfile.TemporaryDirectory(prefix="eigrp-auth-md5-") as tmp:
        with paired_nodes(Path(tmp), ROUTES, md5=True) as (nodes, broker):
            counts, up, queues = wait_for(nodes["r2"], ROUTES, deadline,
                                          "MD5 authenticated redistribution", min_peers=1, min_rib=ROUTES)
            _, up1, _ = nodes["r1"].state_counts()
            if len(up1) != 1 or len(up) != 1:
                raise AssertionError("MD5 peers did not remain UP after flooding")
            check_live(nodes, broker=broker)
            print(f"PASS scale/auth-md5: {ROUTES} authenticated redistributed routes at R2; "
                  f"RSS={rss_kib(nodes['r2'].proc.pid)}KiB")


def synthetic_peers(peers_count: int):
    return [str(ipaddress.IPv4Address(0x0A000002 + i)) for i in range(peers_count)]


@contextlib.contextmanager
def single_node_with_peers(path: Path, peers):
    sock = path / "wire.sock"
    peer_names = set(peers)
    wire = base.SyntheticLink(sock, "inject", peer_names, broker=False)
    wire.start()
    node = None
    try:
        cfg = path / "r1.conf"
        cfg.write_text(base.config_for("r1", [("eth0", "inject", "10.0.0.1/16")]))
        node = base.ScaleNode("r1", cfg, sock)
        wire.wait_connect()
        yield node, wire
    finally:
        if wire:
            wire.close()
        if node:
            node.stop()


def establish(node, wire, peers, deadline, versions=None):
    for index in range(0, len(peers), 64):
        group = peers[index:index+64]
        for peer in group:
            if versions is None:
                hello = base.HELLO
            else:
                sw = struct.pack("!HHBBBB", 4, 8, 1, 0, versions[peer], 2)
                hello = base.packet(5, tlvs=base.PARAMETER + sw)
                wire.hello_packets[peer] = hello
            wire.transmit(hello, peer)
        while time.monotonic() < deadline:
            _, up, _ = node.state_counts(timeout=80)
            if len(up) >= index + len(group):
                break
            time.sleep(.1)
        else:
            raise AssertionError(f"peer admission stalled: {index+len(group)}/{len(peers)}")
    wire.start_heartbeats()


def source_routes_add(node: base.ScaleNode, start: int, end: int) -> None:
    """Originate routes in R1's host RIB, not from a neighbor on the same LAN.

    A learned route cannot be reflected back onto its incoming interface due
    to split horizon.  Source-route redistribution causes R1 to originate
    multicast UPDATEs to the simulated peers, exercising actual TRANSMIT.
    """
    for index in range(start, end):
        command(node, f"SOURCE-ADD {prefix(index)} 1")


def mcast_cr_exclusions_fit(count: int, mtu: int = 1500) -> bool:
    """RFC IPv4 CR Hello overhead plus five bytes per excluded peer.

    EIGRP header (20), Parameter (12), Software Version (8), SEQUENCE
    header (4), NEXT_MULTICAST_SEQUENCE (8), and address-length+IPv4 (5).
    This test's UUT interface has no authentication TLV; production transmit
    remains responsible for making the final MTU admission decision.
    """
    return 20 + 12 + 8 + 4 + 8 + 5 * count <= mtu


def mcast():
    deadline = time.monotonic() + WATCHDOG
    if not 2 <= MCAST_PEERS <= 2048 or not 1 <= MCAST_SLOW < MCAST_PEERS:
        raise ValueError("mcast needs 2..2048 peers and at least one fast recipient")
    if not 2 <= MCAST_ROUTES < (1 << 24):
        raise ValueError("mcast needs at least two distinct source prefixes")
    peers = synthetic_peers(MCAST_PEERS)
    with base.tempfile.TemporaryDirectory(prefix="eigrp-mcast-") as tmp:
        with single_node_with_peers(Path(tmp), peers) as (node, wire):
            establish(node, wire, peers, deadline)
            # Allow all initial INIT/EOT traffic to settle so the deliberate
            # ACK hold concerns route UPDATEs, not adjacency synchronization.
            while time.monotonic() < deadline:
                initial_counts, up, queues = node.state_counts(timeout=30)
                if len(up) == MCAST_PEERS and all(not oq and not rq
                                                  for _, oq, rq in queues):
                    break
                time.sleep(.1)
            else:
                raise AssertionError("mcast neighbors did not reach RTP quiescence")

            # Enable real R1 static-route redistribution.  No routes exist yet;
            # adding the first source after the hold must generate a reliable
            # outbound multicast UPDATE on the shared LAN.
            command(node, "REDIST-ON static")
            slow = peers[1:1 + MCAST_SLOW]
            baseline_updates = wire.route_wire_count
            baseline_multicast = wire.multicast_route_updates
            baseline_cr = wire.cr_hellos
            wire.hold_peer_acks(slow)
            source_routes_add(node, 0, 1)

            first_ack_deadline = min(deadline, time.monotonic() + 20)
            while time.monotonic() < first_ack_deadline and wire.withheld_count == 0:
                check_live({"r1": node}, wire=wire)
                time.sleep(.05)
            if wire.withheld_count == 0:
                raise AssertionError(
                    "R1 did not multicast the first redistributed route to "
                    f"held peers (updates={wire.route_wire_count - baseline_updates}, "
                    f"UP={len(node.state_counts()[1])})")

            # Now each slow peer has an outstanding reliable packet. Further
            # originations must either use CR (small exclusion list) or stop
            # multicast admission (exclusion list exceeds the interface MTU).
            first_multicast = wire.multicast_route_updates
            if first_multicast <= baseline_multicast:
                raise AssertionError("first redistributed route was not multicast")
            source_routes_add(node, 1, MCAST_ROUTES)
            expected_topology = initial_counts["TOPO"] + MCAST_ROUTES
            wait_for(node, expected_topology, deadline, "originated source routes",
                     min_peers=MCAST_PEERS)
            cr_fits = mcast_cr_exclusions_fit(MCAST_SLOW)
            if cr_fits:
                cr_deadline = min(deadline, time.monotonic() + 20)
                while time.monotonic() < cr_deadline and wire.cr_hellos == baseline_cr:
                    check_live({"r1": node}, wire=wire)
                    time.sleep(.05)
                if wire.cr_hellos == baseline_cr:
                    raise AssertionError(
                        "R1 did not send CR SEQUENCE Hello with a valid "
                        f"{MCAST_SLOW}-peer exclusion list "
                        f"(multicasts={wire.multicast_route_updates-baseline_multicast}, "
                        f"held_acks={wire.withheld_count})")
            else:
                # With the CR list too large, TRANSMIT must not admit the next
                # reliable multicast. Verify the deliberate pause *before*
                # releasing ACKs, then verify progress after release.
                time.sleep(.5)
                check_live({"r1": node}, wire=wire)
                if wire.cr_hellos != baseline_cr:
                    raise AssertionError("oversized CR exclusion list was transmitted")
                if wire.multicast_route_updates != first_multicast:
                    raise AssertionError(
                        "multicast continued despite CR MTU exhaustion "
                        f"(before={first_multicast}, now={wire.multicast_route_updates})")
            if wire.max_frame_size > 1500:
                raise AssertionError(f"CR or UPDATE exceeded MTU: {wire.max_frame_size}")

            released = wire.release_held_acks()
            if not released:
                raise AssertionError("no held ACKs released to RTP")
            # R1 must drain reliable RTP queues without dropping advertisements
            # or losing any of the existing adjacencies.
            while time.monotonic() < deadline:
                counts, up, queues = node.state_counts(120)
                if counts["TOPO"] >= expected_topology and len(up) == MCAST_PEERS \
                        and all(not oq and not rq for _, oq, rq in queues):
                    break
                check_live({"r1": node}, wire=wire)
                time.sleep(1)
            else:
                raise AssertionError("ACK-driven multicast recovery failed to drain")
            if wire.max_frame_size > 1500:
                raise AssertionError("oversize packet after CR drain")
            if wire.multicast_route_updates <= first_multicast:
                raise AssertionError("held multicast did not resume after ACK drain")
            node.alive()
            print(f"PASS scale/mcast: peers={MCAST_PEERS} slow={MCAST_SLOW} "
                  f"routes={MCAST_ROUTES} held_ack={released} "
                  f"cr_hellos={wire.cr_hellos - baseline_cr} "
                  f"route_updates={wire.route_wire_count - baseline_updates} "
                  f"multicasts={wire.multicast_route_updates - baseline_multicast} "
                  f"cr_fits={cr_fits} max_payload={wire.max_frame_size}")


def mixed_tlv():
    deadline = time.monotonic() + WATCHDOG
    peers = synthetic_peers(3)
    versions = {peers[0]: 1, peers[1]: 2, peers[2]: 2}
    with base.tempfile.TemporaryDirectory(prefix="eigrp-mixed-tlv-") as tmp:
        with single_node_with_peers(Path(tmp), peers) as (node, wire):
            establish(node, wire, peers, deadline, versions=versions)
            # Routes learned from peers[0] on this same LAN cannot be sent
            # back across it (split horizon). Originate routes locally instead
            # so packetizer must encode for both peer capability versions.
            count = int(os.environ.get("EIGRP_SCALE_TLV_ROUTES", "2048"))
            if not 1 <= count < (1 << 24):
                raise ValueError("mixed-TLV route count is outside 100/8 test space")
            before_counts, _, _ = node.state_counts()
            before_tlvs = wire.route_tlv_types.copy()
            command(node, "REDIST-ON static")
            source_routes_add(node, 0, count)
            counts, up, _ = wait_for(node, before_counts["TOPO"] + count,
                                     deadline, "mixed-TLV originated routes", min_peers=3)
            # Require new on-wire TLVs after redistribution, not merely
            # capability negotiation, INIT traffic or UP neighbor state.
            observed = wire.route_tlv_types
            while time.monotonic() < deadline:
                classic = sum(observed[v] - before_tlvs[v] for v in (0x0102, 0x0103))
                modern = sum(observed[v] - before_tlvs[v] for v in (0x0602, 0x0603))
                if classic > 0 and modern > 0:
                    break
                check_live({"r1": node}, wire=wire)
                time.sleep(.1)
            else:
                raise AssertionError(f"mixed-capability UPDATE missing classic or MP TLVs: "
                                     f"classic={classic}, modern={modern}")
            if wire.max_frame_size > 1500:
                raise AssertionError(f"mixed-TLV UPDATE exceeds MTU: {wire.max_frame_size}")
            node.alive()
            print(f"PASS scale/mixed-tlv: neighbors={len(up)} routes={count} "
                  f"types={dict(observed)} max_payload={wire.max_frame_size}")


def linkdown():
    deadline = time.monotonic() + WATCHDOG
    with base.tempfile.TemporaryDirectory(prefix="eigrp-linkdown-") as tmp:
        path = Path(tmp)
        cfgs = base.topology(3)
        sock = path / "wire.sock"
        owners = {(4, ipaddress.ip_interface(addr).ip.packed): name
                  for name, ifs in cfgs.items() for _, _, addr in ifs}
        owners[(4, socket.inet_aton("10.250.0.2"))] = "injection-peer"
        broker = base.Broker(str(sock), owners)
        broker.journal = collections.deque(maxlen=8192)
        nodes, wire = {}, None
        broker.start()
        assert broker.ready.wait(10) and broker.error is None
        try:
            wire = base.SyntheticLink(sock, "inject", {"10.250.0.2"}, broker=True)
            wire.start()
            for name, ifs in cfgs.items():
                conf = path / f"{name}.conf"
                conf.write_text(base.config_for(name, ifs))
                nodes[name] = base.ScaleNode(name, conf, sock)
            wire.wait_connect()
            establish(nodes["r1"], wire, ["10.250.0.2"], deadline)
            for name, needed in (("r1", 2), ("r2", 2), ("r3", 1)):
                wait_for(nodes[name], 0, deadline, f"{name} initial adjacency", min_peers=needed)
            # Flood from a real synthetic neighbor asynchronously and tear down
            # R1-R2 only when roughly half of the routes have been ACKed by R1.
            errors = queue.Queue()
            def flood():
                try:
                    base.inject(wire, ["10.250.0.2"], route_count=ROUTES,
                                pattern="mixed", deadline=deadline)
                except Exception as exc:
                    errors.put(exc)
            worker = threading.Thread(target=flood, daemon=True)
            worker.start()
            halfway = max(1, (ROUTES // base.BATCH_SIZE) // 2)
            while time.monotonic() < deadline and wire.received_acks < halfway:
                check_live(nodes, wire, broker)
                if not errors.empty():
                    raise errors.get()
                time.sleep(.1)
            if wire.received_acks < halfway:
                raise AssertionError("flood never reached the link failure boundary")
            command(nodes["r1"], "DOWN eth12", timeout=60)
            command(nodes["r2"], "DOWN eth21", timeout=60)
            check_live(nodes, wire, broker)
            command(nodes["r1"], "UP eth12", timeout=60)
            command(nodes["r2"], "UP eth21", timeout=60)
            worker.join(timeout=max(1, deadline-time.monotonic()))
            if worker.is_alive():
                raise AssertionError("route injection blocked across link failure")
            if not errors.empty():
                raise errors.get()
            for name, needed in (("r1", 2), ("r2", 2), ("r3", 1)):
                wait_for(nodes[name], ROUTES, deadline,
                         f"{name} after R1-R2 recovery", min_peers=needed)
            check_live(nodes, wire, broker)
            print(f"PASS scale/linkdown: injected={ROUTES}, DOWN/UP at ACK {halfway}")
        finally:
            if wire:
                wire.close()
            for node in reversed(list(nodes.values())):
                node.stop()
            broker.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("test", choices=("withdraw", "mcast", "linkdown", "mixed-tlv", "churn", "auth-md5"))
    args = parser.parse_args()
    if not NODE.is_file():
        raise SystemExit(f"Missing {NODE}; run make -C eigrp/test/uut eigrp-uut-node")
    if args.test == "mcast":
        details = (f"peers={MCAST_PEERS} slow_peers={MCAST_SLOW} "
                   f"routes={MCAST_ROUTES}")
    elif args.test == "mixed-tlv":
        details = f"routes={os.environ.get('EIGRP_SCALE_TLV_ROUTES', '2048')}"
    else:
        details = f"routes={ROUTES}"
    print(f"START scale/{args.test} watchdog={WATCHDOG}s {details}", flush=True)
    start = time.monotonic()
    {"withdraw": withdraw, "mcast": mcast, "linkdown": linkdown,
     "mixed-tlv": mixed_tlv, "churn": churn, "auth-md5": auth_md5}[args.test]()
    print(f"INFORMATIONAL elapsed={time.monotonic()-start:.3f}s", flush=True)


if __name__ == "__main__":
    main()
