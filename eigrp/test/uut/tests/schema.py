from pathlib import Path
import importlib.util
P=Path(__file__).resolve().parents[1]/"run.py"
spec=importlib.util.spec_from_file_location("uut_run",P); m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m)

def test_example_schemas():
    root=Path(__file__).resolve().parents[1]/"examples"
    m.validate_topology(m.load_yaml(root/"two-router.yaml"))
    m.validate_scenario(m.load_yaml(root/"two-router-neighbor-route.yaml"))
    m.validate_topology(m.load_yaml(root/"ipv4-basic-core.yaml"))
    m.validate_scenario(m.load_yaml(root/"ipv4-basic-core-gate.yaml"))
    m.validate_topology(m.load_yaml(root/"ipv6-basic-core.yaml"))
    m.validate_scenario(m.load_yaml(root/"ipv6-basic-core-gate.yaml"))
    diamond=root.parent/"dual-diamond"
    m.validate_topology(m.load_yaml(diamond/"ipv4-diamond-fs.yaml"))
    m.validate_scenario(m.load_yaml(diamond/"scenario-a-feasible-successor.yaml"))

def test_arbitrary_shared_segment_width():
    t={"schema":m.TOPOLOGY_SCHEMA,"segments":{"lan":{}},"routers":{}}
    for x in range(1,33):
        t["routers"][f"r{x}"]={"asn":4453,"router_id":f"10.255.0.{x}","interfaces":{"eth0":{"segment":"lan","ipv4":f"10.0.0.{x}/24"}}}
    m.validate_topology(t)

def test_source_network_is_a_topology_primitive():
    t={"schema":m.TOPOLOGY_SCHEMA,"segments":{"lan":{}},"routers":{"r1":{"asn":4453,"router_id":"10.255.0.1","interfaces":{"eth0":{"segment":"lan","ipv4":"10.0.0.1/24"}},"source_networks":[{"prefix":"192.0.2.0/24","protocol":"static","metric":10}]}}}
    m.validate_topology(t)


def test_single_family_topologies_support_ipv4_and_ipv6():
    root=Path(__file__).resolve().parents[1]/"examples"
    assert m.topology_afi(m.load_yaml(root/"ipv4-basic-core.yaml")) == 4
    assert m.topology_afi(m.load_yaml(root/"ipv6-basic-core.yaml")) == 6


def test_batched_link_operations_are_valid_scenario_steps():
    scenario={
        "schema":m.TEST_SCHEMA,
        "name":"batched-links",
        "topology":"dummy.yaml",
        "steps":[
            {"links-down":[{"uut":"r1","interface":"eth0"},{"uut":"r2","interface":"eth0"}]},
            {"links-up":[{"uut":"r1","interface":"eth0"},{"uut":"r2","interface":"eth0"}]},
        ],
    }
    m.validate_scenario(scenario)


def test_route_absence_filters_the_specific_path_not_the_whole_prefix():
    class Node:
        def state(self):
            return {
                "rib":[], "source":[], "topology":[],
                "paths":[
                    {"prefix":"10.0.0.0/24","next_hop":"10.0.1.2","interface":"eth0","connected":False,"successor":True,"feasible_successor":False,"distance":10,"rd":5},
                    {"prefix":"10.0.0.0/24","next_hop":"10.0.2.2","interface":"eth1","connected":False,"successor":False,"feasible_successor":True,"distance":20,"rd":4},
                ],
            }
    runner=m.Runner.__new__(m.Runner); runner.nodes={"r1":Node()}
    assert runner.route_ok({"uut":"r1","prefix":"10.0.0.0/24","table":"path","next_hop":"10.0.3.2","present":False})
    assert not runner.route_ok({"uut":"r1","prefix":"10.0.0.0/24","table":"path","next_hop":"10.0.1.2","present":False})
    assert runner.route_ok({"uut":"r1","prefix":"10.0.0.0/24","table":"path","count":2})


def test_packet_group_upper_bound_detects_duplicate_query_flow():
    class Broker:
        journal=[
            {"event":"delivered","opcode":"query","source_uut":"r1","destination_uut":"r2","prefixes":["10.0.0.0/24"]},
            {"event":"delivered","opcode":"query","source_uut":"r1","destination_uut":"r2","prefixes":["10.0.0.0/24"]},
        ]
    runner=m.Runner.__new__(m.Runner); runner.broker=Broker(); runner.packet_marks={"x":0}
    spec={"event":"delivered","opcode":"query","prefix":"10.0.0.0/24","since":"x","min":1,"max":4,"group_by":["source_uut","destination_uut"],"max_per_group":1}
    assert not runner.packet_ok(spec)

