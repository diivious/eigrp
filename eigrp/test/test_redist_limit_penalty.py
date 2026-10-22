#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Executable regression test of actual redistribution lifecycle through UUT nodes."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
uut = root / "eigrp/test/uut"
scenarios = (
    ("examples/two-router.yaml", "prefix-limit/scenario-redist-penalty-ipv4.yaml"),
    ("examples/two-router.yaml", "prefix-limit/scenario-redist-lockout-ipv4.yaml"),
    ("examples/two-router.yaml", "prefix-limit/scenario-redist-count-ipv4.yaml"),
    ("examples/ipv6-basic-core.yaml", "prefix-limit/scenario-redist-count-ipv6.yaml"),
)


def main():
    subprocess.run(["make", "-C", str(uut)], check=True)
    for topology, scenario in scenarios:
        print(f"Executing {scenario}", flush=True)
        subprocess.run([sys.executable, str(uut / "run.py"),
                        str(uut / topology), str(uut / scenario)], check=True)
    print(f"PASS: {len(scenarios)} real-implementation UUT regression scenarios")


if __name__ == "__main__":
    main()
