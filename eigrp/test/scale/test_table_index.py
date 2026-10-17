# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Exercise the actual portable indexed topology table in a small C harness."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[3]
CODE = ROOT / "eigrp" / "code"


def test_portable_table_index(tmp_path):
    executable = tmp_path / "table-index"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        f"-I{CODE}",
        str(Path(__file__).with_name("table_index.c")),
        str(CODE / "eigrp_table.c"),
        str(CODE / "eigrp_prefix.c"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)


def test_portable_change_list_index(tmp_path):
    executable = tmp_path / "list-index"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        f"-I{CODE}",
        str(Path(__file__).with_name("list_index.c")),
        str(CODE / "eigrp_list.c"),
        "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
