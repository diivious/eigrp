# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for the public integration header contract.

from pathlib import Path
import re

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
EIGRPD = ROOT / "eigrpd" / "code"
PUBLIC_HEADERS = (
    "eigrp.h",
    "eigrp_cli.h",
    "eigrp_mgnt.h",
    "eigrp_rib.h",
    "eigrp_sys.h",
)


def read(path: Path) -> str:
    return path.read_text()


def test_public_integration_header_set_exists_and_legacy_split_headers_are_gone():
    for name in PUBLIC_HEADERS:
        assert (EIGRPD / name).is_file()

    assert not (EIGRPD / "eigrp_result.h").exists()
    assert not (EIGRPD / "eigrp_southbound.h").exists()


def test_public_headers_do_not_include_private_eigrp_headers():
    allowed = set(PUBLIC_HEADERS)
    include_re = re.compile(r'^\s*#\s*include\s+"eigrpd/([^"]+)"', re.MULTILINE)

    for name in PUBLIC_HEADERS:
        text = read(EIGRPD / name)
        for included in include_re.findall(text):
            assert included in allowed, f"{name} includes private header {included}"


def test_public_headers_do_not_expose_private_protocol_objects():
    forbidden = (
        "eigrp_prefix_descriptor_t",
        "eigrp_route_descriptor_t",
        "eigrp_packet_t",
        "eigrp_packetizer_work_t",
        "eigrp_stream_t",
        "struct stream",
        "struct list",
        "struct route_table",
        "struct route_node",
        "eigrp_tlv_codec_t",
    )

    public = "\n".join(read(EIGRPD / name) for name in PUBLIC_HEADERS)
    for token in forbidden:
        assert token not in public


def test_frr_installs_only_the_five_public_integration_headers():
    subdir = read(ROOT / "frr" / "code" / "subdir.am")
    start = subdir.index("eigrpdheader_HEADERS =")
    end = subdir.index("# end", start)
    installed = subdir[start:end]

    for name in PUBLIC_HEADERS:
        assert f"eigrpd/{name}" in installed

    for private in (
        "eigrpd/eigrpd.h",
        "eigrpd/eigrp_types.h",
        "eigrpd/eigrp_structs.h",
        "eigrpd/eigrp_topology.h",
        "eigrp/code/eigrp_packet.h",
        "eigrp/code/eigrp_packetizer.h",
        "eigrp/code/eigrp_tlv1.h",
        "eigrp/code/eigrp_tlv2.h",
        "eigrp/code/eigrp_fsm.h",
    ):
        assert private not in installed


def test_redistribution_public_identity_is_eigrp_owned_protocol_plus_route_instance():
    common = read(EIGRPD / "eigrp.h")
    cli = read(EIGRPD / "eigrp_cli.h")
    rib = read(EIGRPD / "eigrp_rib.h")

    assert "typedef enum eigrp_redist_protocol" in common
    assert "typedef uint32_t eigrp_route_instance_t;" in common
    assert "typedef struct eigrp_redist_source" in common
    assert "eigrp_redist_protocol_t protocol;" in common
    assert "eigrp_route_instance_t route_instance;" in common
    assert "const eigrp_redist_source_t *source" in cli
    assert "eigrp_redist_source_t source;" in rib
    assert "eigrp_metrics_t vecmetric;" in rib
    assert "uint32_t admin_dist;" in rib
    assert "source_protocol" not in rib
    assert "source_instance" not in rib
