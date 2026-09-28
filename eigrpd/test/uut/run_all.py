#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Discover and run every portable EIGRP YAML UUT scenario."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import yaml

TEST_SCHEMA = "eigrp-uut-test/v1"
TOPOLOGY_SCHEMA = "eigrp-uut-topology/v1"
HERE = Path(__file__).resolve().parent
TEST_ROOT = HERE.parent
RUNNER = HERE / "run.py"


def document(path: Path) -> dict:
    value = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise RuntimeError(f"{path}: YAML document must be a mapping")
    return value


def discover() -> list[tuple[Path, Path]]:
    scenarios: list[tuple[Path, Path]] = []
    errors: list[str] = []
    for path in sorted(TEST_ROOT.rglob("*.yaml")):
        data = document(path)
        schema = data.get("schema")
        if schema == TOPOLOGY_SCHEMA:
            continue
        if schema != TEST_SCHEMA:
            errors.append(f"{path}: unknown/missing schema {schema!r}")
            continue
        topology_name = data.get("topology")
        if not isinstance(topology_name, str) or not topology_name:
            errors.append(f"{path}: scenario has no topology")
            continue
        topology = (path.parent / topology_name).resolve()
        if not topology.is_file():
            errors.append(f"{path}: topology does not exist: {topology_name}")
            continue
        top_data = document(topology)
        if top_data.get("schema") != TOPOLOGY_SCHEMA:
            errors.append(f"{path}: {topology_name} is not an EIGRP UUT topology")
            continue
        scenarios.append((topology, path.resolve()))
    if errors:
        raise RuntimeError("YAML UUT discovery failed:\n  " + "\n  ".join(errors))
    if not scenarios:
        raise RuntimeError("no portable EIGRP YAML UUT scenarios discovered")
    return scenarios


def main() -> int:
    try:
        scenarios = discover()
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    print(f"YAML UUT scenarios discovered: {len(scenarios)}")
    for index, (topology, scenario) in enumerate(scenarios, 1):
        rel_scenario = scenario.relative_to(TEST_ROOT.parent.parent)
        print(f"[{index}/{len(scenarios)}] {rel_scenario}", flush=True)
        result = subprocess.run(
            [sys.executable, str(RUNNER), str(topology), str(scenario)],
            cwd=TEST_ROOT.parent.parent,
        )
        if result.returncode:
            return result.returncode
    print(f"YAML UUT scenarios passed: {len(scenarios)}/{len(scenarios)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
