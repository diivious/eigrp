# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path


def _project_root():
    return next((q for q in Path(__file__).resolve().parents if (q / "eigrpd" / "code").is_dir()), None)


def _portable_source(name: str) -> str:
    here = Path(__file__).resolve()
    project_root = _project_root()
    if project_root:
        return (project_root / "eigrpd" / "code" / name).read_text() + (project_root / "frr" / "code" / "eigrp_frr_ipv6.c").read_text()
    staged = here.parents[2] / "eigrpd"
    return (staged / name).read_text() + (staged / "eigrp_frr_ipv6.c").read_text()


def _southbound_source() -> str:
    here = Path(__file__).resolve()
    project_root = _project_root()
    if project_root:
        frr_code = project_root / "frr" / "code"
        return (frr_code / "eigrp_southbound.c").read_text() + (frr_code / "eigrp_frr_ipv6.c").read_text()
    staged = here.parents[2] / "eigrpd"
    return (staged / "eigrp_southbound.c").read_text() + (staged / "eigrp_frr_ipv6.c").read_text()


def test_ipv6_hello_frr_source_selection_and_capture_fields():
    southbound = _southbound_source()
    hello = _portable_source("eigrp_hello.c")

    assert "eigrp_southbound_ipv6_interface_linklocal" in southbound
    assert "pktinfo->ipi6_addr = source" in southbound
    assert "pktinfo->ipi6_ifindex = ifindex" in southbound
    assert 'inet_pton(AF_INET6, "ff02::a", &destination.ip.v6)' in hello
    assert "EIGRP_TLV_PARAMETER" in hello
    assert "EIGRP_TLV_SW_VERSION" in hello


def test_ipv6_hello_debug_path_is_address_family_aware():
    packet = _portable_source("eigrp_packet.c")
    debug = _portable_source("eigrp_debug.c")

    assert "eigrp_packet_addr_text" in packet
    assert "AF_INET6" in debug
    assert "peer termination neighbor %s" in debug
