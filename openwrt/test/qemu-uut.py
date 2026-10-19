#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""OW-10: actual two-node OpenWrt EIGRP adjacency and kernel FIB UUT.

No protocol simulation: both guests boot the selected OpenWrt disk image,
configure their native UCI/procd stack and exchange EIGRP IP protocol 88
across a QEMU socket-backed Ethernet link. Missing prerequisites are FAIL.
"""
from __future__ import annotations

import argparse
import contextlib
import gzip
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import zlib


class UUTError(RuntimeError):
    pass


def parse_neighbor(output: str, address: str) -> bool:
    return any(re.search(r"\bneighbor=" + re.escape(address) + r"\b", line)
               and re.search(r"\bstate=Up\b", line)
               for line in output.splitlines())


def parse_kernel_route(output: str, prefix: str, gateway: str) -> bool:
    # iproute2 represents protocol number 192 as '192' unless iproute2's
    # rt_protos database defines the local openeigrp name.
    return any(line.startswith(prefix + " ")
               and re.search(r"\bvia\s+" + re.escape(gateway) + r"\b", line)
               and re.search(r"\bproto\s+(?:192|eigrp)\b", line, re.I)
               for line in output.splitlines())


def available_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def copy_guest_disk(image: Path, destination: Path) -> None:
    if not image.is_file() or image.stat().st_size == 0:
        raise UUTError(f"OpenWrt image missing or empty: {image}")
    if image.suffix == ".gz":
        # OpenWrt's combined image recipe can append firmware metadata after
        # the gzip member. Python gzip.open tries to parse that metadata as
        # another gzip member and rejects otherwise valid OpenWrt images.
        # Decode one complete member, keeping its trailing metadata intact in
        # the source image (which is never modified).
        with image.open("rb") as src, destination.open("wb") as dst:
            decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
            while True:
                chunk = src.read(1024 * 1024)
                if not chunk:
                    break
                dst.write(decoder.decompress(chunk))
                if decoder.eof:
                    break
            if not decoder.eof:
                raise UUTError(f"truncated or invalid gzip OpenWrt image: {image}")
            dst.write(decoder.flush())
    else:
        with image.open("rb") as src, destination.open("wb") as dst:
            shutil.copyfileobj(src, dst, 2 * 1024 * 1024)
    if destination.stat().st_size < 8 * 1024 * 1024:
        raise UUTError(f"Image is too small to be a bootable OpenWrt disk: {image}")


class Guest:
    def __init__(self, name: str, argv: list[str], serial: Path, log: Path):
        self.name, self.serial, self.log = name, serial, log
        self.process = None
        self.console = None
        self._buffer = b""
        self._sequence = 0
        self._log_file = log.open("wb")
        self._serial_log_file = (log.parent / f"{name}.serial.log").open("wb")
        try:
            self.process = subprocess.Popen(argv, stdin=subprocess.DEVNULL,
                                            stdout=self._log_file, stderr=subprocess.STDOUT)
        except Exception:
            self._serial_log_file.close()
            self._log_file.close()
            raise

    def connect(self, deadline: float) -> None:
        while time.monotonic() < deadline:
            self.check_alive()
            if self.serial.exists():
                try:
                    console = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    console.settimeout(1.0)
                    console.connect(str(self.serial))
                    self.console = console
                    return
                except (ConnectionRefusedError, FileNotFoundError, OSError):
                    console.close()
            time.sleep(0.2)
        raise UUTError(f"{self.name}: QEMU serial socket did not appear")

    def check_alive(self) -> None:
        if self.process and self.process.poll() is not None:
            raise UUTError(f"{self.name}: QEMU exited {self.process.returncode}; see {self.log}")

    def read_until(self, marker: bytes, deadline: float) -> bytes:
        while time.monotonic() < deadline:
            self.check_alive()
            index = self._buffer.find(marker)
            if index >= 0:
                taken = self._buffer[:index + len(marker)]
                self._buffer = self._buffer[index + len(marker):]
                return taken
            self.console.settimeout(min(1.0, max(0.1, deadline - time.monotonic())))
            try:
                data = self.console.recv(65536)
            except socket.timeout:
                continue
            if not data:
                raise UUTError(f"{self.name}: serial console disconnected")
            self._serial_log_file.write(data)
            self._serial_log_file.flush()
            self._buffer += data
            if len(self._buffer) > 3 * 1024 * 1024:
                self._buffer = self._buffer[-512 * 1024:]
        tail = self._buffer[-1000:].decode("utf-8", "replace")
        raise UUTError(f"{self.name}: timed out waiting for {marker!r}, last output: {tail}")

    def send(self, data: str) -> None:
        self.console.sendall(data.encode("utf-8"))

    def boot_diagnostic(self) -> str:
        # Serial output is distinct from QEMU process stdout/stderr. Report
        # both even when --log-dir already retains the complete files.
        serial_path = self.log.parent / f"{self.name}.serial.log"
        reports = []
        for title, path in (("serial", serial_path), ("QEMU", self.log)):
            try:
                data = path.read_bytes()
            except OSError as exc:
                reports.append(f"{title} log unavailable: {path}: {exc}")
                continue
            if data:
                excerpt = data[-2400:].decode("utf-8", "replace")
                reports.append(f"{title} log {path} (last {min(len(data), 2400)} bytes):\n{excerpt}")
            else:
                reports.append(f"{title} log empty: {path}")
        return "\n".join(reports)

    def boot(self, seconds: int) -> None:
        deadline = time.monotonic() + seconds
        # Console may show 'press Enter to activate' or a default root shell.
        # No interactive password is supplied; UUT images must have default
        # passwordless serial login. This is isolated test-only VM state.
        while time.monotonic() < deadline:
            self.send("\n")
            try:
                output = self.read_until(b"root@", min(deadline, time.monotonic() + 8))
                if b"root@" in output:
                    self.send("stty -echo\n")
                    time.sleep(0.2)
                    return
            except UUTError as exc:
                if "timed out waiting" not in str(exc):
                    raise UUTError(f"{self.name}: serial boot interrupted: {exc}\n"
                                   f"{self.boot_diagnostic()}") from exc
        raise UUTError(f"{self.name}: OpenWrt serial shell did not start in {seconds}s. "
                       "Inspect the captured boot trace; do not assume an EIGRP failure.\n"
                       f"{self.boot_diagnostic()}")

    def run(self, command: str, seconds: int = 25, *, check: bool = True) -> str:
        self._sequence += 1
        marker = f"__OW10_DONE_{self.name}_{self._sequence}__"
        # Commands are constructed locally from constants, never user-supplied
        # shell text. A unique trailing marker establishes an exact exit code.
        self.send(f"{command}\n__ow10_status=$?; printf '\\n{marker}:%d\\n' \"$__ow10_status\"\n")
        output = self.read_until((marker + ":").encode(), time.monotonic() + seconds)
        # Shell echo can contain the marker in the submitted command. Discard
        # it and wait for the marker emitted by the actual printf invocation.
        while not re.search(rb"\r?\n" + re.escape(marker.encode()) + rb":$", output):
            output += self.read_until((marker + ":").encode(), time.monotonic() + seconds)
        result = self.read_until(b"\n", time.monotonic() + 4)
        m = re.match(rb"(\d+)\r?\n", result)
        if not m:
            raise UUTError(f"{self.name}: malformed command exit status: {result!r}")
        rc = int(m.group(1))
        message = output.decode("utf-8", "replace")
        if check and rc:
            raise UUTError(f"{self.name}: command rc={rc}: {command}\n{message[-1400:]}")
        return message

    def close(self) -> None:
        if self.console:
            self.console.close()
            self.console = None
        if self.process:
            if self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            self.process = None
        self._serial_log_file.close()
        self._log_file.close()


def qemu_args(arch: str, disk: Path, serial: Path, port: int,
              node: int, firmware: Path | None) -> list[str]:
    if arch == "x86/64":
        argv = ["qemu-system-x86_64", "-machine", "pc"]
    elif arch == "armsr/armv8":
        if not firmware or not firmware.is_file():
            raise UUTError("ARM64 QEMU requires --firmware PATH to U-Boot qemu_armv8/u-boot.bin")
        argv = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a53",
                "-bios", str(firmware), "-device", "virtio-rng-pci"]
    else:
        raise UUTError(f"unsupported QEMU UUT architecture {arch}")
    argv += ["-nodefaults", "-display", "none", "-monitor", "none",
             "-no-reboot", "-m", "1024", "-smp", "2",
             "-drive", f"file={disk},format=raw,if=virtio",
             "-chardev", f"socket,id=serial,path={serial},server=on,wait=off",
             "-serial", "chardev:serial",
             "-netdev", (f"socket,id=lan,listen=127.0.0.1:{port}" if node == 1
                         else f"socket,id=lan,connect=127.0.0.1:{port}"),
             "-device", f"virtio-net-pci,netdev=lan,mac=52:54:00:10:10:{node:02x}"]
    return argv


def configure_guest(guest: Guest, node: int) -> None:
    address = f"10.99.0.{node}"
    route = f"10.77.{node}.1"
    # Check that the daemon and its CLI really came from the guest image.
    guest.run("test -x /usr/sbin/openeigrpd && test -x /usr/sbin/openeigrpctl")
    guest.run("command -v uci && command -v ip && test -x /etc/init.d/openeigrp")
    guest.run("ip link show br-lan")
    guest.run("uci set network.lan.ipaddr='" + address + "' && "
              "uci set network.lan.netmask='255.255.255.0' && "
              "uci commit network && /etc/init.d/network restart", seconds=55)
    deadline = time.monotonic() + 40
    while True:
        try:
            guest.run("ip -4 addr show br-lan | grep -F '" + address + "/24'", seconds=10)
            break
        except UUTError as exc:
            if "command rc=" not in str(exc) or time.monotonic() >= deadline:
                raise
            time.sleep(1)
    # The dummy NIC is an on-guest connected prefix to be advertised. It
    # must be a different interface, not a secondary address on br-lan.
    guest.run("modprobe dummy && ip link add eigrptest0 type dummy && "
              f"ip address add {route}/24 dev eigrptest0 && ip link set eigrptest0 up")
    guest.run("uci -q delete openeigrp.savage; "
              "uci set openeigrp.savage=process; "
              "uci set openeigrp.savage.name='savage'; "
              "uci set openeigrp.ipv4=address_family; "
              "uci set openeigrp.ipv4.process='savage'; "
              "uci set openeigrp.ipv4.afi='ipv4'; "
              "uci set openeigrp.ipv4.asn='4453'; "
              f"uci set openeigrp.ipv4.router_id='10.0.0.{node}'; "
              "uci set openeigrp.link=network; "
              "uci set openeigrp.link.process='savage'; "
              "uci set openeigrp.link.afi='ipv4'; "
              "uci set openeigrp.link.asn='4453'; "
              "uci set openeigrp.link.prefix='10.99.0.0/24'; "
              "uci set openeigrp.advertise=network; "
              "uci set openeigrp.advertise.process='savage'; "
              "uci set openeigrp.advertise.afi='ipv4'; "
              "uci set openeigrp.advertise.asn='4453'; "
              f"uci set openeigrp.advertise.prefix='10.77.{node}.0/24'; "
              "uci commit openeigrp")
    guest.run("openeigrpd --check")
    guest.run("/etc/init.d/openeigrp start", seconds=40)
    deadline = time.monotonic() + 40
    while True:
        try:
            guest.run("openeigrpctl status | grep -F 'asn=4453'", seconds=10)
            break
        except UUTError as exc:
            if "command rc=" not in str(exc) or time.monotonic() >= deadline:
                raise
            time.sleep(1)


def run_uut(args: argparse.Namespace) -> None:
    image = Path(args.image).resolve()
    firmware = Path(args.firmware).resolve() if args.firmware else None
    binary = "qemu-system-x86_64" if args.arch == "x86/64" else "qemu-system-aarch64"
    if not shutil.which(binary):
        raise UUTError(f"required QEMU binary is not installed: {binary}")
    if not image.is_file():
        raise UUTError(f"image not found: {image}")
    # The caller may provide a directory for persistent failure logs. All
    # guest disks, sockets and processes are disposable, even on failure.
    logs = Path(args.log_dir).resolve() if args.log_dir else None
    if logs:
        logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="eigrp-ow10-") as temp:
        root = Path(temp)
        guests = []
        try:
            port = available_port()
            for node in (1, 2):
                disk = root / f"node{node}.img"
                print(f"OW-10: preparing node {node} disk from {image}", flush=True)
                copy_guest_disk(image, disk)
                serial = root / f"node{node}.sock"
                log = (logs or root) / f"node{node}.qemu.log"
                guest = Guest(f"node{node}", qemu_args(args.arch, disk, serial, port,
                              node, firmware), serial, log)
                guests.append(guest)
                if node == 1:
                    time.sleep(1)  # let the QEMU socket-listener start before connecting peer
            for guest in guests:
                guest.connect(time.monotonic() + 20)
            for guest in guests:
                guest.boot(args.boot_timeout)
            for node, guest in enumerate(guests, 1):
                print(f"OW-10: configuring OpenWrt node {node}", flush=True)
                configure_guest(guest, node)
            print("OW-10: waiting for bidirectional EIGRP neighbor UP and kernel routes", flush=True)
            deadline = time.monotonic() + args.converge_timeout
            final = ["", ""]
            while time.monotonic() < deadline:
                neighbors = [guest.run("openeigrpctl neighbors", seconds=12)
                             for guest in guests]
                final = neighbors
                if (parse_neighbor(neighbors[0], "10.99.0.2") and
                        parse_neighbor(neighbors[1], "10.99.0.1")):
                    routes = [guest.run(f"ip -4 route show 10.77.{3-node}.0/24", seconds=12)
                              for node, guest in enumerate(guests, 1)]
                    if (parse_kernel_route(routes[0], "10.77.2.0/24", "10.99.0.2") and
                            parse_kernel_route(routes[1], "10.77.1.0/24", "10.99.0.1")):
                        print("OW-10 QEMU UUT: PASS (2 neighbors Up, two EIGRP kernel routes)", flush=True)
                        return
                time.sleep(3)
            raise UUTError("two-node convergence failed: expected neighbor Up and EIGRP "
                           f"kernel FIB routes; last neighbors: {final!r}")
        finally:
            for guest in reversed(guests):
                with contextlib.suppress(Exception):
                    guest.close()
            if not logs and sys.exc_info()[0] is not None:
                for path in list(root.glob("node*.qemu.log")) + list(root.glob("node*.serial.log")):
                    print(f"--- {path.name} ---", file=sys.stderr)
                    print(path.read_text(errors="replace")[-5000:], file=sys.stderr)


class ContractTests(unittest.TestCase):
    def test_neighbor_requires_up(self) -> None:
        self.assertFalse(parse_neighbor("no configured instances", "10.99.0.2"))
        self.assertFalse(parse_neighbor("neighbor=10.99.0.2 state=Down", "10.99.0.2"))
        self.assertTrue(parse_neighbor("neighbor=10.99.0.2 interface=br-lan state=Up", "10.99.0.2"))

    def test_route_requires_gateway_and_eigrp_protocol(self) -> None:
        self.assertFalse(parse_kernel_route("", "10.77.2.0/24", "10.99.0.2"))
        self.assertFalse(parse_kernel_route("10.77.2.0/24 via 10.99.0.2 proto static", "10.77.2.0/24", "10.99.0.2"))
        self.assertTrue(parse_kernel_route("10.77.2.0/24 via 10.99.0.2 dev br-lan proto 192", "10.77.2.0/24", "10.99.0.2"))

    def test_architecture_rejection(self) -> None:
        with self.assertRaises(UUTError):
            qemu_args("mips", Path("disk"), Path("serial"), 2000, 1, None)
        with self.assertRaises(UUTError):
            qemu_args("armsr/armv8", Path("disk"), Path("serial"), 2000, 1, None)

    def test_qemu_link_is_private_socket_and_disks_are_per_guest(self) -> None:
        a = qemu_args("x86/64", Path("node1.img"), Path("node1.sock"), 20000, 1, None)
        b = qemu_args("x86/64", Path("node2.img"), Path("node2.sock"), 20000, 2, None)
        self.assertIn("socket,id=lan,listen=127.0.0.1:20000", a)
        self.assertIn("socket,id=lan,connect=127.0.0.1:20000", b)
        self.assertIn("file=node1.img,format=raw,if=virtio", a)
        self.assertIn("file=node2.img,format=raw,if=virtio", b)

    def test_arm_efi_boot_devices_and_firmware(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            firmware = Path(d) / "u-boot.bin"
            firmware.write_bytes(b"firmware")
            argv = qemu_args("armsr/armv8", Path("node.img"), Path("serial.sock"),
                             32102, 1, firmware)
            self.assertEqual(argv[0], "qemu-system-aarch64")
            self.assertIn("virtio-rng-pci", argv)
            self.assertIn("chardev:serial", argv)
            self.assertIn("file=node.img,format=raw,if=virtio", argv)
            self.assertIn(str(firmware), argv)

    def test_boot_failure_includes_both_log_paths_and_content(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            qemu_log = root / "node1.qemu.log"
            serial_log = root / "node1.serial.log"
            qemu_log.write_text("firmware warning\n")
            serial_log.write_text("U-Boot: No bootable EFI image\n")
            guest = Guest.__new__(Guest)
            guest.name = "node1"
            guest.log = qemu_log
            diag = guest.boot_diagnostic()
            self.assertIn(str(qemu_log), diag)
            self.assertIn(str(serial_log), diag)
            self.assertIn("No bootable EFI image", diag)
            self.assertIn("firmware warning", diag)

    def test_guest_command_status_has_to_be_zero(self) -> None:
        left, right = socket.socketpair()
        try:
            guest = Guest.__new__(Guest)
            guest.name = "node1"
            guest.console = left
            guest.process = None
            guest._sequence = 0
            guest._buffer = b"some output\n__OW10_DONE_node1_1__:1\n"
            with self.assertRaises(UUTError):
                guest.run("false", seconds=1)
        finally:
            left.close()
            right.close()

    def test_source_image_does_not_get_modified(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            original = root / "source.img.gz"
            payload = b"x" * (8 * 1024 * 1024)
            with gzip.open(original, "wb") as out:
                out.write(payload)
            with original.open("ab") as trailer:
                trailer.write(b"OWRT appended firmware metadata")
            destination = root / "guest.img"
            copy_guest_disk(original, destination)
            self.assertEqual(destination.stat().st_size, len(payload))
            self.assertEqual(original.read_bytes()[:2], b"\x1f\x8b")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", help="OpenWrt ext4 combined disk image, optionally .gz")
    parser.add_argument("--arch", choices=("x86/64", "armsr/armv8"), default="x86/64")
    parser.add_argument("--firmware", help="ARM64 QEMU U-Boot firmware .bin")
    parser.add_argument("--boot-timeout", type=int, default=180)
    parser.add_argument("--converge-timeout", type=int, default=150)
    parser.add_argument("--log-dir", help="Persist QEMU guest stderr/stdout logs here")
    parser.add_argument("--self-test", action="store_true", help="Run offline contract tests")
    ns = parser.parse_args()
    if ns.self_test:
        result = unittest.defaultTestLoader.loadTestsFromTestCase(ContractTests)
        sys.exit(0 if unittest.TextTestRunner(stream=sys.stderr, verbosity=0).run(result).wasSuccessful() else 1)
    if not ns.image:
        parser.error("--image is required for a real two-node QEMU UUT")
    try:
        run_uut(ns)
    except (UUTError, OSError, subprocess.SubprocessError) as exc:
        print(f"OW-10 QEMU UUT: FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
