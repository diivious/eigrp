#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Execute the portable prefix-limit threshold helper against real C code."""

from pathlib import Path
import subprocess
import textwrap

ROOT = Path(__file__).resolve().parents[3]


def test_default_and_explicit_warning_thresholds(tmp_path):
    source = tmp_path / "threshold.c"
    binary = tmp_path / "threshold"
    source.write_text(textwrap.dedent(r'''
        #include <assert.h>
        #include <stdint.h>
        #include "eigrp/code/eigrp.h"

        int main(void) {
            eigrp_prefix_limit_t limit = {.maximum = 4};
            /* Omitted threshold must warn on the crossing from 2 to 3. */
            assert(!eigrp_prefix_limit_threshold_crossed(&limit, 0));
            assert(!eigrp_prefix_limit_threshold_crossed(&limit, 1));
            assert(eigrp_prefix_limit_threshold_crossed(&limit, 2));
            assert(!eigrp_prefix_limit_threshold_crossed(&limit, 3));
            assert(!eigrp_prefix_limit_threshold_crossed(&limit, 4));
            /* Explicit values override, not add to, Cisco's 75% default. */
            limit.threshold = 50;
            assert(eigrp_prefix_limit_threshold_crossed(&limit, 1));
            assert(!eigrp_prefix_limit_threshold_crossed(&limit, 2));
            limit.threshold = 100;
            assert(eigrp_prefix_limit_threshold_crossed(&limit, 3));
            limit.maximum = 1;
            limit.threshold = 0;
            assert(eigrp_prefix_limit_threshold_crossed(&limit, 0));
            limit.maximum = UINT32_MAX;
            assert(!eigrp_prefix_limit_threshold_crossed(&limit, 0));
            assert(eigrp_prefix_limit_threshold_crossed(&limit, 3221225471U));
            assert(!eigrp_prefix_limit_threshold_crossed(&limit, UINT32_MAX));
            return 0;
        }
    '''))
    compile = subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", f"-I{ROOT}",
         str(source), "-o", str(binary)],
        cwd=ROOT, text=True, capture_output=True)
    assert compile.returncode == 0, compile.stderr
    result = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True)
    assert result.returncode == 0, result.stderr


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main(["-q", __file__]))
