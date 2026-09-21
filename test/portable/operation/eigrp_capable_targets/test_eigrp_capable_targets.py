# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for audit item 4: connect already-capable named targets
# to the IPv4 runtime established by the address-family lifecycle.

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


def test_named_interface_context_uses_address_family_runtime_binding():
    northbound = read("frr/eigrp_northbound.c")
    resolve = function_body(northbound, "eigrpd_named_interface_context_resolve")

    assert "runtime = af->runtime;" in resolve
    assert "eigrp_intf_lookup_by_name(runtime, interface_name)" in resolve


def test_named_hello_hold_and_passive_callbacks_reach_common_targets():
    northbound = read("frr/eigrp_northbound.c")

    hello = function_body(northbound, "eigrpd_named_af_interface_hello_modify")
    hello_no = function_body(northbound, "eigrpd_named_af_interface_hello_destroy")
    hold = function_body(northbound, "eigrpd_named_af_interface_hold_modify")
    hold_no = function_body(northbound, "eigrpd_named_af_interface_hold_destroy")
    passive = function_body(northbound, "eigrpd_named_af_interface_passive_create")
    passive_no = function_body(northbound, "eigrpd_named_af_interface_passive_destroy")

    assert "eigrp_interface_hello_interval_update(&context" in hello
    assert "eigrp_interface_hello_interval_delete(&context)" in hello_no
    assert "eigrp_interface_hold_time_update(&context" in hold
    assert "eigrp_interface_hold_time_delete(&context)" in hold_no
    assert "eigrp_interface_passive_update(&context, true)" in passive
    assert "eigrp_interface_passive_update(&context, false)" in passive_no


def test_interface_targets_update_active_runtime_and_passive_controls_hello_io():
    interface = read("eigrpd/eigrp_interface.c")
    hello_source = read("eigrpd/eigrp_hello.c")

    hello = function_body(interface, "eigrp_interface_hello_interval_update")
    hold = function_body(interface, "eigrp_interface_hold_time_update")
    passive = function_body(interface, "eigrp_interface_passive_update")
    is_passive = function_body(interface, "eigrp_intf_is_passive")
    hello_timer = function_body(hello_source, "eigrp_hello_timer")

    assert "context->runtime->params.v_hello = seconds;" in hello
    assert "context->runtime->params.v_wait = seconds;" in hold
    assert "context->runtime->params.passive_interface" in passive
    assert "eigrp_intf_set_multicast(context->runtime);" in passive
    assert "ei->params.passive_interface == EIGRP_INTF_PASSIVE" in is_passive
    assert "if (!eigrp_intf_is_passive(ei))" in hello_timer
    assert "eigrp_neighbor_static_hello_send(ei)" in hello_timer


def test_metric_weights_variance_and_maximum_paths_callbacks_reach_runtime_targets():
    northbound = read("frr/eigrp_northbound.c")
    metric = read("eigrpd/eigrp_metric.c")
    topology = read("eigrpd/eigrp_topology.c")

    weights = function_body(northbound, "eigrpd_named_metric_weights_apply")
    variance = function_body(northbound, "eigrpd_named_variance_modify")
    maximum_paths = function_body(northbound, "eigrpd_named_maximum_paths_modify")
    weight_target = function_body(metric, "eigrp_metric_weights_update")
    variance_target = function_body(metric, "eigrp_metric_variance_update")
    paths_target = function_body(topology, "eigrp_topology_maximum_paths_update")

    assert "eigrp_metric_weights_update(&context, &weights)" in weights
    assert "eigrp_metric_variance_update(&context" in variance
    assert "eigrp_topology_maximum_paths_update(" in maximum_paths

    for index in range(6):
        assert f"context->runtime->k_values[{index}]" in weight_target
    assert "context->runtime->variance = variance;" in variance_target
    assert "context->runtime->max_paths = maximum_paths;" in paths_target


def test_named_md5_and_keychain_reach_runtime_and_late_interface_bind():
    northbound = read("frr/eigrp_northbound.c")
    auth = read("eigrpd/eigrp_auth.c")
    interface = read("eigrpd/eigrp_interface.c")

    apply = function_body(northbound, "eigrpd_named_af_interface_authentication_apply")
    keychain = function_body(northbound, "eigrpd_named_af_interface_keychain_modify")
    mode_target = function_body(auth, "eigrp_auth_mode_update")
    key_target = function_body(auth, "eigrp_auth_keychain_update")
    bind = function_body(interface, "eigrp_interface_runtime_bind")

    assert "eigrp_auth_mode_update(&context, mode, hmac_ptr)" in apply
    assert "eigrp_auth_keychain_update(&context" in keychain
    assert "context->runtime->params.auth_type =" in mode_target
    assert "EIGRP_AUTH_TYPE_MD5" in mode_target
    assert "context->runtime->params.auth_keychain = runtime_copy;" in key_target

    # If configuration exists before a network statement creates the runtime
    # interface, the same real authentication targets are applied at bind.
    assert "eigrp_auth_mode_update(&context, EIGRP_AUTHENTICATION_MD5, NULL)" in bind
    assert "eigrp_auth_keychain_update(&context, config->keychain)" in bind


def test_ipv6_config_only_stage_is_not_promoted_to_runtime_by_item_4():
    northbound = read("frr/eigrp_northbound.c")
    resolve = function_body(northbound, "eigrpd_named_interface_context_resolve")

    assert "afi == EIGRP_ADDRESS_FAMILY_IPV4" in resolve
    assert "context->runtime =" in resolve


def test_eigrp_stub_feature_is_not_implemented_by_audit_item_4():
    changed_modules = "\n".join(
        read(path)
        for path in (
            "eigrpd/eigrp_interface.c",
            "eigrpd/eigrp_hello.c",
            "eigrpd/eigrp_metric.c",
            "eigrpd/eigrp_topology.c",
            "eigrpd/eigrp_auth.c",
            "frr/eigrp_northbound.c",
        )
    )
    assert "eigrp_stub" not in changed_modules
