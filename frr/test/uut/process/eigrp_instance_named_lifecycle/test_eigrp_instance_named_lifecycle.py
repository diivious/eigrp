# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for named address-family/runtime lifecycle ownership.

from pathlib import Path
import re


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
INSTANCE_H = ROOT / "eigrpd" / "code" / "eigrp_instance.h"
INSTANCE_C = ROOT / "eigrpd" / "code" / "eigrp_instance.c"
SOUTHBOUND_H = ROOT / "eigrpd" / "code" / "eigrp_sys.h"
SOUTHBOUND_C = ROOT / "frr" / "code" / "eigrp_southbound.c"
NORTHBOUND = ROOT / "frr" / "code" / "eigrp_northbound.c"
EIGRPD_C = ROOT / "eigrpd" / "code" / "eigrpd.c"
INTERFACE_C = ROOT / "eigrpd" / "code" / "eigrp_interface.c"
SUMMARY_C = ROOT / "eigrpd" / "code" / "eigrp_summary.c"
NAMED_CLI = ROOT / "frr" / "code" / "eigrp_cli_named.c"


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
    create = function_body(instance, "eigrp_af_instance_create")
    create_with_data_path = function_body(
        instance, "eigrp_af_instance_create_with_data_path"
    )
    runtime_create = function_body(
        instance, "eigrp_af_instance_runtime_create"
    )

    assert "eigrp_instance_t *runtime;" in header
    assert "eigrp_af_instance_create_with_data_path(" in create
    assert "name, afi, vrf_name, asn, true" in " ".join(create.split())
    assert "eigrp_af_instance_runtime_create(name, af, data_path_ready)" in (
        " ".join(create_with_data_path.split())
    )
    assert "eigrp_sys_vrf_resolve(" in runtime_create
    assert "eigrp_lookup_by_af_as_vrf(af->afi, af->asn, vrf_id)" in runtime_create
    assert "eigrp_instance_lookup_or_create_by_af(" in runtime_create
    assert "af->runtime = runtime;" in runtime_create
    assert "EIGRP_AFI_IPV6" not in runtime_create
    assert "af->runtime = runtime;" in runtime_create


def test_failed_implicit_address_family_creation_does_not_leave_empty_parent():
    instance = read(INSTANCE_C)
    create = function_body(
        instance, "eigrp_af_instance_create_with_data_path"
    )

    assert "bool parent_created = false;" in create
    assert "parent_created = true;" in create
    failure = create.index("result = eigrp_af_instance_runtime_create")
    rollback = create.index("(void)eigrp_instance_parent_delete(name);", failure)
    assert rollback > failure


def test_address_family_delete_stops_runtime_before_freeing_configuration():
    instance = read(INSTANCE_C)
    delete = function_body(instance, "eigrp_af_instance_delete")
    parent_delete = function_body(instance, "eigrp_instance_parent_delete")
    runtime_delete = function_body(
        instance, "eigrp_af_instance_runtime_delete"
    )

    assert "eigrp_instance_delete_final(af->runtime);" in runtime_delete
    assert "eigrp_af_instance_runtime_delete(" in delete
    assert delete.index("eigrp_af_instance_runtime_delete(") < delete.index(
        "eigrp_af_instance_free(af)"
    )
    assert "eigrp_af_instance_runtime_delete(" in parent_delete
    assert parent_delete.index(
        "eigrp_af_instance_runtime_delete("
    ) < parent_delete.index("*cursor = parent->next")


def test_runtime_teardown_unbinds_named_configuration_before_storage_is_freed():
    instance = read(INSTANCE_C)
    daemon = read(EIGRPD_C)
    unbind = function_body(instance, "eigrp_instance_runtime_remove")
    finish = function_body(daemon, "eigrp_instance_delete_final")

    assert "if (af->runtime == runtime)" in unbind
    assert "af->runtime = NULL;" in unbind
    assert "eigrp_instance_runtime_remove(eigrp);" in finish
    assert finish.index("eigrp_instance_runtime_remove(eigrp);") < finish.index(
        "eigrp_intf_free"
    )


def test_frr_southbound_only_resolves_host_vrf_for_common_runtime_creation():
    header = read(SOUTHBOUND_H)
    southbound = read(SOUTHBOUND_C)
    instance = read(INSTANCE_C)
    resolve = function_body(southbound, "eigrp_sys_vrf_resolve")
    create = function_body(instance, "eigrp_af_instance_runtime_create")

    assert "eigrp_sys_vrf_resolve(" in header
    assert "vrf_lookup_by_name(vrf_name)" in resolve
    assert "eigrp_lookup_by_af_as_vrf" not in resolve
    assert "eigrp_instance_lookup_or_create_by_af" not in resolve
    assert "eigrp_lookup_by_af_as_vrf(af->afi, af->asn, vrf_id)" in create
    assert "eigrp_instance_lookup_or_create_by_af(" in create
    assert "eigrp_name_update(EIGRP_SET, runtime, name);" in create


