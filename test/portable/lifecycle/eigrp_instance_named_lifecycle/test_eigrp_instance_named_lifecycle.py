# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for named address-family/runtime lifecycle ownership.

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[4]
INSTANCE_H = ROOT / "eigrpd" / "eigrp_instance.h"
INSTANCE_C = ROOT / "eigrpd" / "eigrp_instance.c"
SOUTHBOUND_H = ROOT / "eigrpd" / "eigrp_southbound.h"
SOUTHBOUND_C = ROOT / "frr" / "eigrp_southbound.c"
NORTHBOUND = ROOT / "frr" / "eigrp_northbound.c"
EIGRPD_C = ROOT / "eigrpd" / "eigrpd.c"
INTERFACE_C = ROOT / "eigrpd" / "eigrp_interface.c"
SUMMARY_C = ROOT / "eigrpd" / "eigrp_summary.c"
NAMED_CLI = ROOT / "frr" / "eigrp_cli_named.c"


def read(path: Path) -> str:
    return path.read_text()


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


def test_address_family_configuration_owns_runtime_binding():
    header = read(INSTANCE_H)
    instance = read(INSTANCE_C)
    create = function_body(instance, "eigrp_instance_address_family_create")
    runtime_create = function_body(
        instance, "eigrp_instance_address_family_runtime_create"
    )

    assert "eigrp_instance_t *runtime;" in header
    assert "eigrp_instance_address_family_runtime_create(name, af)" in create
    assert "af->afi == EIGRP_ADDRESS_FAMILY_IPV6" in runtime_create
    assert "eigrp_southbound_instance_create(" in runtime_create
    assert "af->runtime = runtime;" in runtime_create


def test_failed_implicit_address_family_creation_does_not_leave_empty_parent():
    instance = read(INSTANCE_C)
    create = function_body(instance, "eigrp_instance_address_family_create")

    assert "bool parent_created = false;" in create
    assert "parent_created = true;" in create
    failure = create.index("result = eigrp_instance_address_family_runtime_create")
    rollback = create.index("(void)eigrp_instance_parent_delete(name);", failure)
    assert rollback > failure


def test_address_family_delete_stops_runtime_before_freeing_configuration():
    instance = read(INSTANCE_C)
    delete = function_body(instance, "eigrp_instance_address_family_delete")
    parent_delete = function_body(instance, "eigrp_instance_parent_delete")
    runtime_delete = function_body(
        instance, "eigrp_instance_address_family_runtime_delete"
    )

    assert "eigrp_southbound_instance_delete(name, af->runtime)" in runtime_delete
    assert "eigrp_instance_address_family_runtime_delete(" in delete
    assert delete.index("eigrp_instance_address_family_runtime_delete(") < delete.index(
        "eigrp_instance_address_family_free(af)"
    )
    assert "eigrp_instance_address_family_runtime_delete(" in parent_delete
    assert parent_delete.index(
        "eigrp_instance_address_family_runtime_delete("
    ) < parent_delete.index("*cursor = parent->next")


def test_runtime_teardown_unbinds_named_configuration_before_storage_is_freed():
    instance = read(INSTANCE_C)
    daemon = read(EIGRPD_C)
    unbind = function_body(instance, "eigrp_instance_runtime_unbind")
    finish = function_body(daemon, "eigrp_finish_final")

    assert "if (af->runtime == runtime)" in unbind
    assert "af->runtime = NULL;" in unbind
    assert "eigrp_instance_runtime_unbind(eigrp);" in finish
    assert finish.index("eigrp_instance_runtime_unbind(eigrp);") < finish.index(
        "eigrp_intf_free"
    )


def test_frr_southbound_owns_named_vrf_resolution_and_runtime_creation():
    header = read(SOUTHBOUND_H)
    southbound = read(SOUTHBOUND_C)
    create = function_body(southbound, "eigrp_southbound_instance_create")
    delete = function_body(southbound, "eigrp_southbound_instance_delete")

    assert "eigrp_southbound_instance_create(" in header
    assert "eigrp_southbound_instance_delete(" in header
    assert "vrf_lookup_by_name(vrf_name)" in create
    assert "eigrp_lookup_by_as_vrf(asn, vrf->vrf_id)" in create
    assert "return EIGRP_RESULT_CONFLICT;" in create
    assert "eigrp_get(asn, vrf->vrf_id)" in create
    assert "eigrp_name_set(eigrp, name);" in create
    assert "eigrp_finish_final(runtime);" in delete


