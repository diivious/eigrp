# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for IPv4 packet destination family initialization.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


def read(relative: str) -> str:
    return (ROOT / relative).read_text()


def test_runtime_address_keeps_system_address_family_identity():
    structs = read("eigrpd/eigrp_structs.h")
    start = structs.index("struct eigrp_addr {")
    end = structs.index("};", start)
    address = structs[start:end]

    assert "uint8_t afi;" in address
    assert "struct in_addr" in address
    assert "struct in6_addr" in address


def test_ipv4_vector_uses_posix_family_for_runtime_addresses():
    ipv4 = read("eigrpd/eigrp_ipv4.c")

    assert "source->afi = AF_INET;" in ipv4
    assert "destination->afi = AF_INET;" in ipv4
    assert "address->afi = AF_INET;" in ipv4
    assert "packet->dst.afi != AF_INET" in ipv4


def test_ipv4_packet_destinations_set_family_before_send():
    hello = read("eigrpd/eigrp_hello.c")
    update = read("eigrpd/eigrp_update.c")
    packetizer = read("eigrpd/eigrp_packetizer.c")
    packet = read("eigrpd/eigrp_packet.c")

    assert "packet->dst.afi = AF_INET;" in hello
    assert "eigrp_addr_copy(&packet->dst, &nbr->src);" in update
    assert "eigrp_packet_multicast_reliable_enqueue" in packetizer
    assert "packet->dst.afi = AF_INET;" in packet


def test_no_custom_runtime_family_conversion_was_introduced():
    structs = read("eigrpd/eigrp_structs.h")
    start = structs.index("struct eigrp_addr {")
    end = structs.index("};", start)
    address = structs[start:end]

    assert "eigrp_address_family_t afi;" not in address
