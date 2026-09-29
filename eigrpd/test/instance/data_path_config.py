# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
INSTANCE = ROOT / "eigrpd" / "code" / "eigrp_instance.c"
EIGRPD = ROOT / "eigrpd" / "code" / "eigrpd.c"
STRUCTS = ROOT / "eigrpd" / "code" / "eigrp_structs.h"


def test_af_runtime_has_no_per_instance_data_path_gate():
    source = INSTANCE.read_text() + EIGRPD.read_text() + STRUCTS.read_text()
    assert "data_path_ready" not in source
    assert "eigrp_af_instance_create_with_data_path" not in source


def test_af_runtime_requires_packet_vectors_and_opens_socket():
    source = EIGRPD.read_text()
    assert "EIGRP_AF_VECTOR_REQUIRE(packet_send);" in source
    assert "EIGRP_AF_VECTOR_REQUIRE(packet_receive);" in source
    assert "eigrp_sys_socket_open(eigrp)" in source
