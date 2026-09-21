# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Portable source-level guards for topology-owned prefix representation.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def protocol_source() -> str:
    return "\n".join(path.read_text() for path in (ROOT / "eigrpd").glob("*.c"))


def test_topology_descriptors_use_native_eigrp_prefixes():
    structs = read("eigrpd/eigrp_structs.h")

    assert "eigrp_prefix_t destination;" in structs
    assert "eigrp_prefix_t dest;" in structs
    assert "struct prefix *destination;" not in structs
    assert "struct prefix dest;" not in structs


def test_topology_lookup_api_is_address_family_neutral():
    topology_h = read("eigrpd/eigrp_topology.h")

    assert "eigrp_topology_table_lookup(eigrp_table_t *table," in topology_h
    assert "const eigrp_prefix_t *prefix" in topology_h
    assert "eigrp_topology_table_lookup_ipv4" not in topology_h


def test_protocol_paths_do_not_allocate_frr_ipv4_prefixes():
    source = protocol_source()

    assert "prefix_ipv4_new(" not in source
    assert "eigrp_topology_prefix_import" not in source
    assert "eigrp_topology_prefix_export" not in source


def test_tlv_route_destination_uses_generic_prefix_codec():
    tlv1 = read("eigrpd/eigrp_tlv1.c")
    tlv2 = read("eigrpd/eigrp_tlv2.c")
    ipv4 = read("eigrpd/eigrp_ipv4.c")
    types = read("eigrpd/eigrp_types.h")

    for source in (tlv1, tlv2):
        assert "packet_prefix_decode" in source
        assert "packet_prefix_encode" in source
        assert "packet_route_prefix_decode" not in source
        assert "packet_route_prefix_encode" not in source

    assert "packet_route_prefix_decode" not in ipv4
    assert "packet_route_prefix_encode" not in ipv4
    assert "packet_route_prefix_decode" not in types
    assert "packet_route_prefix_encode" not in types


def test_deferred_descriptor_names_are_unchanged():
    structs = read("eigrpd/eigrp_structs.h")

    assert "typedef struct eigrp_prefix_descriptor" in structs
    assert "typedef struct eigrp_route_descriptor" in structs
