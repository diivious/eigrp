# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def test_release_2_wide_metrics_is_default():
    header = read("eigrpd/eigrpd.h")
    runtime = read("eigrpd/eigrpd.c")
    assert "#define EIGRP_MAJOR_VERSION 2" in header
    assert "eigrp->metric_version = EIGRP_MAJOR_VERSION;" in runtime


def test_common_selection_policy_prefers_highest_mutual_version():
    metric = read("eigrpd/eigrp_metric.c")
    hello = read("eigrpd/eigrp_hello.c")
    assert "eigrp_metric_version_select" in metric
    assert "local_version >= EIGRP_TLV_64B_VERSION" in metric
    assert "peer_version >= EIGRP_TLV_64B_VERSION" in metric
    assert "eigrp_metric_version_select(ei->eigrp, nbr->tlv_rel_major)" in hello
    assert "eigrp ? eigrp->metric_version : EIGRP_MAJOR_VERSION" in hello


def test_32bit_configuration_has_real_metric_targets_and_default_reset():
    metric_h = read("eigrpd/eigrp_metric.h")
    metric = read("eigrpd/eigrp_metric.c")
    cli_h = read("eigrpd/eigrp_cli.h")
    for source in (metric_h, cli_h):
        assert "eigrp_metric_version_set" in source
        assert "eigrp_metric_version_reset" in source
    assert "context->runtime->metric_version = EIGRP_TLV_32B_VERSION;" in metric
    assert "context->runtime->metric_version = EIGRP_MAJOR_VERSION;" in metric
    assert "eigrp_neighbor_codec_refresh(context->runtime);" in metric


def test_mixed_peer_aggregate_policy_is_preserved():
    interface = read("eigrpd/eigrp_interface.c")
    packet = read("eigrpd/eigrp_packet.c")
    assert "ei->tlv1_peer_count && ei->tlv2_peer_count" in interface
    assert "ei->encoder = eigrp_packet_encoder_both;" in interface
    assert "eigrp->tlv1_codec.encoder" in packet
    assert "eigrp->tlv2_codec.encoder" in packet


def test_named_cli_retains_and_writes_32bit_override_for_both_afs():
    cli = read("frr/eigrp_cli_named.c")
    nb = read("frr/eigrp_northbound.c")
    yang = read("frr/patch/frr-eigrp-yang.patch")
    assert '"metric version 32bit"' in cli
    assert '"no metric version 32bit"' in cli
    assert '"./metric-version-32bit"' in cli
    assert 'vty_out(vty, "   metric version 32bit\\n")' in cli
    assert "eigrp_metric_version_set(&context)" in nb
    assert "eigrp_metric_version_reset(&context)" in nb
    assert "leaf metric-version-32bit { type empty; }" in yang
    # The topology child resolver is AF-neutral and is used by both callbacks.
    assert nb.count("eigrpd_named_topology_child_context(args->dnode") >= 2
