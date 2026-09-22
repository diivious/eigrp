# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for Step 5 policy/filter boundary.

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def function_body(source: str, name: str) -> str:
    pattern = rf"(?:^|\n)(?:static\s+)?[^\n;{{]*(?:\n[ \t]*)?\b{name}\("
    for match in re.finditer(pattern, source):
        start = match.start()
        brace = source.find("{", start)
        semicolon = source.find(";", start)
        if brace < 0 or (semicolon >= 0 and semicolon < brace):
            continue
        depth = 0
        for index in range(brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
    raise AssertionError(f"missing function definition {name}")


def test_portable_headers_do_not_expose_frr_policy_objects_or_callbacks():
    forbidden = (
        r"\bstruct\s+access_list\b",
        r"\bstruct\s+prefix_list\b",
        r"\bstruct\s+route_map\b",
        r"\bstruct\s+distribute_ctx\b",
        r"\bstruct\s+distribute\b",
        r"\bstruct\s+if_rmap\b",
        r"\broute_map_object_t\b",
        r"\bstruct\s+route_map_index\b",
    )

    for header in sorted((ROOT / "eigrpd").glob("*.h")):
        text = header.read_text()
        for pattern in forbidden:
            assert not re.search(pattern, text), (
                f"{header.relative_to(ROOT)} exposes FRR policy type {pattern}"
            )


def test_portable_runtime_structs_store_policy_names_not_host_objects():
    types = read("eigrpd/eigrp_types.h")
    structs = read("eigrpd/eigrp_structs.h")

    assert "typedef struct eigrp_filter_runtime_state" in types
    assert "char *access_list[EIGRP_FILTER_MAX];" in types
    assert "char *prefix_list[EIGRP_FILTER_MAX];" in types
    assert "char *route_map[EIGRP_FILTER_MAX];" not in types
    assert structs.count("eigrp_filter_runtime_state_t filter;") == 2
    assert "struct access_list" not in structs
    assert "struct prefix_list" not in structs
    assert "struct route_map" not in structs
    assert "struct distribute_ctx" not in structs


def test_filter_feature_owner_uses_portable_runtime_state_and_southbound_decision():
    filt = read("eigrpd/eigrp_filter.c")
    apply = function_body(filt, "eigrp_filter_prefix_apply")
    replace = function_body(filt, "eigrp_sys_filter_runtime_replace")

    assert "eigrp_filter_runtime_state_denies" in apply
    assert "eigrp_sys_filter_evaluate" in filt
    assert "eigrp_filter_runtime_state_copy" in replace
    assert "eigrp_filter_schedule_interface" in replace
    assert "eigrp_filter_schedule_process" in replace

    for host_api in (
        "access_list_lookup",
        "access_list_apply",
        "prefix_list_lookup",
        "prefix_list_apply",
        "distribute_lookup",
        "route_map_lookup_by_name",
    ):
        assert host_api not in filt


def test_frr_policy_adapter_owns_policy_objects_and_callbacks():
    policy = read("frr/eigrp_policy.c")

    assert "struct distribute_ctx *distribute_ctx;" in policy
    assert "struct access_list *access;" in policy
    assert "struct prefix_list *plist;" in policy
    assert "distribute_list_ctx_create" in policy
    assert "distribute_list_add_hook" in policy
    assert "distribute_list_delete_hook" in policy
    assert "access_list_add_hook" in policy
    assert "prefix_list_add_hook" in policy
    assert "route_map_init();" in policy
    assert "eigrp_sys_filter_runtime_replace" in policy
    assert "eigrp_sys_policy_runtime_refresh" in policy


def test_southbound_filter_contract_returns_eigrp_decision_only():
    header = read("eigrpd/eigrp_sys.h")
    southbound = read("frr/eigrp_southbound.c")
    evaluate = function_body(southbound, "eigrp_sys_filter_evaluate")

    assert "eigrp_filter_decision_t *decision" in header
    assert "const eigrp_prefix_t *prefix" in header
    assert "return eigrp_policy_filter_evaluate" in evaluate
    for host_type in (
        "struct access_list",
        "struct prefix_list",
        "struct route_map",
        "struct distribute_ctx",
        "struct distribute",
    ):
        assert host_type not in header


def test_classic_distribute_context_is_private_to_frr_adapter():
    northbound = read("frr/eigrp_northbound.c")
    policy_h = read("frr/eigrp_policy.h")
    structs = read("eigrpd/eigrp_structs.h")

    assert "eigrp_policy_distribute_context(eigrp)" in northbound
    assert "group_distribute_list_create_helper" in northbound
    assert "struct distribute_ctx *eigrp_policy_distribute_context" in policy_h
    assert "distribute_ctx" not in structs


def test_redistribution_retains_route_map_by_name_not_host_object():
    redistribute = read("eigrpd/eigrp_redistribute.c")

    assert "char *route_map;" in redistribute
    assert "struct route_map" not in redistribute
    assert "route_map_lookup_by_name" not in redistribute


def test_obsolete_common_route_map_skeleton_is_removed():
    assert not (ROOT / "eigrpd" / "eigrp_routemap.c").exists()
    assert not (ROOT / "eigrpd" / "eigrp_routemap.h").exists()
