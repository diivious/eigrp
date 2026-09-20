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
NORTHBOUND_C = ROOT / "frr" / "eigrp_northbound.c"
NORTHBOUND_H = ROOT / "frr" / "eigrp_northbound.h"
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
    assert '"clear eigrp events"' in vty
    assert 'events"' in vty


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


def test_named_neighbor_clear_accepts_ipv6_at_northbound_boundary():
    named = read(VTY)
    clippy = read(CLIPPY)
    northbound_c = read(NORTHBOUND_C)
    northbound_h = read(NORTHBOUND_H)
    neighbor_h = read(NEIGHBOR_H)

    assert (
        'neighbors <A.B.C.D$ipv4_addr|X:X::X:X$ipv6_addr> [soft]$soft'
        in named
    )
    assert "struct in6_addr ipv6_addr" in clippy
    assert "inet_pton(AF_INET6, argv[_i]->arg, &ipv6_addr)" in clippy

    # FRR host values stop at the northbound adapter.  It copies the parsed
    # address into the portable runtime eigrp_addr_t union before calling core.
    assert "eigrp_northbound_neighbor_clear_address(" in northbound_h
    assert "destination->ip.v4 = *ipv4_address;" in northbound_c
    assert "destination->ip.v6 = *ipv6_address;" in northbound_c
    assert "return eigrp_neighbor_clear(runtime, &request" in northbound_c
    assert "const eigrp_addr_t *address;" in neighbor_h
    assert "eigrp_addr_t address;" in neighbor_h

    clear_address = named[
        named.index("static void clear_eigrp_neighbor_address_cb("):
        named.index("DEFPY(clear_eigrp_neighbor,", named.index("static void clear_eigrp_neighbor_address_cb("))
    ]
    assert "eigrp_northbound_neighbor_clear_address(" in clear_address
    assert "inet_pton(" not in clear_address


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
    assert '"show ip eigrp [vrf NAME$vrf] events' in classic
    assert '"clear ip eigrp [vrf NAME$vrf] events' in classic
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



def test_debug_eigrp_packet_uses_portable_targets_and_retry_category():
    dump = read(DUMP)
    header = read(ROOT / "eigrpd" / "eigrp_dump.h")

    debug_region = dump[
        dump.index("DEFUN(debug_eigrp_packet,"):
        dump.index("/* Debug node. */")
    ]
    assert "eigrp_debug_packet_set(type, flag, scope)" in debug_region
    assert "eigrp_debug_packet_reset(type, flag, scope)" in debug_region
    assert "DEBUG_PACKET_ON" not in debug_region
    assert "DEBUG_PACKET_OFF" not in debug_region
    assert "EIGRP_DEBUG_RETRY" in debug_region
    assert "ipxsap" not in debug_region
    assert "stub" not in debug_region

    target_region = header[
        header.index("/* Packet-debug targets and runtime hooks. */"):
        header.index("/* Prototypes. */")
    ]
    assert "eigrp_result_t eigrp_debug_packet_set(" in target_region
    assert "eigrp_result_t eigrp_debug_packet_reset(" in target_region
    assert "struct vty *" not in target_region


def test_debug_eigrp_common_code_does_not_depend_on_frr_vrf_default_macro():
    dump = read(DUMP)

    assert "VRF_DEFAULT" not in dump


def test_debug_eigrp_packet_runtime_hooks_cover_send_receive_retry_and_ack():
    dump = read(DUMP)
    packet = read(ROOT / "eigrpd" / "eigrp_packet.c")

    assert "eigrp_debug_packet_send(ei, packet, ret);" in packet
    assert "eigrp_debug_packet_receive(ei, &src, &dst, eigrph, length);" in packet
    assert packet.count("eigrp_debug_packet_retry(nbr, packet") == 2

    classifier = dump[
        dump.index("eigrp_debug_packet_category_get("):
        dump.index("static uint16_t eigrp_debug_get16", dump.index("eigrp_debug_packet_category_get("))
    ]
    assert "header->opcode == EIGRP_OPC_HELLO" in classifier
    assert "ntohl(header->sequence) == 0" in classifier
    assert "ntohl(header->ack) != 0" in classifier
    assert "return EIGRP_DEBUG_PACKET_ACK;" in classifier


