#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Compile and run the real EIGRP event-ring restart diagnostic formatter."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())

def test_restart_diagnostic_ring(tmp_path):
    executable = tmp_path / "restart_diagnostics"
    command = [
        os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
        "-O0", "-ffunction-sections", "-fdata-sections",
        "-I", str(ROOT / "eigrp/code"),
        str(ROOT / "eigrp/test/rtp/restart_diagnostics.c"),
        str(ROOT / "eigrp/code/eigrp_eventlog.c"),
        *(["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]),
        "-o", str(executable),
    ]
    build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert "restart diagnostic ring: PASS" in run.stdout

if __name__ == "__main__":
    from tempfile import TemporaryDirectory

    with TemporaryDirectory(prefix="eigrp-restart-diag-") as directory:
        test_restart_diagnostic_ring(Path(directory))
    print("restart_diagnostics.py: PASS")
