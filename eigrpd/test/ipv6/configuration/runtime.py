# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
INTERFACE = ROOT / "eigrpd" / "code" / "eigrp_interface.c"
STRUCTS = ROOT / "eigrpd" / "code" / "eigrp_structs.h"
PACKETIZER = ROOT / "eigrpd" / "code" / "eigrp_packetizer.c"
TOPOLOGY = ROOT / "eigrpd" / "code" / "eigrp_topology.c"
INSTANCE = ROOT / "eigrpd" / "code" / "eigrp_instance.c"
TIMER = ROOT / "eigrpd" / "code" / "eigrp_timer.c"
NEIGHBOR = ROOT / "eigrpd" / "code" / "eigrp_neighbor.c"
AUTH = ROOT / "eigrpd" / "code" / "eigrp_auth.c"


def read(path):
    return path.read_text()


def body(source, name):
    start = source.index(name + "(")
    brace = source.index("{", start)
    depth = 0
    for i in range(brace, len(source)):
        depth += source[i] == "{"
        depth -= source[i] == "}"
        if depth == 0:
            return source[start:i + 1]
    raise AssertionError(name)


def test_af_interface_runtime_mutation_is_af_neutral():
    source = read(INTERFACE)
    structs = read(STRUCTS)
    assert "uint32_t bandwidth_percent;" in structs
    assert "bool next_hop_self;" in structs
    for name, mutation in (
        ("eigrp_intf_bandwidth_percent_update", "context->runtime->bandwidth_percent = percent;"),
        ("eigrp_intf_hello_interval_update", "context->runtime->params.v_hello = seconds;"),
        ("eigrp_intf_hold_time_update", "context->runtime->params.v_wait = seconds;"),
        ("eigrp_intf_passive_update", "context->runtime->params.passive_interface"),
        ("eigrp_intf_nexthop_self_update", "context->runtime->next_hop_self = enabled;"),
        ("eigrp_intf_split_horizon_update", "context->runtime->split_horizon = enabled;"),
        ("eigrp_intf_shutdown_update", "eigrp_intf_down(context->runtime)"),
    ):
        block = body(source, name)
        assert mutation in block
        assert "EIGRP_AFI_IPV4" not in block


def test_next_hop_self_changes_the_actual_route_wire_image():
    packetizer = read(PACKETIZER)
    block = body(packetizer, "eigrp_packetizer_builder_route_add")
    assert "if (builder->ei->next_hop_self)" in block
    assert "memset(&wire_route.nexthop, 0, sizeof(wire_route.nexthop));" in block


def test_ipv6_topology_clear_uses_the_real_shared_runtime_path():
    block = body(read(TOPOLOGY), "eigrp_topology_clear")
    assert "EIGRP_AFI_IPV6" in block
    assert "eigrp_topology_clear_all(context->runtime" in block
    assert "eigrp_topology_clear_prefix(context->runtime, prefix)" in block
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in block


def test_distance_and_active_timer_runtime_paths_are_shared_by_af():
    instance = read(INSTANCE)
    timer = read(TIMER)
    distance = body(instance, "eigrp_instance_distance_update")
    active = body(timer, "eigrp_timer_active_time_update")
    assert "context->runtime->distance_internal" not in distance  # target takes AF config
    assert "af->runtime->distance_internal = internal_distance;" in distance
    assert "eigrp_rib_routes_replay_instance(af->runtime);" in distance
    assert "EIGRP_AFI_IPV4" not in distance
    assert "active_time_seconds = seconds;" in active
    assert "EIGRP_AFI_IPV4" not in active


def test_runtime_feature_targets_do_not_hide_incomplete_work():
    neighbor = read(NEIGHBOR)
    topology = read(TOPOLOGY)
    auth = read(AUTH)
    assert "eigrp_nbr_prefix_admit" in neighbor
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in body(neighbor, "eigrp_nbr_max_prefix_update")
    assert "eigrp_topology_prefix_admit" in topology
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in body(topology, "eigrp_topology_max_prefix_update")
    auth_mode = body(auth, "eigrp_auth_mode_update")
    assert "hmac->encryption_type == 7" in auth_mode
    assert "return EIGRP_RESULT_UNSUPPORTED;" in auth_mode
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in auth_mode
