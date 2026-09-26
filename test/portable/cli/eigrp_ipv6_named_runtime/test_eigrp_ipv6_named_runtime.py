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
YANG_PATCH = ROOT / "frr" / "patch" / "frr-eigrp-yang.patch"
DESIGN = ROOT / "specs" / "design-spec.md"
PROCESS = ROOT / "specs" / "design-spec.md"


def read(path: Path) -> str:
    return path.read_text()


def test_named_ipv6_creates_a_real_control_runtime():
    instance = read(INSTANCE)
    runtime_create = instance[
        instance.index("static eigrp_result_t eigrp_instance_address_family_runtime_create"):
        instance.index("static eigrp_result_t eigrp_instance_address_family_runtime_delete")
    ]

    assert "eigrp_sys_vrf_resolve" in runtime_create
    assert "eigrp_lookup_by_af_as_vrf(af->afi, af->asn, vrf_id)" in runtime_create
    assert "eigrp_get_by_af(" in runtime_create
    assert "eigrp_get_by_af(af->afi, af->asn, vrf_id, true)" in runtime_create
    assert "af->runtime = runtime;" in runtime_create


def test_runtime_identity_is_af_vrf_as_and_legacy_lookup_stays_ipv4():
    eigrpd = read(EIGRPD)

    assert "eigrp_lookup_by_af_as_vrf(eigrp_address_family_t afi" in eigrpd
    assert "eigrp->af_vectors.afi == afi && eigrp->AS == as" in eigrpd
    assert "eigrp_lookup_by_af_as_vrf(EIGRP_ADDRESS_FAMILY_IPV4, as, vrf_id)" in eigrpd
    assert "eigrp_get_by_af(EIGRP_ADDRESS_FAMILY_IPV4, as, vrf_id, true)" in eigrpd


def test_ipv6_runtime_uses_the_normal_live_datapath_lifecycle():
    eigrpd = read(EIGRPD)
    ipv6 = read(ROOT / "eigrpd" / "eigrp_ipv6.c")
    sys_header = read(ROOT / "eigrpd" / "eigrp_sys.h")

    assert "eigrp_get_by_af(EIGRP_ADDRESS_FAMILY_IPV4, as, vrf_id, true)" in eigrpd
    assert "vectors->packet_send = eigrp_ipv6_packet_send;" in ipv6
    assert "vectors->packet_receive = eigrp_ipv6_packet_receive;" in ipv6
    assert "eigrp_sys_ipv6_packet_send" in sys_header
    assert "eigrp_sys_ipv6_packet_receive" in sys_header


def test_capability_gate_remains_generic_not_ipv6_specific():
    instance = read(INSTANCE)
    eigrpd = read(EIGRPD)

    assert "bool data_path_ready;" in read(STRUCTS)
    assert "if (!runtime->data_path_ready)" in instance
    assert "if (!data_path_ready)\n\t\treturn eigrp;" in eigrpd
    runtime_create = instance[
        instance.index("static eigrp_result_t eigrp_instance_address_family_runtime_create"):
        instance.index("static eigrp_result_t eigrp_instance_address_family_runtime_delete")
    ]
    assert "EIGRP_ADDRESS_FAMILY_IPV4" not in runtime_create


def test_router_id_refresh_uses_live_runtime_path_for_both_families():
    instance = read(INSTANCE)
    start = instance.index("static void eigrp_instance_router_id_refresh")
    end = instance.index("bool eigrp_instance_data_path_ready", start)
    block = instance[start:end]
    assert "eigrp_router_id_update(runtime);" in block


def test_ipv4_and_ipv6_summaries_share_generic_prefix_storage_and_targets():
    vty = read(VTY)
    northbound = read(NORTHBOUND)
    patch = read(YANG_PATCH)

    assert '"summary-address X:X::X:X/M' in vty
    assert '"summary-metric X:X::X:X/M' in vty
    assert "./summary-address[prefix='%s']" in vty
    assert "./summary-metric[prefix='%s']" in vty
    assert 'yang_dnode_get_string(dnode, "prefix")' in northbound
    assert "prefix.address.afi != afi" in northbound
    assert "eigrp_summary_create(&context, &prefix" in northbound
    assert "eigrp_summary_metric_set(&context, &prefix" in northbound
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

    patches = [
        line.strip()
        for line in series.splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    assert patches == ["vtysh-named-eigrp.patch", "frr-eigrp-yang.patch"]
    yang_patch = read(YANG_PATCH)
    assert 'description "IPv4 or IPv6 summary prefix";' in yang_patch
    assert 'description "Fixed metric for an IPv4 or IPv6 summary aggregate";' in yang_patch
    assert 'key "protocol route-instance";' in yang_patch
    assert 'type eigrp-redistribution-protocol;' in yang_patch
    assert 'type eigrp-redistribution-route-instance;' in yang_patch
    assert "A BGP ASN or IS-IS area tag is a protocol" in yang_patch
    assert "When `data_path_ready` is false" in process
    assert "EIGRP Stub routing is explicitly outside project scope" in design
    assert "IPv4 and IPv6 named address families both create live runtimes" in process_words
