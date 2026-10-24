# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Production receive/DUAL/neighbor limit and descriptor lifecycle regression."""
import os
import pytest
from pathlib import Path
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'eigrp' / 'code').is_dir())
CODE = ROOT / 'eigrp' / 'code'


@pytest.mark.parametrize("packet", ["query", "reply", "query-filter", "reply-filter"])
def test_query_reply_neighbor_path_admission(tmp_path, packet):
    binary = tmp_path / 'neighbor_path_admission'
    sources = ['eigrp_query.c', 'eigrp_reply.c', 'eigrp_fsm.c',
               'eigrp_topology.c', 'eigrp_neighbor.c', 'eigrp_metric.c',
               'eigrp_list.c', 'eigrp_table.c', 'eigrp_prefix.c']
    cmd = [os.getenv('CC', 'cc'), '-std=gnu11', '-O0', '-Wall', '-Wextra',
           '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
           '-ffunction-sections', '-fdata-sections', '-pthread',
           '-I', str(ROOT), '-I', str(CODE),
           str(Path(__file__).with_suffix('.c')),
           *(str(CODE / name) for name in sources),
           '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
           '-o', str(binary)]
    build = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary), packet], cwd=ROOT, capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
