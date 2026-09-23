# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Guard the host-order/network-order boundary used by production packet encoders.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


def read(relative: str) -> str:
    return (ROOT / relative).read_text()


def internet_checksum(raw: bytes) -> int:
    if len(raw) % 2:
        raw += b"\x00"

    total = 0
    for index in range(0, len(raw), 2):
        total += (raw[index] << 8) | raw[index + 1]
        total = (total & 0xFFFF) + (total >> 16)

    return (~total) & 0xFFFF


def test_captured_hello_checksum_is_written_most_significant_byte_first():
    # UUT Hello captured with the checksum field cleared.  The checksum is
    # 0xdf63, so the wire bytes must be df 63, not the little-endian 63 df.
    hello = bytes.fromhex(
        "02 05 00 00 "
        "00 00 00 00 "
        "00 00 00 00 "
        "00 00 00 00 "
        "00 00 11 65 "
        "00 01 00 0c 01 00 01 00 00 00 00 0f "
        "00 04 00 08 0a 08 01 02"
    )

    checksum = internet_checksum(hello)

    assert checksum == 0xDF63
    assert checksum.to_bytes(2, "big") == bytes.fromhex("df 63")


def test_packet_header_multibyte_fields_are_serialized_to_network_order():
    packet = read("eigrpd/eigrp_packet.c")

    assert "eigrph->checksum = htons(eigrp_checksum(eigrph, length));" in packet
    assert "eigrph->vrid = htons(eigrp->vrid);" in packet
    assert "eigrph->ASNumber = htons(eigrp->AS);" in packet
    assert "eigrph->ack = htonl(ack);" in packet
    assert "eigrph->sequence = htonl(sequence);" in packet
    assert "eigrph->flags = htonl(flags);" in packet


def test_stream_word_and_long_writers_own_network_order_conversion():
    stream = read("eigrpd/eigrp_stream.c")

    putw = stream[stream.index("size_t eigrp_stream_putw") : stream.index("size_t eigrp_stream_putl")]
    putl = stream[stream.index("size_t eigrp_stream_putl") : stream.index("size_t eigrp_stream_put_ipv4")]

    assert "value = htons(value);" in putw
    assert "value = htonl(value);" in putl


def test_tlv2_router_id_converts_in_addr_before_stream_long_writer():
    tlv2 = read("eigrpd/eigrp_tlv2.c")

    assert "return ntohl(eigrp->router_id.s_addr);" in tlv2
    assert "eigrp_stream_putl(pkt, eigrp_tlv2_router_id_host(eigrp));" in tlv2
    assert ": eigrp_tlv2_router_id_host(eigrp);" in tlv2


def test_authentication_multibyte_fields_are_explicitly_network_ordered():
    auth = read("eigrpd/eigrp_auth.c")

    for statement in (
        "authTLV->type = htons(EIGRP_TLV_AUTH);",
        "authTLV->length = htons(EIGRP_AUTH_MD5_TLV_SIZE);",
        "authTLV->auth_type = htons(EIGRP_AUTH_TYPE_MD5);",
        "authTLV->auth_length = htons(EIGRP_AUTH_TYPE_MD5_LEN);",
        "authTLV->key_id = htonl(key_id);",
        "authTLV->length = htons(EIGRP_AUTH_SHA256_TLV_SIZE);",
        "authTLV->auth_type = htons(EIGRP_AUTH_TYPE_SHA256);",
        "authTLV->auth_length = htons(EIGRP_AUTH_TYPE_SHA256_LEN);",
    ):
        assert statement in auth
