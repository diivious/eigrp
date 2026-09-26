# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Capability-boundary coverage for redistribution route-map attachment.

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[4]
REDISTRIBUTE_C = ROOT / "eigrpd" / "eigrp_redistribute.c"
INTEGRATION_SPEC = ROOT / "specs" / "integration-spec.md"


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


def test_route_map_name_is_retained_on_redistribution_entry():
    redistribute = REDISTRIBUTE_C.read_text()
    add = function_body(redistribute, "eigrp_redistribute_add")

    assert "char *route_map;" in redistribute
    assert "new_config->route_map = new_route_map;" in add
    assert "config->route_map = new_route_map;" in add
    assert "free(config->route_map);" in redistribute
    assert "struct route_map" not in redistribute
    assert "route_map_lookup_by_name" not in redistribute


def test_route_map_candidate_is_not_imported_without_evaluation():
    redistribute = REDISTRIBUTE_C.read_text()
    receive = function_body(redistribute, "eigrp_redistribute_source_route_receive")

    deferred = receive.index("if (importing && config->route_map)")
    not_implemented = receive.index("return EIGRP_RESULT_NOT_IMPLEMENTED;", deferred)
    topology_update = receive.index("eigrp_topology_redistributed_route_update")

    assert deferred < not_implemented < topology_update
    assert "eigrp_sys_filter_evaluate" not in receive
    assert "route_map_lookup_by_name" not in receive


def test_route_map_capability_boundary_is_documented():
    spec = INTEGRATION_SPEC.read_text()

    assert "Current route-map capability boundary" in spec
    assert "valid retained configuration" in spec
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in spec
    assert "No partial or synthetic route-map evaluator is provided" in spec
