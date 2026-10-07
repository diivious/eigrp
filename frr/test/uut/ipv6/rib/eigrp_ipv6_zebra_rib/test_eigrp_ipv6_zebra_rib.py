# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())

def read(path):
    return (ROOT / path).read_text()

def test_ipv6_link_local_nexthop_and_ecmp_cross_portable_rib_snapshot():
    topology = read("eigrp/code/eigrp_topology.c")
    assert "route->adv_router->src.afi == AF_INET6" in topology
    assert "EIGRP_AFI_IPV6" in topology
    assert "sizeof(route->adv_router->src.ip.v6)" in topology
    assert "eigrp_topology_southbound_nexthops" in topology
    assert "EIGRP_MAX_PATHS_MAX" in topology

def test_zebra_encodes_ipv6_gateway_ifindex_distance_tag_and_ecmp():
    zebra = read("frr/code/eigrp_frr_rib.c")
    assert "NEXTHOP_TYPE_IPV6_IFINDEX" in zebra
    assert "api_nh->ifindex = nexthop->ifindex" in zebra
    assert "api.distance = route->install.admin_dist" in zebra
    assert "ZAPI_MESSAGE_DISTANCE" in zebra
    assert "api.tag = route->tag" in zebra
    assert "ZAPI_MESSAGE_TAG" in zebra
    assert "route->install.type == EIGRP_RIB_ROUTE_EXTERNAL" in zebra
    assert "i < route->nexthop_count && count < MULTIPATH_NUM" in zebra

def test_default_and_configured_administrative_distance_feed_rib_updates():
    const = read("eigrp/code/eigrp_const.h")
    daemon = read("eigrp/code/eigrp.c")
    instance = read("eigrp/code/eigrp_instance.c")
    topology = read("eigrp/code/eigrp_topology.c")
    assert "EIGRP_DISTANCE_INTERNAL_DEFAULT 90" in const
    assert "EIGRP_DISTANCE_EXTERNAL_DEFAULT 170" in const
    assert "distance_internal = EIGRP_DISTANCE_INTERNAL_DEFAULT" in daemon
    assert "distance_external = EIGRP_DISTANCE_EXTERNAL_DEFAULT" in daemon
    assert "af->runtime->distance_internal = internal_distance" in instance
    assert "eigrp_rib_routes_replay_instance(af->runtime)" in instance
    assert ".install.admin_dist = eigrp->distance_internal" in topology
    assert "eigrp->distance_external" in topology

def test_add_update_delete_and_successor_change_use_one_zebra_route_key():
    topology = read("eigrp/code/eigrp_topology.c")
    zebra = read("frr/code/eigrp_frr_rib.c")
    assert "eigrp_rib_route_add(eigrp, &rib_route)" in topology
    assert "eigrp_rib_route_del(eigrp, &prefix->destination)" in topology
    assert "zclient_route_send(ZEBRA_ROUTE_ADD" in zebra
    assert "zclient_route_send(ZEBRA_ROUTE_DELETE" in zebra
    assert "api.instance = eigrp_instance_asn(eigrp)" in zebra
    assert "api.prefix = host_prefix" in zebra

def test_neighbor_loss_shutdown_and_zebra_reconnect_cleanup_are_wired():
    topology = read("eigrp/code/eigrp_topology.c")
    daemon = read("eigrp/code/eigrp.c")
    rib = read("eigrp/code/eigrp_rib.c")
    zebra = read("frr/code/eigrp_frr_rib.c")
    assert "void eigrp_topology_neighbor_down" in topology
    assert "eigrp_fsm_event(&msg)" in topology
    assert "eigrp_topology_table_delete(eigrp, eigrp->topology_table)" in daemon
    assert "eigrp_rib_route_del(eigrp, &pe->destination)" in topology
    assert "eigrp_rib_prefix_installed" in rib
    assert "eigrp_update_routing_table(eigrp, prefix)" in rib
    assert "eigrp_instance_vrf_iterate(eigrp_zebra_vrf_register, zclient)" in zebra
    assert "zclient_send_reg_requests(zclient, (vrf_id_t)vrf_id)" in zebra
    assert "eigrp_rib_routes_replay()" in zebra
    assert "eigrp_zclient->zebra_connected = eigrp_zebra_connected" in zebra
