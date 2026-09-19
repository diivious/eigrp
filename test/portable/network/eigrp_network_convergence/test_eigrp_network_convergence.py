# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for classic/named network runtime convergence and the
# address-family/interface participation boundary.

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[4]
NETWORK_C = ROOT / "eigrpd" / "eigrp_network.c"
NETWORK_H = ROOT / "eigrpd" / "eigrp_network.h"
TYPES_H = ROOT / "eigrpd" / "eigrp_types.h"
IPV4_C = ROOT / "eigrpd" / "eigrp_ipv4.c"
IPV6_C = ROOT / "eigrpd" / "eigrp_ipv6.c"
SOUTHBOUND_H = ROOT / "eigrpd" / "eigrp_southbound.h"
SOUTHBOUND_C = ROOT / "frr" / "eigrp_southbound.c"
NORTHBOUND = ROOT / "frr" / "eigrp_northbound.c"
FRR_ADAPTER_C = ROOT / "frr" / "eigrp_frr.c"
FRR_ADAPTER_H = ROOT / "frr" / "eigrp_frr.h"


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    offset = 0
    needle = f"{name}("
    while True:
        start = source.find(needle, offset)
        if start < 0:
            break
        brace = source.find("{", start)
        semicolon = source.find(";", start)
        if brace < 0:
            break
        if semicolon >= 0 and semicolon < brace:
            offset = semicolon + 1
            continue
        depth = 0
        for index in range(brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
        break
    raise AssertionError(f"missing function definition {name}")


def test_network_public_target_uses_existing_eigrp_instance_context_and_prefix():
    header = read(NETWORK_H)

    assert "eigrp_network_create(eigrp_instance_context_t *context," in header
    assert "eigrp_network_delete(eigrp_instance_context_t *context," in header
    assert "const eigrp_prefix_t *prefix" in header
    assert "eigrp_network_context" not in header
    assert "eigrp_network_set" not in header
    assert "eigrp_network_unset" not in header


def test_classic_and_named_converge_on_one_network_processor():
    network = read(NETWORK_C)
    northbound = read(NORTHBOUND)

    processor = function_body(network, "eigrp_network_process")
    classic_create = function_body(northbound, "eigrpd_instance_network_create")
    classic_destroy = function_body(northbound, "eigrpd_instance_network_destroy")
    named_create = function_body(northbound, "eigrpd_named_network_create")
    named_destroy = function_body(northbound, "eigrpd_named_network_destroy")

    assert "eigrp_network_config_create" in processor
    assert "eigrp_southbound_network_create" in processor
    assert "eigrp_network_config_delete" in processor
    assert "eigrp_southbound_network_delete" in processor

    assert "eigrp_network_create(&context, &network)" in classic_create
    assert "eigrp_network_delete(&context, &network)" in classic_destroy
    assert "eigrp_network_create(&context, &prefix);" in named_create
    assert "eigrp_network_delete(&context, &prefix);" in named_destroy

    assert "eigrp_network_set" not in network
    assert "eigrp_network_unset" not in network


def test_common_network_validation_dispatches_through_selected_af_vector():
    network = read(NETWORK_C)
    validate = function_body(network, "eigrp_network_validate")
    vectors = function_body(network, "eigrp_network_vectors")

    assert "context->config->af_vectors" in vectors
    assert "context->runtime->af_vectors" in vectors
    assert "vectors->network_validate(prefix)" in validate
    assert "EIGRP_ADDRESS_FAMILY_IPV4" not in validate
    assert "prefix->prefix_length > 32" not in validate


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


def test_classic_frr_network_callbacks_convert_then_call_portable_targets():
    northbound = read(NORTHBOUND)
    create = function_body(northbound, "eigrpd_instance_network_create")
    destroy = function_body(northbound, "eigrpd_instance_network_destroy")

    assert "eigrp_frr_prefix_import(&prefix, &network)" in create
    assert "eigrp_frr_prefix_import(&prefix, &network)" in destroy
    assert "context.runtime = eigrp;" in create
    assert "context.runtime = eigrp;" in destroy
    assert "eigrp_network_create(&context, &network)" in create
    assert "eigrp_network_delete(&context, &network)" in destroy
    assert "eigrp_southbound_network_exists" in create
    assert "eigrp_southbound_network_exists" in destroy
    assert "route_node_" not in create
    assert "route_node_" not in destroy
    assert "eigrp_network_set" not in create
    assert "eigrp_network_unset" not in destroy


def test_frr_prefix_conversion_is_owned_by_frr_adapter():
    adapter_h = read(FRR_ADAPTER_H)
    adapter_c = read(FRR_ADAPTER_C)
    network = read(NETWORK_C)
    southbound = read(SOUTHBOUND_C)

    assert "eigrp_frr_prefix_import" in adapter_h
    assert "eigrp_frr_prefix_export" in adapter_h
    assert "AF_INET" in adapter_c
    assert "AF_INET6" in adapter_c
    assert "struct prefix" in adapter_c
    assert "eigrp_network_prefix_from_host" not in network
    assert "eigrp_southbound_prefix_from_host" not in southbound
    assert "eigrp_southbound_prefix_to_host" not in southbound


def test_frr_southbound_owns_network_interface_walk_and_runtime_storage():
    network = read(NETWORK_C)
    southbound = read(SOUTHBOUND_C)
    runtime_create = function_body(southbound, "eigrp_southbound_network_create")
    runtime_delete = function_body(southbound, "eigrp_southbound_network_delete")
    refresh = function_body(southbound, "eigrp_southbound_interfaces_refresh")
    refresh_one = function_body(southbound, "eigrp_southbound_interface_refresh_one")

    assert "FOR_ALL_INTERFACES" in runtime_create
    assert "route_node_get" in runtime_create
    assert "route_node_lookup" in runtime_delete
    assert "eigrp_intf_free" in runtime_delete
    assert "FOR_ALL_INTERFACES" in refresh
    assert "route_top(eigrp->networks)" in refresh_one
    assert "eigrp_southbound_network_run_interface" in refresh_one
    assert "void eigrp_intf_update" not in network
    assert "void eigrp_intf_update" not in southbound


def test_network_interface_participation_uses_selected_af_vector():
    types = read(TYPES_H)
    ipv4 = read(IPV4_C)
    ipv6 = read(IPV6_C)
    southbound = read(SOUTHBOUND_C)
    run_interface = function_body(southbound, "eigrp_southbound_network_run_interface")
    runtime_delete = function_body(southbound, "eigrp_southbound_network_delete")
    ipv4_match = function_body(ipv4, "eigrp_ipv4_network_interface_match")
    ipv6_match = function_body(ipv6, "eigrp_ipv6_network_interface_match")

    assert "network_interface_match" in types
    assert "eigrp->af_vectors.network_interface_match" in run_interface
    assert "eigrp->af_vectors.network_interface_match" in runtime_delete
    assert "memcmp" in ipv4_match
    assert "return false;" in ipv6_match
    assert "vectors->network_interface_match = eigrp_ipv4_network_interface_match" in ipv4
    assert "vectors->network_interface_match = eigrp_ipv6_network_interface_match" in ipv6


def test_ipv6_network_feature_remains_explicitly_unsupported():
    ipv6 = read(IPV6_C)
    validate = function_body(ipv6, "eigrp_ipv6_network_validate")

    assert "EIGRP_RESULT_UNSUPPORTED" in validate
