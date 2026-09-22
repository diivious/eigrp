# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level closure guards for Post Command Audit item 5.

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[4]
INSTANCE_H = ROOT / "eigrpd" / "eigrp_instance.h"
INSTANCE_C = ROOT / "eigrpd" / "eigrp_instance.c"
INTERFACE_C = ROOT / "eigrpd" / "eigrp_interface.c"
AUTH_C = ROOT / "eigrpd" / "eigrp_auth.c"
FILTER_C = ROOT / "eigrpd" / "eigrp_filter.c"
REDISTRIBUTE_C = ROOT / "eigrpd" / "eigrp_redistribute.c"
SUMMARY_C = ROOT / "eigrpd" / "eigrp_summary.c"
METRIC_C = ROOT / "eigrpd" / "eigrp_metric.c"
TIMER_C = ROOT / "eigrpd" / "eigrp_timer.c"
NEIGHBOR_C = ROOT / "eigrpd" / "eigrp_neighbor.c"
HELLO_C = ROOT / "eigrpd" / "eigrp_hello.c"
PACKET_C = ROOT / "eigrpd" / "eigrp_packet.c"
UPDATE_C = ROOT / "eigrpd" / "eigrp_update.c"
EIGRPD_C = ROOT / "eigrpd" / "eigrpd.c"
NORTHBOUND_C = ROOT / "frr" / "eigrp_northbound.c"


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


def test_address_family_owns_and_releases_item5_retained_state():
    header = read(INSTANCE_H)
    source = read(INSTANCE_C)

    owned = (
        "eigrp_offset_config_t *offsets;",
        "eigrp_metric_config_t *metric_config;",
        "eigrp_summary_state_t *summary_state;",
        "eigrp_timer_config_t *timer_config;",
        "eigrp_neighbor_policy_state_t *neighbor_policy;",
        "eigrp_redistribute_policy_config_t *redistribute_policy;",
    )
    cleanup = (
        "eigrp_offset_config_delete_all(af);",
        "eigrp_metric_config_delete_all(af);",
        "eigrp_summary_state_delete_all(af);",
        "eigrp_timer_config_delete_all(af);",
        "eigrp_neighbor_policy_delete_all(af);",
        "eigrp_redistribute_policy_delete_all(af);",
    )
    for text in owned:
        assert text in header
    for text in cleanup:
        assert text in source


def test_interface_controls_retain_configuration_and_apply_available_runtime_behavior():
    source = read(INTERFACE_C)

    bandwidth = function_body(source, "eigrp_interface_bandwidth_percent_set")
    next_hop = function_body(source, "eigrp_interface_next_hop_self_apply")
    split_horizon = function_body(source, "eigrp_interface_split_horizon_apply")
    shutdown = function_body(source, "eigrp_interface_shutdown_apply")
    bind = function_body(source, "eigrp_interface_runtime_bind")

    assert "context->config->bandwidth_percent = percent;" in bandwidth
    assert "context->config->bandwidth_percent_configured = true;" in bandwidth
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in bandwidth

    assert "context->config->next_hop_self = enabled;" in next_hop
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in next_hop

    assert "context->config->split_horizon = enabled;" in split_horizon
    assert "context->runtime->split_horizon = enabled;" in split_horizon
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in split_horizon
    assert "runtime->split_horizon = config->split_horizon;" in bind

    assert "context->config->shutdown = shutdown;" in shutdown
    assert "eigrp_hello_send" in shutdown
    assert "eigrp_intf_down(context->runtime);" in shutdown
    assert "eigrp_instance_address_family_start" in shutdown
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in shutdown


def test_authentication_keeps_md5_runtime_and_truthful_hmac_boundary():
    source = read(AUTH_C)
    mode = function_body(source, "eigrp_auth_mode_set")
    keychain = function_body(source, "eigrp_auth_keychain_set")

    assert "context->config->authentication_mode = (uint8_t)mode;" in mode
    assert "mode == EIGRP_AUTHENTICATION_HMAC_SHA256" in mode
    assert "return EIGRP_RESULT_NOT_IMPLEMENTED;" in mode
    assert "context->runtime->params.auth_type =" in mode
    assert "EIGRP_AUTH_TYPE_MD5" in mode
    assert "context->config->keychain" in keychain
    assert "context->runtime->params.auth_keychain" in keychain


