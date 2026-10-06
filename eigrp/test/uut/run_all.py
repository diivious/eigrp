#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Discover and run every portable EIGRP YAML UUT scenario."""
from __future__ import annotations

import shlex
import subprocess
import sys
from pathlib import Path

import yaml

TEST_SCHEMA = "eigrp-uut-test/v1"
TOPOLOGY_SCHEMA = "eigrp-uut-topology/v1"
HERE = Path(__file__).resolve().parent
TEST_ROOT = HERE.parent
PROJECT_ROOT = TEST_ROOT.parent.parent
RUNNER = HERE / "run.py"
LOG_ROOT = HERE / "logs"


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


def scenario_name(scenario: Path) -> str:
    name = str(document(scenario).get("name") or scenario.stem)
    safe = "".join(ch.lower() if ch.isalnum() else "-" for ch in name).strip("-")
    while "--" in safe:
        safe = safe.replace("--", "-")
    return safe or scenario.stem


def display_arg(path: Path) -> str:
    try:
        return str(path.relative_to(PROJECT_ROOT))
    except ValueError:
        return str(path)


def main() -> int:
    try:
        scenarios = discover()
    except Exception as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1

    LOG_ROOT.mkdir(parents=True, exist_ok=True)
    for old_log in LOG_ROOT.glob("*.log"):
        old_log.unlink()

    print(f"YAML UUT scenarios discovered: {len(scenarios)}")
    failures: list[tuple[str, Path]] = []
    for index, (topology, scenario) in enumerate(scenarios, 1):
        name = scenario_name(scenario)
        log_path = LOG_ROOT / f"{name}.log"
        rel_scenario = scenario.relative_to(PROJECT_ROOT)
        command = [sys.executable, str(RUNNER), str(topology), str(scenario), "--verbose", "--report"]
        display_command = [sys.executable, display_arg(RUNNER), display_arg(topology), display_arg(scenario), "--verbose", "--report"]
        print(f"[{index}/{len(scenarios)}] {rel_scenario} -> {log_path.relative_to(PROJECT_ROOT)}", flush=True)
        result = subprocess.run(command, cwd=PROJECT_ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with log_path.open("w", encoding="utf-8") as log:
            log.write(f"test case: {name}\n")
            log.write(f"scenario: {rel_scenario}\n")
            log.write(f"command: {shlex.join(display_command)}\n")
            rerun = ["./tools/python", display_arg(RUNNER), display_arg(topology), display_arg(scenario), "--verbose", "--report"]
            log.write(f"rerun: {shlex.join(rerun)}\n")
            log.write(f"exit status: {result.returncode}\n")
            log.write("\n===== runner/debug output =====\n")
            log.write(result.stdout)
            if result.stdout and not result.stdout.endswith("\n"):
                log.write("\n")
        if result.returncode:
            failures.append((name, log_path))
            print(f"  FAIL: see {log_path.relative_to(PROJECT_ROOT)}", file=sys.stderr)
        else:
            print("  PASS", flush=True)

    if failures:
        print(f"YAML UUT scenarios failed: {len(failures)}/{len(scenarios)}", file=sys.stderr)
        for name, log_path in failures:
            print(f"  {name}: {log_path.relative_to(PROJECT_ROOT)}", file=sys.stderr)
        return 1
    print(f"YAML UUT scenarios passed: {len(scenarios)}/{len(scenarios)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
