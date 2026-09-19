# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for Step 7 increment 3 redistribution/filter boundaries.

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[4]
INSTANCE_H = ROOT / "eigrpd" / "eigrp_instance.h"
INSTANCE_C = ROOT / "eigrpd" / "eigrp_instance.c"
REDIST_C = ROOT / "eigrpd" / "eigrp_redistribute.c"
FILTER_C = ROOT / "eigrpd" / "eigrp_filter.c"
SOUTHBOUND_H = ROOT / "eigrpd" / "eigrp_southbound.h"
SOUTHBOUND_C = ROOT / "frr" / "eigrp_southbound.c"
POLICY_C = ROOT / "frr" / "eigrp_policy.c"
ZEBRA_C = ROOT / "frr" / "eigrp_zebra.c"
NORTHBOUND = ROOT / "frr" / "eigrp_northbound.c"
CONVENTIONS = ROOT / "specs" / "code-conventions.md"


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


def test_address_family_owns_portable_redistribution_and_filter_state():
    instance_h = read(INSTANCE_H)
    instance_c = read(INSTANCE_C)

    assert "eigrp_redistribute_config_t *redistributions;" in instance_h
    assert "eigrp_distribute_list_config_t *distribute_lists;" in instance_h
    assert "eigrp_redistribute_config_delete_all(af);" in instance_c
    assert "eigrp_distribute_list_config_delete_all(af);" in instance_c


def test_named_redistribution_target_owns_state_then_crosses_southbound_boundary():
    redist = read(REDIST_C)
    update = function_body(redist, "eigrp_redistribute_update")
    delete = function_body(redist, "eigrp_redistribute_delete")

    assert "eigrp_redistribute_config_find" in update
    assert "eigrp_southbound_redistribute_update" in update
    assert "context->config->redistributions" in update
    assert "eigrp_southbound_redistribute_delete" in delete
    assert "zclient_redistribute" not in update
    assert "eigrp_redistribute_set" not in update
    assert "eigrp_redistribute_unset" not in delete


def test_named_distribute_list_target_owns_common_filter_state_and_runtime_names():
    filt = read(FILTER_C)
    update = function_body(filt, "eigrp_distribute_list_update")
    delete = function_body(filt, "eigrp_distribute_list_delete")

    assert "eigrp_distribute_list_config_find" in update
    assert "eigrp_filter_runtime_reference_update" in update
    assert "context->config->distribute_lists" in update
    assert "eigrp_filter_runtime_reference_update" in delete
    assert "eigrp_southbound_distribute_list_update" not in update
    assert "eigrp_southbound_distribute_list_delete" not in delete
    assert "access_list_lookup" not in update
    assert "prefix_list_lookup" not in update
    assert "group_distribute_list" not in update


def test_southbound_contract_is_eigrp_owned_not_frr_cli_or_yang_objects():
    header = read(SOUTHBOUND_H)

    for target in (
        "eigrp_southbound_redistribute_update",
        "eigrp_southbound_redistribute_delete",
        "eigrp_southbound_policy_instance_create",
        "eigrp_southbound_policy_instance_delete",
        "eigrp_southbound_filter_evaluate",
    ):
        assert target in header
    for host_type in (
        "struct vty",
        "struct lyd_node",
        "struct distribute",
        "struct access_list",
        "struct prefix_list",
        "struct zclient",
    ):
        assert host_type not in header


def test_named_northbound_resolves_runtime_and_terminates_at_portable_targets():
    northbound = read(NORTHBOUND)
    instance_resolver = function_body(
        northbound, "eigrpd_named_instance_context_resolve"
    )
    resolver = function_body(northbound, "eigrpd_named_runtime_context_resolve")
    redist_apply = function_body(northbound, "eigrpd_named_redistribute_apply_options")
    redist_delete = function_body(northbound, "eigrpd_named_redistribute_destroy")
    dist_update = function_body(northbound, "eigrpd_named_distribute_list_modify")
    dist_delete = function_body(northbound, "eigrpd_named_distribute_list_destroy")

    assert "context->runtime = context->config->runtime;" in instance_resolver
    assert "eigrpd_named_instance_context_resolve" in resolver
    assert "vrf_lookup_by_name" not in resolver
    assert "eigrp_lookup_by_as_vrf" not in resolver
    assert "eigrpd_named_runtime_context_resolve" in redist_apply
    assert "eigrp_redistribute_update" in redist_apply
    assert "eigrpd_named_runtime_context_resolve" in redist_delete
    assert "eigrp_redistribute_delete" in redist_delete
    assert "eigrpd_named_runtime_context_resolve" in dist_update
    assert "eigrp_distribute_list_update" in dist_update
    assert "eigrpd_named_runtime_context_resolve" in dist_delete
    assert "eigrp_distribute_list_delete" in dist_delete


