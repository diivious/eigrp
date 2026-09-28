# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def read(path):
    return (ROOT / path).read_text()


def test_ipv6_named_af_discovers_only_link_local_participating_addresses():
    network = read("eigrpd/code/eigrp_network.c")
    assert "EIGRP_AFI_IPV6" in network
    assert "IN6_IS_ADDR_LINKLOCAL(&address)" in network
    assert "eigrp_sys_interface_walk" in network


def test_ipv6_interface_up_originates_connected_topology_route():
    interface = read("eigrpd/code/eigrp_interface.c")
    assert "route->type = eigrp->af_vectors.classic_internal_tlv_type;" in interface
    assert "eigrp_intf_destination(ei, &destination)" in interface
    assert "prefix->nt = EIGRP_TOPOLOGY_TYPE_CONNECTED;" in interface
    assert "eigrp_update_send_all(eigrp, NULL);" in interface


def test_multicast_membership_tracks_interface_and_passive_lifecycle():
    interface = read("eigrpd/code/eigrp_interface.c")
    southbound = read("frr/code/eigrp_southbound_ipv6.c")
    assert "eigrp_sys_multicast_join(ei->eigrp, ei)" in interface
    assert "eigrp_sys_multicast_leave(ei->eigrp, ei)" in interface
    assert "IPV6_JOIN_GROUP" in southbound
    assert "IPV6_LEAVE_GROUP" in southbound
    assert 'inet_pton(AF_INET6, "ff02::a"' in southbound


def test_named_af_interface_runtime_is_family_neutral_and_inherits_default():
    northbound = (read("frr/code/eigrp_northbound.c") + read("frr/code/eigrp_northbound_ipv4.c") + read("frr/code/eigrp_northbound_ipv6.c"))
    interface = read("eigrpd/code/eigrp_interface.c")
    assert 'strcmp(interface_name, "default") != 0' in northbound
    assert "afi == EIGRP_AFI_IPV4" not in northbound[
        northbound.index("static bool eigrpd_named_interface_context_resolve"):
        northbound.index("static bool eigrpd_named_network_context_resolve")
    ]
    assert 'eigrp_intf_config_read(af, "default")' in interface
    assert "passive_configured" in interface
    assert "shutdown_configured" in interface


def test_ipv6_hello_uses_ff02_a_and_starts_common_adjacency_state_machine():
    hello = read("eigrpd/code/eigrp_hello.c")
    assert 'inet_pton(AF_INET6, "ff02::a", &destination.ip.v6)' in hello
    assert "adjacency_start_allowed = true;" in hello
    assert "eigrp_update_send_init(eigrp, nbr);" in hello
    assert "eigrp_nbr_state_update(EIGRP_SET, nbr, EIGRP_NEIGHBOR_PENDING);" in hello
    assert "if (!adjacency_start_allowed)" in hello


def test_interface_state_show_exposes_runtime_multicast_and_timers():
    interface = read("eigrpd/code/eigrp_interface.c")
    assert "state.multicast_enabled = ei->member_allrouters;" in interface
    assert "state.hello_interval = ei->params.v_hello;" in interface
    assert "state.hold_time = ei->params.v_wait;" in interface
    assert "state.hello_timer_running = ei->t_hello != NULL;" in interface
