# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for the host-shim boundary.  Host adapters normalize
# host state and provide host services; EIGRP decisions stay in common code.

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / path).read_text()


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


def test_design_spec_defines_narrow_host_shim_contract():
    design = read("specs/design-spec.md")

    assert "convert host/system objects, values, and events to or from EIGRP-owned types" in design
    assert "abstract host/system calls and services" in design
    assert "prevent EIGRP behavior from being reimplemented separately" in design
    assert "If FRR, BIRD, macOS, and another host should" in design
    assert "that decision belongs in `eigrpd/`" in design


def test_frr_southbound_does_not_own_common_eigrp_lifecycle_or_network_decisions():
    southbound = read("frr/eigrp_southbound.c")
    forbidden = (
        "eigrp_get(",
        "eigrp_get_by_af(",
        "eigrp_lookup_by_af_as_vrf(",
        "eigrp_finish_final(",
        "eigrp_router_id_update(",
        "eigrp_intf_up(",
        "eigrp_intf_down(",
        "eigrp_intf_free(",
        "network_interface_match",
        "route_top(eigrp->networks)",
        "eigrp->networks",
        "eigrp_southbound_network_create(",
        "eigrp_southbound_network_delete(",
        "eigrp_southbound_address_family_start(",
        "eigrp_southbound_address_family_stop(",
        "eigrp_southbound_instance_create(",
        "eigrp_southbound_instance_delete(",
    )
    for token in forbidden:
        assert token not in southbound

    assert "FOR_ALL_INTERFACES" in southbound
    assert "vrf_lookup_by_name" in southbound
    assert "vrf_lookup_by_id" in southbound
    assert "event_add_" in southbound
    assert "work_queue_new" in southbound
    assert "vrf_socket" in southbound
    assert "setsockopt" in southbound


def test_interface_adapter_reports_host_facts_and_common_code_decides_participation():
    adapter = read("frr/eigrp_frr.c")
    southbound = read("frr/eigrp_southbound.c")
    types = read("eigrpd/eigrp_types.h")
    network = read("eigrpd/eigrp_network.c")
    interface = read("eigrpd/eigrp_interface.c")

    imported = function_body(adapter, "eigrp_frr_interface_state_import")
    walk = function_body(southbound, "eigrp_southbound_interface_walk")
    matches = function_body(network, "eigrp_network_runtime_matches")
    refresh = function_body(interface, "eigrp_interface_runtime_refresh")

    assert "bool secondary;" in types
    assert "state->secondary = secondary;" in imported
    assert "state->operative = if_is_operative(ifp);" in imported
    assert "state->bandwidth = ifp->bandwidth;" in imported
    assert "state->mtu = ifp->mtu;" in imported

    # The host walk reports addresses as host facts.  It does not decide that
    # only IPv4 primary addresses participate in EIGRP.
    assert "co->address->family != AF_INET" not in walk
    assert "ZEBRA_IFA_SECONDARY" in walk
    assert "eigrp_frr_interface_state_import" in walk

    assert "eigrp_prefix_address_match" in matches
    assert "state->secondary" in network
    assert "eigrp_instance_runtime_config(eigrp)" in refresh
    assert "config && config->shutdown" in refresh
    assert "old_mtu != state->mtu" in refresh
    assert "eigrp_intf_up(eigrp, ei)" in refresh


def test_common_instance_code_owns_runtime_identity_and_address_family_operation():
    instance = read("eigrpd/eigrp_instance.c")
    southbound = read("frr/eigrp_southbound.c")

    runtime_create = function_body(
        instance, "eigrp_instance_address_family_runtime_create"
    )
    start = function_body(instance, "eigrp_instance_address_family_start")
    stop = function_body(instance, "eigrp_instance_address_family_stop")
    vrf_resolve = function_body(southbound, "eigrp_southbound_vrf_resolve")

    assert "eigrp_southbound_vrf_resolve" in runtime_create
    assert "eigrp_lookup_by_af_as_vrf" in runtime_create
    assert "eigrp_get_by_af" in runtime_create
    assert "eigrp_name_set" in runtime_create
    assert "eigrp_intf_up(runtime, ei)" in start
    assert "eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL)" in stop
    assert "eigrp_intf_down(ei)" in stop

    assert "vrf_lookup_by_name" in vrf_resolve
    assert "eigrp_lookup" not in vrf_resolve
    assert "eigrp_get" not in vrf_resolve