def test_router_id_runtime_refresh_is_owned_by_common_instance_code():
    header = read(SOUTHBOUND_H)
    instance = read(INSTANCE_C)
    update = function_body(instance, "eigrp_instance_router_id_update")
    delete = function_body(instance, "eigrp_instance_router_id_update")
    refresh = function_body(instance, "eigrp_instance_router_id_runtime_update")

    assert "eigrp_southbound_router_id_refresh(" not in header
    assert "context->runtime->router_id_static.s_addr = htonl(router_id);" in update
    assert "eigrp_instance_router_id_runtime_update(context->runtime);" in update
    assert "context->runtime->router_id_static.s_addr = INADDR_ANY;" in delete
    assert "eigrp_instance_router_id_runtime_update(context->runtime);" in delete
    assert "eigrp_router_id_update(runtime);" in refresh


def test_classic_process_creation_rejects_a_runtime_owned_by_named_mode():
    northbound = (read(NORTHBOUND) + read(NORTHBOUND.with_name("eigrp_northbound_ipv4.c")) + read(NORTHBOUND.with_name("eigrp_northbound_ipv6.c")))
    create = function_body(northbound, "eigrpd_instance_create")

    assert "eigrp_instance_classic_validate(asn, vrf_id, &owner_name)" in create
    assert "result == EIGRP_RESULT_CONFLICT" in create
    assert "return NB_ERR_VALIDATION;" in create

    instance = read(INSTANCE_C)
    validate = function_body(instance, "eigrp_instance_classic_validate")
    assert "eigrp_lookup_by_as_vrf(asn, vrf_id)" in validate
    assert "runtime->name" in validate


def test_named_configuration_commands_consume_lifecycle_binding_not_relookup_process():
    northbound = (read(NORTHBOUND) + read(NORTHBOUND.with_name("eigrp_northbound_ipv4.c")) + read(NORTHBOUND.with_name("eigrp_northbound_ipv6.c")))
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
        assert "eigrp_instance_lookup_or_create(" not in body
        assert "eigrp_lookup_by_as_vrf" not in body


def test_runtime_binding_does_not_prevent_retained_interface_configuration():
    interface = read(INTERFACE_C)
    northbound = (read(NORTHBOUND) + read(NORTHBOUND.with_name("eigrp_northbound_ipv4.c")) + read(NORTHBOUND.with_name("eigrp_northbound_ipv6.c")))

    targets = (
        ("eigrp_intf_bandwidth_percent_update", "bandwidth_percent"),
        ("eigrp_intf_bandwidth_percent_update", "bandwidth_percent_configured"),
        ("eigrp_intf_nexthop_self_update", "next_hop_self"),
        ("eigrp_intf_split_horizon_update", "split_horizon"),
        ("eigrp_intf_shutdown_update", "shutdown"),
    )
    for name, config_field in targets:
        body = function_body(interface, name)
        if name in {
            "eigrp_intf_nexthop_self_update",
            "eigrp_intf_split_horizon_update",
            "eigrp_intf_shutdown_update",
        }:
            helper = name
            body = function_body(interface, helper)
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
    northbound = (read(NORTHBOUND) + read(NORTHBOUND.with_name("eigrp_northbound_ipv4.c")) + read(NORTHBOUND.with_name("eigrp_northbound_ipv6.c")))
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
    assert "eigrp_summary_withdraw(context->runtime->eigrp, &summary->prefix);" in delete
    assert "eigrp_summary_runtime_update(context->runtime->eigrp);" in delete
    assert "eigrpd_named_config_result(result, true)" in destroy_cb


def test_named_exec_state_walkers_consume_address_family_runtime_binding():
    named_cli = read(NAMED_CLI)
    runtime_lookup = function_body(named_cli, "eigrp_vty_named_runtime_lookup")

    assert "return af->runtime;" in runtime_lookup
    assert "vrf_lookup_by_name" not in runtime_lookup
    assert "eigrp_lookup_by_as_vrf" not in runtime_lookup
    assert "eigrp_intf_state_iterate(" in named_cli
    assert "eigrp_nbr_state_iterate(" in named_cli
    assert "eigrp_topology_state_iterate(" in named_cli



def test_named_runtime_identity_and_start_stop_are_address_family_generic():
    instance = read(INSTANCE_C)
    create = function_body(
        instance, "eigrp_af_instance_runtime_create"
    )
    start = function_body(instance, "eigrp_af_instance_start")
    stop = function_body(instance, "eigrp_af_instance_stop")

    create_words = " ".join(create.split())
    assert "eigrp_lookup_by_af_as_vrf(af->afi, af->asn, vrf_id)" in create
    assert (
        "eigrp_instance_lookup_or_create_by_af(af->afi, af->asn, vrf_id, "
        "data_path_ready);"
    ) in create_words
    assert "EIGRP_AFI_IPV4" not in create
    assert "EIGRP_AFI_IPV6" not in create
    assert "EIGRP_AFI_IPV4" not in start
    # IPv6 may emit its protocol-required missing-router-ID warning here; that
    # presentation branch does not change the shared AF lifecycle.
    assert "EIGRP_AFI_IPV4" not in stop
    assert "EIGRP_AFI_IPV6" not in stop


def test_protocol_status_reports_runtime_and_datapath_capability():
    status = read(ROOT / "eigrpd" / "code" / "eigrp_status.c")
    snapshot = function_body(status, "eigrp_status_af")

    assert ".runtime_present = af->runtime != NULL" in snapshot
    assert ".data_path_ready = eigrp_instance_data_path_ready(af->runtime)" in snapshot
