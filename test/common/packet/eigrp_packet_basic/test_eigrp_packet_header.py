# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# First-pass portable EIGRP packet-header sanity tests.
# These tests validate fixture structure only. They do not exercise
# production eigrpd encode/decode code yet.

from dataclasses import dataclass
from pathlib import Path
import struct

import pytest


EIGRP_HEADER_FORMAT = "!BBHIIIHH"
EIGRP_HEADER_LEN = struct.calcsize(EIGRP_HEADER_FORMAT)
EIGRP_OPC_HELLO = 5


@dataclass(frozen=True)
class EigrpHeader:
    version: int
    opcode: int
    checksum: int
    flags: int
    sequence: int
    acknowledgment: int
    vrid: int
    autonomous_system: int


def decode_eigrp_header(raw: bytes) -> EigrpHeader:
    if len(raw) < EIGRP_HEADER_LEN:
        raise ValueError("short eigrp header")

    fields = struct.unpack(EIGRP_HEADER_FORMAT, raw[:EIGRP_HEADER_LEN])
    return EigrpHeader(*fields)




def internet_checksum(raw: bytes) -> int:
    if len(raw) % 2:
        raw += b"\x00"

    total = 0
    for index in range(0, len(raw), 2):
        total += (raw[index] << 8) + raw[index + 1]
        total = (total & 0xFFFF) + (total >> 16)

    return (~total) & 0xFFFF


def packet_with_checksum(raw: bytes) -> bytes:
    raw = bytearray(raw)
    raw[2:4] = b"\x00\x00"
    checksum = internet_checksum(bytes(raw))
    raw[2:4] = checksum.to_bytes(2, "big")
    return bytes(raw)

def load_hex_fixture(name: str) -> bytes:
    fixture = (
        Path(__file__).resolve().parents[3]
        / "common"
        / "packets"
        / name
    )
    text = "".join(
        line.split("#", 1)[0].strip()
        for line in fixture.read_text().splitlines()
    )
    return bytes.fromhex(text)


def test_eigrp_hello_header_fixture_decodes():
    raw = load_hex_fixture("eigrp-hello-header-v2-as100.hex")
    header = decode_eigrp_header(raw)

    assert EIGRP_HEADER_LEN == 20
    assert header.version == 2
    assert header.opcode == EIGRP_OPC_HELLO
    assert header.checksum == 0
    assert header.flags == 0
    assert header.sequence == 0
    assert header.acknowledgment == 0
    assert header.vrid == 0
    assert header.autonomous_system == 100


def test_eigrp_short_header_is_rejected():
    with pytest.raises(ValueError, match="short eigrp header"):
        decode_eigrp_header(b"\x02\x05")


def test_eigrp_checksum_accepts_valid_packet():
    raw = load_hex_fixture("eigrp-hello-header-v2-as100.hex")
    checked = packet_with_checksum(raw)

    assert internet_checksum(checked) == 0


def test_eigrp_checksum_rejects_corrupted_packet():
    raw = bytearray(packet_with_checksum(load_hex_fixture("eigrp-hello-header-v2-as100.hex")))
    raw[-1] ^= 0x01

    assert internet_checksum(bytes(raw)) != 0
