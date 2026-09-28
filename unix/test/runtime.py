# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import os
import subprocess

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
UNIX = ROOT / "unix" / "code"
PUBLIC = ROOT / "eigrpd" / "code"
HARNESS = ROOT / "unix" / "test" / "runtime_harness.c"


def test_unix_runtime_builds_and_executes(tmp_path):
    compiler = os.environ.get("CC", "cc")
    binary = tmp_path / "runtime-harness"
    build = subprocess.run(
        [
            compiler,
            "-std=c11",
            "-D_POSIX_C_SOURCE=200809L",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-pthread",
            f"-I{PUBLIC}",
            f"-I{UNIX}",
            str(UNIX / "eigrp_unix_sys.c"),
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

    run = subprocess.run(
        [str(binary)],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=10,
    )
    assert run.returncode == 0, run.stderr
    assert "unix runtime tests: PASS" in run.stdout


def test_standalone_runtime_archive_builds_without_frr_or_bird():
    clean = subprocess.run(["make", "-C", "unix/code", "clean"], cwd=ROOT)
    assert clean.returncode == 0
    build = subprocess.run(
        ["make", "-C", "unix/code"],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert build.returncode == 0, build.stderr
