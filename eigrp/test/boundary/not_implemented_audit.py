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
    instance = ROOT / "eigrp/code/eigrp_instance.c"
    topology = ROOT / "eigrp/code/eigrp_topology.c"
    summary = ROOT / "eigrp/code/eigrp_summary.c"

    assert "return EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        instance, "eigrp_named_config_shutdown_update")
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
    source = ROOT / "eigrp/code/eigrp_instance.c"
    target = body(source, "eigrp_named_config_shutdown_update")
    assert target.index("parent->shutdown = shutdown;") < target.index(
        "eigrp_instance_stop")
    assert "if (!af->runtime || (!shutdown && af->shutdown))" in target
    assert "aggregate = result" in target


def test_topology_api_does_not_reject_non_base_tid():
    source = ROOT / "eigrp/code/eigrp_topology.c"
    create = body(source, "eigrp_topology_create")
    delete = body(source, "eigrp_topology_delete")
    validate = body(source, "eigrp_topology_context_validate")
    assert "topology_base_configured = true" in create
    assert "topology_base_configured = false" in delete
    assert "context->topology_id != EIGRP_TOPOLOGY_ID_BASE" not in validate
    assert "EIGRP_RESULT_NOT_IMPLEMENTED" not in validate


def test_multicast_af_is_not_exposed_by_current_frr_cli():
    cli = ROOT / "frr/code/eigrp_cli_named.c"
    text = read(cli)
    assert "[multicast]" not in text
    assert "eigrp_af_config_iterate" in text


def test_no_eigrp_stub_runtime_symbols_added():
    production = []
    for base in (ROOT / "eigrp/code", ROOT / "frr/code", ROOT / "unix/code"):
        production.extend(base.glob("*.c"))
        production.extend(base.glob("*.h"))
    text = "\n".join(read(path) for path in production)
    assert not re.search(r"\\beigrp_(?:stub|stub_router|stub_route)_", text)


def test_completed_runtime_paths_do_not_tolerate_not_implemented():
    instance = read(ROOT / "eigrp/code/eigrp_instance.c")
    interface = read(ROOT / "eigrp/code/eigrp_interface.c")
    filt = read(ROOT / "eigrp/code/eigrp_filter.c")
    redist = read(ROOT / "eigrp/code/eigrp_redistribute.c")

    assert "NOT_IMPLEMENTED is a truthful data-path boundary" not in instance
    assert "result != EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        ROOT / "eigrp/code/eigrp_instance.c", "eigrp_af_config_shutdown_update")
    assert "result == EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        ROOT / "eigrp/code/eigrp_instance.c", "eigrp_named_config_shutdown_update")
    assert "reports NOT_IMPLEMENTED" not in interface
    assert "result == EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        ROOT / "eigrp/code/eigrp_filter.c", "eigrp_distribute_runtime_result_committable")
    assert "result != EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        ROOT / "eigrp/code/eigrp_filter.c", "eigrp_distribute_remove")
    assert "result == EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        ROOT / "eigrp/code/eigrp_redistribute.c", "eigrp_redist_runtime_result_committable")
    assert "result != EIGRP_RESULT_NOT_IMPLEMENTED" not in body(
        ROOT / "eigrp/code/eigrp_redistribute.c", "eigrp_redist_remove")


def test_production_targets_do_not_return_or_tolerate_not_implemented():
    for base in (ROOT / "eigrp/code", ROOT / "frr/code", ROOT / "unix/code"):
        for source in base.glob("*.c"):
            text = read(source)
            assert "return EIGRP_RESULT_NOT_IMPLEMENTED" not in text, source
            assert "== EIGRP_RESULT_NOT_IMPLEMENTED" not in text, source
            assert "!= EIGRP_RESULT_NOT_IMPLEMENTED" not in text, source

    renderer = body(ROOT / "frr/code/eigrp_cli_named.c", "eigrp_cli_result_render")
    assert "case EIGRP_RESULT_NOT_IMPLEMENTED:" in renderer


def test_frr_named_hmac_cli_exposes_plaintext_and_type7_password_forms():
    cli = read(ROOT / "frr/code/eigrp_cli_named.c")
    assert 'authentication mode <md5|hmac-sha-256 <0|7> WORD>' in cli
    auth_mode = body(ROOT / "eigrp/code/eigrp_auth.c", "eigrp_auth_mode_update")
    assert "hmac->encryption_type == 7" in auth_mode
    assert "return EIGRP_RESULT_UNSUPPORTED;" in auth_mode
