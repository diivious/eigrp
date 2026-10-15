# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
#
# Direct behavior checks against the real portable redistribution and DUAL FSM.
# The last scenario guards against the route-less stuck-ACTIVE regression.

import os
from pathlib import Path
import subprocess
import sys

import pytest


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"
TEST_SOURCE = Path(__file__).with_suffix(".c")


@pytest.fixture(scope="module")
def withdrawal_binary(tmp_path_factory):
    binary = tmp_path_factory.mktemp("redistribution-withdrawal") / "withdrawal_fsm"
    compiler = os.environ.get("CC", "cc")
    sources = [
        "eigrp_redistribute.c",
        "eigrp_topology.c",
        "eigrp_fsm.c",
        "eigrp_timer.c",
        "eigrp_metric.c",
        "eigrp_list.c",
        "eigrp_table.c",
        "eigrp_prefix.c",
    ]
    command = [
        compiler,
        "-std=gnu11",
        "-O0",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-function",
        "-Wno-unused-parameter",
        "-ffunction-sections",
        "-fdata-sections",
        f"-I{ROOT}",
        f"-I{ROOT / 'test' / 'build' / 'include'}",
        str(TEST_SOURCE),
        *(str(CODE / name) for name in sources),
        *(["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]),
        "-pthread",
        "-o",
        str(binary),
    ]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return binary


@pytest.mark.parametrize(
    "scenario",
    [
        "no-neighbors",
        "query-route",
        "query-peer-reply",
        "query-empty",
    ],
)
def test_redistributed_route_withdrawal_dual_fsm(withdrawal_binary, scenario):
    result = subprocess.run(
        [str(withdrawal_binary), scenario],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr or result.stdout