def test_debug_eigrp_packet_detail_walks_tlvs_without_exposing_auth_digest():
    dump = read(DUMP)

    detail = dump[
        dump.index("static void eigrp_debug_packet_detail_dump"):
        dump.index("void eigrp_debug_packet_send", dump.index("static void eigrp_debug_packet_detail_dump"))
    ]
    assert "EIGRP_TLV_HDR_SIZE" in detail
    assert "tlv_length > length - offset" in detail
    assert "eigrp_debug_tlv_detail_dump" in detail
    assert "digest length" in dump
    assert "digest %" not in dump

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

    assert "CLI/VTY/debug" in design
    assert "command-surface rules are owned by `cli-spec.md`." in design
    assert "## 3. Named-Mode CLI Direction" not in design
    assert "## 4. Named-Mode VTY and Debug CLI Direction" not in design

    assert "## 3. Classic and named entry forms" in cli
    assert "## 10. Named operational commands" in cli
    assert "debug eigrp packet" in cli
    assert "show_eigrp_neighbor_cmd" in cli


def test_named_operational_commands_use_owner_specific_targets():
    vty = read(VTY)
    targets = {
        "eigrp_interface_state_walk": ROOT / "eigrpd" / "eigrp_interface.c",
        "eigrp_neighbor_state_walk": ROOT / "eigrpd" / "eigrp_neighbor.c",
        "eigrp_topology_state_walk": ROOT / "eigrpd" / "eigrp_topology.c",
        "eigrp_statistics_accounting_show": ROOT / "eigrpd" / "eigrp_statistics.c",
        "eigrp_eventlog_show": ROOT / "eigrpd" / "eigrp_eventlog.c",
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

    assert "topology [(1-65535)$as] WORD$target [all-links]$all" in vty
    assert "inet_pton(family, address, destination->address.bytes)" in vty
    assert "EIGRP_ADDRESS_FAMILY_IPV6" in vty
    assert "const char * target" in clippy
    assert 'varname, "target"' in clippy


def test_topology_show_places_optional_as_after_topology_keyword():
    named = read(VTY)
    classic = read(CLASSIC_VTY)

    assert (
        '"show eigrp address-family <ipv4|ipv6>$afi [vrf NAME$vrf] '
        '[multicast] topology [(1-65535)$as] [all-links]$all"'
        in named
    )
    assert (
        '"show ip eigrp [vrf NAME] topology [(1-65535)$as] '
        '[all-links$all]"'
        in classic
    )
    assert "[(1-65535)$as] [multicast] topology" not in named[
        named.index("DEFPY(show_eigrp_topology_all,"):
        named.index("struct eigrp_vty_accounting_show")
    ]


def test_topology_show_uses_common_runtime_instance_walk():
    named = read(VTY)
    classic = read(CLASSIC_VTY)
    topology = read(TOPOLOGY_C)
    header = read(ROOT / "eigrpd" / "eigrp_topology.h")

    assert "eigrp_topology_instance_walk(" in header
    assert "for (ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp" in topology
    assert "runtime->af_vectors.afi != afi" in topology
    assert "runtime->vrf_id != vrf_id" in topology
    assert "if (asn && runtime->AS != asn)" in topology
    assert "eigrp_topology_instance_walk(" in named
    assert "eigrp_topology_instance_walk(" in classic


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


def test_traffic_show_reports_all_ipv4_packet_counters():
    statistics = read(ROOT / "eigrpd" / "eigrp_statistics.c")
    vty = read(VTY)

    assert "Validated packet I/O owns all IPv4 traffic counters." in statistics
    assert "state->received_valid = state->sent_valid;" in statistics
    for label in (
        "Hellos sent/received",
        "Updates sent/received",
        "Queries sent/received",
        "Replies sent/received",
        "Acks sent/received",
        "SIA-Queries sent/received",
        "SIA-Replies sent/received",
    ):
        assert label in vty
    assert "n/a means the current packet path does not maintain that counter" not in vty


def test_protocol_and_tech_support_walk_all_named_vrfs():
    status = read(ROOT / "eigrpd" / "eigrp_status.c")
    types = read(ROOT / "eigrpd" / "eigrp_types.h")

    assert "bool all_vrfs" in types
    assert ".all_vrfs = true" in status


def test_complete_eigrp_debug_command_family_is_registered_and_targeted():
    dump = read(DUMP)
    header = read(ROOT / "eigrpd" / "eigrp_dump.h")
    init = function_body(dump, "eigrp_debug_init")

    command_fragments = (
        '"debug eigrp event [detail]"',
        '"debug eigrp timers"',
        '"debug eigrp fsm"',
        '"debug eigrp nsf"',
        '"debug eigrp frr"',
        '"debug eigrp neighbor [siatimer] [static]"',
        '"debug eigrp notifications <rib|interface>"',
        '"debug eigrp transmit [ack] [build] [detail] [link] [packetize] [peerdown] [sia] [startup] [strange]"',
        '"debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)]"',
        '"debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] neighbor [WORD]"',
        '"debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] notifications"',
        '"debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] summary"',
    )
    for fragment in command_fragments:
        assert fragment in dump
        assert fragment.replace('"debug ', '"no debug ') in dump

    commands = (
        "event", "timers", "fsm", "nsf", "frr", "neighbor",
        "notifications", "transmit", "address_family",
        "address_family_neighbor", "address_family_notifications",
        "address_family_summary", "packet",
    )
    for name in commands:
        assert f"&debug_eigrp_{name}_cmd" in init
        assert f"&no_debug_eigrp_{name}_cmd" in init

    targets = (
        "eigrp_debug_event_set", "eigrp_debug_event_reset",
        "eigrp_debug_timers_set", "eigrp_debug_timers_reset",
        "eigrp_debug_fsm_set", "eigrp_debug_fsm_reset",
        "eigrp_debug_nsf_set", "eigrp_debug_nsf_reset",
        "eigrp_debug_fast_reroute_set", "eigrp_debug_fast_reroute_reset",
        "eigrp_debug_neighbor_set", "eigrp_debug_neighbor_reset",
        "eigrp_debug_notifications_set", "eigrp_debug_notifications_reset",
        "eigrp_debug_transmit_set", "eigrp_debug_transmit_reset",
        "eigrp_debug_address_family_set", "eigrp_debug_address_family_reset",
    )
    for target in targets:
        assert target in header
        assert f"{target}(" in dump


def test_debug_families_have_runtime_hooks_at_their_protocol_owners():
    fsm = read(ROOT / "eigrpd" / "eigrp_fsm.c")
    neighbor = read(NEIGHBOR_C)
    packet = read(ROOT / "eigrpd" / "eigrp_packet.c")
    packetizer = read(PACKETIZER_C)
    update = read(ROOT / "eigrpd" / "eigrp_update.c")
    siaquery = read(ROOT / "eigrpd" / "eigrp_siaquery.c")
    siareply = read(ROOT / "eigrpd" / "eigrp_siareply.c")
    topology = read(TOPOLOGY_C)
    zebra = read(ROOT / "frr" / "eigrp_zebra.c")

    assert "EIGRP_DEBUG_AF_ROUTE" in fsm
    assert "eigrp_debug_neighbor_state" in neighbor
    assert "EIGRP_DEBUG_TRANSMIT_PEERDOWN" in neighbor
    assert "EIGRP_DEBUG_NEI_STATIC" in neighbor
    assert "EIGRP_DEBUG_TRANSMIT_ACK" in packet
    assert "EIGRP_DEBUG_TRANSMIT_LINK" in packet
    assert "EIGRP_DEBUG_TRANSMIT_PACKETIZE" in packetizer
    assert "EIGRP_DEBUG_TRANSMIT_STARTUP" in update
    assert "eigrp_debug_nsf_event" in update
    assert "eigrp_debug_neighbor_sia" in siaquery
    assert "eigrp_debug_neighbor_sia" in siareply
    assert "EIGRP_DEBUG_TRANSMIT_SIA" in siaquery
    assert "EIGRP_DEBUG_TRANSMIT_SIA" in siareply
    assert "FAST_REROUTE" in topology
    assert "EIGRP_DEBUG_AF_NOTIFICATIONS" in zebra
