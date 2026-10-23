#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Exercise real process-limit decay and lockout with 90s monotonic uptime."""
import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[3]
UUT = ROOT / "eigrp/test/uut"

# This interposer affects only the subprocess test environment. It simulates
# a newly booted Linux CI runner without advancing the actual machine clock.
CLOCK_SHIM = r'''
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>

int clock_gettime(clockid_t id, struct timespec *ts)
{
    static int (*next_clock)(clockid_t, struct timespec *);
    static uint64_t start;
    uint64_t now, fake;
    int result;
    if (!next_clock)
        next_clock = dlsym(RTLD_NEXT, "clock_gettime");
    result = next_clock(id, ts);
    if (result || id != CLOCK_MONOTONIC)
        return result;
    now = (uint64_t)ts->tv_sec * 1000U + (uint64_t)ts->tv_nsec / 1000000U;
    if (!start)
        start = now;
    fake = 90000U + now - start;
    ts->tv_sec = (time_t)(fake / 1000U);
    ts->tv_nsec = (long)((fake % 1000U) * 1000000U);
    return 0;
}
'''


def test_process_dampening_fresh_linux_uptime(tmp_path):
    if sys.platform != "linux":
        pytest.skip("clock interposition test is Linux-specific; YAML UUT runs on macOS")
    shim = tmp_path / "monotime.c"
    library = tmp_path / "monotime.so"
    shim.write_text(CLOCK_SHIM)
    built = subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC",
                            "-o", str(library), str(shim), "-ldl"],
                           text=True, capture_output=True)
    assert built.returncode == 0, built.stdout + built.stderr
    built = subprocess.run(["make", "-C", str(UUT), "eigrp-uut-node"],
                           text=True, capture_output=True)
    assert built.returncode == 0, built.stdout + built.stderr
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(library)
    ran = subprocess.run([sys.executable, str(UUT / "run.py"),
                          str(UUT / "examples/two-router.yaml"),
                          str(UUT / "prefix-limit/scenario-process-dampened-ipv4.yaml")],
                         cwd=ROOT, env=env, text=True, capture_output=True,
                         timeout=70)
    assert ran.returncode == 0, ran.stdout + ran.stderr
