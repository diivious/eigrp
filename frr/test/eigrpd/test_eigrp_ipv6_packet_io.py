# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path


def _southbound_source() -> str:
    # Project tree: eigrp/frr/test/eigrpd -> eigrp/frr/code/eigrp_southbound.c
    project_root = next((q for q in Path(__file__).resolve().parents if (q / "frr" / "code").is_dir()), None)
    project_dir = project_root / "frr" / "code" if project_root else Path()
    project_source = project_dir / "eigrp_southbound.c"
    if project_source.exists():
        return project_source.read_text() + (project_dir / "eigrp_frr_ipv6.c").read_text()

    # FRR UUT staging: frr/tests/eigrpd -> frr/eigrpd/eigrp_southbound.c
    frr_dir = Path(__file__).resolve().parents[2] / "eigrpd"
    return (frr_dir / "eigrp_southbound.c").read_text() + (frr_dir / "eigrp_frr_ipv6.c").read_text()


def test_frr_ipv6_raw_socket_and_multicast_contract():
    source = _southbound_source()

    assert "vrf_socket(family, SOCK_RAW, IPPROTO_EIGRPIGP" in source
    assert "EIGRP_AFI_IPV6" in source
    assert "IPV6_UNICAST_HOPS" in source
    assert "IPV6_MULTICAST_HOPS" in source
    assert "IPV6_MULTICAST_LOOP" in source
    assert "IPV6_MULTICAST_IF" in source
    assert "IPV6_JOIN_GROUP" in source
    assert "IPV6_LEAVE_GROUP" in source
    assert 'inet_pton(AF_INET6, "ff02::a"' in source


def test_frr_ipv6_send_receive_preserves_link_scope_and_pktinfo():
    source = _southbound_source()

    assert "eigrp_southbound_ipv6_interface_linklocal" in source
    assert "IN6_IS_ADDR_LINKLOCAL" in source
    assert "pktinfo->ipi6_ifindex = ifindex" in source
    assert "pktinfo->ipi6_addr = source" in source
    assert "sa.sin6_scope_id = ifindex" in source
    assert "cmsg->cmsg_type == IPV6_PKTINFO" in source
    assert "MSG_TRUNC | MSG_CTRUNC" in source
    assert "*ifindex = pktinfo->ipi6_ifindex" in source
    assert "memcpy(destination->bytes, &pktinfo->ipi6_addr" in source
