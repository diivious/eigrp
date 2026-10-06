# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import re

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
PORTABLE = ROOT / "eigrp" / "code"
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


def test_eigrp_owned_threads_start_and_stop_logging_context():
    log_header = (PORTABLE / "eigrp_log.h").read_text()
    daemon = (PORTABLE / "eigrp.c").read_text()
    packet = (PORTABLE / "eigrp_packet.c").read_text()
    unix_runtime = (ROOT / "unix" / "code" / "eigrp_unix_sys.c").read_text()

    assert "void eigrp_log_start(void);" in log_header
    assert "void eigrp_log_stop(void);" in log_header

    process_start = daemon.index("static void *eigrp_packet_thread")
    process_end = daemon.index("bool eigrp_process_packet_submit", process_start)
    process_thread = daemon[process_start:process_end]
    assert process_thread.index("eigrp_log_start();") < process_thread.index("for (;;)")
    assert process_thread.rindex("eigrp_log_stop();") < process_thread.rindex("return NULL;")

    af_start = packet.index("static void *eigrp_instance_thread")
    af_end = packet.index("bool eigrp_instance_thread_start", af_start)
    af_thread = packet[af_start:af_end]
    assert af_thread.index("eigrp_log_start();") < af_thread.index("while (eigrp_instance_wait(eigrp))")
    assert af_thread.rindex("eigrp_log_stop();") < af_thread.rindex("return NULL;")

    unix_start = unix_runtime.index("static void *eigrp_unix_runtime_loop")
    unix_end = unix_runtime.index("void eigrp_sys_runtime_init", unix_start)
    unix_thread = unix_runtime[unix_start:unix_end]
    assert unix_thread.index("eigrp_log_start();") < unix_thread.index("for (;;)")
    assert unix_thread.rindex("eigrp_log_stop();") < unix_thread.rindex("return NULL;")


def test_vrid_wire_state_remains_portable_runtime_state():
    types = (PORTABLE / "eigrp_types.h").read_text()
    packet = (PORTABLE / "eigrp_packet.c").read_text()
    assert "eigrp_vrid_t vrid;" in types
    assert "eigrph->vrid = htons(eigrp->virt_router->vrid);" in packet
    assert "ntohs(eigrph->vrid) != ei->eigrp->virt_router->vrid" in packet


def test_packet_identity_demux_keeps_vrf_vrid_and_tid_explicit():
    sys_header = (PORTABLE / "eigrp_sys.h").read_text()
    structs = (PORTABLE / "eigrp_structs.h").read_text()
    ipv4 = (PORTABLE / "eigrp_ipv4.c").read_text()
    ipv6 = (PORTABLE / "eigrp_ipv6.c").read_text()
    tlv2 = (PORTABLE / "eigrp_tlv2.c").read_text()
    instance = (PORTABLE / "eigrp_instance.c").read_text()

    assert "eigrp_vrf_id_t ingress_vrf_id;" in sys_header
    assert "meta->ingress_vrf_id != eigrp->vrf_id" in ipv4
    assert "meta->ingress_vrf_id != eigrp->vrf_id" in ipv6
    assert "virt_router->vrid = vrid;" in instance
    assert "eigrp->virt_router = virt_router;" in instance
    assert "eigrp_topology_id_t topology_id;" in structs
    assert "tid != EIGRP_TOPOLOGY_ID_BASE" not in tlv2
    assert "route->topology_id = (eigrp_topology_id_t)tid;" in tlv2
    assert "route->topology_id != EIGRP_TOPOLOGY_ID_BASE" not in tlv2
