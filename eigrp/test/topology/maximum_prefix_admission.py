# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Execute production topology and neighbor maximum-prefix admission tests."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'eigrp' / 'code').is_dir())


def test_maximum_prefix_query_retention_and_replacement(tmp_path):
    binary = tmp_path / 'maximum_prefix_admission'
    topology_only = os.getenv('EIGRP_PREFIX_TEST_TOPOLOGY_ONLY') == '1'
    sources = ['eigrp_topology.c', *([] if topology_only else ['eigrp_neighbor.c']), 'eigrp_timer.c', 'eigrp_metric.c',
               'eigrp_list.c', 'eigrp_table.c', 'eigrp_prefix.c']
    cmd = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O0', '-Wall', '-Wextra',
           '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
           '-ffunction-sections', '-fdata-sections', '-pthread',
           *([] if topology_only else ['-DTEST_NEIGHBOR_LIMIT']), '-I', str(ROOT),
           '-I', str(ROOT / 'eigrp' / 'code'),
           str(Path(__file__).with_suffix('.c')),
           *(str(ROOT / 'eigrp' / 'code' / src) for src in sources),
           *(['-Wl,-dead_strip'] if sys.platform == 'darwin' else
             ['-Wl,--gc-sections']), '-o', str(binary)]
    build = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True)
    assert run.returncode == 0, run.stderr
