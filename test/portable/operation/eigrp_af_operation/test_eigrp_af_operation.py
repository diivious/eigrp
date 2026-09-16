# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for fundamental named address-family operation.

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


def test_address_family_shutdown_stops_and_restarts_runtime_interfaces():
    instance = read("eigrpd/eigrp_instance.c")
    southbound = read("frr/eigrp_southbound.c")
    update = function_body(instance, "eigrp_instance_address_family_shutdown_update")
    stop = function_body(southbound, "eigrp_southbound_address_family_stop")
    start = function_body(southbound, "eigrp_southbound_address_family_start")

    assert "eigrp_southbound_address_family_stop(af->runtime)" in update
    assert "eigrp_southbound_address_family_start(af->runtime)" in update
    assert "eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL)" in stop
    assert "eigrp_intf_down(ei)" in stop
    assert "eigrp_intf_up(runtime, ei)" in start


def test_router_id_rejects_reserved_values_and_refreshes_runtime():
    instance = read("eigrpd/eigrp_instance.c")
    update = function_body(instance, "eigrp_instance_router_id_update")

    assert "router_id == 0" in update
    assert "router_id == UINT32_MAX" in update
    assert "context->runtime->router_id_static.s_addr = htonl(router_id);" in update
    assert "eigrp_southbound_router_id_refresh(context->runtime);" in update


def test_new_runtime_interface_binds_retained_named_interface_configuration():
    southbound = read("frr/eigrp_southbound.c")
    interface = read("eigrpd/eigrp_interface.c")
    run_interface = function_body(southbound, "eigrp_southbound_network_run_interface")
    bind = function_body(interface, "eigrp_interface_runtime_bind")

    assert "eigrp_instance_runtime_config(eigrp)" in run_interface
    assert "eigrp_interface_config_read(af, ifp->name)" in run_interface
    assert "eigrp_interface_runtime_bind(ei, config)" in run_interface
    assert "config && config->shutdown" in run_interface
    assert "runtime->params.delay = config->delay;" in bind
    assert "runtime->params.v_hello = config->hello_interval;" in bind
    assert "runtime->params.v_wait = config->hold_time;" in bind
    assert "runtime->params.passive_interface" in bind


def test_static_neighbor_uses_configured_interface_for_unicast_hello():
    neighbor = read("eigrpd/eigrp_neighbor.c")
    hello = read("eigrpd/eigrp_hello.c")
    timer = function_body(hello, "eigrp_hello_timer")
    send = function_body(hello, "eigrp_hello_send_unicast")
    static_send = function_body(neighbor, "eigrp_neighbor_static_hello_send")
    source_allowed = function_body(neighbor, "eigrp_neighbor_static_source_allowed")
    receive = function_body(hello, "eigrp_hello_receive")

    assert "eigrp_neighbor_static_hello_send(ei)" in timer
    assert "strcmp(neighbor->interface_name, interface_name)" in static_send
    assert "eigrp_hello_send_unicast(ei, &dst);" in static_send
    assert "eigrp_hello_encode(ei, dst->ip.v4.s_addr" in send
    assert "return !has_static;" in source_allowed
    assert "eigrp_neighbor_static_source_allowed(ei, src)" in receive


def test_eigrp_stub_feature_is_not_implemented_by_audit_item_3():
    changed_modules = "\n".join(
        read(path)
        for path in (
            "eigrpd/eigrp_instance.c",
            "eigrpd/eigrp_interface.c",
            "eigrpd/eigrp_neighbor.c",
            "eigrpd/eigrp_network.c",
            "frr/eigrp_southbound.c",
        )
    )
    assert "eigrp_stub" not in changed_modules
