#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Link and execute the AF prefix-limit API using FRR's portable source list."""
from pathlib import Path
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
CODE = ROOT / "eigrp" / "code"
UNIX = ROOT / "unix" / "code"
MANIFEST = ROOT / "frr" / "code" / "subdir.am"


def test_frr_prefix_limit_link(tmp_path):
    # Select the same portable files that the FRR Automake target compiles.
    source_block = MANIFEST.read_text().split("eigrpd_eigrpd_SOURCES =", 1)[1].split("# end", 1)[0]
    names = set(re.findall(r"eigrpd/([a-z0-9_]+\.c)", source_block))
    core = sorted(p for p in CODE.glob("*.c") if p.name in names)
    assert core, "no portable source files selected by FRR's eigrpd manifest"

    # Unix supplies the external EIGRP services for this standalone link test.
    unix = sorted(p for p in UNIX.glob("*.c") if p.name not in (
        "eigrp_unix_config.c", "eigrp_unix_wire_broker.c"))
    harness = tmp_path / "pfxlimits_link.c"
    harness.write_text("""\
#include <stddef.h>
#include "eigrp.h"
#include "eigrp_pfxlimits.h"
int main(void)
{
    if (eigrp_pfxlimits_state_clear(NULL) != EIGRP_RESULT_INVALID_ARGUMENT)
        return 1;
    eigrp_pfxlimits_state_cleanup(NULL);
    eigrp_pfxlimits_prefix_exceeded(NULL, NULL);
    return 0;
}
""")
    binary = tmp_path / "pfxlimits_link"
    cmd = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L",
           "-DEIGRP_DISABLE_SNMP", "-pthread", "-ffunction-sections", "-fdata-sections",
           f"-I{CODE}", f"-I{UNIX}", str(harness), *(str(p) for p in core),
           str(CODE / "eigrp_log.c"), *(str(p) for p in unix),
           "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections",
           "-lm", "-o", str(binary)]
    built = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    ran = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
    assert ran.returncode == 0, ran.stdout + ran.stderr
