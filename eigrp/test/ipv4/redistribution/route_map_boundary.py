# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Capability-boundary coverage for redistribution route-map attachment.

from pathlib import Path
import re


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
REDISTRIBUTE_C = ROOT / "eigrp" / "code" / "eigrp_redistribute.c"
PLATFORM_INTEGRATION = ROOT / "specs" / "platform-integration.md"


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
    add = function_body(redistribute, "eigrp_redist_add")

    assert "char *route_map;" in redistribute
    assert "new_config->route_map = new_route_map;" in add
    assert "config->route_map = new_route_map;" in add
    assert "free(config->route_map);" in redistribute
    assert "struct route_map" not in redistribute
    assert "route_map_lookup_by_name" not in redistribute


def test_route_map_candidate_is_evaluated_at_host_policy_boundary():
    redistribute = REDISTRIBUTE_C.read_text()
    receive = function_body(redistribute, "eigrp_redist_source_route_receive")

    assert "eigrp_sys_redistribute_route_map_evaluate" in receive
    assert "EIGRP_FILTER_DECISION_DENY" in receive
    assert "eigrp_topology_redistributed_route_remove" in receive
    assert "route_map_lookup_by_name" not in receive
    assert "struct route_map" not in receive


def test_route_map_capability_boundary_is_documented():
    spec = PLATFORM_INTEGRATION.read_text()

    assert "## 12. Redistribution flow" in spec
    assert "Host route-map or policy objects remain on the host side" in spec
    assert "eigrp_sys.h" in spec
    assert "normalized result" in spec
    assert "redistribution route-map evaluation" in spec
