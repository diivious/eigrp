# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for the portable manual-summary model and the IPv4-only
# classful auto-summary address-family boundary.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]
SUMMARY_C = ROOT / "eigrpd" / "eigrp_summary.c"
SUMMARY_H = ROOT / "eigrpd" / "eigrp_summary.h"
TYPES_H = ROOT / "eigrpd" / "eigrp_types.h"
IPV4_C = ROOT / "eigrpd" / "eigrp_ipv4.c"
IPV6_C = ROOT / "eigrpd" / "eigrp_ipv6.c"
NORTHBOUND = ROOT / "frr" / "eigrp_northbound.c"
CLI_NAMED = ROOT / "frr" / "eigrp_cli_named.c"


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    offset = 0
    needle = f"{name}("
    while True:
        start = source.find(needle, offset)
        if start < 0:
            break
        brace = source.find("{", start)
        semicolon = source.find(";", start)
        if brace < 0:
            break
        if semicolon >= 0 and semicolon < brace:
            offset = semicolon + 1
            continue
        depth = 0
        for index in range(brace, len(source)):
            if source[index] == "{":
                depth += 1
            elif source[index] == "}":
                depth -= 1
                if depth == 0:
                    return source[start : index + 1]
        break
    raise AssertionError(f"missing function definition {name}")


def test_manual_summary_public_target_uses_eigrp_prefix_type():
    header = read(SUMMARY_H)
    summary = read(SUMMARY_C)

    assert "const eigrp_prefix_t *prefix" in header
    assert "const eigrp_address_t *address" not in header
    assert "const eigrp_address_t *mask" not in header
    assert "eigrp_prefix_t prefix;" in summary
    assert "eigrp_address_t mask;" not in summary
    assert "eigrp_summary_ipv4_pair_valid" not in summary


def test_manual_summary_common_path_has_no_frr_or_ipv4_wire_types():
    summary = read(SUMMARY_C)
    create = function_body(summary, "eigrp_summary_create")
    delete = function_body(summary, "eigrp_summary_delete")

    for text in ("struct prefix", "struct in_addr", "AF_INET", "prefix4"):
        assert text not in summary
    assert "EIGRP_ADDRESS_FAMILY_IPV4" not in create
    assert "EIGRP_ADDRESS_FAMILY_IPV4" not in delete
    assert "eigrp_summary_prefix_normalize" in create
    assert "eigrp_summary_prefix_normalize" in delete


def test_named_summary_adapter_uses_generic_prefix_boundary():
    northbound = read(NORTHBOUND)
    cli = read(CLI_NAMED)
    parser = function_body(northbound, "eigrpd_named_prefix_parse")
    ipv4_adapter = function_body(cli, "eigrp_cli_ipv4_summary_prefix")
    apply = function_body(
        northbound, "eigrpd_named_af_interface_summary_apply_options"
    )
    destroy = function_body(northbound, "eigrpd_named_af_interface_summary_destroy")

    assert "INET6_ADDRSTRLEN" in parser
    assert "EIGRP_ADDRESS_FAMILY_IPV6" in parser
    assert "prefix->prefix_length = (uint8_t)prefix_length" in parser
    assert "network4.s_addr = address4.s_addr & mask4.s_addr" in ipv4_adapter
    assert '"%s/%u"' in ipv4_adapter
    assert "eigrp_summary_create(&context, &prefix, &options)" in apply
    assert "eigrp_summary_delete(&context, &prefix)" in destroy
    assert 'yang_dnode_get_string(dnode, "prefix")' in apply


def test_auto_summary_command_applicability_is_explicit_not_a_null_vector():
    summary = read(SUMMARY_C)
    update = function_body(summary, "eigrp_summary_auto_update")
    types = read(TYPES_H)

    assert "summary_auto_prefix" in types
    assert "eigrp_summary_context_vectors(context)" in update
    assert "!vectors->summary_auto_prefix" not in update
    assert "vectors->afi != EIGRP_ADDRESS_FAMILY_IPV4" in update
    assert "EIGRP_ADDRESS_FAMILY_IPV6" not in update


def test_ipv4_owns_classful_auto_summary_derivation():
    ipv4 = read(IPV4_C)
    derive = function_body(ipv4, "eigrp_ipv4_summary_auto_prefix")

    assert "first_octet < 128" in derive
    assert "classful_length = 8" in derive
    assert "first_octet < 192" in derive
    assert "classful_length = 16" in derive
    assert "first_octet < 224" in derive
    assert "classful_length = 24" in derive
    assert "component->prefix_length < classful_length" in derive
    assert "vectors->summary_auto_prefix = eigrp_ipv4_summary_auto_prefix" in ipv4


def test_ipv6_binds_explicit_unsupported_auto_summary_semantics():
    ipv6 = read(IPV6_C)
    unsupported = function_body(ipv6, "eigrp_ipv6_summary_auto_prefix")

    assert "return EIGRP_RESULT_UNSUPPORTED" in unsupported
    assert "vectors->summary_auto_prefix = eigrp_ipv6_summary_auto_prefix" in ipv6
