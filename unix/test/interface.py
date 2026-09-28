# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import os
import subprocess

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
UNIX = ROOT / "unix" / "code"
PUBLIC = ROOT / "eigrpd" / "code"
HARNESS = ROOT / "unix" / "test" / "interface_segment_harness.c"


def test_unix_interface_and_shared_segment_model(tmp_path):
    compiler = os.environ.get("CC", "cc")
    binary = tmp_path / "interface-segment-harness"
    build = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-D_POSIX_C_SOURCE=200809L",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{PUBLIC}",
            f"-I{UNIX}",
            str(UNIX / "eigrp_unix_interface.c"),
            str(UNIX / "eigrp_unix_rib.c"),
            str(UNIX / "eigrp_unix_segment.c"),
            str(HARNESS),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert build.returncode == 0, build.stderr
    run = subprocess.run(
        [str(binary)], cwd=ROOT, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert run.returncode == 0, run.stderr
    assert "unix interface/segment tests: PASS" in run.stdout


def test_segment_model_has_no_point_to_point_or_ethernet_assumptions():
    text = (UNIX / "eigrp_unix_segment.c").read_text()
    assert "endpoint_count" in text
    for forbidden in ('"R1"', '"R2"', "endpoint_a =", "endpoint_b =", "vlan_id", "mac_address"):
        assert forbidden not in text
