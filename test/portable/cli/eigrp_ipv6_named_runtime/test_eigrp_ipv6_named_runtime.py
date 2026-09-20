# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level closure guards for Post Command Audit item 7.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
EIGRPD = ROOT / "eigrpd" / "eigrpd.c"
STRUCTS = ROOT / "eigrpd" / "eigrp_structs.h"
INSTANCE = ROOT / "eigrpd" / "eigrp_instance.c"
SOUTHBOUND = ROOT / "frr" / "eigrp_southbound.c"
NORTHBOUND = ROOT / "frr" / "eigrp_northbound.c"
VTY = ROOT / "frr" / "eigrp_cli_named.c"
INTERFACE = ROOT / "eigrpd" / "eigrp_interface.c"
NEIGHBOR = ROOT / "eigrpd" / "eigrp_neighbor.c"
TOPOLOGY = ROOT / "eigrpd" / "eigrp_topology.c"
STATISTICS = ROOT / "eigrpd" / "eigrp_statistics.c"
TIMER = ROOT / "eigrpd" / "eigrp_timer.c"
UUT = ROOT / "tools" / "frr-named-uut.sh"
SERIES = ROOT / "frr" / "patch" / "series"
IPV6_PATCH = ROOT / "frr" / "patch" / "eigrp-named-ipv6.patch"
DESIGN = ROOT / "specs" / "design-spec.md"
PROCESS = ROOT / "specs" / "process-spec.md"


def read(path: Path) -> str:
    return path.read_text()


def test_named_ipv6_creates_a_real_control_runtime():
    instance = read(INSTANCE)
    southbound = read(SOUTHBOUND)

    assert "eigrp_southbound_instance_create" in instance
    assert "afi != EIGRP_ADDRESS_FAMILY_IPV4" in southbound
    assert "&& afi != EIGRP_ADDRESS_FAMILY_IPV6" in southbound
    assert "eigrp_get_by_af(afi, asn, vrf->vrf_id" in southbound
    assert "afi == EIGRP_ADDRESS_FAMILY_IPV4" in southbound


def test_runtime_identity_is_af_vrf_as_and_legacy_lookup_stays_ipv4():
    eigrpd = read(EIGRPD)

    assert "eigrp_lookup_by_af_as_vrf(eigrp_address_family_t afi" in eigrpd
    assert "eigrp->af_vectors.afi == afi && eigrp->AS == as" in eigrpd
    assert "eigrp_lookup_by_af_as_vrf(EIGRP_ADDRESS_FAMILY_IPV4, as, vrf_id)" in eigrpd
    assert "eigrp_get_by_af(EIGRP_ADDRESS_FAMILY_IPV4, as, vrf_id, true)" in eigrpd


def test_ipv6_control_runtime_has_an_explicit_datapath_gate():
    structs = read(STRUCTS)
    eigrpd = read(EIGRPD)

    assert "bool data_path_ready;" in structs
    gate = eigrpd.index("if (!data_path_ready)\n\t\treturn eigrp;")
    for marker in (
        "eigrp_southbound_socket_open(eigrp)",
        "eigrp_southbound_read_add",
        "eigrp_nbr_create(NULL, &src)",
        "eigrp_packetizer_init(eigrp)",
    ):
        assert eigrpd.index(marker) > gate


def test_ipv6_datapath_actions_return_structured_not_implemented():
    southbound = read(SOUTHBOUND)

    assert southbound.count("if (!runtime->data_path_ready)\n\t\treturn EIGRP_RESULT_NOT_IMPLEMENTED;") >= 2
    assert "if (!eigrp || !eigrp->data_path_ready)" in southbound
    assert "return eigrp ? EIGRP_RESULT_NOT_IMPLEMENTED : EIGRP_RESULT_NOT_FOUND;" in southbound


def test_ipv6_operational_walks_are_blocked_at_real_targets():
    for path in (INTERFACE, NEIGHBOR, TOPOLOGY, STATISTICS, TIMER):
        source = read(path)
        assert "data_path_ready" in source
        assert "EIGRP_RESULT_NOT_IMPLEMENTED" in source


def test_ipv6_router_id_is_control_state_without_host_interface_refresh():
    southbound = read(SOUTHBOUND)

    block = southbound[
        southbound.index("void eigrp_southbound_router_id_refresh"):
        southbound.index("eigrp_result_t eigrp_southbound_address_family_stop")
    ]
    assert "if (!runtime->data_path_ready)" in block
    assert "runtime->router_id = runtime->router_id_static;" in block
    assert "eigrp_router_id_update(runtime);" in block


def test_ipv4_and_ipv6_summaries_share_generic_prefix_storage_and_targets():
    vty = read(VTY)
    northbound = read(NORTHBOUND)
    patch = read(IPV6_PATCH)

    assert '"summary-address X:X::X:X/M' in vty
    assert '"summary-metric X:X::X:X/M' in vty
    assert "./summary-address[prefix='%s']" in vty
    assert "./summary-metric[prefix='%s']" in vty
    assert 'yang_dnode_get_string(dnode, "prefix")' in northbound
    assert "prefix.address.afi != afi" in northbound
    assert "eigrp_summary_create(&context, &prefix" in northbound
    assert "eigrp_summary_metric_update(&context, &prefix" in northbound
    assert 'type inet:ip-prefix;' in patch


def test_stage2_precedes_multi_as_and_mirrors_applicable_command_families():
    uut = read(UUT)
    stage2 = uut[uut.index('phase "stage 2: create only IPv6 AS 4453'):uut.index('# Only after both single-AS')]
    stage3_pos = uut.index('phase "stage 3: add second AS 6473 contexts"')

    assert uut.index('phase "stage 2: create only IPv6 AS 4453') < stage3_pos
    for command in (
        "neighbor 2001:db8:4453::2 description STEP2-PEER",
        "neighbor maximum-prefix 500 75 warning-only",
        "authentication mode hmac-sha-256 0 Step2Secret",
        "summary-address 2001:db8:4453::/48",
        "maximum-paths 9",
        "metric maximum-hops 201",
        "eigrp event-log-size 1001",
        "distribute-list STEP2-ACL-IN in",
        "offset-list EIGRP-UUT6 in 101",
        "redistribute maximum-prefix 301",
        "summary-metric 2001:db8:4453::/48",
    ):
        assert command in stage2
    assert '"network ' not in stage2
    assert '"auto-summary"' not in stage2


def test_managed_patch_and_design_spec_record_ipv6_control_runtime_contract():
    series = read(SERIES)
    design = read(DESIGN)
    process = read(PROCESS)
    process_words = " ".join(process.split())

    assert series.rstrip().endswith("eigrp-named-ipv6.patch")
    assert "When `data_path_ready` is false" in process
    assert "EIGRP Stub routing is explicitly outside project scope" in design
    assert "IPv6 named configuration uses the same ownership model as IPv4" in process_words
