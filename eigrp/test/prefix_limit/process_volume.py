# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Exercise 100K actual redistributed DRDB/DNDB withdrawals under process limit."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"
TEST_SOURCE = ROOT / "eigrp" / "test" / "redistribution" / "withdrawal_fsm.c"


def test_process_limit_100k_withdrawal_retirement_and_readmission(tmp_path):
    binary = tmp_path / "process_limit_100k"
    sources = (
        "eigrp_redistribute.c", "eigrp_pfxlimits.c", "eigrp_topology.c",
        "eigrp_fsm.c", "eigrp_timer.c", "eigrp_metric.c", "eigrp_list.c",
        "eigrp_table.c", "eigrp_prefix.c",
    )
    command = [
        os.environ.get("CC", "cc"), "-std=gnu11", "-O0", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
        "-DEIGRP_TEST_PROCESS_VOLUME", "-ffunction-sections", "-fdata-sections",
        f"-I{ROOT}", f"-I{ROOT / 'test' / 'build' / 'include'}",
        str(TEST_SOURCE), *(str(CODE / file) for file in sources),
        *(["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]),
        "-pthread", "-o", str(binary),
    ]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary), "process-volume"], cwd=ROOT,
                            capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stderr or result.stdout
