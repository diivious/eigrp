# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for named-mode EIGRP show/clear/debug CLI.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
VTY = ROOT / "frr" / "eigrp_cli_named.c"
CLASSIC_VTY = ROOT / "frr" / "eigrp_vty.c"
DUMP = ROOT / "eigrpd" / "eigrp_dump.c"
CLIPPY = ROOT / "frr" / "eigrp_cli_named_clippy.c"
NEIGHBOR_C = ROOT / "eigrpd" / "eigrp_neighbor.c"
NEIGHBOR_H = ROOT / "eigrpd" / "eigrp_neighbor.h"
TOPOLOGY_C = ROOT / "eigrpd" / "eigrp_topology.c"
TOPOLOGY_H = ROOT / "eigrpd" / "eigrp_topology.h"
PACKETIZER_C = ROOT / "eigrpd" / "eigrp_packetizer.c"
QUERY_C = ROOT / "eigrpd" / "eigrp_query.c"


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    start = source.index(f"{name}(void)")
    end = source.index("\n}\n", start) + 3
    return source[start:end]


def test_named_show_commands_are_defpy_and_installed():
    vty = read(VTY)
    init = function_body(vty, "eigrp_cli_named_init")

    for name in (
        "show_eigrp_neighbor",
        "show_eigrp_interface",
        "show_eigrp_topology",
        "show_eigrp_topology_all",
        "show_eigrp_accounting",
        "show_eigrp_event",
        "show_eigrp_timer",
        "show_eigrp_traffic",
        "show_eigrp_protocol",
        "show_eigrp_tech_support",
    ):
        assert f"DEFPY({name}," in vty
        assert f"&{name}_cmd" in init

    assert '"show eigrp address-family <ipv4|ipv6>$afi' in vty


def test_named_clear_commands_are_defpy_and_installed():
    vty = read(VTY)
    init = function_body(vty, "eigrp_cli_named_init")

    for name in (
        "clear_eigrp_topology",
        "clear_eigrp_topology_prefix",
        "clear_eigrp_topology_mask",
        "clear_eigrp_neighbor",
        "clear_eigrp_neighbor_interface",
        "clear_eigrp_neighbor_address",
    ):
        assert f"DEFPY({name}," in vty
        assert f"&{name}_cmd" in init

    assert '"clear eigrp address-family <ipv4|ipv6>$afi' in vty


def test_neighbor_clear_uses_one_portable_neighbor_target():
    named = read(VTY)
    classic = read(CLASSIC_VTY)
    neighbor_c = read(NEIGHBOR_C)
    neighbor_h = read(NEIGHBOR_H)

    assert "eigrp_result_t eigrp_neighbor_clear(" in neighbor_c
    assert "eigrp_result_t eigrp_neighbor_clear(" in neighbor_h
    assert "eigrp_neighbor_clear_request_t" in neighbor_h
    assert "struct vty *" not in neighbor_h

    named_clear = named[
        named.index("static void clear_eigrp_neighbor_render("):
        named.index("\n\nvoid eigrp_cli_named_init(void)")
    ]
    classic_clear = classic[
        classic.index("static void eigrp_vty_neighbor_clear_render("):
        classic.index("\nvoid eigrp_vty_show_init(void)")
    ]

    for region in (named_clear, classic_clear):
        assert "eigrp_neighbor_clear(" in region
        assert "eigrp_nbr_hard_restart(" not in region
        assert "eigrp_nbr_delete(" not in region
        assert "eigrp_nbr_state_set(" not in region
        assert "eigrp_update_send_GR(" not in region
        assert "eigrp_update_send_interface_GR(" not in region
        assert "eigrp_update_send_process_GR(" not in region
        assert "eigrp_hello_send(" not in region