def test_frr_management_and_zebra_callbacks_delegate_instead_of_mutating_runtime():
    northbound = read("frr/eigrp_northbound.c")
    zebra = read("frr/eigrp_zebra.c")

    assert "eigrp_interface_delay_set(" in northbound
    assert "eigrp_interface_bandwidth_set(" in northbound
    assert "eigrp_auth_mode_update(" in northbound
    assert "eigrp_auth_keychain_update(" in northbound
    assert "eigrp_instance_classic_validate(" in northbound
    assert "eigrp_instance_classic_create(" in northbound

    for token in (
        "intf->params.auth_type =",
        "intf->params.auth_keychain =",
        "ei->params.delay =",
        "ei->params.bandwidth =",
        "eigrp_finish_final(",
        "eigrp_get(",
    ):
        assert token not in northbound

    router_id = function_body(zebra, "eigrp_zebra_router_id_update")
    address_add = function_body(zebra, "eigrp_zebra_interface_address_add")
    address_delete = function_body(zebra, "eigrp_zebra_interface_address_delete")

    assert "eigrp_instance_router_id_refresh_vrf" in router_id
    assert "ALL_LIST_ELEMENTS" not in router_id
    assert "eigrp_network_interface_refresh" in address_add
    assert "ALL_LIST_ELEMENTS" not in address_add
    assert "eigrp_interface_runtime_address_remove" in address_delete
    assert "ALL_LIST_ELEMENTS" not in address_delete


def test_common_tree_has_no_frr_headers_or_host_library_types():
    forbidden_includes = re.compile(
        r'^\s*#\s*include\s*[<"](?:zebra\.h|lib/|linklist\.h|stream\.h|'
        r'table\.h|command\.h|vty\.h|vrf\.h|keychain\.h|sockopt\.h|'
        r'sockunion\.h|memory\.h|log\.h|frrevent\.h|workqueue\.h|zclient\.h)',
        re.MULTILINE,
    )
    forbidden_types = (
        r"\bstruct\s+event\b",
        r"\bstruct\s+interface\b",
        r"\bstruct\s+vty\b",
        r"\bstruct\s+vrf\b",
        r"\bstruct\s+stream\b",
        r"\bstruct\s+list(?:node)?\b",
        r"\bstruct\s+route_(?:table|node)\b",
        r"\bstruct\s+key(?:chain)?\b",
        r"\bstruct\s+zclient\b",
        r"\bstruct\s+zapi_[A-Za-z0-9_]+\b",
    )

    for path in sorted((ROOT / "eigrpd").glob("*.[ch]")):
        text = path.read_text()
        assert not forbidden_includes.search(text), (
            f"{path.relative_to(ROOT)} includes an FRR/host-library header"
        )
        for pattern in forbidden_types:
            assert not re.search(pattern, text), (
                f"{path.relative_to(ROOT)} exposes host-library type {pattern}"
            )


