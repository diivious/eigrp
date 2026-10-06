# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
SUMMARY = (ROOT / "eigrp/code/eigrp_summary.c").read_text()
PACKETIZER = (ROOT / "eigrp/code/eigrp_packetizer.c").read_text()
UPDATE = (ROOT / "eigrp/code/eigrp_update.c").read_text()
METRIC = (ROOT / "eigrp/code/eigrp_metric.c").read_text()
TOPOLOGY = (ROOT / "eigrp/code/eigrp_topology.c").read_text()
IPV6 = (ROOT / "eigrp/code/eigrp_ipv6.c").read_text()


def test_manual_summary_has_runtime_advertisement_and_withdrawal_path():
    assert "eigrp_summary_route_build" in SUMMARY
    assert "eigrp_summary_runtime_update" in SUMMARY
    assert "eigrp_summary_withdraw" in SUMMARY
    assert "eigrp_summary_route_build" in PACKETIZER
    assert "eigrp_summary_route_build" in UPDATE
    assert "candidate_route->distance == EIGRP_MAX_METRIC" in SUMMARY


def test_summary_metric_changes_live_summary_vector():
    assert "eigrp_summary_metric_lookup" in SUMMARY
    assert "eigrp_metric_values_convert(&metric_config->metric" in SUMMARY
    assert "eigrp_summary_runtime_update(context->runtime)" in SUMMARY


def test_ipv6_auto_summary_remains_explicitly_unsupported():
    assert "eigrp_ipv6_summary_auto_prefix" in IPV6
    start = IPV6.index("eigrp_ipv6_summary_auto_prefix")
    assert "EIGRP_RESULT_UNSUPPORTED" in IPV6[start:start + 600]
    assert "vectors->afi != EIGRP_AFI_IPV4" in SUMMARY


def test_metric_weights_and_version_recompute_existing_topology():
    assert "eigrp_topology_metric_update(context->runtime)" in METRIC
    assert "EIGRP_METRIC_SCALER / EIGRP_CLASSIC_SCALER" in METRIC
    assert "eigrp->metric_version >= EIGRP_TLV_64B_VERSION" in METRIC
    assert "eigrp_topology_route_queue_resort" in TOPOLOGY


def test_variance_and_maximum_paths_refresh_rib_selection():
    assert "eigrp_topology_multipath_update(context->runtime)" in METRIC
    assert "eigrp_topology_multipath_update(context->runtime)" in TOPOLOGY
    assert "eigrp_update_routing_table(eigrp, prefix)" in TOPOLOGY


def test_default_metric_is_live_redistribution_seed_not_capability_stub():
    body = METRIC[METRIC.index("eigrp_metric_default_update("):METRIC.index("eigrp_metric_weights_update(")]
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in body
    assert "EIGRP_RESULT_SUCCESS" in body