def test_filter_and_redistribution_targets_retain_policy_before_runtime_boundary():
    filter_source = read(FILTER_C)
    redistribute_source = read(REDISTRIBUTE_C)
    northbound = read(NORTHBOUND_C)

    offset = function_body(filter_source, "eigrp_offset_add")
    offset_delete = function_body(filter_source, "eigrp_offset_remove")
    redist_limit = function_body(
        redistribute_source, "eigrp_redistribute_maximum_prefix_set"
    )

    assert "context->config->offsets" in offset
    assert "config->offset = offset;" in offset
    assert offset.index("context->config->offsets") < offset.index(
        "EIGRP_RESULT_NOT_IMPLEMENTED"
    )
    assert "*cursor = config->next;" in offset_delete

    assert "context->config->redistribute_policy" in redist_limit
    assert "maximum_prefix_configured = true" in redist_limit
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in redist_limit

    for target in (
        "eigrp_offset_add",
        "eigrp_offset_remove",
        "eigrp_redistribute_add",
        "eigrp_redistribute_remove",
        "eigrp_redistribute_maximum_prefix_set",
        "eigrp_redistribute_maximum_prefix_reset",
    ):
        assert target in northbound


def test_summary_metric_and_timer_targets_own_retained_state():
    summary = read(SUMMARY_C)
    metric = read(METRIC_C)
    timer = read(TIMER_C)

    auto_summary = function_body(summary, "eigrp_summary_auto_apply")
    summary_metric = function_body(summary, "eigrp_summary_metric_set")
    default_metric = function_body(metric, "eigrp_metric_default_set")
    traffic_share = function_body(metric, "eigrp_metric_traffic_share_balanced_apply")
    holddown = function_body(metric, "eigrp_metric_holddown_set")
    active_time = function_body(timer, "eigrp_timer_active_time_set")

    assert "state->auto_summary = enabled;" in auto_summary
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in auto_summary
    assert "state->metrics" in summary_metric
    assert "entry->config = *config;" in summary_metric
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in summary_metric

    assert "config->default_metric = *metric;" in default_metric
    assert "config->default_metric_configured = true;" in default_metric
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in default_metric
    assert "config->traffic_share_balanced = enabled;" in traffic_share
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in traffic_share
    assert "config->holddown_enabled = enabled;" in holddown
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in holddown

    assert "context->config->timer_config" in active_time
    assert "active_time_configured = true;" in active_time
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in active_time


def test_metric_targets_apply_supported_runtime_state_and_hop_limit():
    source = read(METRIC_C)

    weights = function_body(source, "eigrp_metric_weights_set")
    variance = function_body(source, "eigrp_metric_variance_set")
    maximum_hops = function_body(source, "eigrp_metric_maximum_hops_set")
    calculate = function_body(source, "eigrp_calculate_total_metrics")

    assert "config->weights = *weights;" in weights
    assert "context->runtime->k_values[5] = weights->k6;" in weights
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in weights

    assert "config->variance = variance;" in variance
    assert "context->runtime->variance = variance;" in variance
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in variance

    assert "config->maximum_hops = maximum_hops;" in maximum_hops
    assert "context->runtime->max_hops = maximum_hops;" in maximum_hops
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in maximum_hops

    assert "entry->total_metric.hop_count++;" in calculate
    assert "entry->total_metric.hop_count > eigrp->max_hops" in calculate
    assert "entry->total_metric.delay = EIGRP_METRIC_MAX;" in calculate
    assert "EIGRP_METRIC_MAX - link_delay" in calculate


