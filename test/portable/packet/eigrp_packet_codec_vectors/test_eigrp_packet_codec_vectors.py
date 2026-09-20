# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for the TLV codec vector design.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / "eigrpd" / path).read_text()


def all_production_source() -> str:
    chunks = []
    for path in ROOT.rglob("*"):
        if path.suffix not in {".c", ".h"}:
            continue
        if any(part in {".git", "test"} for part in path.parts):
            continue
        chunks.append(path.read_text())
    return "\n".join(chunks)


def test_instance_owns_tlv_codecs():
    structs = read("eigrp_structs.h")

    assert "eigrp_tlv_codec_t tlv1_codec;" in structs
    assert "eigrp_tlv_codec_t tlv2_codec;" in structs


def test_neighbor_owns_tlv_version_and_unicast_vectors():
    neighbor = read("eigrp_neighbor.h")

    assert "uint8_t tlv_version;" in neighbor
    assert "eigrp_packet_decoder_t decoder;" in neighbor
    assert "eigrp_packet_encoder_t encoder;" in neighbor
    assert "bound_interface_codec" not in all_production_source()


def test_interface_owns_aggregate_encoder_and_peer_counts():
    structs = read("eigrp_structs.h")

    assert "uint16_t tlv1_peer_count;" in structs
    assert "uint16_t tlv2_peer_count;" in structs
    assert "eigrp_packet_encoder_t encoder;" in structs


def test_runtime_path_uses_single_interface_encoder_vector():
    source = all_production_source()
    packetizer = read("eigrp_packetizer.c")

    assert "ei->encoder[" not in source
    assert "encoder->next" not in source
    assert "builder->encoder = nbr ? nbr->encoder : ei->encoder;" in packetizer
    assert "eigrp_packet_route_encode_append(" in packetizer


def test_packet_owns_safe_and_both_vectors():
    packet = read("eigrp_packet.c")

    assert "eigrp_packet_encoder_safe" in packet
    assert "eigrp_packet_decoder_safe" in packet
    assert "eigrp_packet_encoder_both" in packet
    assert "eigrp->tlv1_codec.encoder" in packet
    assert "eigrp->tlv2_codec.encoder" in packet


def test_tlv_modules_export_init_and_bind_without_leaking_wire_types():
    tlv1_h = read("eigrp_tlv1.h")
    tlv2_h = read("eigrp_tlv2.h")

    for header in (tlv1_h, tlv2_h):
        assert "eigrp_tlv_codec_t" in header
        assert "neighbor_bind" in header
        assert "interface_bind" in header
        assert "struct eigrp_tlv1" not in header
        assert "struct eigrp_tlv2" not in header


def test_route_tlv_codecs_delegate_address_family_wire_details():
    tlv1 = read("eigrp_tlv1.c")
    tlv2 = read("eigrp_tlv2.c")

    for source in (tlv1, tlv2):
        assert "packet_prefix_decode" in source
        assert "packet_prefix_encode" in source
        assert "packet_route_prefix_decode" not in source
        assert "packet_route_prefix_encode" not in source
        assert "AF_INET" not in source
        assert "prefix4" not in source
        assert "EIGRP_TLV_IPv4" not in source
        assert "EIGRP_AF_IPv4" not in source

    assert "packet_address_decode" in tlv1
    assert "packet_address_encode" in tlv1
    assert "classic_internal_tlv_type" in tlv1
    assert "classic_external_tlv_type" in tlv1
    assert "multiprotocol_afi" in tlv2


def test_address_family_vector_owns_route_family_selectors():
    types = read("eigrp_types.h")
    ipv4 = read("eigrp_ipv4.c")
    ipv6 = read("eigrp_ipv6.c")

    assert "uint8_t packet_address_bytes;" in types
    assert "uint16_t classic_internal_tlv_type;" in types
    assert "uint16_t classic_external_tlv_type;" in types
    assert "uint16_t multiprotocol_afi;" in types
    assert "packet_prefix_decode" in types
    assert "packet_prefix_encode" in types
    assert "packet_route_prefix_decode" not in types
    assert "packet_route_prefix_encode" not in types

    assert "classic_internal_tlv_type = EIGRP_TLV_IPv4_INT" in ipv4
    assert "classic_external_tlv_type = EIGRP_TLV_IPv4_EXT" in ipv4
    assert "multiprotocol_afi = EIGRP_AF_IPv4" in ipv4

    assert "classic_internal_tlv_type = EIGRP_TLV_IPv6_INT" in ipv6
    assert "classic_external_tlv_type = EIGRP_TLV_IPv6_EXT" in ipv6
    assert "multiprotocol_afi = EIGRP_AF_IPv6" in ipv6
