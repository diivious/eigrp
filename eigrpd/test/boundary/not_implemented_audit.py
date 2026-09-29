from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]


def read(path):
    return path.read_text()


def body(source, name):
    text = read(source)
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


def test_completed_targets_do_not_regress_to_not_implemented():
    instance = ROOT / "eigrpd/code/eigrp_instance.c"
    topology = ROOT / "eigrpd/code/eigrp_topology.c"
    summary = ROOT / "eigrpd/code/eigrp_summary.c"

    assert "return EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        instance, "eigrp_instance_parent_shutdown_update")
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        topology, "eigrp_topology_create")
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        topology, "eigrp_topology_delete")
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        topology, "eigrp_topology_default_information_update")
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        summary, "eigrp_summary_create")
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        summary, "eigrp_summary_delete")


def test_parent_retention_precedes_runtime_capability_result():
    source = ROOT / "eigrpd/code/eigrp_instance.c"
    target = body(source, "eigrp_instance_parent_shutdown_update")
    assert target.index("parent->shutdown = shutdown;") < target.index(
        "eigrp_af_instance_stop")
    assert "if (!af->runtime || (!shutdown && af->shutdown))" in target
    assert "aggregate = result" in target


def test_base_topology_is_real_retained_state_but_non_base_stays_boundary():
    source = ROOT / "eigrpd/code/eigrp_topology.c"
    create = body(source, "eigrp_topology_create")
    delete = body(source, "eigrp_topology_delete")
    validate = body(source, "eigrp_topology_context_validate")
    assert "topology_base_configured = true" in create
    assert "topology_base_configured = false" in delete
    assert "context->topology_id != EIGRP_TOPOLOGY_ID_BASE" in validate
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in validate


def test_maf_boundary_is_owned_by_portable_state_request():
    cli = ROOT / "frr/code/eigrp_cli_named.c"
    walk = body(cli, "eigrp_vty_topology_walk")
    instance = body(ROOT / "eigrpd/code/eigrp_instance.c",
                    "eigrp_af_instance_iterate")
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in walk
    assert "eigrp_af_instance_iterate" in walk
    assert "request->multicast" in instance
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" in instance


def test_no_eigrp_stub_runtime_symbols_added():
    production = []
    for base in (ROOT / "eigrpd/code", ROOT / "frr/code", ROOT / "unix/code"):
        production.extend(base.glob("*.c"))
        production.extend(base.glob("*.h"))
    text = "\n".join(read(path) for path in production)
    assert not re.search(r"\\beigrp_(?:stub|stub_router|stub_route)_", text)
