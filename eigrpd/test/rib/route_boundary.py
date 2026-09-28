# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import re

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
CODE = ROOT / "eigrpd" / "code"


def function_body(text, name):
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", text, re.S)
    assert match, name
    start = match.end()
    depth = 1
    pos = start
    while depth and pos < len(text):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
        pos += 1
    assert depth == 0
    return text[start : pos - 1]


def test_selected_routes_cross_only_the_public_rib_snapshot_boundary():
    topology = (CODE / "eigrp_topology.c").read_text()
    header = (CODE / "eigrp_rib.h").read_text()

    update = function_body(topology, "eigrp_update_routing_table")
    assert "eigrp_rib_route_t rib_route" in update
    assert "eigrp_rib_route_install(eigrp, &rib_route)" in update
    assert "eigrp_rib_route_remove(eigrp, &prefix->destination)" in update
    assert "eigrp_route_descriptor_t *" not in header
    assert "eigrp_prefix_descriptor_t *" not in header


def test_rib_contract_is_address_family_neutral_and_has_no_test_injection_api():
    header = (CODE / "eigrp_rib.h").read_text()

    assert "eigrp_prefix_t prefix;" in header
    assert "eigrp_address_t gateway;" in header
    assert "eigrp_rib_route_install" in header
    assert "eigrp_rib_route_remove" in header
    assert "eigrp_rib_source_route_add" in header
    assert "eigrp_rib_source_route_remove" in header
    assert "eigrp_test_" not in header
