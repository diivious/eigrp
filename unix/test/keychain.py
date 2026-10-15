# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Compile the real Unix keychain southbound and exercise lookup semantics."""
from pathlib import Path
import os
import subprocess

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / "eigrp" / "code").is_dir() and (p / "unix" / "code").is_dir())
UNIX = ROOT / "unix" / "code"
PUBLIC = ROOT / "eigrp" / "code"
HARNESS = ROOT / "unix" / "test" / "keychain_harness.c"


def test_unix_keychain_host_provider(tmp_path):
    binary = tmp_path / "unix-keychain"
    cmd = [
        os.environ.get("CC", "cc"), "-std=c11",
        "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
        "-pthread", f"-I{PUBLIC}", f"-I{UNIX}",
        str(UNIX / "eigrp_unix_keychain.c"), str(HARNESS), "-o", str(binary),
    ]
    build = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                         capture_output=True, timeout=10)
    assert run.returncode == 0, run.stderr
    assert "unix keychain tests: PASS" in run.stdout
