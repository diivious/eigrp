from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
STRUCTS = (ROOT / "eigrpd" / "eigrp_structs.h").read_text()
TOPOLOGY = (ROOT / "eigrpd" / "eigrp_topology.c").read_text()


def test_prefix_has_separate_internal_external_queues():
    assert "eigrp_list_t *internal_routes, *external_routes, *rij;" in STRUCTS
    assert "eigrp_list_t *entries, *rij;" not in STRUCTS


def test_both_route_queues_use_cd_comparator():
    assert "new->internal_routes->cmp" in TOPOLOGY
    assert "new->external_routes->cmp" in TOPOLOGY
    assert "if (route1->distance < route2->distance)" in TOPOLOGY


def test_selector_searches_internal_then_external_and_fc_before_accept():
    start = TOPOLOGY.index("eigrp_topology_route_select(")
    end = TOPOLOGY.index("/*\n * Returns new topology route", start)
    selector = TOPOLOGY[start:end]
    assert "for (q = 0; q < 2; q++)" in selector
    assert "route->reported_distance >= prefix->fdistance" in selector
    assert selector.index("route->reported_distance >= prefix->fdistance") < selector.index("return route;")


def test_rib_type_is_selected_path_type_not_prefix_nt():
    start = TOPOLOGY.index("void eigrp_update_routing_table(")
    end = TOPOLOGY.index("void eigrp_topology_neighbor_down", start)
    rib = TOPOLOGY[start:end]
    assert "eigrp_topology_route_external(route)" in rib
    assert "prefix->nt == EIGRP_TOPOLOGY_TYPE_REMOTE_EXTERNAL" not in rib
