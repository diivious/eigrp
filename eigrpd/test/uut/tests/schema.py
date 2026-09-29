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