def test_every_common_source_compiles_with_posix_only_surface(tmp_path):
    import os
    import subprocess

    compiler = os.environ.get("CC", "cc")
    for source in sorted((ROOT / "eigrpd").glob("*.c")):
        result = subprocess.run(
            [
                compiler,
                "-std=c11",
                "-D_POSIX_C_SOURCE=200809L",
                "-fsyntax-only",
                "-Werror=implicit-function-declaration",
                f"-I{ROOT}",
                str(source),
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        assert result.returncode == 0, (
            f"{source.relative_to(ROOT)} requires non-POSIX or host build context:\n"
            f"{result.stderr}"
        )


def test_common_tree_does_not_clone_host_flag_or_string_helpers():
    forbidden = (
        "EIGRP_CHECK_FLAG",
        "EIGRP_SET_FLAG",
        "EIGRP_UNSET_FLAG",
        "strlcpy(",
        "strlcat(",
        "explicit_bzero(",
        "bzero(",
        "bcopy(",
    )

    for path in sorted((ROOT / "eigrpd").glob("*.[ch]")):
        text = path.read_text()
        for token in forbidden:
            assert token not in text, (
                f"{path.relative_to(ROOT)} reintroduces host/non-POSIX helper {token}"
            )


def test_frr_memory_tracking_stays_inside_frr_adapter():
    common = "\n".join(
        path.read_text() for path in sorted((ROOT / "eigrpd").glob("*.[ch]"))
    )
    memory_header = read("frr/eigrp_frr_memory.h")
    memory_source = read("frr/eigrp_frr_memory.c")
    southbound = read("frr/eigrp_southbound.c")
    zebra = read("frr/eigrp_zebra.c")

    assert "DECLARE_MGROUP(EIGRPD)" in memory_header
    assert 'DEFINE_MGROUP(EIGRPD, "eigrpd")' in memory_source
    assert "eigrp_frr_memory.h" in southbound
    assert "eigrp_frr_memory.h" in zebra
    assert "DEFINE_MGROUP(EIGRPD" not in common
    assert "DECLARE_MGROUP(EIGRPD" not in common
    assert "DEFINE_MTYPE" not in common


def test_design_spec_requires_boundary_validation_before_common_code():
    design = read("specs/design-spec.md")

    assert "External data is validated and normalized at the boundary" in design
    assert "Common protocol code must not repeatedly defend" in design
    assert "A missing required callback is a binding/programming error" in design
    assert "Wire packets are themselves untrusted external input" in design


def test_address_family_vectors_are_validated_once_before_common_use():
    types = read("eigrpd/eigrp_types.h")
    runtime = read("eigrpd/eigrpd.c")
    packet = read("eigrpd/eigrp_packet.c")
    tlv1 = read("eigrpd/eigrp_tlv1.c")
    tlv2 = read("eigrpd/eigrp_tlv2.c")

    assert "eigrp_af_vectors_runtime_validate" in runtime
    for field in (
        "packet_source_on_link",
        "packet_address_bytes",
        "packet_address_decode",
        "packet_address_encode",
        "packet_prefix_decode",
        "packet_prefix_encode",
        "classic_internal_tlv_type",
        "classic_external_tlv_type",
        "multiprotocol_afi",
        "addr_snprintf",
        "summary_auto_prefix",
    ):
        assert f"EIGRP_AF_VECTOR_REQUIRE({field})" in runtime

    assert "if (!eigrp->af_vectors.packet_send)" not in packet
    assert "if (!eigrp->af_vectors.packet_receive)" not in packet
    assert "|| !ei->eigrp->af_vectors.packet_source_on_link" not in packet
    assert "if (!ei->eigrp->af_vectors.packet_source_on_link\n" not in packet
    assert "eigrp_tlv1_af_ready" not in tlv1
    assert "eigrp_tlv2_af_ready" not in tlv2
    assert "prefix_snprintf" not in types


def test_frr_debug_logging_uses_eigrp_logging_boundary():
    frr_log = read("frr/eigrp_log.c")
    zebra = read("frr/eigrp_zebra.c")
    common_dump = read("eigrpd/eigrp_dump.c")

    assert "zlog_debug(\"%s\", message)" in frr_log
    assert "eigrp_log_debug(" in zebra
    assert "zlog_debug(" not in zebra
    assert "zlog_debug(" not in common_dump


def test_frr_event_and_rib_ingress_logs_invalid_host_data_at_boundary():
    southbound = read("frr/eigrp_southbound.c")
    zebra = read("frr/eigrp_zebra.c")

    event_prepare = function_body(southbound, "eigrp_southbound_event_prepare")
    event_run = function_body(southbound, "eigrp_southbound_event_run")
    work_new = function_body(southbound, "eigrp_work_queue_new")
    work_run = function_body(southbound, "eigrp_work_queue_host_run")
    address_add = function_body(zebra, "eigrp_zebra_interface_address_add")
    address_delete = function_body(zebra, "eigrp_zebra_interface_address_delete")
    redistribute = function_body(zebra, "eigrp_zebra_redistribute_route")

    assert "eigrp_log_error" in event_prepare
    assert "eigrp_log_error" in event_run
    assert "eigrp_log_error" in work_new
    assert "eigrp_log_error" in work_run
    assert "eigrp_frr_interface_state_import" in address_add
    assert "eigrp_log_error" in address_add
    assert "eigrp_frr_prefix_import" in address_delete
    assert "eigrp_log_error" in address_delete
    assert "zapi_route_decode" in redistribute
    assert "eigrp_log_error" in redistribute
