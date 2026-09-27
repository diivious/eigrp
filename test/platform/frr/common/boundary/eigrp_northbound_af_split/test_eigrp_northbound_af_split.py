from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd").is_dir())


def read(path: str) -> str:
    return (ROOT / path).read_text()


def test_frr_northbound_is_split_without_duplicate_classic_callbacks():
    common = read("frr/eigrp_northbound.c")
    ipv4 = read("frr/eigrp_northbound_ipv4.c")
    ipv6 = read("frr/eigrp_northbound_ipv6.c")
    build = read("frr/subdir.am")

    assert "const struct frr_yang_module_info frr_eigrpd_info" in common
    assert "eigrpd_instance_create(" not in common.split("/* clang-format off */", 1)[0]
    assert "eigrpd_instance_create(" in ipv4
    assert "EIGRP_AFI_IPV4" in ipv4
    assert "eigrp_northbound_ipv6_neighbor_address_copy(" in ipv6
    assert "EIGRP_AFI_IPV6" in ipv6
    assert "eigrpd/eigrp_northbound_ipv4.c" in build
    assert "eigrpd/eigrp_northbound_ipv6.c" in build


def test_ipv4_only_helpers_are_private_to_ipv4_northbound():
    common = read("frr/eigrp_northbound.c")
    ipv4 = read("frr/eigrp_northbound_ipv4.c")
    internal = read("frr/eigrp_northbound_internal.h")

    assert "void redistribute_get_metrics(" not in common
    assert "eigrp_interface_lookup_host(" not in common
    assert "static void eigrp_northbound_ipv4_redistribute_metrics_get(" in ipv4
    assert "static eigrp_intf_t *\neigrp_northbound_ipv4_interface_lookup_host(" in ipv4
    assert "redistribute_get_metrics" not in internal
    assert "eigrp_interface_lookup_host" not in internal


def test_ipv6_northbound_does_not_pull_unrelated_protocol_modules():
    ipv6 = read("frr/eigrp_northbound_ipv6.c")

    assert '#include "eigrpd/eigrp.h"' in ipv6
    assert '#include "eigrpd/eigrp_topology.h"' not in ipv6
    assert '#include "eigrp_zebra.h"' not in ipv6
    assert '#include "eigrp_policy.h"' not in ipv6
