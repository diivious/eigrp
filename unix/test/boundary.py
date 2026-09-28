# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import os
import re
import subprocess

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
UNIX = ROOT / "unix" / "code"
PUBLIC = ROOT / "eigrpd" / "code"


def test_unix_adapter_uses_only_public_eigrp_headers():
    allowed = {"eigrp.h", "eigrp_cli.h", "eigrp_mgnt.h", "eigrp_rib.h", "eigrp_sys.h"}
    include_re = re.compile(r'^\s*#\s*include\s+"(eigrp[^"/]*\.h)"', re.MULTILINE)

    for path in UNIX.glob("*.[ch]"):
        for header in include_re.findall(path.read_text()):
            if header.startswith("eigrp_unix"):
                continue
            assert header in allowed, f"{path.name} includes private portable header {header}"


def test_unix_sys_runtime_compiles_against_public_contract(tmp_path):
    compiler = os.environ.get("CC", "cc")
    source = UNIX / "eigrp_unix_sys.c"
    result = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-D_POSIX_C_SOURCE=200809L",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            "-fsyntax-only",
            f"-I{PUBLIC}",
            f"-I{UNIX}",
            str(source),
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert result.returncode == 0, result.stderr


def test_unix_runtime_keeps_packet_transport_separate_from_scheduler_and_rib():
    runtime = (UNIX / "eigrp_unix_sys.c").read_text()
    wire = (UNIX / "eigrp_unix_wire.c").read_text()
    assert "eigrp_sys_packet_send(" not in runtime
    assert "eigrp_sys_packet_send(" in wire
    assert "eigrp_rib_route_install(" not in runtime
    assert "eigrp_rib_route_install(" not in wire
