# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Portable classic IPv4 TLV wire-format sanity tests.
# These tests validate the RFC 7868 byte layout used by eigrp_tlv1.c.

import ipaddress
import struct

import pytest


EIGRP_TLV_IPV4_INTERNAL = 0x0102
EIGRP_TLV_IPV4_EXTERNAL = 0x0103
EIGRP_TLV_HEADER_LEN = 4
EIGRP_TLV1_NEXTHOP_LEN = 4
EIGRP_TLV1_METRIC_LEN = 16
EIGRP_TLV1_EXTDATA_LEN = 20


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


def metric_fixture() -> bytes:
    return struct.pack(
        "!IIBBBBBBBB",
        0x00002800,  # scaled delay
        0x000A0000,  # scaled bandwidth
        0x00,
        0x05,
        0xDC,  # mtu 1500, 24-bit network order
        1,  # hop count
        255,  # reliability
        1,  # load
        0,  # tag
        0,  # flags
    )


def test_prefix_bit_count_is_not_replaced_by_byte_count():
    raw = encode_destination("192.168.1.0/24")

    assert raw == bytes.fromhex("18 c0 a8 01")
    assert decode_destination(raw) == ("192.168.1.0", 24)


def test_default_route_uses_bit_count_zero_and_no_address_bytes():
    raw = encode_destination("0.0.0.0/0")

    assert raw == b"\x00"
    assert decode_destination(raw) == ("0.0.0.0", 0)


@pytest.mark.parametrize(
    ("network", "expected"),
    [
        ("10.0.0.0/8", "08 0a"),
        ("172.16.0.0/16", "10 ac 10"),
        ("192.168.1.0/24", "18 c0 a8 01"),
        ("203.0.113.9/32", "20 cb 00 71 09"),
    ],
)
def test_destination_encode_writes_exact_prefix_octets(network: str, expected: str):
    assert encode_destination(network) == bytes.fromhex(expected)


def test_internal_tlv_length_includes_header_metric_and_destination():
    dest = encode_destination("192.168.1.0/24")
    value = b"\x00\x00\x00\x00" + metric_fixture() + dest
    raw = struct.pack("!HH", EIGRP_TLV_IPV4_INTERNAL, EIGRP_TLV_HEADER_LEN + len(value)) + value
    tlv_type, tlv_len = struct.unpack("!HH", raw[:EIGRP_TLV_HEADER_LEN])

    assert tlv_type == EIGRP_TLV_IPV4_INTERNAL
    assert tlv_len == 28
    assert len(raw) == tlv_len
    assert raw[-4:] == bytes.fromhex("18 c0 a8 01")


def test_external_tlv_includes_exterior_section_then_metric_then_destination():
    extdata = struct.pack(
        "!III IHBB".replace(" ", ""),
        0x0A000001,  # originating router
        100,  # external as
        0x12345678,  # administrative tag
        20,  # external protocol metric
        0,  # reserved
        6,  # ospf
        0,  # flags
    )
    dest = encode_destination("198.51.100.0/24")
    value = b"\x00\x00\x00\x00" + extdata + metric_fixture() + dest
    raw = struct.pack("!HH", EIGRP_TLV_IPV4_EXTERNAL, EIGRP_TLV_HEADER_LEN + len(value)) + value
    tlv_type, tlv_len = struct.unpack("!HH", raw[:EIGRP_TLV_HEADER_LEN])

    assert tlv_type == EIGRP_TLV_IPV4_EXTERNAL
    assert tlv_len == 48
    assert len(raw) == tlv_len
    assert raw[8:28] == extdata
    assert raw[28:44] == metric_fixture()
    assert raw[44:] == bytes.fromhex("18 c6 33 64")


def test_destination_decode_rejects_short_address():
    with pytest.raises(ValueError, match="short destination address"):
        decode_destination(bytes.fromhex("18 c0 a8"))


def test_destination_decode_rejects_invalid_prefix_length():
    with pytest.raises(ValueError, match="invalid ipv4 prefix length"):
        decode_destination(b"\x21")
