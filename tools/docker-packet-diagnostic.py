#!/usr/bin/env python3
# Copyright (C) 2026 Donnie V. Savage
"""Read-only diagnostic of EIGRP protocol 88 on a Linux Docker interface.

Run by tools/docker-verify.sh through Python stdin, with INTERFACE and SECONDS.
AF_PACKET observes Ethernet delivery; an independent AF_INET/SOCK_RAW socket
checks whether the kernel also delivers protocol 88 to IPv4 raw sockets.
No packets are sent, interfaces changed, or multicast memberships added.
"""

import fcntl
import select
import socket
import struct
import sys
import time


def checksum_valid(data):
    """Validate the Internet ones-complement checksum over a complete header."""
    if len(data) & 1:
        data += b"\x00"
    words = struct.unpack("!" + "H" * (len(data) // 2), data)
    total = sum(words)
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return total == 0xFFFF


def packet_fields(data):
    """Return header data without assuming every captured packet is valid."""
    if len(data) < 20 or data[0] >> 4 != 4 or data[9] != 88:
        return None
    ihl = (data[0] & 0xF) * 4
    if ihl < 20 or ihl > len(data):
        return None
    total_len = struct.unpack_from("!H", data, 2)[0]
    eigrp_data = data[ihl:total_len] if total_len <= len(data) else b""
    valid_ip = checksum_valid(data[:ihl])
    # The capture should be complete; a short capture cannot establish validity.
    valid_length = (total_len == len(data))
    valid_eigrp = (len(eigrp_data) >= 20 and checksum_valid(eigrp_data)
                   and valid_length)
    version = eigrp_data[0] if len(eigrp_data) else None
    opcode = eigrp_data[1] if len(eigrp_data) >= 2 else None
    asn = struct.unpack_from("!H", eigrp_data, 18)[0] if len(eigrp_data) >= 20 else None
    vrid = struct.unpack_from("!H", eigrp_data, 16)[0] if len(eigrp_data) >= 20 else None
    return {
        "source": socket.inet_ntoa(data[12:16]),
        "destination": socket.inet_ntoa(data[16:20]),
        "ttl": data[8],
        "bytes": len(data),
        "version": version,
        "opcode": opcode,
        "as": asn,
        "vrid": vrid,
        "ip_checksum_valid": valid_ip,
        "length_valid": valid_length,
        "eigrp_checksum_valid": valid_eigrp,
    }


def interface_ipv4(fd, interface):
    try:
        request = struct.pack("256s", interface.encode()[:15])
        return socket.inet_ntoa(fcntl.ioctl(fd, 0x8915, request)[20:24])
    except OSError:
        return None


def main():
    interface = sys.argv[1] if len(sys.argv) > 1 else "eth0"
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
    try:
        ethernet = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                 socket.htons(0x0800))
        ethernet.bind((interface, 0))
    except OSError as exc:
        print(f"Packet capture unavailable on {interface}: {exc}", flush=True)
        return 1
    local_ip = interface_ipv4(ethernet.fileno(), interface)
    print(f"{interface}: local IPv4 address={local_ip or 'unknown'}", flush=True)

    # Passive probe: opening a second raw socket does not steal packets from
    # eigrpd's own raw socket.  The existing EIGRP multicast joins remain intact.
    try:
        raw = socket.socket(socket.AF_INET, socket.SOCK_RAW, 88)
        raw.setblocking(False)
    except OSError as exc:
        print(f"IPv4 raw-socket probe unavailable: {exc}", flush=True)
        raw = None
    ethernet.setblocking(False)
    sockets = [ethernet] + ([raw] if raw is not None else [])
    counters = {
        "l2_peer": 0, "l2_self": 0, "l2_ip_bad": 0,
        "l2_eigrp_bad": 0, "l2_length_bad": 0,
        "ipv4_raw_peer": 0, "ipv4_raw_self": 0,
    }
    printed = 0
    end = time.monotonic() + duration
    try:
        while time.monotonic() < end:
            ready, _, _ = select.select(sockets, [], [],
                                        min(1.0, max(0.0, end - time.monotonic())))
            for s in ready:
                try:
                    if s is ethernet:
                        frame, details = s.recvfrom(65535)
                        if len(frame) < 34 or frame[12:14] != b"\x08\x00":
                            continue
                        data = frame[14:]
                        kind = "L2"
                        packet_type = details[2]
                    else:
                        data, _ = s.recvfrom(65535)
                        kind = "IPv4-RAW"
                        packet_type = None
                except BlockingIOError:
                    continue
                fields = packet_fields(data)
                if fields is None:
                    continue
                peer = local_ip is None or fields["source"] != local_ip
                if kind == "L2":
                    counters["l2_peer" if peer else "l2_self"] += 1
                    if not fields["ip_checksum_valid"]:
                        counters["l2_ip_bad"] += 1
                    if not fields["eigrp_checksum_valid"]:
                        counters["l2_eigrp_bad"] += 1
                    if not fields["length_valid"]:
                        counters["l2_length_bad"] += 1
                else:
                    counters["ipv4_raw_peer" if peer else "ipv4_raw_self"] += 1
                if printed < 24 and (peer or printed < 2):
                    print(f"{kind} {'PEER' if peer else 'SELF'}: "
                          f"{fields['source']} -> {fields['destination']} "
                          f"ttl={fields['ttl']} bytes={fields['bytes']} "
                          f"v={fields['version']} opcode={fields['opcode']} "
                          f"AS={fields['as']} VRID={fields['vrid']} "
                          f"ip-checksum={'OK' if fields['ip_checksum_valid'] else 'BAD'} "
                          f"eigrp-checksum={'OK' if fields['eigrp_checksum_valid'] else 'BAD'} "
                          f"ip-length={'OK' if fields['length_valid'] else 'BAD'} "
                          f"pkt-type={packet_type}", flush=True)
                    printed += 1
    finally:
        ethernet.close()
        if raw is not None:
            raw.close()

    print(f"{interface}: peer Ethernet={counters['l2_peer']}, "
          f"self Ethernet={counters['l2_self']}, "
          f"peer IPv4 raw-socket={counters['ipv4_raw_peer']}, "
          f"self IPv4 raw-socket={counters['ipv4_raw_self']} "
          f"over {duration:g}s", flush=True)
    print(f"{interface}: invalid L2 IP checksums={counters['l2_ip_bad']}, "
          f"EIGRP checksums={counters['l2_eigrp_bad']}, "
          f"IP lengths={counters['l2_length_bad']}", flush=True)
    if counters['l2_peer'] and not counters['ipv4_raw_peer']:
        print('Diagnostic: peer traffic is visible at Ethernet but not to a '
              'second IPv4 raw socket. Check kernel IP receive/admission, '
              'IP header validity, and multicast delivery.', flush=True)
    elif counters['ipv4_raw_peer']:
        print('Diagnostic: peer packets reach an IPv4 raw socket. If EIGRP '
              'counters remain zero, inspect the FRR socket receive callback '
              'and EIGRP input validation/demultiplexing.', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
