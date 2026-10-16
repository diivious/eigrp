# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Production-C neighbor limits, fault injection, flood and recovery."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"
UNIX = ROOT / "unix" / "code"
SOURCE = Path(__file__).with_suffix(".c")


def test_neighbor_admission_allocations_flood_and_recovery():
    core = sorted(p for p in CODE.glob("*.c") if p.name not in ("eigrp_dump.c", "eigrp.c"))
    unix = sorted(p for p in UNIX.glob("*.c")
                  if p.name not in ("eigrp_unix_config.c", "eigrp_unix_wire_broker.c", "eigrp_unix_keychain.c", "eigrp_unix_policy.c"))
    with tempfile.TemporaryDirectory() as tmp:
        executable = Path(tmp) / "resource-admission"
        test_object = Path(tmp) / "resource-test.o"
        includes = [f"-I{CODE}", f"-I{UNIX}"]
        linker_gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DEIGRP_DISABLE_SNMP", "-pthread", "-ffunction-sections", "-fdata-sections"]
        subprocess.run(["cc", *flags, *includes, "-c", str(SOURCE), "-o", str(test_object)], check=True)
        subprocess.run(
            ["cc", *flags, *includes, "-Dcalloc=eigrp_test_calloc", *map(str, core),
             *map(str, unix), str(test_object), linker_gc, "-o", str(executable)],
            check=True, cwd=ROOT,
        )
        subprocess.run([str(executable)], check=True, cwd=ROOT)
