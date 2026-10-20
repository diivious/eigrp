# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Execute the production topology metric update with QUERY-retained RDBs."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'eigrp' / 'code').is_dir())

def test_query_retained_routes_not_promoted_by_metric_update(tmp_path):
    binary = tmp_path / 'query_metric_retention'
    sources = ['eigrp_topology.c', 'eigrp_timer.c', 'eigrp_metric.c',
               'eigrp_list.c', 'eigrp_table.c', 'eigrp_prefix.c']
    cmd = [os.environ.get('CC', 'cc'), '-std=gnu11', '-O0', '-Wall', '-Wextra',
           '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
           '-ffunction-sections', '-fdata-sections', '-I', str(ROOT),
           '-I', str(ROOT / 'eigrp' / 'code'),
           str(Path(__file__).with_suffix('.c')),
           *(str(ROOT / 'eigrp' / 'code' / src) for src in sources),
           *(['-Wl,-dead_strip'] if sys.platform == 'darwin' else
             ['-Wl,--gc-sections']), '-o', str(binary)]
    build = subprocess.run(cmd, cwd=ROOT, text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True)
    assert run.returncode == 0, run.stderr
