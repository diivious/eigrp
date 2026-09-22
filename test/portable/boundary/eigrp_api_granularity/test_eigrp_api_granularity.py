# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for the public API granularity rule.

from pathlib import Path


ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def production_source() -> str:
    chunks = []
    for base in (ROOT / "eigrpd", ROOT / "frr", ROOT / "bird"):
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.suffix in {".c", ".h"}:
                chunks.append(path.read_text())
    return "\n".join(chunks)


def test_logging_uses_one_public_endpoint_with_a_level_selector():
    header = read("eigrpd/eigrp_log.h")
    source = production_source()

    assert "typedef enum eigrp_log_level" in header
    assert "void eigrp_log(eigrp_log_level_t level" in header
    for old in (
        "eigrp_log_debug",
        "eigrp_log_info",
        "eigrp_log_notice",
        "eigrp_log_warn",
        "eigrp_log_error",
    ):
        assert old not in source


def test_nonpacket_debug_uses_target_selector_instead_of_category_endpoints():
    header = read("eigrpd/eigrp_cli.h")
    source = production_source()

    assert "typedef enum eigrp_debug_target" in header
    assert "eigrp_debug_set(eigrp_debug_target_t target" in header
    assert "eigrp_debug_reset(eigrp_debug_target_t target" in header
    for family in (
        "event",
        "timers",
        "fsm",
        "nsf",
        "fast_reroute",
        "neighbor",
        "notifications",
        "transmit",
    ):
        assert f"eigrp_debug_{family}_set" not in source
        assert f"eigrp_debug_{family}_reset" not in source


def test_neighbor_codec_bind_uses_tlv_version_as_the_selector():
    neighbor_h = read("eigrpd/eigrp_neighbor.h")
    neighbor_c = read("eigrpd/eigrp_neighbor.c")
    tlv1_h = read("eigrpd/eigrp_tlv1.h")
    tlv2_h = read("eigrpd/eigrp_tlv2.h")

    assert "eigrp_neighbor_codec_bind(eigrp_neighbor_t *, uint8_t tlv_version)" in neighbor_h
    assert "case EIGRP_TLV_32B_VERSION:" in neighbor_c
    assert "case EIGRP_TLV_64B_VERSION:" in neighbor_c
    for header in (tlv1_h, tlv2_h):
        assert "neighbor_bind" not in header
        assert "interface_bind" not in header


def test_southbound_timer_add_has_one_canonical_millisecond_contract():
    header = read("eigrpd/eigrp_sys.h")
    source = production_source()

    assert "eigrp_sys_timer_add" in header
    assert "uint32_t delay_msec" in header
    assert "eigrp_southbound_timer_msec_add" not in source


def test_neighbor_logging_uses_log_type_selector_and_set_reset_actions():
    header = read("eigrpd/eigrp_cli.h")
    source = production_source()

    assert "typedef enum eigrp_neighbor_log_type" in header
    assert "eigrp_neighbor_log_set" in header
    assert "eigrp_neighbor_log_reset" in header
    for old in (
        "eigrp_neighbor_log_changes_update",
        "eigrp_neighbor_log_changes_reset",
        "eigrp_neighbor_log_warnings_update",
        "eigrp_neighbor_log_warnings_delete",
    ):
        assert old not in source


def test_code_conventions_define_public_api_granularity_rule():
    conventions = read("specs/design-spec.md")

    assert "Public API granularity" in conventions
    assert "semantic action" in conventions
    assert "selector" in conventions
