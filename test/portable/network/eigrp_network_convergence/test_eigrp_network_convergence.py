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
SOUTHBOUND_H = ROOT / "eigrpd" / "eigrp_sys.h"
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
    assert "eigrp_network_runtime_create" in processor
    assert "eigrp_network_config_delete" in processor
    assert "eigrp_network_runtime_delete" in processor

    assert "eigrp_network_create(&context, &network)" in classic_create
    assert "eigrp_network_delete(&context, &network)" in classic_destroy
    assert "eigrp_network_create(&context, &prefix);" in named_create
    assert "eigrp_network_delete(&context, &prefix);" in named_destroy

    assert "eigrp_network_set" not in network
    assert "eigrp_network_unset" not in network


def test_common_network_validation_uses_prefix_shape_and_explicit_ipv4_semantics():
    network = read(NETWORK_C)
    types = read(TYPES_H)
    validate = function_body(network, "eigrp_network_validate")

    assert "eigrp_prefix_valid(prefix)" in validate
    assert "EIGRP_ADDRESS_FAMILY_IPV4" in validate
    assert "EIGRP_RESULT_UNSUPPORTED" in validate
    assert "network_validate" not in types
    assert "prefix_validate" not in types
    assert "address_validate" not in types
    assert "network_interface_match" not in types
    assert "prefix->prefix_length > 32" not in validate


def test_common_network_processor_owns_runtime_decisions_and_uses_host_walk_only():
    network = read(NETWORK_C)
    southbound_h = read(SOUTHBOUND_H)
    processor = function_body(network, "eigrp_network_process")

    assert "eigrp_sys_interface_walk" in southbound_h
    assert "eigrp_southbound_network_create" not in southbound_h
    assert "eigrp_southbound_network_delete" not in southbound_h
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
    assert "eigrp_network_runtime_exists" in create
    assert "eigrp_network_runtime_exists" in destroy
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


def test_common_network_owns_runtime_storage_while_frr_only_enumerates_interfaces():
    network = read(NETWORK_C)
    southbound = read(SOUTHBOUND_C)
    runtime_create = function_body(network, "eigrp_network_runtime_create")
    runtime_delete = function_body(network, "eigrp_network_runtime_delete")
    refresh = function_body(network, "eigrp_network_interfaces_refresh")
    host_walk = function_body(southbound, "eigrp_sys_interface_walk")

    assert "network->next = eigrp->networks" in runtime_create
    assert "eigrp_network_interfaces_refresh(eigrp)" in runtime_create
    assert "eigrp_network_runtime_matches(eigrp, &ei->address)" in runtime_delete
    assert "eigrp_intf_free" in runtime_delete
    assert "eigrp_sys_interface_walk(" in refresh
    assert "FOR_ALL_INTERFACES" in host_walk
    assert "eigrp_frr_interface_state_import" in host_walk
    assert "route_node_" not in southbound
    assert "eigrp->networks" not in southbound


def test_network_interface_participation_uses_common_prefix_matching():
    types = read(TYPES_H)
    ipv4 = read(IPV4_C)
    ipv6 = read(IPV6_C)
    network = read(NETWORK_C)
    prefix = read(ROOT / "eigrpd" / "eigrp_prefix.c")
    matches = function_body(network, "eigrp_network_runtime_matches")
    runtime_delete = function_body(network, "eigrp_network_runtime_delete")
    prefix_match = function_body(prefix, "eigrp_prefix_address_match")

    assert "network_interface_match" not in types
    assert "eigrp_prefix_address_match" in matches
    assert "eigrp_network_runtime_matches(eigrp, &ei->address)" in runtime_delete
    assert "memcmp" in prefix_match
    assert "eigrp_ipv4_network_interface_match" not in ipv4
    assert "network_interface_match" not in ipv6
    assert "eigrp_ipv6_network_" not in ipv6