def test_vty_command_names_follow_mode_eigrp_command_pattern():
    vty = read(VTY)

    command_definition_region = vty.rsplit("void eigrp_cli_named_init", 1)[0]

    assert "eigrp_vty_show" not in command_definition_region
    assert "eigrp_vty_clear" not in command_definition_region
    assert "show_eigrp_neighbor_cmd" in vty
    assert "clear_eigrp_neighbor_cmd" in vty


def test_classic_operational_commands_remain_in_eigrp_vty():
    classic = read(CLASSIC_VTY)

    assert '"show ip eigrp [vrf NAME] topology' in classic
    assert '"show ip eigrp [vrf NAME] interfaces' in classic
    assert '"show ip eigrp [vrf NAME] neighbors' in classic
    assert '"clear ip eigrp [vrf NAME] neighbors' in classic
    assert "show_eigrp_neighbor_cmd" not in classic
    assert "clear_eigrp_neighbor_cmd" not in classic


def test_named_vty_clippy_matches_new_defpy_commands():
    clippy = read(CLIPPY)

    for name in (
        "show_eigrp_neighbor",
        "show_eigrp_interface",
        "show_eigrp_topology",
        "show_eigrp_accounting",
        "show_eigrp_event",
        "show_eigrp_timer",
        "show_eigrp_traffic",
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


def test_named_operational_commands_use_owner_specific_targets():
    vty = read(VTY)
    targets = {
        "eigrp_interface_state_walk": ROOT / "eigrpd" / "eigrp_interface.c",
        "eigrp_neighbor_state_walk": ROOT / "eigrpd" / "eigrp_neighbor.c",
        "eigrp_topology_state_walk": ROOT / "eigrpd" / "eigrp_topology.c",
        "eigrp_statistics_accounting_show": ROOT / "eigrpd" / "eigrp_statistics.c",
        "eigrp_event_show": ROOT / "eigrpd" / "eigrp_event.c",
        "eigrp_timer_show": ROOT / "eigrpd" / "eigrp_timer.c",
        "eigrp_statistics_traffic_show": ROOT / "eigrpd" / "eigrp_statistics.c",
        "eigrp_status_protocol_show": ROOT / "eigrpd" / "eigrp_status.c",
        "eigrp_status_tech_support_show": ROOT / "eigrpd" / "eigrp_status.c",
    }

    assert "show_eigrp_stub" not in vty
    assert "eigrp_cli_not_configured" not in vty
    assert not (ROOT / "eigrpd" / "eigrp_operational.c").exists()
    assert not (ROOT / "eigrpd" / "eigrp_operational.h").exists()
    for target, source in targets.items():
        assert target in vty
        assert f"{target}(" in read(source)

    # Explicitly excluded by cli-spec.md.
    assert "install_element(VIEW_NODE, &show_eigrp_plugin_cmd);" not in vty



def test_named_topology_clear_uses_portable_topology_target():
    named = read(VTY)
    topology_c = read(TOPOLOGY_C)
    topology_h = read(TOPOLOGY_H)
    packetizer_c = read(PACKETIZER_C)
    query_c = read(QUERY_C)

    assert "eigrp_result_t eigrp_topology_clear(" in topology_c
    assert "eigrp_result_t eigrp_topology_clear(" in topology_h
    assert "eigrp_topology_clear_request_t" in topology_h
    assert "struct vty *" not in topology_h

    clear_region = named[
        named.index("static bool eigrp_vty_ipv4_mask_prefix_length("):
        named.index("static void clear_eigrp_neighbor_render(")
    ]
    assert "eigrp_topology_clear(&context, &request, &affected)" in clear_region
    assert "eigrp_prefix_descriptor_delete" not in clear_region
    assert "eigrp_query_send_route" not in clear_region

    assert '"clear eigrp [(1-65535)$as] [vrf <NAME$vrf|all$vrf_all>] <ipv4|ipv6>$afi topology"' in named
    assert "A.B.C.D/M$ipv4_prefix" in named
    assert "X:X::X:X/M$ipv6_prefix" in named
    assert "A.B.C.D$network A.B.C.D$mask" in named

    assert "eigrp_query_send_route(eigrp, prefix, query_route" in topology_c
    assert "eigrp_packetizer_prefix_defer_free(eigrp, prefix)" in topology_c
    assert "EIGRP_FSM_STATE_ACTIVE_1" in topology_c
    assert "void eigrp_query_send_route(" in query_c
    assert "if (work->flags & EIGRP_PACKETIZER_WORK_F_DEFER_FREE)" in packetizer_c


def test_named_topology_clear_clippy_adapters_exist():
    clippy = read(CLIPPY)

    for name in (
        "clear_eigrp_topology",
        "clear_eigrp_topology_prefix",
        "clear_eigrp_topology_mask",
    ):
        assert f"DEFUN_CMD_FUNC_DECL({name})" in clippy
        assert f"#define funcdecl_{name}" in clippy
        assert f"return {name}_magic(" in clippy

    assert 'varname, "vrf_all"' in clippy
    assert 'varname, "ipv4_prefix"' in clippy
    assert 'varname, "ipv6_prefix"' in clippy
    assert 'varname, "network"' in clippy
    assert 'varname, "mask"' in clippy


def test_named_topology_clear_retains_ipv6_command_target_until_runtime_exists():
    topology = read(TOPOLOGY_C)

    assert "context->config->afi == EIGRP_ADDRESS_FAMILY_IPV6" in topology
    assert "return EIGRP_RESULT_NOT_IMPLEMENTED;" in topology

def test_named_show_path_does_not_fall_back_to_legacy_vty_dumpers():
    vty = read(VTY)
    show_region = vty[vty.index("DEFPY(show_eigrp_interface,"):vty.index("DEFPY(clear_eigrp_neighbor,")]

    assert "show_ip_eigrp_" not in show_region
    assert "struct vty *" not in read(ROOT / "eigrpd" / "eigrp_statistics.h")
    assert "struct vty *" not in read(ROOT / "eigrpd" / "eigrp_status.h")
    assert "struct vty *" not in read(ROOT / "eigrpd" / "eigrp_timer.h")


def test_named_topology_destination_accepts_ipv4_and_ipv6_text():
    vty = read(VTY)
    clippy = read(CLIPPY)

    assert "topology WORD$target [all-links]$all" in vty
    assert "inet_pton(family, address, destination->address.bytes)" in vty
    assert "EIGRP_ADDRESS_FAMILY_IPV6" in vty
    assert "const char * target" in clippy
    assert 'varname, "target"' in clippy


def test_named_show_clippy_forwards_address_family_filters():
    clippy = read(CLIPPY)

    for name in (
        "show_eigrp_accounting",
        "show_eigrp_event",
        "show_eigrp_timer",
        "show_eigrp_traffic",
    ):
        start = clippy.index(f"/* {name} =>")
        end = clippy.find("\n/* ", start + 4)
        block = clippy[start:end if end != -1 else None]
        assert f"return {name}_magic(self, vty, argc, argv, afi, vrf, as, as_str);" in block
        assert f"{name}_magic(self, vty, argc, argv, NULL, NULL, 0, NULL)" not in block


def test_traffic_show_marks_unmaintained_counters_unavailable():
    statistics = read(ROOT / "eigrpd" / "eigrp_statistics.c")
    vty = read(VTY)

    assert "sent_valid" in statistics
    assert "received_valid" in statistics
    assert "state->sent_valid & field" in vty
    assert "state->received_valid & field" in vty
    assert "n/a means the current packet path does not maintain that counter" in vty


def test_protocol_and_tech_support_walk_all_named_vrfs():
    status = read(ROOT / "eigrpd" / "eigrp_status.c")
    types = read(ROOT / "eigrpd" / "eigrp_types.h")

    assert "bool all_vrfs" in types
    assert ".all_vrfs = true" in status
