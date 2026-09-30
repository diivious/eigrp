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
