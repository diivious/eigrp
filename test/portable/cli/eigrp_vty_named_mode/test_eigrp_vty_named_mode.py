# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for named-mode EIGRP show/clear/debug CLI.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
VTY = ROOT / "frr" / "eigrp_vty.c"
DUMP = ROOT / "eigrpd" / "eigrp_dump.c"
CLIPPY = ROOT / "frr" / "eigrp_vty_clippy.c"


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    start = source.index(f"{name}(void)")
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def test_named_show_commands_are_defpy_and_installed():
    vty = read(VTY)
    init = function_body(vty, "eigrp_vty_show_init")

    for name in (
        "show_eigrp_neighbor",
        "show_eigrp_interface",
        "show_eigrp_topology",
        "show_eigrp_topology_all",
    ):
        assert f"DEFPY({name}," in vty
        assert f"&{name}_cmd" in init

    assert '"show eigrp address-family <ipv4|ipv6>$afi' in vty


def test_named_clear_commands_are_defpy_and_installed():
    vty = read(VTY)
    init = function_body(vty, "eigrp_vty_init")

    for name in (
        "clear_eigrp_neighbor",
        "clear_eigrp_neighbor_interface",
        "clear_eigrp_neighbor_address",
    ):
        assert f"DEFPY({name}," in vty
        assert f"&{name}_cmd" in init

    assert '"clear eigrp address-family <ipv4|ipv6>$afi' in vty


def test_vty_command_names_follow_mode_eigrp_command_pattern():
    vty = read(VTY)

    command_definition_region = vty.rsplit("void eigrp_vty_show_init", 1)[0]

    assert "eigrp_vty_show" not in command_definition_region
    assert "eigrp_vty_clear" not in command_definition_region
    assert "show_eigrp_neighbor_cmd" in vty
    assert "clear_eigrp_neighbor_cmd" in vty


def test_named_vty_clippy_matches_new_defpy_commands():
    clippy = read(CLIPPY)

    for name in (
        "show_eigrp_neighbor",
        "show_eigrp_interface",
        "show_eigrp_topology",
        "clear_eigrp_neighbor",
    ):
        assert f"DEFUN_CMD_FUNC_DECL({name})" in clippy
        assert f"{name}_magic" in clippy


def test_debug_eigrp_packet_is_singular_and_registered():
    dump = read(DUMP)

    assert '"debug eigrp packet <' in dump
    assert '"no debug eigrp packet <' in dump
    assert "debug_eigrp_packet_cmd" in dump
    assert "no_debug_eigrp_packet_cmd" in dump
    assert "debug_eigrp_packets_all_cmd" not in dump
    assert '"debug eigrp packets' not in dump


def test_debug_eigrp_supports_event_timer_neighbor_transmit_packet():
    dump = read(DUMP)
    init = function_body(dump, "eigrp_debug_init")

    for name in (
        "debug_eigrp_event_cmd",
        "debug_eigrp_timers_cmd",
        "debug_eigrp_neighbor_cmd",
        "debug_eigrp_transmit_cmd",
        "debug_eigrp_packet_cmd",
    ):
        assert f"&{name}" in init


def test_cli_implementation_notes_are_in_cli_spec():
    design = read(ROOT / "specs" / "design-spec.md")
    cli = read(ROOT / "specs" / "cli-spec.md")

    assert "CLI/VTY/debug command-surface rules are owned by `cli-spec.md`." in design
    assert "## 3. Named-Mode CLI Direction" not in design
    assert "## 4. Named-Mode VTY and Debug CLI Direction" not in design

    assert "## 3. Classic and Named-Mode CLI Direction" in cli
    assert "## 8. Named-Mode Show, Clear, and Debug Direction" in cli
    assert "debug eigrp packet" in cli
    assert "show_eigrp_neighbor_cmd" in cli


def test_named_operational_commands_use_feature_specific_targets():
    vty = read(VTY)
    operational = read(ROOT / "eigrpd" / "eigrp_operational.c")

    assert "show_eigrp_stub" not in vty
    assert "eigrp_cli_not_configured" not in vty
    for target in (
        "eigrp_accounting_show",
        "eigrp_event_show",
        "eigrp_timer_show",
        "eigrp_traffic_show",
        "eigrp_protocol_show",
        "eigrp_tech_support_show",
    ):
        assert target in vty
        assert f"{target}(" in operational

    # Explicitly excluded by cli-spec.md.
    assert "install_element(VIEW_NODE, &show_eigrp_plugin_cmd);" not in vty
