#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Compile the production VTY filter and neighbor status implementation."""

from pathlib import Path
import subprocess
import sys
import textwrap

ROOT = Path(__file__).resolve().parents[3]


def test_process_pending_neighbor_remains_in_frr_neighbors(tmp_path):
    source = tmp_path / "visible.c"
    binary = tmp_path / "visible"
    source.write_text(textwrap.dedent(r'''
        #include <assert.h>
        #include "eigrp_structs.h"
        #include "eigrp_vty.h"
        int main(void) {
            eigrp_nbr_t nbr = {0};
            assert(!eigrp_vty_neighbor_visible(NULL, true));
            assert(!eigrp_vty_neighbor_visible(&nbr, false));
            assert(eigrp_vty_neighbor_visible(&nbr, true));
            nbr.state = EIGRP_NEIGHBOR_UP;
            assert(eigrp_nbr_limit_status(&nbr) == 'A');
            assert(eigrp_vty_neighbor_visible(&nbr, false));
            nbr.state = EIGRP_NEIGHBOR_DOWN;
            nbr.eigrp_pfxlimits_af_suspended = true;
            assert(nbr.limit_state == EIGRP_NBR_LIMIT_NONE);
            assert(eigrp_nbr_limit_status(&nbr) == 'P');
            assert(eigrp_vty_neighbor_visible(&nbr, false));
            nbr.eigrp_pfxlimits_af_suspended = false;
            nbr.limit_cleanup_pending = true;
            assert(eigrp_nbr_limit_status(&nbr) == 'P');
            assert(eigrp_vty_neighbor_visible(&nbr, false));
            nbr.limit_cleanup_pending = false;
            nbr.limit_state = EIGRP_NBR_LIMIT_SUSPENDED;
            assert(eigrp_nbr_limit_status(&nbr) == 'P');
            assert(eigrp_vty_neighbor_visible(&nbr, false));
            nbr.limit_state = EIGRP_NBR_LIMIT_LOCKED;
            assert(eigrp_nbr_limit_status(&nbr) == 'D');
            assert(eigrp_vty_neighbor_visible(&nbr, false));
            nbr.limit_state = EIGRP_NBR_LIMIT_NONE;
            assert(!eigrp_vty_neighbor_visible(&nbr, false));
            return 0;
        }
    '''))
    compile = subprocess.run(
        ["cc", "-std=c11", "-O1", "-D_POSIX_C_SOURCE=200809L",
         "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
         "-Werror", "-Wno-unused-parameter",
         f"-I{ROOT / 'eigrp/code'}", f"-I{ROOT / 'frr/code'}",
         str(source), str(ROOT / 'eigrp/code/eigrp_neighbor.c'),
         "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
         "-pthread", "-o", str(binary)],
        cwd=ROOT, text=True, capture_output=True)
    assert compile.returncode == 0, compile.stderr
    result = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True)
    assert result.returncode == 0, result.stderr


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main(["-q", __file__]))
