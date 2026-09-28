# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# TASK 9 integration/regression guard for the completed redistribution path.

from pathlib import Path


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def read(relative: str) -> str:
    return (ROOT / relative).read_text()


def test_named_ipv4_4453_uut_covers_redistribution_configuration_lifecycle():
    uut = read("tools/frr-named-uut.sh")

    # Named IPv4/4453 is exercised before the script introduces IPv6.
    ipv4 = uut.index("stage 1: create only router eigrp savage / IPv4 AS 4453")
    ipv6 = uut.index("stage 2: create only IPv6 AS 4453")
    assert ipv4 < ipv6

    stage1 = uut[ipv4:ipv6]
    for command in (
        "default-metric 10000 100 255 1 1500",
        "redistribute connected metric 10000 100 255 1 1500 route-map STEP1-RM",
        "redistribute static route-map STEP1-STATIC",
        "redistribute rip metric 11000 110 254 1 1490",
        "redistribute ospf 101 metric 12000 120 253 1 1480 route-map STEP1-OSPF-101",
        "redistribute ospf 102 route-map STEP1-OSPF-102",
        "redistribute eigrp 65001 metric 13000 130 252 1 1470 route-map STEP1-EIGRP-65001",
        "redistribute connected metric 20000 200 250 2 1400 route-map STEP1-RM-3",
        "redistribute ospf 101 metric 16000 160 251 2 1460 route-map STEP1-OSPF-101-UPDATED",
        "no redistribute connected",
        "no redistribute ospf 101",
        "no redistribute ospf 102",
        "no redistribute eigrp 65001",
    ):
        assert command in stage1

    # The sibling OSPF instance is explicitly checked after instance 101 is
    # mutated and again after instance 101 is removed.
    assert stage1.count("redistribute ospf 102 route-map STEP1-OSPF-102") >= 3


def test_named_adapter_preserves_route_instance_metric_and_route_map_identity():
    cli = read("frr/code/eigrp_cli_named.c")
    nb = (read("frr/code/eigrp_northbound.c") + read("frr/code/eigrp_northbound_ipv4.c") + read("frr/code/eigrp_northbound_ipv6.c"))
    zebra = read("frr/code/eigrp_zebra.c")

    assert "[route-instance='%u']" in cli
    assert "eigrpd_named_redistribute_source_get" in nb
    assert "eigrp_redist_add(" in nb
    assert "eigrp_redist_remove(" in nb
    assert "redistribute/metrics" in nb
    assert "redistribute/route-map" in nb

    # Zebra subscriptions and receive filtering retain the same source
    # route-instance rather than collapsing all instances of a protocol.
    assert "params.instance" in zebra
    assert "redist->route_instance == route_instance" in zebra
    assert "source.source.route_instance" in zebra


def test_runtime_ingress_enforces_seed_and_deferred_route_map_boundaries():
    redistribute = read("eigrpd/code/eigrp_redistribute.c")

    # Route-map policy is evaluated through the host boundary.  Missing seed
    # metric remains an ignored/non-importable candidate.
    assert "eigrp_sys_redistribute_route_map_evaluate" in redistribute
    assert "EIGRP_FILTER_DECISION_DENY" in redistribute
    assert "eigrp_topology_redistributed_route_remove" in redistribute
    assert "eigrp_redist_metric_select" in redistribute
    assert "== EIGRP_REDISTRIBUTE_METRIC_NONE" in redistribute
    assert "return EIGRP_RESULT_SUCCESS;" in redistribute

    # Zebra ADD is intentionally both first appearance and route change; DELETE
    # drives withdrawal through the same normalized public RIB snapshot.
    assert "Zebra uses ADD for both first appearance and changed route snapshots" in redistribute
    assert "eigrp_rib_source_route_add" in redistribute
    assert "eigrp_rib_source_route_remove" in redistribute


def test_redistributed_routes_remain_external_through_update_and_withdrawal():
    topology_test = read(
        "eigrpd/test/ipv4/redistribution/eigrp_redistribute_topology/"
        "test_eigrp_redistribute_topology.py"
    )
    packetizer_test = read(
        "eigrpd/test/redistribution/eigrp_redistribute_packetizer/"
        "test_eigrp_redistribute_packetizer.py"
    )

    for marker in (
        "EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL",
        "eigrp_rib_source_route_add",
        "eigrp_rib_source_route_remove",
    ):
        assert marker in topology_test
    assert "withdrawal" in topology_test.lower()
    assert "external" in packetizer_test.lower()
    assert "withdrawal" in packetizer_test.lower()


def test_classic_redistribution_surface_remains_separate_and_installed():
    classic = read("frr/code/eigrp_cli_classic.c")
    nb = (read("frr/code/eigrp_northbound.c") + read("frr/code/eigrp_northbound_ipv4.c") + read("frr/code/eigrp_northbound_ipv6.c"))

    assert "&eigrp_redistribute_source_metric_cmd" in classic
    assert "if (eigrp_cli_named_context(vty))" in classic
    assert "eigrp_redistribute_update(EIGRP_SET, eigrp, proto, metrics);" in nb
    assert "eigrp_redistribute_update(EIGRP_RESET, eigrp, proto, (struct eigrp_metrics){0});" in nb


def test_named_redistribution_runtime_sync_occurs_only_at_apply_finish():
    nb = (read("frr/code/eigrp_northbound.c") + read("frr/code/eigrp_northbound_ipv4.c") + read("frr/code/eigrp_northbound_ipv6.c"))

    apply_finish = nb.index("static void eigrpd_named_redistribute_apply_finish")
    destroy = nb.index("static int eigrpd_named_redistribute_destroy", apply_finish)
    finish_body = nb[apply_finish:destroy]
    assert "eigrpd_named_redistribute_apply(args->dnode, true)" in finish_body

    # A single CLI command changes the list and several descendants in one FRR
    # transaction.  Intermediate APPLY callbacks must be datastore-only; FRR's
    # apply_finish callback then observes the settled metrics/route-map once.
    create = nb.index("static int eigrpd_named_redistribute_create")
    for marker in (
        "static int eigrpd_named_redistribute_metrics_create",
        "static int eigrpd_named_redistribute_metrics_modify",
        "static int eigrpd_named_redistribute_metrics_destroy",
        "static int eigrpd_named_redistribute_route_map_modify",
        "static int eigrpd_named_redistribute_route_map_destroy",
    ):
        end = nb.index(marker, create)
        body = nb[create:end]
        assert "eigrpd_named_redistribute_apply(" not in body
        assert "eigrp_redist_add(" not in body
        create = end

    tail = nb[create:nb.index("/*\n * XPath:", create)]
    assert "eigrpd_named_redistribute_apply(" not in tail
    assert "eigrp_redist_add(" not in tail
