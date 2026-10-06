# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Portable multiprotocol/wide TLV wire-format sanity tests.
# These tests validate the RFC 7868 byte layout used by eigrp_tlv2.c.

import ipaddress
import struct

import pytest


EIGRP_TLV_MP_INTERNAL = 0x0602
EIGRP_TLV_MP_EXTERNAL = 0x0603
EIGRP_AF_IPV4 = 1
EIGRP_BASE_TOPOLOGY = 0
EIGRP_TLV_HEADER_LEN = 4
EIGRP_TLV2_HEADER_EXT_LEN = 8
EIGRP_TLV2_METRIC_LEN = 24
EIGRP_TLV2_EXTDATA_LEN = 16
EIGRP_TLV2_INACCESSIBLE = 0xFFFFFFFFFFFF


def put48(value: int) -> bytes:
    if value < 0 or value > 0xFFFFFFFFFFFF:
        raise ValueError("48-bit value out of range")
    return value.to_bytes(6, "big")


def get48(raw: bytes) -> int:
    if len(raw) != 6:
        raise ValueError("48-bit value must be six bytes")
    return int.from_bytes(raw, "big")


def prefix_bytes(prefixlen: int) -> int:
    if prefixlen < 0 or prefixlen > 32:
        raise ValueError("invalid ipv4 prefix length")
    if prefixlen == 0:
        return 0
    return ((prefixlen - 1) // 8) + 1


def encode_destination(network: str) -> bytes:
    net = ipaddress.ip_network(network, strict=True)
    plen = net.prefixlen
    addr_len = prefix_bytes(plen)
    return bytes([plen]) + net.network_address.packed[:addr_len]


def decode_destination(raw: bytes) -> tuple[str, int]:
    if not raw:
        raise ValueError("short destination")
    plen = raw[0]
    addr_len = prefix_bytes(plen)
    if len(raw) < 1 + addr_len:
        raise ValueError("short destination address")
    addr = raw[1 : 1 + addr_len] + b"\x00" * (4 - addr_len)
    return str(ipaddress.ip_address(addr)), plen


def header_ext(router_id: int = 0x0A000001) -> bytes:
    return struct.pack("!HHI", EIGRP_AF_IPV4, EIGRP_BASE_TOPOLOGY, router_id)


def metric_fixture(delay: int = 0x000000123456, bandwidth: int = 0x000000ABCDEF) -> bytes:
    return b"".join(
        [
            bytes([0]),  # offset / no extended attrs
            bytes([0]),  # priority
            bytes([255]),  # reliability
            bytes([1]),  # load
            bytes.fromhex("00 05 dc"),  # mtu 1500
            bytes([1]),  # hop count
            put48(delay),
            put48(bandwidth),
            struct.pack("!HH", 0, 0),  # reserved, opaque flags
        ]
    )


def test_put48_and_get48_round_trip():
    raw = put48(0x010203040506)

    assert raw == bytes.fromhex("01 02 03 04 05 06")
    assert get48(raw) == 0x010203040506


def test_inaccessible_wide_metric_is_six_octets():
    assert put48(EIGRP_TLV2_INACCESSIBLE) == bytes.fromhex("ff ff ff ff ff ff")


def test_internal_mp_tlv_length_includes_header_extension_metric_and_destination():
    dest = encode_destination("192.168.1.0/24")
    value = header_ext() + metric_fixture() + dest
    raw = struct.pack("!HH", EIGRP_TLV_MP_INTERNAL, EIGRP_TLV_HEADER_LEN + len(value)) + value
    tlv_type, tlv_len = struct.unpack("!HH", raw[:EIGRP_TLV_HEADER_LEN])

    assert tlv_type == EIGRP_TLV_MP_INTERNAL
    assert tlv_len == 40
    assert len(raw) == tlv_len
    assert raw[4:12] == header_ext()
    assert raw[12:36] == metric_fixture()
    assert raw[36:] == bytes.fromhex("18 c0 a8 01")


def test_external_mp_tlv_places_exterior_after_metric_before_destination():
    extdata = struct.pack(
        "!IIIHBB",
        0x0A000002,  # external router id
        100,  # external as
        20,  # external protocol metric
        0,  # reserved
        6,  # ospf
        0,  # flags
    )
    dest = encode_destination("198.51.100.0/24")
    value = header_ext() + metric_fixture() + extdata + dest
    raw = struct.pack("!HH", EIGRP_TLV_MP_EXTERNAL, EIGRP_TLV_HEADER_LEN + len(value)) + value
    tlv_type, tlv_len = struct.unpack("!HH", raw[:EIGRP_TLV_HEADER_LEN])

    assert tlv_type == EIGRP_TLV_MP_EXTERNAL
    assert tlv_len == 56
    assert len(raw) == tlv_len
    assert raw[12:36] == metric_fixture()
    assert raw[36:52] == extdata
    assert raw[52:] == bytes.fromhex("18 c6 33 64")


@pytest.mark.parametrize(
    ("network", "expected"),
    [
        ("0.0.0.0/0", "00"),
        ("10.0.0.0/8", "08 0a"),
        ("172.16.0.0/16", "10 ac 10"),
        ("192.168.1.0/24", "18 c0 a8 01"),
        ("203.0.113.9/32", "20 cb 00 71 09"),
    ],
)
def test_destination_encoding_matches_classic_tlv_rules(network: str, expected: str):
    assert encode_destination(network) == bytes.fromhex(expected)


def test_destination_decode_rejects_short_address():
    with pytest.raises(ValueError, match="short destination address"):
        decode_destination(bytes.fromhex("18 c0 a8"))


def test_destination_decode_rejects_invalid_prefix_length():
    with pytest.raises(ValueError, match="invalid ipv4 prefix length"):
        decode_destination(b"\x21")
