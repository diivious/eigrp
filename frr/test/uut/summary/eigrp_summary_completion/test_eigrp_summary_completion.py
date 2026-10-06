# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
SUMMARY = (ROOT / "eigrp/code/eigrp_summary.c").read_text()
PACKETIZER = (ROOT / "eigrp/code/eigrp_packetizer.c").read_text()
UPDATE = (ROOT / "eigrp/code/eigrp_update.c").read_text()
SYS = (ROOT / "eigrp/code/eigrp_sys.h").read_text()
POLICY = (ROOT / "frr/code/eigrp_policy.c").read_text()


def body(source, name):
    start = source.index(name + "(")
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        depth += source[i] == "{"
        depth -= source[i] == "}"
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError(name)


def test_leak_map_uses_portable_policy_boundary_and_fails_closed():
    leak = body(SUMMARY, "eigrp_summary_specific_leak")
    adapter = body(POLICY, "eigrp_policy_summary_leak_map_evaluate")
    assert "eigrp_sys_summary_leak_map_evaluate" in SYS
    assert "eigrp_sys_summary_leak_map_evaluate" in leak
    assert "EIGRP_FILTER_DECISION_DENY" in leak
    assert "route_map_lookup_by_name" in adapter
    assert "route_map_apply" in adapter
    assert "struct route_map" not in SUMMARY


def test_packetizer_emits_aggregate_and_permitted_specific_on_same_path():
    assert "eigrp_summary_specific_leak" in PACKETIZER
    assert "eigrp_packetizer_builder_route_add(&builder, route)" in PACKETIZER
    assert "eigrp_summary_specific_leak" in UPDATE
    assert "eigrp_packet_route_encode_append" in UPDATE


def test_policy_change_refreshes_existing_summary_advertisements():
    hook = body(POLICY, "eigrp_policy_route_map_changed")
    assert "eigrp_summary_policy_update_all" in hook
    refresh = body(SUMMARY, "eigrp_summary_policy_update_all")
    assert "eigrp_summary_runtime_update(runtime)" in refresh


def test_ipv4_auto_summary_is_runtime_enabled_at_classful_boundary():
    auto = body(SUMMARY, "eigrp_summary_auto_update")
    match = body(SUMMARY, "eigrp_summary_auto_match")
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in auto
    assert "eigrp_summary_runtime_update(context->runtime)" in auto
    assert "summary_auto_prefix(component, summary)" in match
    assert "summary_auto_prefix(&ei->address, &interface_major)" in match
    assert "!eigrp_summary_prefix_match(summary, &interface_major)" in match


def test_ipv6_auto_summary_remains_unsupported():
    auto = body(SUMMARY, "eigrp_summary_auto_update")
    assert "vectors->afi != EIGRP_AFI_IPV4" in auto
    assert "EIGRP_RESULT_UNSUPPORTED" in auto
