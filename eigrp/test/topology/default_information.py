# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())


def body(path, name):
    text = path.read_text()
    start = text.index(name + "(")
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[brace:pos + 1]
    raise AssertionError(name)


def test_default_information_target_owns_directional_state_and_reset():
    topology = ROOT / "eigrp/code/eigrp_topology.c"
    instance = (ROOT / "eigrp/code/eigrp_instance.h").read_text()
    target = body(topology, "eigrp_topology_default_information_update")

    assert "default_information_enabled[2]" in instance
    assert "default_information_access_list[2]" in instance
    assert "af->default_information_enabled[direction] = enabled" in target
    assert "free(af->default_information_access_list[direction])" in target
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in target


def test_default_information_policy_is_default_prefix_only_and_fail_closed():
    topology = ROOT / "eigrp/code/eigrp_topology.c"
    decision = body(topology, "eigrp_topology_default_information_denies")

    assert "prefix->prefix_length == 0" in body(topology, "eigrp_topology_default_prefix")
    assert "EIGRP_AFI_IPV4" in body(topology, "eigrp_topology_default_prefix")
    assert "EIGRP_AFI_IPV6" in body(topology, "eigrp_topology_default_prefix")
    assert "eigrp_sys_filter_evaluate" in decision
    assert "EIGRP_DISTRIBUTE_ACCESS_LIST" in decision
    assert "result != EIGRP_RESULT_SUCCESS" in decision
    assert "decision != EIGRP_FILTER_DECISION_PERMIT" in decision


def test_default_information_uses_common_filter_and_resync_paths():
    filter_c = (ROOT / "eigrp/code/eigrp_filter.c").read_text()
    topology = ROOT / "eigrp/code/eigrp_topology.c"
    refresh = body(topology, "eigrp_topology_default_information_refresh")

    assert "eigrp_topology_default_information_denies" in filter_c
    assert "eigrp_update_send_process_GR(eigrp, EIGRP_GR_FILTER)" in refresh
    assert "request.soft = true" in refresh
    assert "eigrp_nbr_clear(eigrp, &request" in refresh
