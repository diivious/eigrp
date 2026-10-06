# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
PACKET = ROOT / "eigrp" / "code" / "eigrp_packet.c"
INSTANCE = ROOT / "eigrp" / "code" / "eigrp_instance.c"
STRUCTS = ROOT / "eigrp" / "code" / "eigrp_structs.h"
ZEBRA = ROOT / "frr" / "code" / "eigrp_zebra.c"
FRR_INTEGRATION = ROOT / "frr" / "specs" / "integration.md"


def test_af_has_distinct_event_packet_rib_and_message_queues():
    structs = STRUCTS.read_text()
    assert "eigrp_af_event_t *event_head;" in structs
    assert "eigrp_packet_input_t *packet_head;" in structs
    assert "eigrp_rib_event_t *rib_head;" in structs
    assert "eigrp_message_request_t *message_head;" in structs


def test_af_worker_services_system_protocol_rib_then_user():
    source = PACKET.read_text()
    start = source.index("static void *eigrp_instance_thread")
    end = source.index("bool eigrp_instance_thread_start", start)
    worker = source[start:end]

    system = worker.index("eigrp_instance_system_service(eigrp)")
    protocol = worker.index("eigrp_instance_protocol_service(eigrp)")
    rib = worker.index("eigrp_instance_rib_service(eigrp)")
    user = worker.index("eigrp_instance_user_service(eigrp)")
    assert system < protocol < rib < user


def test_protocol_service_advances_pending_output_before_next_rx_and_rechecks_timers():
    source = PACKET.read_text()
    start = source.index("static void eigrp_instance_protocol_service")
    end = source.index("static void eigrp_instance_user_service", start)
    service = source[start:end]

    timer_first = service.index("eigrp_instance_timer_service(eigrp)")
    packet = service.index("eigrp_packet_input_process(eigrp, input)")
    packetizer = service.index("eigrp_packetizer_work_process(eigrp, packetizer)")
    write = service.index("eigrp_packet_write(eigrp)")
    timer_last = service.rindex("eigrp_instance_timer_service(eigrp)")
    assert timer_first < packetizer < write < packet < timer_last


def test_routerid_host_notification_fans_out_as_af_event():
    zebra = ZEBRA.read_text()
    instance = INSTANCE.read_text()
    integration = FRR_INTEGRATION.read_text()

    assert "eigrp_process_routerid_cb((eigrp_vrf_id_t)vrf_id);" in zebra
    assert "EIGRP_AF_EVENT_ROUTERID_UPDATE" in instance
    assert "eigrp_instance_event_enqueue(" in instance
    assert "eigrp_router_id_update(runtime);" in instance
    assert "## 12. Router-ID notification flow" in integration
    section = integration[integration.index("## 12. Router-ID notification flow"):integration.index("## 13. EIGRP route installation into Zebra")]
    assert "eigrp_process_routerid_cb(vrf_id)" in section
    assert "eigrp_instance_event_enqueue(" in section
    assert "EIGRP_AF_EVENT_ROUTERID_UPDATE" in section


def test_interface_down_is_fanned_out_to_af_eventq():
    interface = (ROOT / "eigrp" / "code" / "eigrp_interface.c").read_text()
    unix_interface = (ROOT / "unix" / "code" / "eigrp_unix_interface.c").read_text()
    instance = INSTANCE.read_text()

    assert "eigrp_sys_intf_down(" in unix_interface
    start = interface.index("void eigrp_sys_intf_down(")
    end = interface.index("void eigrp_sys_intf_remove(", start)
    fanout = interface[start:end]
    assert "eigrp_instance_intf_down_event_enqueue(" in fanout
    assert "eigrp_intf_down(" not in fanout
    assert "EIGRP_AF_EVENT_INTF_DOWN" in instance
    assert "eigrp_intf_down(ei);" in instance


