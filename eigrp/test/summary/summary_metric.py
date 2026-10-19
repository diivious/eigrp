#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Compile and execute summary-metric behavior against real portable C code."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]
CORE = ROOT / "eigrp" / "code"
SOURCE = Path(__file__).with_suffix(".c")


def test_summary_metric_behavior():
    with tempfile.TemporaryDirectory(prefix="eigrp-summary-") as temp:
        binary = Path(temp) / "summary-metric"
        command = [
            os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
            "-DEIGRP_DISABLE_SNMP", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-ffunction-sections", "-fdata-sections", "-I" + str(CORE),
            str(SOURCE), str(CORE / "eigrp_metric.c"),
            "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
            "-o", str(binary),
        ]
        subprocess.run(command, cwd=ROOT, check=True)
        subprocess.run([str(binary)], cwd=ROOT, check=True)


if __name__ == "__main__":
    test_summary_metric_behavior()
