# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import os
import subprocess

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
UNIX = ROOT / "unix" / "code"
PUBLIC = ROOT / "eigrpd" / "code"
HARNESS = ROOT / "unix" / "test" / "rib_harness.c"


def test_unix_internal_rib_route_and_source_lifecycle(tmp_path):
    compiler = os.environ.get("CC", "cc")
    binary = tmp_path / "rib-harness"
    build = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{PUBLIC}",
            f"-I{UNIX}",
            str(UNIX / "eigrp_unix_rib.c"),
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
    assert "unix rib tests: PASS" in run.stdout


def test_unix_rib_is_internal_and_not_kernel_route_programming():
    rib = (UNIX / "eigrp_unix_rib.c").read_text()
    assert "eigrp_rib_route_add" in rib
    assert "eigrp_rib_route_del" in rib
    assert "eigrp_rib_route_add" in rib
    assert "eigrp_rib_route_del" in rib
    for forbidden in ("NETLINK_ROUTE", "PF_ROUTE", "RTM_ADD", "RTM_DELETE", "/sbin/route", "ip route"):
        assert forbidden not in rib