def test_neighbor_policy_targets_retain_configuration_and_logging_state():
    source = read(NEIGHBOR_C)
    northbound = read(NORTHBOUND_C)

    description = function_body(source, "eigrp_neighbor_description_set")
    maximum_prefix = function_body(source, "eigrp_neighbor_maximum_prefix_set")
    maximum_prefix_all = function_body(
        source, "eigrp_neighbor_maximum_prefix_all_set"
    )
    log_set = function_body(source, "eigrp_neighbor_log_set")
    log_reset = function_body(source, "eigrp_neighbor_log_reset")

    assert "entry->description = copy;" in description
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in description

    assert "entry->maximum_prefix = *limit;" in maximum_prefix
    assert "entry->maximum_prefix_configured = true;" in maximum_prefix
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in maximum_prefix
    assert "state->maximum_prefix_all = *limit;" in maximum_prefix_all
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in maximum_prefix_all

    assert "case EIGRP_NEIGHBOR_LOG_CHANGES:" in log_set
    assert "state->log_changes = enabled;" in log_set
    assert "context->runtime->log_neighbor_changes = enabled;" in log_set
    assert "case EIGRP_NEIGHBOR_LOG_WARNINGS:" in log_set
    assert "state->log_warnings = enabled;" in log_set
    assert "context->runtime->log_neighbor_warning_interval" in log_set
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in log_set

    assert "case EIGRP_NEIGHBOR_LOG_CHANGES:" in log_reset
    assert "context->runtime->log_neighbor_changes = true;" in log_reset
    assert "case EIGRP_NEIGHBOR_LOG_WARNINGS:" in log_reset
    assert "eigrp_neighbor_log_reset(&context, EIGRP_NEIGHBOR_LOG_CHANGES)" in northbound


def test_neighbor_change_logging_switch_guards_adjacency_messages():
    sources = {
        "hello": read(HELLO_C),
        "neighbor": read(NEIGHBOR_C),
        "packet": read(PACKET_C),
        "update": read(UPDATE_C),
    }

    assert "if (eigrp->log_neighbor_changes)" in sources["hello"]
    assert "if (nbr->ei->eigrp->log_neighbor_changes)" in sources["neighbor"]
    assert "if (eigrp->log_neighbor_changes)" in sources["packet"]
    assert "if (eigrp->log_neighbor_changes)" in sources["update"]

    init = read(EIGRPD_C)
    assert "eigrp->log_neighbor_changes = true;" in init
    assert "eigrp->log_neighbor_warnings = true;" in init
    assert "eigrp->log_neighbor_warning_interval = 10;" in init


def test_item5_northbound_commands_terminate_at_module_targets():
    northbound = read(NORTHBOUND_C)

    targets = (
        # interface/auth
        "eigrp_interface_bandwidth_percent_set",
        "eigrp_interface_next_hop_self_set",
        "eigrp_interface_split_horizon_set",
        "eigrp_interface_shutdown_set",
        "eigrp_auth_mode_set",
        "eigrp_auth_keychain_set",
        # filtering/redistribution
        "eigrp_distribute_add",
        "eigrp_offset_add",
        "eigrp_redistribute_add",
        "eigrp_redistribute_maximum_prefix_set",
        # summaries/metrics/timers
        "eigrp_summary_create",
        "eigrp_summary_auto_set",
        "eigrp_summary_metric_set",
        "eigrp_metric_default_set",
        "eigrp_metric_weights_set",
        "eigrp_metric_variance_set",
        "eigrp_metric_traffic_share_balanced_set",
        "eigrp_metric_maximum_hops_set",
        "eigrp_metric_holddown_set",
        "eigrp_timer_active_time_set",
        # neighbor policy/logging
        "eigrp_neighbor_description_set",
        "eigrp_neighbor_maximum_prefix_set",
        "eigrp_neighbor_maximum_prefix_all_set",
        "eigrp_neighbor_log_set",
        "eigrp_neighbor_log_reset",
    )
    for target in targets:
        assert target in northbound

    result = function_body(northbound, "eigrpd_named_config_result")
    assert "result == EIGRP_RESULT_NOT_IMPLEMENTED" in result