def test_fault_prefix_matches_only_route_bearing_packet():
    b=m.Broker("/tmp/eigrp-wire-test.sock", {})
    b.add_fault({"name":"pfx","action":"drop","opcode":"reply","prefix":"10.4.0.0/24"})
    assert b._actions({"opcode":"reply","prefixes":["10.4.0.0/24"]})
    assert not b._actions({"opcode":"reply","prefixes":["10.5.0.0/24"]})


def test_topology_accepts_configured_active_time():
    topology={
        "schema":m.TOPOLOGY_SCHEMA,
        "segments":{"s":{}},
        "routers":{"r1":{"asn":4453,"router_id":"10.255.0.1","active_time":4,
                          "interfaces":{"eth0":{"segment":"s","ipv4":"10.0.0.1/24"}}}},
    }
    assert m.validate_topology(topology)["routers"]["r1"]["active_time"] == 4


def test_redistribution_scenarios_use_three_real_routers():
    root = Path(__file__).resolve().parents[2] / "redistribution"
    topology = m.validate_topology(m.load_yaml(root / "ipv4-static-three-router.yaml"))
    assert set(topology["routers"]) == {"r1", "r2", "r3"}
    assert topology["routers"]["r1"]["redistribute"] == ["static"]
    for name in ("scenario-static-source-withdrawal.yaml",
                 "scenario-static-disable-redistribution.yaml"):
        scenario = m.validate_scenario(m.load_yaml(root / name))
        operations = [next(iter(s)) for s in scenario["steps"]]
        assert "validate-convergence" in operations
        assert all(target in str(scenario["steps"]) for target in ("r2", "r3"))


def test_neighbor_assertion_rejects_silent_filters():
    """Unknown fields cannot turn an absence check into an Up check."""
    import pytest
    base = {"schema": m.TEST_SCHEMA, "topology": "dummy.yaml",
            "steps": [{"validate-neighbor": {"uut": "r3", "neighbor": "10.23.0.2",
                                             "interface": "eth32", "state": "down"}}]}
    assert m.validate_scenario(base) == base
    for invalid in ({"present": False}, {"stat": "down"}, {"state": "gone"},
                    {"state": "down", "rto": 200}):
        step = {**base["steps"][0]["validate-neighbor"], **invalid}
        scenario = {**base, "steps": [{"validate-neighbor": step}]}
        with pytest.raises(m.SpecError):
            m.validate_scenario(scenario)


def test_neighbor_assertion_is_checked_in_convergence_and_runtime():
    import pytest
    wrong = {"uut": "r3", "neighbor": "10.23.0.2", "present": False}
    scenario = {"schema": m.TEST_SCHEMA, "topology": "dummy.yaml",
                "steps": [{"validate-convergence": {"expect": [{"neighbor": wrong}]}}]}
    with pytest.raises(m.SpecError, match="unsupported keys: present"):
        m.validate_scenario(scenario)
    runner = m.Runner.__new__(m.Runner)
    with pytest.raises(m.SpecError, match="unsupported keys: present"):
        runner.neighbor_ok(wrong)


def test_assertion_schema_rejects_false_positive_filters_and_empty_scenarios():
    import pytest
    base={"schema":m.TEST_SCHEMA,"topology":"dummy.yaml"}
    wrong=[
        [],
        [{"validate-convergence":{"expect":[]}}],
        [{"validate-convergence":{"expect":[{"bogus":{"uut":"r1"}}]}}],
        [{"validate-route":{"uut":"r1","prefix":"10.1.0.0/24","next_hops":"INVALID"}}],
        [{"validate-route":{"uut":"r1","prefix":"10.1.0.0/24","table":"wrong"}}],
        [{"validate-route":{"uut":"r1","prefix":"10.1.0.0/24","table":"rib","rd":1}}],
        [{"validate-convergence":{"expect":[{"route":{"uut":"r1","prefix":"10.1.0.0/24","next_hops":"WRONG"}}]}}],
        [{"validate-packet":{"opcode":"reply","min":1}}],
        [{"validate-packet":{"opcode":"reply","event":"any","min":1}}],
        [{"validate-packet":{"opcode":"reply","event":"delivered","since":"typo-mark"}}],
        [{"validate-packet":{"opcode":"reply","event":"delivered","min":0,"max":0,"group_by":"not-real"}}],
        [{"validate-packet":{"opcode":"reply","event":"delivered","typod":"any"}}],
        [{"packet-mark":"start"},{"packet-mark":"start"}],
        [{"link-down":{"uut":"r1","interfac":"eth0"}}],
        [{"link-up":{"uut":"r1","interface":"eth0","unexpected":True}}],
    ]
    for steps in wrong:
        with pytest.raises(m.SpecError): m.validate_scenario({**base,"steps":steps})


