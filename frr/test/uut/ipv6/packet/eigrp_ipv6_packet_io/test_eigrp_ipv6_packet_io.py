# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def read(path: str) -> str:
    return (ROOT / path).read_text()


def test_public_packet_io_contract_is_af_neutral_and_eigrp_owned():
    sys_header = read("eigrp/code/eigrp_sys.h")

    assert "int eigrp_sys_packet_send(" in sys_header
    assert "bool eigrp_sys_packet_receive(" in sys_header
    assert "const eigrp_address_t *destination" in sys_header
    assert "eigrp_ifindex_t *ifindex" in sys_header
    assert "eigrp_address_t *source" in sys_header
    assert "eigrp_address_t *destination" in sys_header
    assert "eigrp_packet_rx_meta_t *meta" in sys_header
    for host_type in ("struct interface", "struct vrf", "struct event",
                      "struct stream", "struct sockaddr", "struct in6_addr"):
        assert host_type not in sys_header


def test_ipv4_and_ipv6_vectors_share_packet_io_boundary():
    ipv4 = read("eigrp/code/eigrp_ipv4.c")
    ipv6 = read("eigrp/code/eigrp_ipv6.c")

    assert "eigrp_sys_packet_send(" in ipv4
    assert "eigrp_sys_packet_receive(" in ipv4
    assert "eigrp_sys_packet_send(" in ipv6
    assert "eigrp_sys_packet_receive(" in ipv6
    assert "eigrp_sys_ipv6_packet_send(" not in ipv6
    assert "eigrp_sys_ipv6_packet_receive(" not in ipv6


def test_ipv6_receive_preserves_link_local_interface_context():
    ipv6 = read("eigrp/code/eigrp_ipv6.c")

    assert "eigrp_ifindex_t ifindex = 0;" in ipv6
    assert "eigrp_intf_lookup_by_ifindex(eigrp, ifindex)" in ipv6
    assert "IN6_IS_ADDR_LINKLOCAL(&source->ip.v6)" in ipv6
    assert "public_source.afi != EIGRP_AFI_IPV6" in ipv6
    assert "public_destination.afi != EIGRP_AFI_IPV6" in ipv6


def test_frr_keeps_existing_family_specific_socket_backends():
    common = read("frr/code/eigrp_southbound.c")
    ipv4 = read("frr/code/eigrp_frr_ipv4.c")
    ipv6 = read("frr/code/eigrp_frr_ipv6.c")

    assert "int eigrp_sys_ipv4_packet_send(" in ipv4
    assert "bool eigrp_sys_ipv4_packet_receive(" in ipv4
    assert "int eigrp_sys_ipv6_packet_send(" in ipv6
    assert "bool eigrp_sys_ipv6_packet_receive(" in ipv6
    assert "switch (destination->afi)" in common
    assert "eigrp_instance_afi(eigrp) == EIGRP_AFI_IPV6" in common


def test_frr_ipv6_backend_uses_link_local_source_and_required_socket_context():
    common = read("frr/code/eigrp_southbound.c")
    ipv6 = read("frr/code/eigrp_frr_ipv6.c")

    assert "vrf_socket(family, SOCK_RAW, IPPROTO_EIGRPIGP" in common
    assert "IPV6_UNICAST_HOPS" in ipv6
    assert "IPV6_MULTICAST_HOPS" in ipv6
    assert "IPV6_MULTICAST_LOOP" in ipv6
    assert "IPV6_PKTINFO" in ipv6
    assert "eigrp_southbound_ipv6_interface_linklocal" in ipv6
    assert "pktinfo->ipi6_addr = source" in ipv6
    assert "MSG_TRUNC | MSG_CTRUNC" in ipv6
    assert "IPV6_JOIN_GROUP" in ipv6
    assert "IPV6_LEAVE_GROUP" in ipv6
    assert 'inet_pton(AF_INET6, "ff02::a"' in ipv6
