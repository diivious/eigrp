# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())

def read(path):
    return (ROOT / path).read_text()

def test_ipv6_update_receive_decodes_and_uses_af_external_type():
    update = read("eigrpd/code/eigrp_update.c")
    assert "route = (nbr->decoder)(eigrp, nbr, pkt, length);" in update
    assert "received_route->type == eigrp->af_vectors.classic_external_tlv_type" in update
    assert "prefix->destination = route->dest;" in update
    assert "eigrp_prefix_normalize(&prefix->destination);" in update
    assert "eigrp_route_descriptor_add(eigrp, prefix, route);" in update

def test_ipv6_update_processing_is_bounded_to_passive_state():
    update = read("eigrpd/code/eigrp_update.c")
    # ACTIVE destinations still accept per-neighbor observations; destination-level
    # state remains frozen by the DUAL FSM invariant.
    fsm = read("eigrpd/code/eigrp_fsm.c")
    assert "prefix->state != EIGRP_FSM_STATE_PASSIVE" not in update
    assert "eigrp_fsm_event(&msg);" in update
    assert "EIGRP_FSM_STATE_PASSIVE" in fsm

def test_ipv6_withdrawal_poison_uses_destination_family():
    packetizer = read("eigrpd/code/eigrp_packetizer.c")
    assert "prefix->destination.address.afi == EIGRP_AFI_IPV6" in packetizer
    assert "EIGRP_TLV_IPv6_EXT" in packetizer
    assert "EIGRP_TLV_IPv6_INT" in packetizer

def test_ipv6_update_reaches_common_host_rib_path():
    topology = read("eigrpd/code/eigrp_topology.c")
    rib = topology[topology.index("void eigrp_update_routing_table"):
                   topology.index("void eigrp_topology_neighbor_down")]
    assert "Task 11 stops at IPv6 DUAL/topology selection" not in rib
    assert "eigrp_rib_route_install(eigrp, &rib_route)" in rib
    assert "eigrp_rib_route_remove(eigrp, &prefix->destination)" in rib

def test_named_ipv6_topology_show_uses_common_state_walk():
    cli = read("frr/code/eigrp_cli_named.c")
    assert '"show eigrp address-family <ipv4|ipv6>$afi' in cli
    assert "eigrp_topology_state_iterate(" in cli
    assert "INET6_ADDRSTRLEN" in cli
