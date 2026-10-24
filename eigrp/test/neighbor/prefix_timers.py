#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Build and exercise real portable prefix-limit penalty timers."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"
UNIX = ROOT / "unix" / "code"


def test_prefix_limit_restart_reset_dampening_and_lockout(tmp_path):
    core = sorted(p for p in CODE.glob("*.c") if p.name not in ("eigrp_dump.c", "eigrp.c"))
    unix = sorted(p for p in UNIX.glob("*.c") if p.name not in (
        "eigrp_unix_config.c", "eigrp_unix_wire_broker.c", "eigrp_unix_keychain.c", "eigrp_unix_policy.c"))
    binary = tmp_path / "prefix-timers"
    cmd = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
           "-DEIGRP_DISABLE_SNMP", "-pthread", "-ffunction-sections", "-fdata-sections",
           f"-I{CODE}", f"-I{UNIX}",
           str(Path(__file__).with_suffix(".c")),
           *(str(p) for p in core), *(str(p) for p in unix),
           "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
           "-lm", "-o", str(binary)]
    built = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    ran = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
    assert ran.returncode == 0, ran.stdout + ran.stderr
