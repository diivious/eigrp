# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd").is_dir())


def read(path):
    return (ROOT / path).read_text()


def test_ipv6_hello_wire_destination_and_link_local_source_contract():
    hello = read("eigrpd/eigrp_hello.c")
    southbound = read("frr/eigrp_southbound_ipv6.c")
    ipv6 = read("eigrpd/eigrp_ipv6.c")

    assert 'inet_pton(AF_INET6, "ff02::a", &destination.ip.v6)' in hello
    assert "eigrp_southbound_ipv6_interface_linklocal" in southbound
    assert "pktinfo->ipi6_addr = source" in southbound
    assert "IN6_IS_ADDR_LINKLOCAL(&source->ip.v6)" in ipv6


def test_ipv6_hello_carries_parameter_hold_and_metric_version_advertisement():
    hello = read("eigrpd/eigrp_hello.c")

    assert "EIGRP_TLV_PARAMETER" in hello
    assert "ei->eigrp->k_values[5]" in hello
    assert "ei->params.v_wait" in hello
    assert "EIGRP_TLV_SW_VERSION" in hello
    assert "eigrp->metric_version" in hello
    assert "eigrp_metric_version_select" in hello


def test_ipv6_hello_receive_decodes_but_does_not_start_adjacency():
    hello = read("eigrpd/eigrp_hello.c")

    assert "adjacency_start_allowed" in hello
    assert "if (!adjacency_start_allowed)" in hello
    assert "eigrp_update_send_init(eigrp, nbr);" in hello
    assert "if (ntohl(eigrph->ack) == 0 && nbr && adjacency_start_allowed)" in hello


def test_hello_rejects_short_known_tlvs_and_requires_parameter_tlv():
    hello = read("eigrpd/eigrp_hello.c")

    assert "length < EIGRP_TLV_PARAMETER_LEN" in hello
    assert "length < EIGRP_TLV_SW_VERSION_LEN" in hello
    assert "length < EIGRP_TLV_PEER_TERMINATION_LEN" in hello
    assert "if (!parameter_seen)" in hello


def test_goodbye_and_peer_termination_are_address_family_aware():
    hello = read("eigrpd/eigrp_hello.c")

    assert "eigrp_hello_goodbye" in hello
    assert "Interface Goodbye received" in hello
    assert "eigrp_peer_termination_contains" in hello
    assert "packet_address_bytes" in hello
    assert "packet_address_encode" in hello
    assert "nbr_addr->afi == AF_INET6" in hello
