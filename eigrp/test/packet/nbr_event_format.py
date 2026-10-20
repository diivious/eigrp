#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Compile and execute the real neighbor event formatter."""
from pathlib import Path
import os
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())


def test_neighbor_event_format(tmp_path):
    executable = tmp_path / "nbr_event_format"
    command = [
        os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
        "-O0", "-ffunction-sections", "-fdata-sections",
        "-I", str(ROOT / "eigrp/code"),
        str(ROOT / "eigrp/test/packet/nbr_event_format.c"),
        str(ROOT / "eigrp/code/eigrp_eventlog.c"),
        *(["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]),
        "-o", str(executable),
    ]
    build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(executable)], cwd=ROOT, capture_output=True, text=True)
    assert run.returncode == 0, run.stderr
    assert "neighbor event format: PASS" in run.stdout


if __name__ == "__main__":
    from tempfile import TemporaryDirectory

    with TemporaryDirectory(prefix="eigrp-nbr-event-") as directory:
        test_neighbor_event_format(Path(directory))
