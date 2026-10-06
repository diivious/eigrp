# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def read(path):
    return (ROOT / path).read_text()


def test_ipv6_zebra_redistribution_is_af_scoped_and_subscribed():
    zebra = read("frr/code/eigrp_zebra.c")
    assert "case EIGRP_AFI_IPV6:" in zebra
    assert "return AFI_IP6;" in zebra
    assert "eigrp_instance_afi(state->eigrp)" in zebra
    assert "!= route.prefix.address.afi" in zebra
    assert "zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD" in zebra
    assert "zclient_redistribute(ZEBRA_REDISTRIBUTE_DELETE" in zebra


def test_ipv6_external_candidate_uses_default_metric_and_policy_boundary():
    redistribute = read("eigrp/code/eigrp_redistribute.c")
    metric = read("eigrp/code/eigrp_metric.c")
    assert "eigrp_metric_default(af, &values)" in redistribute
    assert "eigrp_sys_redistribute_route_map_evaluate" in redistribute
    assert "EIGRP_FILTER_DECISION_DENY" in redistribute
    assert "eigrp_topology_redistributed_route_update" in redistribute
    assert "eigrp_topology_redistributed_route_remove" in redistribute
    assert "default_metric_configured" in metric


def test_ipv6_external_route_is_encoded_by_both_tlv_families():
    ipv6 = read("eigrp/code/eigrp_ipv6.c")
    tlv1 = read("eigrp/code/eigrp_tlv1.c")
    tlv2 = read("eigrp/code/eigrp_tlv2.c")
    assert "vectors->classic_external_tlv_type = EIGRP_TLV_IPv6_EXT" in ipv6
    assert "eigrp_tlv1_external_encode(pkt, &route->extdata)" in tlv1
    assert "route->type == EIGRP_TLV_MP_EXT || route->type == EIGRP_EXT" in tlv2
    assert "eigrp_tlv2_external_encode(eigrp, pkt, &route->extdata)" in tlv2
    assert "eigrp_tlv2_addpath_encode(eigrp, pkt, route, external)" in tlv2


def test_route_map_permit_deny_and_set_tag_stay_in_frr_policy_adapter():
    policy = read("frr/code/eigrp_policy.c")
    core = read("eigrp/code/eigrp_redistribute.c")
    assert "route_map_lookup_by_name(name)" in policy
    assert "route_map_apply(route_map, &host_prefix, NULL)" in policy
    assert "eigrp_frr_prefix_export(&route->prefix, &host_prefix)" in policy
    assert "struct route_map" not in core
    assert "route_map_lookup_by_name" not in core


def test_changed_candidate_denied_by_policy_withdraws_stale_external_path():
    core = read("eigrp/code/eigrp_redistribute.c")
    assert "Zebra uses ADD for both first appearance and changed route snapshots" in core
    assert "configured route-map that is absent" not in core  # wording belongs in spec
    assert "decision == EIGRP_FILTER_DECISION_DENY" in core
    assert "eigrp_topology_redistributed_route_remove(runtime, route)" in core


def test_removing_redistribution_source_withdraws_existing_external_paths():
    redistribute = read("eigrp/code/eigrp_redistribute.c")
    topology = read("eigrp/code/eigrp_topology.c")
    assert "eigrp_topology_redistributed_source_remove" in redistribute
    assert "eigrp_topology_redistributed_source_remove" in topology
    assert "route->extdata.protocol != protocol" in topology
    assert "route->extdata.as != external_as" in topology
    assert "eigrp_topology_redistributed_route_remove" in topology
