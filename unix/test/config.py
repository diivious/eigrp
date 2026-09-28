# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import os
import subprocess

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
UNIX = ROOT / "unix" / "code"
PUBLIC = ROOT / "eigrpd" / "code"
HARNESS = ROOT / "unix" / "test" / "config_harness.c"


def test_named_ipv4_config_reaches_real_semantic_targets(tmp_path):
    compiler = os.environ.get("CC", "cc")
    binary = tmp_path / "config-harness"
    build = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-D_POSIX_C_SOURCE=200809L",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{PUBLIC}",
            f"-I{UNIX}",
            str(UNIX / "eigrp_unix_config.c"),
            str(HARNESS),
            "-o",
            str(binary),
        ],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(binary)], cwd=ROOT, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    assert run.returncode == 0, run.stderr
    assert "unix config tests: PASS" in run.stdout


def test_unix_parser_objects_do_not_leak_into_portable_code():
    for path in (ROOT / "eigrpd" / "code").glob("*.[ch]"):
        text = path.read_text()
        assert "eigrp_unix_config" not in text
