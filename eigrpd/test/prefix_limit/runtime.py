# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import subprocess
import textwrap

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def test_prefix_limit_policy_behavior_executes(tmp_path):
    source = tmp_path / "prefix_limit.c"
    binary = tmp_path / "prefix_limit"
    source.write_text(textwrap.dedent(r'''
        #include <assert.h>
        #include "eigrpd/code/eigrp.h"

        int main(void) {
            eigrp_prefix_limit_t limit = {.maximum = 10, .threshold = 80};
            assert(eigrp_prefix_limit_allows(&limit, 7, false));
            assert(eigrp_prefix_limit_threshold_crossed(&limit, 7));
            assert(eigrp_prefix_limit_allows(&limit, 9, false));
            assert(!eigrp_prefix_limit_allows(&limit, 10, false));
            assert(eigrp_prefix_limit_allows(&limit, 10, true));
            limit.warning_only = true;
            assert(eigrp_prefix_limit_allows(&limit, 10, false));
            limit.warning_only = false;
            limit.dampened = true;
            assert(!eigrp_prefix_limit_runtime_supported(&limit));
            assert(eigrp_prefix_limit_allows(&limit, 10, false));
            return 0;
        }
    '''))
    result = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                             f"-I{ROOT}", str(source), "-o", str(binary)],
                            cwd=ROOT, text=True, capture_output=True)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True)
    assert result.returncode == 0, result.stderr


def test_prefix_limit_scopes_are_wired_to_real_lifecycle_points():
    update = (ROOT / "eigrpd/code/eigrp_update.c").read_text()
    redist = (ROOT / "eigrpd/code/eigrp_redistribute.c").read_text()
    neighbor = (ROOT / "eigrpd/code/eigrp_neighbor.c").read_text()
    topology = (ROOT / "eigrpd/code/eigrp_topology.c").read_text()
    assert "!eigrp_nbr_prefix_admit(eigrp, nbr" in update
    assert "!eigrp_topology_prefix_admit(eigrp" in update
    assert "!eigrp_redist_prefix_admit(runtime, route)" in redist
    assert "eigrp_nbr_prefix_count(runtime, neighbor)" in neighbor
    assert "eigrp_topology_prefix_count(runtime)" in topology
    assert "eigrp_redist_prefix_count(runtime)" in redist
