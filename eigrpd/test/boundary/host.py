# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import re

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
PORTABLE = ROOT / "eigrpd" / "code"
PUBLIC_HEADERS = ("eigrp.h", "eigrp_cli.h", "eigrp_mgnt.h", "eigrp_rib.h", "eigrp_sys.h")


def test_public_contract_contains_no_frr_or_bird_native_types():
    text = "\n".join((PORTABLE / name).read_text() for name in PUBLIC_HEADERS)
    forbidden = (
        "struct vty", "struct interface", "struct event", "struct stream",
        "zapi_route", "zapi_nexthop", "struct lyd_node", "struct nb_cb",
        "struct proto", "struct rtable", "struct timer", "struct socket",
    )
    for token in forbidden:
        assert token not in text


def test_portable_sources_do_not_include_host_framework_headers():
    host_include = re.compile(
        r'^\s*#\s*include\s*[<"](?:zebra\.h|lib/|command\.h|vty\.h|'
        r'northbound\.h|frrevent\.h|workqueue\.h|zclient\.h|bird/)',
        re.MULTILINE,
    )
    for path in PORTABLE.glob("*.[ch]"):
        assert not host_include.search(path.read_text()), path.name


def test_vrid_wire_state_remains_portable_runtime_state():
    structs = (PORTABLE / "eigrp_structs.h").read_text()
    packet = (PORTABLE / "eigrp_packet.c").read_text()
    assert "uint16_t vrid;" in structs
    assert "eigrph->vrid = htons(eigrp->vrid);" in packet
    assert "ntohs(eigrph->vrid) != ei->eigrp->vrid" in packet


def test_packet_identity_demux_keeps_vrf_vrid_and_tid_explicit():
    sys_header = (PORTABLE / "eigrp_sys.h").read_text()
    structs = (PORTABLE / "eigrp_structs.h").read_text()
    ipv4 = (PORTABLE / "eigrp_ipv4.c").read_text()
    ipv6 = (PORTABLE / "eigrp_ipv6.c").read_text()
    tlv2 = (PORTABLE / "eigrp_tlv2.c").read_text()
    daemon = (PORTABLE / "eigrpd.c").read_text()

    assert "eigrp_vrf_id_t ingress_vrf_id;" in sys_header
    assert "meta->ingress_vrf_id != eigrp->vrf_id" in ipv4
    assert "meta->ingress_vrf_id != eigrp->vrf_id" in ipv6
    assert "eigrp->vrid = EIGRP_VRID_AF_BASE;" in daemon
    assert "eigrp_topology_id_t topology_id;" in structs
    assert "tid != EIGRP_TOPOLOGY_ID_BASE" in tlv2
    assert "route->topology_id = (eigrp_topology_id_t)tid;" in tlv2
    assert "route->topology_id != EIGRP_TOPOLOGY_ID_BASE" in tlv2