def test_router_id_runtime_refresh_stays_behind_southbound_boundary():
    header = read(SOUTHBOUND_H)
    instance = read(INSTANCE_C)
    southbound = read(SOUTHBOUND_C)
    update = function_body(instance, "eigrp_instance_router_id_update")
    delete = function_body(instance, "eigrp_instance_router_id_delete")
    refresh = function_body(southbound, "eigrp_southbound_router_id_refresh")

    assert "eigrp_southbound_router_id_refresh(" in header
    assert "context->runtime->router_id_static.s_addr = htonl(router_id);" in update
    assert "eigrp_southbound_router_id_refresh(context->runtime);" in update
    assert "context->runtime->router_id_static.s_addr = INADDR_ANY;" in delete
    assert "eigrp_southbound_router_id_refresh(context->runtime);" in delete
    assert "eigrp_router_id_update(runtime);" in refresh


def test_classic_process_creation_rejects_a_runtime_owned_by_named_mode():
    northbound = read(NORTHBOUND)
    create = function_body(northbound, "eigrpd_instance_create")

    assert "eigrp_lookup_by_as_vrf(asn, vrfid)" in create
    assert "if (eigrp && eigrp->name)" in create
    assert "return NB_ERR_VALIDATION;" in create


def test_named_configuration_commands_consume_lifecycle_binding_not_relookup_process():
    northbound = read(NORTHBOUND)
    instance_resolver = function_body(
        northbound, "eigrpd_named_instance_context_resolve"
    )
    runtime_resolver = function_body(
        northbound, "eigrpd_named_runtime_context_resolve"
    )
    network_resolver = function_body(
        northbound, "eigrpd_named_network_context_resolve"
    )
    interface_resolver = function_body(
        northbound, "eigrpd_named_interface_context_resolve"
    )

    assert "context->runtime = context->config->runtime;" in instance_resolver
    assert "eigrpd_named_instance_context_resolve" in runtime_resolver
    assert "eigrpd_named_instance_context_resolve" in network_resolver
    assert "runtime = af->runtime;" in interface_resolver
    for body in (runtime_resolver, network_resolver, interface_resolver):
        assert "eigrp_get(" not in body
        assert "eigrp_lookup_by_as_vrf" not in body


def test_runtime_binding_does_not_prevent_retained_interface_configuration():
    interface = read(INTERFACE_C)
    northbound = read(NORTHBOUND)

    targets = (
        ("eigrp_interface_bandwidth_percent_update", "bandwidth_percent"),
        ("eigrp_interface_bandwidth_percent_delete", "bandwidth_percent_configured"),
        ("eigrp_interface_next_hop_self_update", "next_hop_self"),
        ("eigrp_interface_split_horizon_update", "split_horizon"),
        ("eigrp_interface_shutdown_update", "shutdown"),
    )
    for name, config_field in targets:
        body = function_body(interface, name)
        assert f"context->config->{config_field}" in body
        assert "if (context->runtime)" in body
        assert body.index(f"context->config->{config_field}") < body.index(
            "if (context->runtime)"
        )

    callbacks = (
        ("eigrpd_named_af_interface_bandwidth_percent_modify", "false"),
        ("eigrpd_named_af_interface_bandwidth_percent_destroy", "true"),
        ("eigrpd_named_af_interface_next_hop_modify", "false"),
        ("eigrpd_named_af_interface_split_horizon_modify", "false"),
        ("eigrpd_named_af_interface_shutdown_create", "false"),
        ("eigrpd_named_af_interface_shutdown_destroy", "true"),
    )
    for name, removing in callbacks:
        body = function_body(northbound, name)
        assert f"eigrpd_named_config_result(result, {removing})" in body


def test_runtime_binding_does_not_prevent_retained_summary_configuration():
    summary = read(SUMMARY_C)
    northbound = read(NORTHBOUND)
    create = function_body(summary, "eigrp_summary_create")
    delete = function_body(summary, "eigrp_summary_delete")
    destroy_cb = function_body(
        northbound, "eigrpd_named_af_interface_summary_destroy"
    )

    assert "context->config->summaries = summary;" in create
    assert create.index("context->config->summaries = summary;") < create.rindex(
        "EIGRP_RESULT_NOT_IMPLEMENTED"
    )
    assert "*cursor = summary->next;" in delete
    assert delete.index("*cursor = summary->next;") < delete.rindex(
        "EIGRP_RESULT_NOT_IMPLEMENTED"
    )
    assert "eigrpd_named_config_result(result, true)" in destroy_cb


def test_named_exec_state_walkers_remain_common_and_unchanged_in_shape():
    named_cli = read(NAMED_CLI)

    assert "eigrp_interface_state_walk(" in named_cli
    assert "eigrp_neighbor_state_walk(" in named_cli
    assert "eigrp_topology_state_walk(" in named_cli