def test_frr_redistribution_adapter_delegates_to_named_zebra_operations():
    southbound = read(SOUTHBOUND_C)
    zebra = read(ZEBRA_C)
    adapter_update = function_body(southbound, "eigrp_southbound_redistribute_update")
    adapter_delete = function_body(southbound, "eigrp_southbound_redistribute_delete")
    zebra_update = function_body(zebra, "eigrp_zebra_redistribute_update")
    zebra_delete = function_body(zebra, "eigrp_zebra_redistribute_delete")

    assert "eigrp_zebra_redistribute_update" in adapter_update
    assert "eigrp_zebra_redistribute_delete" in adapter_delete
    assert "proto_redistnum(AFI_IP, protocol)" in zebra
    assert "zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD" in zebra_update
    assert "eigrp->dmetric[type] = runtime_metric;" in zebra_update
    assert "zclient_redistribute(ZEBRA_REDISTRIBUTE_DELETE" in zebra_delete
    assert "eigrp_redistribute_set" not in adapter_update
    assert "eigrp_redistribute_unset" not in adapter_delete
    assert "eigrp_redistribute_set" not in zebra_update
    assert "eigrp_redistribute_unset" not in zebra_delete


def test_frr_filter_adapter_owns_host_policy_objects_and_returns_portable_decision():
    southbound = read(SOUTHBOUND_C)
    policy = read(POLICY_C)
    evaluate = function_body(policy, "eigrp_policy_filter_evaluate")
    callback = function_body(policy, "eigrp_policy_distribute_update")

    assert "access_list_lookup(afi, name)" in evaluate
    assert "prefix_list_lookup(afi, name)" in evaluate
    assert "access_list_apply(access, &host_prefix)" in evaluate
    assert "prefix_list_apply(plist, &host_prefix)" in evaluate
    assert "EIGRP_FILTER_DECISION_DENY" in evaluate
    assert "eigrp_filter_runtime_replace" in callback
    assert "dist->list[DISTRIBUTE_V4_IN]" in callback
    assert "dist->prefix[DISTRIBUTE_V4_OUT]" in callback
    assert "access_list_lookup" not in southbound
    assert "prefix_list_lookup" not in southbound
    assert "return eigrp_policy_filter_evaluate" in function_body(
        southbound, "eigrp_southbound_filter_evaluate"
    )


def test_classic_endpoints_remain_unmodified_and_separate():
    northbound = read(NORTHBOUND)
    classic_redist_create = function_body(northbound, "eigrpd_instance_redistribute_create")
    classic_redist_destroy = function_body(northbound, "eigrpd_instance_redistribute_destroy")
    classic_dist_create = function_body(northbound, "eigrp_northbound_distribute_list_create")

    assert "eigrp_redistribute_set(eigrp, proto, metrics)" in classic_redist_create
    assert "eigrp_redistribute_unset(eigrp, proto)" in classic_redist_destroy
    assert "group_distribute_list_create_helper" in classic_dist_create
    assert "eigrp_redistribute_update" not in classic_redist_create
    assert "eigrp_distribute_list_update" not in classic_dist_create


def test_runtime_redistribution_incompleteness_is_explicit_after_zebra_subscription():
    zebra = read(ZEBRA_C)
    update = function_body(zebra, "eigrp_zebra_redistribute_update")

    add = update.index("zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD")
    unsupported = update.index("(void)route_map;")
    assert add < unsupported
    assert "return EIGRP_RESULT_NOT_IMPLEMENTED;" in update[unsupported:]


def test_non_convergence_exception_is_documented():
    conventions = read(CONVENTIONS)

    assert "### Redistribution and distribute-list exception" in conventions
    assert "eigrp_southbound_redistribute_*()" in conventions
    assert "eigrp_southbound_filter_evaluate()" in conventions
    assert "public classic/named configuration endpoints remain intentionally separate" in conventions