def test_remaining_interface_runtime_notifications_use_af_eventq():
    network = (ROOT / "eigrp" / "code" / "eigrp_network.c").read_text()
    interface = (ROOT / "eigrp" / "code" / "eigrp_interface.c").read_text()
    packet = PACKET.read_text()
    instance = INSTANCE.read_text()

    update_start = network.index("void eigrp_sys_intf_update(")
    update = network[update_start:network.index("static eigrp_result_t eigrp_network_process", update_start)]
    assert "eigrp_instance_intf_update_event_enqueue(" in update
    assert "eigrp_intf_runtime_update(" not in update

    remove_start = interface.index("void eigrp_sys_intf_remove(")
    addr_start = interface.index("void eigrp_sys_intf_addr_update(", remove_start)
    remove = interface[remove_start:addr_start]
    addr = interface[addr_start:interface.index("void eigrp_del_intf_params", addr_start)]
    assert "eigrp_instance_intf_remove_event_enqueue(" in remove
    assert "eigrp_intf_runtime_delete(" not in remove
    assert "eigrp_instance_intf_addr_update_event_enqueue(" in addr
    assert "eigrp_intf_runtime_delete(" not in addr

    assert "EIGRP_AF_EVENT_INTF_UPDATE" in instance
    assert "EIGRP_AF_EVENT_INTF_REMOVE" in instance
    assert "EIGRP_AF_EVENT_INTF_ADDR_UPDATE" in instance
    assert "eigrp_network_intf_update(runtime, &event->data.intf_update)" in instance
    assert "event->interface_name = strdup(state->interface_name)" in packet


def test_old_interface_and_rib_target_names_are_gone():
    old_names = (
        "eigrp_sys_interface_state_update",
        "eigrp_sys_interface_link_remove",
        "eigrp_sys_interface_address_remove",
        "eigrp_rib_source_route_add",
        "eigrp_rib_source_route_remove",
    )
    roots = (ROOT / "eigrp" / "code", ROOT / "frr" / "code", ROOT / "unix" / "code")
    source = "\n".join(
        path.read_text()
        for root in roots
        for path in root.glob("*.c")
    ) + "\n" + "\n".join(
        path.read_text()
        for root in roots
        for path in root.glob("*.h")
    )
    for name in old_names:
        assert name not in source


def test_rib_callbacks_enqueue_portable_snapshot_for_af_consumption():
    redistribute = (ROOT / "eigrp" / "code" / "eigrp_redistribute.c").read_text()
    rib_h = (ROOT / "eigrp" / "code" / "eigrp_rib.h").read_text()
    packet = PACKET.read_text()

    assert "eigrp_rib_route_t route;" in rib_h
    assert "EIGRP_RIB_EVENT_UPDATE" in rib_h
    assert "EIGRP_RIB_EVENT_DELETE" in rib_h
    assert "eigrp_instance_rib_event_enqueue(runtime, EIGRP_RIB_EVENT_UPDATE, route)" in redistribute
    assert "eigrp_instance_rib_event_enqueue(runtime, EIGRP_RIB_EVENT_DELETE, route)" in redistribute
    assert "event->route = *route;" in packet
    assert "eigrp_rib_event_process(eigrp, event)" in packet
    assert "EIGRP_AF_RIB_BATCH 1U" in packet


def test_rib_and_redistribution_lifecycle_are_event_logged():
    eventlog_h = (ROOT / "eigrp" / "code" / "eigrp_eventlog.h").read_text()
    redistribute = (ROOT / "eigrp" / "code" / "eigrp_redistribute.c").read_text()

    for opcode in (
        "EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_UPDATE",
        "EIGRP_EVENTLOG_OPCODE_RIB_ROUTE_DELETE",
        "EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_UPDATE",
        "EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_WITHDRAW",
        "EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_FILTERED",
        "EIGRP_EVENTLOG_OPCODE_REDIST_ROUTE_REJECT",
    ):
        assert opcode in eventlog_h
        assert opcode in redistribute
