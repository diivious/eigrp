# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for the final Step 5 RIB/southbound boundary sweep.

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def test_portable_code_does_not_call_or_include_zebra_rib_adapter_directly():
    forbidden = (
        'eigrp_zebra_',
        '#include "eigrpd/eigrp_zebra.h"',
        '#include "eigrp_zebra.h"',
        '#include "zclient.h"',
    )

    for path in sorted((ROOT / "eigrpd").glob("*.[ch]")):
        text = path.read_text()
        for token in forbidden:
            assert token not in text, (
                f"{path.relative_to(ROOT)} reaches directly into FRR Zebra/RIB: {token}"
            )


def test_portable_code_does_not_expose_zebra_rib_types_or_route_constants():
    host_rib_patterns = (
        r"\bstruct\s+zclient\b",
        r"\bstruct\s+zapi_route\b",
        r"\bstruct\s+zapi_nexthop\b",
        r"\bzclient_[A-Za-z0-9_]+\s*\(",
        r"\bZEBRA_ROUTE_(?:ADD|DELETE|EIGRP|MAX)\b",
        r"\bNEXTHOP_TYPE_[A-Z0-9_]+\b",
    )

    for path in sorted((ROOT / "eigrpd").glob("*.[ch]")):
        text = path.read_text()
        for pattern in host_rib_patterns:
            assert not re.search(pattern, text), (
                f"{path.relative_to(ROOT)} exposes host RIB representation {pattern}"
            )


def test_route_install_remove_cross_eigrp_southbound_contract():
    southbound_h = read("eigrpd/eigrp_southbound.h")
    topology = read("eigrpd/eigrp_topology.c")
    southbound = read("frr/eigrp_southbound.c")

    assert "typedef struct eigrp_southbound_nexthop" in southbound_h
    assert "eigrp_ifindex_t ifindex;" in southbound_h
    assert "eigrp_address_t gateway;" in southbound_h
    assert "eigrp_southbound_route_install(" in southbound_h
    assert "eigrp_southbound_route_remove(" in southbound_h

    assert "eigrp_southbound_route_install(" in topology
    assert "eigrp_southbound_route_remove(" in topology
    assert "eigrp_zebra_route_" not in topology

    assert "return eigrp_zebra_route_install" in southbound
    assert "return eigrp_zebra_route_remove" in southbound


def test_zebra_adapter_builds_host_rib_objects_from_portable_snapshot():
    zebra = read("frr/eigrp_zebra.c")
    zebra_h = read("frr/eigrp_zebra.h")

    assert "struct zapi_route api;" in zebra
    assert "struct zapi_nexthop *api_nh;" in zebra
    assert "zclient_route_send(ZEBRA_ROUTE_ADD" in zebra
    assert "zclient_route_send(ZEBRA_ROUTE_DELETE" in zebra
    assert "const eigrp_southbound_nexthop_t *nexthops" in zebra_h
    assert "struct list *successors" not in zebra_h
    assert "eigrp_route_descriptor_t" not in zebra_h


def test_host_redistribution_bookkeeping_is_not_core_instance_state():
    structs = read("eigrpd/eigrp_structs.h")
    zebra = read("frr/eigrp_zebra.c")

    assert "ZEBRA_ROUTE_MAX" not in structs
    assert "dmetric[" not in structs
    assert "redistribute;" not in structs

    assert "struct eigrp_zebra_instance_state" in zebra
    assert "eigrp_metrics_t dmetric[ZEBRA_ROUTE_MAX];" in zebra
    assert "redistribute_count" in zebra


def test_zebra_lifecycle_is_reached_through_southbound_from_portable_code():
    main = read("frr/eigrp_main.c")
    daemon = read("eigrpd/eigrpd.c")
    header = read("eigrpd/eigrp_southbound.h")

    assert "eigrp_southbound_rib_init();" in main
    assert "eigrp_southbound_rib_finish();" in daemon
    assert "eigrp_southbound_rib_instance_delete(eigrp);" in daemon
    assert "eigrp_zebra_init" not in main
    assert "eigrp_zebra_stop" not in daemon
    assert "void eigrp_southbound_rib_init(void);" in header
    assert "void eigrp_southbound_rib_finish(void);" in header


def test_obsolete_host_route_type_refresh_api_is_removed():
    network_h = read("eigrpd/eigrp_network.h")
    network_c = read("eigrpd/eigrp_network.c")

    assert "eigrp_external_routes_refresh" not in network_h
    assert "eigrp_external_routes_refresh" not in network_c


def test_frr_daemon_bootstrap_declares_vrf_yang_dependency_explicitly():
    main = read("frr/eigrp_main.c")
    zebra_stub = read("test/build/include/zebra.h")
    vrf_stub = read("test/build/include/vrf.h")

    assert '#include "vrf.h"' in main
    assert "frr_vrf_info" not in zebra_stub
    assert "frr_vrf_info" in vrf_stub


def test_gr_update_runtime_api_does_not_receive_frr_vty_objects():
    update = read("eigrpd/eigrp_update.c")
    packet_h = read("eigrpd/eigrp_packet.h")

    for signature in (
        "eigrp_update_send_GR(",
        "eigrp_update_send_interface_GR(",
        "eigrp_update_send_process_GR(",
    ):
        start = packet_h.index(signature)
        declaration = packet_h[start:packet_h.index(";", start) + 1]
        assert "struct vty" not in declaration

    assert "vty_time_print(" not in update
    assert "vty_out(" not in update


def test_host_runtime_shutdown_terminates_at_southbound_boundary():
    daemon = read("eigrpd/eigrpd.c")
    southbound_h = read("eigrpd/eigrp_southbound.h")
    southbound = read("frr/eigrp_southbound.c")
    zebra_stub = read("test/build/include/zebra.h")
    vrf_stub = read("test/build/include/vrf.h")
    libfrr_stub = read("test/build/include/libfrr.h")

    assert "eigrp_southbound_runtime_finish();" in daemon
    assert "vrf_terminate(" not in daemon
    assert "frr_fini(" not in daemon
    assert "void eigrp_southbound_runtime_finish(void);" in southbound_h
    assert '#include "vrf.h"' in southbound
    assert '#include "lib/libfrr.h"' in southbound
    assert "vrf_terminate();" in southbound
    assert "frr_fini();" in southbound

    assert "vrf_terminate" not in zebra_stub
    assert "vrf_terminate" in vrf_stub
    assert "frr_fini" not in zebra_stub
    assert "frr_fini" in libfrr_stub