def test_packet_phase_mark_must_precede_assertion():
    base={"schema":m.TEST_SCHEMA,"topology":"dummy.yaml"}
    e={"opcode":"reply","event":"delivered","since":"my-phase","min":1}
    with __import__("pytest").raises(m.SpecError,match="unknown packet mark"):
        m.validate_scenario({**base,"steps":[{"validate-packet":e},{"packet-mark":"my-phase"}]})
    assert m.validate_scenario({**base,"steps":[{"packet-mark":"my-phase"},{"validate-packet":e}]})


def test_all_yaml_scenarios_require_valid_assertion_fields():
    import yaml
    root=Path(__file__).resolve().parents[3]
    count=0
    for path in root.rglob("*.yaml"):
        data=yaml.safe_load(path.read_text())
        if isinstance(data,dict) and data.get("schema")==m.TEST_SCHEMA:
            m.validate_scenario(data)
            count+=1
    assert count>=37


def test_medium_checkpoint_and_stability_schema_rejects_typo_and_bad_window():
    import pytest
    base={"schema":m.TEST_SCHEMA,"topology":"dummy.yaml"}
    for steps in ([{"checkpoint":""}], [{"checkpoint":{"name":"bad"}}],
                  [{"validate-convergence":{"expect":[{"neighbor":{"uut":"r1","neighbor":"10.0.0.2"}}], "stable_for":-1}}],
                  [{"validate-convergence":{"expect":[{"neighbor":{"uut":"r1","neighbor":"10.0.0.2"}}], "stablefor":1}}]):
        with pytest.raises(m.SpecError):m.validate_scenario({**base,"steps":steps})
    assert m.validate_scenario({**base,"steps":[{"checkpoint":"start"}]})


def test_ipv6_attached_destination_rejects_nonforwardable_addresses():
    import pytest
    root=Path(__file__).resolve().parents[1]/"examples"
    t=m.load_yaml(root/"ipv6-basic-core.yaml")
    for bad in ("fe80:1::1/128", "ff02::1/128", "::/128", "::1/128"):
        t["routers"]["r1"]["attached_networks"][0]["prefix"]=bad
        with pytest.raises(m.SpecError,match="routable unicast"):
            m.validate_topology(t)
    t["routers"]["r1"]["attached_networks"][0]["prefix"]="2001:db8:1234::/64"
    m.validate_topology(t)


def test_ipv6_core_requires_global_internal_routes_and_link_local_next_hops():
    root=Path(__file__).resolve().parents[1]/"examples"
    scenario=m.load_yaml(root/"ipv6-basic-core-gate.yaml")
    convergence=[step["validate-convergence"] for step in scenario["steps"] if "validate-convergence" in step]
    assert len(convergence)==3
    for stage in (convergence[0],convergence[2]):
        rib=[item["route"] for item in stage["expect"] if "route" in item and item["route"].get("table")=="rib"]
        for name in ("2001:db8:1::/64", "2001:db8:3::/64"):
            matched=[row for row in rib if row["prefix"]==name and "next_hop" in row]
            assert matched and all(row["next_hop"].startswith("fe80:") for row in matched)
    packet=[step["validate-packet"] for step in scenario["steps"] if "validate-packet" in step]
    assert any(row.get("opcode")=="query" and row.get("prefix")=="2001:db8:3::/64" for row in packet)
    assert any(row.get("opcode")=="reply" and row.get("prefix")=="2001:db8:3::/64" for row in packet)


def test_attached_interface_name_collision_fails_at_schema_boundary():
    import copy, pytest
    root=Path(__file__).resolve().parents[1]/"examples"
    t=m.load_yaml(root/"ipv6-basic-core.yaml")
    t["routers"]["r1"]["attached_networks"][0]["interface"]="eth12"
    with pytest.raises(m.SpecError,match="interface names must be unique"):
        m.validate_topology(t)
    t=m.load_yaml(root/"ipv6-basic-core.yaml")
    t["routers"]["r1"]["attached_networks"].append({"prefix":"2001:db8:4::/64","interface":"lan1"})
    with pytest.raises(m.SpecError,match="interface names must be unique"):
        m.validate_topology(t)
