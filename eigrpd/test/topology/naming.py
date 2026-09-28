# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Portable source-level guard for topology-owned object lifecycle names.
# These tests prevent descriptor-first create/free names from returning.

from pathlib import Path


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
PRODUCTION_SUFFIXES = {".c", ".h"}
OLD_LIFECYCLE_NAMES = {
    "eigrp_prefix_descriptor_new",
    "eigrp_prefix_descriptor_free",
    "eigrp_route_descriptor_new",
    "eigrp_route_descriptor_free",
}
NEW_LIFECYCLE_NAMES = {
    "eigrp_topology_prefix_create",
    "eigrp_topology_prefix_free",
    "eigrp_topology_route_create",
    "eigrp_topology_route_free",
}


def production_sources() -> list[Path]:
    sources = []
    for path in ROOT.rglob("*"):
        if path.suffix not in PRODUCTION_SUFFIXES:
            continue
        if any(part in {".git", "test"} for part in path.parts):
            continue
        sources.append(path)
    return sources


def read_production_source() -> str:
    return "\n".join(path.read_text() for path in production_sources())


def test_topology_lifecycle_names_are_topology_owned():
    source = read_production_source()

    for name in NEW_LIFECYCLE_NAMES:
        assert name in source


def test_descriptor_first_lifecycle_names_are_not_used_in_production_code():
    source = read_production_source()

    for name in OLD_LIFECYCLE_NAMES:
        assert name not in source



def test_topology_module_owns_lifecycle_definitions():
    topology_c = ROOT / "eigrpd" / "code" / "eigrp_topology.c"
    topology_h = ROOT / "eigrpd" / "code" / "eigrp_topology.h"
    topology_source = topology_c.read_text()
    topology_header = topology_h.read_text()

    expected_definitions = {
        "eigrp_prefix_descriptor_t *eigrp_topology_prefix_create(void)",
        "void eigrp_topology_prefix_free(eigrp_prefix_descriptor_t *pe)",
        "eigrp_route_descriptor_t *eigrp_topology_route_create(eigrp_intf_t *intf)",
        "void eigrp_topology_route_free(eigrp_route_descriptor_t *route)",
    }
    expected_declarations = {
        "eigrp_prefix_descriptor_t *eigrp_topology_prefix_create(void)",
        "void eigrp_topology_prefix_free(eigrp_prefix_descriptor_t *)",
        "eigrp_route_descriptor_t *eigrp_topology_route_create(eigrp_intf_t *)",
        "void eigrp_topology_route_free(eigrp_route_descriptor_t *)",
    }

    for definition in expected_definitions:
        assert definition in topology_source

    for declaration in expected_declarations:
        assert declaration in topology_header
