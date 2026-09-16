# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for classic/named network runtime convergence.

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[4]
NETWORK_C = ROOT / "eigrpd" / "eigrp_network.c"
NETWORK_H = ROOT / "eigrpd" / "eigrp_network.h"
SOUTHBOUND_H = ROOT / "eigrpd" / "eigrp_southbound.h"
SOUTHBOUND_C = ROOT / "frr" / "eigrp_southbound.c"
NORTHBOUND = ROOT / "frr" / "eigrp_northbound.c"


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    for match in re.finditer(rf"(?:^|\n)(?:static\s+)?[^\n]+\b{name}\(", source):
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


def test_network_public_target_uses_existing_eigrp_instance_context_and_prefix():
    header = read(NETWORK_H)

    assert "eigrp_network_create(eigrp_instance_context_t *context," in header
    assert "eigrp_network_delete(eigrp_instance_context_t *context," in header
    assert "const eigrp_prefix_t *prefix" in header
    assert "eigrp_network_context" not in header


def test_classic_and_named_converge_on_one_network_processor():
    network = read(NETWORK_C)
    northbound = read(NORTHBOUND)

    processor = function_body(network, "eigrp_network_process")
    classic_set = function_body(network, "eigrp_network_set")
    classic_unset = function_body(network, "eigrp_network_unset")
    named_create = function_body(northbound, "eigrpd_named_network_create")
    named_destroy = function_body(northbound, "eigrpd_named_network_destroy")

    assert "eigrp_network_config_create" in processor
    assert "eigrp_southbound_network_create" in processor
    assert "eigrp_network_config_delete" in processor
    assert "eigrp_southbound_network_delete" in processor

    assert "eigrp_network_process(&context, &prefix" in classic_set
    assert "EIGRP_NETWORK_OPERATION_CREATE" in classic_set
    assert "eigrp_network_process(&context, &prefix" in classic_unset
    assert "EIGRP_NETWORK_OPERATION_DELETE" in classic_unset

    assert "eigrp_network_create(&context, &prefix);" in named_create
    assert "eigrp_network_delete(&context, &prefix);" in named_destroy


def test_common_network_processor_stops_at_eigrp_southbound_boundary():
    network = read(NETWORK_C)
    southbound_h = read(SOUTHBOUND_H)
    processor = function_body(network, "eigrp_network_process")

    assert "eigrp_southbound_network_create" in southbound_h
    assert "eigrp_southbound_network_delete" in southbound_h
    assert "FOR_ALL_INTERFACES" not in processor
    assert "route_node_" not in processor
    assert "vrf_lookup" not in processor
    assert "struct interface" not in processor


def test_named_network_uses_address_family_owned_runtime_binding():
    northbound = read(NORTHBOUND)
    instance_resolver = function_body(
        northbound, "eigrpd_named_instance_context_resolve"
    )
    resolver = function_body(northbound, "eigrpd_named_network_context_resolve")

    assert "context->runtime = context->config->runtime;" in instance_resolver
    assert "eigrpd_named_instance_context_resolve" in resolver
    assert "vrf_lookup_by_name" not in resolver
    assert "eigrp_lookup_by_as_vrf" not in resolver
    assert "eigrp_get(" not in resolver


def test_classic_frr_network_callbacks_remain_on_existing_entry_points():
    northbound = read(NORTHBOUND)
    create = function_body(northbound, "eigrpd_instance_network_create")
    destroy = function_body(northbound, "eigrpd_instance_network_destroy")

    assert "eigrp_network_set(eigrp, &prefix)" in create
    assert "eigrp_network_unset(eigrp, &prefix)" in destroy
    assert "eigrp_network_create" not in create
    assert "eigrp_network_delete" not in destroy


def test_frr_southbound_owns_network_interface_walk_and_runtime_storage():
    network = read(NETWORK_C)
    southbound = read(SOUTHBOUND_C)
    runtime_create = function_body(southbound, "eigrp_southbound_network_create")
    runtime_delete = function_body(southbound, "eigrp_southbound_network_delete")
    intf_update = function_body(southbound, "eigrp_intf_update")

    assert "FOR_ALL_INTERFACES" in runtime_create
    assert "route_node_get" in runtime_create
    assert "route_node_lookup" in runtime_delete
    assert "eigrp_intf_free" in runtime_delete
    assert "route_top(eigrp->networks)" in intf_update
    assert "void eigrp_intf_update" not in network


def test_network_runtime_matching_uses_normalized_eigrp_prefixes():
    network = read(NETWORK_C)
    southbound = read(SOUTHBOUND_C)
    run_interface = function_body(southbound, "eigrp_southbound_network_run_interface")

    assert "bool eigrp_network_prefix_match(const eigrp_prefix_t *network" in network
    assert "eigrp_southbound_prefix_from_host(co->address," in run_interface
    assert "eigrp_network_prefix_match(network, &connected_prefix)" in run_interface
    assert "prefix_match_network_statement" not in network
    assert "prefix_match_network_statement" not in southbound
