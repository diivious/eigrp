# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Negative-control vectors for the diagnostic IPv6 prefix extractor."""
import ipaddress
import struct
import sys
from pathlib import Path

sys.path.insert(0,str(Path(__file__).resolve().parents[4]/"unix"/"test"))
from eigrp_wire import _route_prefixes, _packet_meta


def _tlv(kind, body):
    return struct.pack("!HH",kind,4+len(body))+body


def test_ipv6_classic_and_mp_truncated_and_cross_afi_are_not_routes():
    prefix=bytes([64])+ipaddress.ip_address("2001:db8:abcd::").packed[:9]
    classic=_tlv(0x0402,bytes(16)+bytes(16)+prefix)
    mp=_tlv(0x0602,struct.pack("!HHI",2,0,0)+bytes(24)+prefix)
    header=bytes(20)
    target="2001:db8:abcd::/64"
    assert _route_prefixes(header+classic,6)==[target]
    assert _route_prefixes(header+mp,6)==[target]
    assert _route_prefixes(header+classic,4)==[]
    assert _route_prefixes(header+mp,4)==[]
    assert _packet_meta(header+mp,6)["prefixes"]==[target]
    for good in (classic,mp):
        assert _route_prefixes(header+good[:-1],6)==[]
        bad=bytearray(good)
        bad[2:4]=struct.pack("!H",len(good)-1)
        # Declared TLV shorter than the destination; must not extract the prefix.
        assert _route_prefixes(header+bad,6)==[]
        bad=bytearray(good); bad[-len(prefix)]=129
        assert _route_prefixes(header+bad,6)==[]
    wrong_af=bytearray(mp);wrong_af[4:6]=struct.pack("!H",1)
    assert _route_prefixes(header+wrong_af,6)==[]


def test_ipv6_prefix_length_must_not_cross_tlv_boundary():
    # Complete TLV header and metrics, but no prefix bytes. Do not borrow
    # bytes from the *next* TLV to form a valid destination.
    body=bytes(16)+bytes(16)+bytes([128])
    malformed=_tlv(0x0402,body)
    followed=_tlv(0x0001,bytes(24))
    assert _route_prefixes(bytes(20)+malformed+followed,6)==[]


def test_ipv6_octet_boundary_requires_nine_bytes_but_ipv4_requires_eight():
    pfx=ipaddress.ip_address("2001:db8:9876::").packed
    header=bytes(20)
    # The C IPv6 AF encodes /64 with nine address bytes, not eight.
    valid=_tlv(0x0402,bytes(32)+bytes([64])+pfx[:9])
    truncated=_tlv(0x0402,bytes(32)+bytes([64])+pfx[:8])
    assert _route_prefixes(header+valid,6)==["2001:db8:9876::/64"]
    assert _route_prefixes(header+truncated,6)==[]
    assert _route_prefixes(header+valid,4)==[]


def test_ipv6_non_byte_aligned_and_max_length_prefixes():
    header=bytes(20)
    pfx=ipaddress.ip_address("2001:db8:1234:5678:9abc:def0::1").packed
    for bitlen in (0,1,7,8,9,63,64,65,127,128):
        n=0 if bitlen==0 else 16 if bitlen==128 else bitlen//8+1
        route=_tlv(0x0402,bytes(32)+bytes([bitlen])+pfx[:n])
        expected=str(ipaddress.ip_network((ipaddress.ip_address(pfx),bitlen),strict=False))
        assert _route_prefixes(header+route,6)==[expected],bitlen
        if n:
            assert _route_prefixes(header+route[:-1],6)==[],bitlen


def test_ipv6_route_with_trailing_unparsed_bytes_does_not_match():
    prefix=bytes([64])+ipaddress.ip_address("2001:db8:abcd::").packed[:9]
    invalid=_tlv(0x0402,bytes(32)+prefix+b"\xff")
    assert _route_prefixes(bytes(20)+invalid,6)==[]
