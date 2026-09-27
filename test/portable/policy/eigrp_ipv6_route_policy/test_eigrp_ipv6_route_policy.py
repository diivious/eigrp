from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def read(path):
    return (ROOT / path).read_text()


def test_ipv6_filter_policy_reaches_runtime_data_path():
    policy = read("frr/eigrp_policy.c")
    filt = read("eigrpd/eigrp_filter.c")
    update = read("eigrpd/eigrp_update.c")
    packetizer = read("eigrpd/eigrp_packetizer.c")

    assert "AFI_IP6" in policy
    assert "prefix_list_lookup(afi, name)" in policy
    assert "access_list_lookup(afi, name)" in policy
    assert "eigrp_filter_soft_resync" in filt
    assert "EIGRP_FILTER_IN" in update
    assert "eigrp_offset_metric_update" in update
    assert "eigrp_offset_metric_update" in packetizer


def test_route_maps_filter_redistributed_ipv6_candidates():
    system = read("eigrpd/eigrp_sys.h")
    policy = read("frr/eigrp_policy.c")
    redist = read("eigrpd/eigrp_redistribute.c")

    assert "eigrp_sys_redistribute_route_map_evaluate" in system
    assert "route_map_lookup_by_name" in policy
    assert "route_map_apply" in policy
    assert "eigrp_sys_redistribute_route_map_evaluate" in redist
    assert "eigrp_topology_redistributed_route_remove" in redist


def test_policy_edits_force_reevaluation():
    policy = read("frr/eigrp_policy.c")
    redist = read("eigrpd/eigrp_redistribute.c")
    filt = read("eigrpd/eigrp_filter.c")

    assert "route_map_add_hook" in policy
    assert "route_map_delete_hook" in policy
    assert "eigrp_redist_policy_update_all" in policy
    assert "eigrp_rib_redistribute_remove" in redist
    assert "eigrp_rib_redistribute_add" in redist
    assert "request.soft = true" in filt


def test_classic_ipv6_distribute_uses_ipv6_slots():
    policy = read("frr/eigrp_policy.c")

    assert "DISTRIBUTE_V6_IN" in policy
    assert "DISTRIBUTE_V6_OUT" in policy
    assert "EIGRP_AFI_IPV6" in policy


def test_outbound_offset_applies_to_normal_and_resync_updates():
    update = read("eigrpd/eigrp_update.c")
    packetizer = read("eigrpd/eigrp_packetizer.c")

    assert "&wire_route.metric" in packetizer
    assert update.count("eigrp_offset_metric_update") >= 4


def test_frr_route_map_types_do_not_leak_into_portable_code():
    portable_files = [
        "eigrpd/eigrp.h",
        "eigrpd/eigrp_sys.h",
        "eigrpd/eigrp_filter.c",
        "eigrpd/eigrp_update.c",
        "eigrpd/eigrp_packetizer.c",
        "eigrpd/eigrp_redistribute.c",
    ]
    forbidden = (
        "struct route_map",
        "route_map_result_t",
        "RMAP_DENYMATCH",
        "RMAP_PERMITMATCH",
        "route_map_lookup_by_name",
        "route_map_apply(",
        '#include "routemap.h"',
    )
    for path in portable_files:
        content = read(path)
        for token in forbidden:
            assert token not in content, f"{token} leaked into {path}"

    shim = read("frr/eigrp_policy.c")
    assert '#include "routemap.h"' in shim
    assert "struct route_map" in shim
    assert "route_map_result_t" in shim
    assert "RMAP_PERMITMATCH" in shim


def test_policy_shim_header_matches_route_map_boundary():
    header = read("frr/eigrp_policy.h")
    assert "eigrp_policy_redistribute_route_map_evaluate" in header
    assert "eigrp_rib_source_route_t" in header
    for native in ("struct route_map", "route_map_result_t", "RMAP_", "routemap.h"):
        assert native not in header
    assert "route_map_result_t" not in header
    assert "struct route_map" not in header
    assert "routemap.h" not in header
    assert "eigrp_policy_filter_evaluate(" in header


def test_packet_host_contract_keeps_generic_and_af_specific_entry_points():
    sys_h = read("eigrpd/eigrp_sys.h")
    assert "int eigrp_sys_packet_send(" in sys_h
    assert "bool eigrp_sys_packet_receive(" in sys_h
    assert "int eigrp_sys_ipv4_packet_send(" in sys_h
    assert "bool eigrp_sys_ipv4_packet_receive(" in sys_h
    assert "int eigrp_sys_ipv6_packet_send(" in sys_h
    assert "bool eigrp_sys_ipv6_packet_receive(" in sys_h


def test_route_map_contract_is_const_end_to_end():
    system = read("eigrpd/eigrp_sys.h")
    southbound = read("frr/eigrp_southbound.c")
    policy_h = read("frr/eigrp_policy.h")
    policy_c = read("frr/eigrp_policy.c")

    signature = "const eigrp_rib_source_route_t *route"
    assert signature in system
    assert signature in southbound
    assert signature in policy_h
    assert signature in policy_c


def test_split_southbound_packet_io_has_single_symbol_owner():
    generic = read("frr/eigrp_southbound.c")
    ipv4 = read("frr/eigrp_southbound_ipv4.c")
    ipv6 = read("frr/eigrp_southbound_ipv6.c")
    internal = read("frr/eigrp_southbound_internal.h")

    assert "int eigrp_sys_ipv4_packet_send(" not in generic
    assert "bool eigrp_sys_ipv4_packet_receive(" not in generic
    assert "int eigrp_sys_ipv6_packet_send(" not in generic
    assert "bool eigrp_sys_ipv6_packet_receive(" not in generic

    assert "int eigrp_sys_ipv4_packet_send(" in ipv4
    assert "bool eigrp_sys_ipv4_packet_receive(" in ipv4
    assert "int eigrp_sys_ipv6_packet_send(" in ipv6
    assert "bool eigrp_sys_ipv6_packet_receive(" in ipv6

    assert "int eigrp_sys_packet_send(" in generic
    assert "bool eigrp_sys_packet_receive(" in generic
    assert "int eigrp_southbound_socket_fd_get(" in generic
    assert "int eigrp_southbound_socket_fd_get(" in internal
