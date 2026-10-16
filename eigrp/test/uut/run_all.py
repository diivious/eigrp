#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Discover and run every portable EIGRP YAML UUT scenario."""
from __future__ import annotations

import argparse
import math
import os
import shlex
import signal
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



# 180 seconds is a watchdog, not the convergence oracle: YAML steps retain
# their own stricter timeouts. Override for slower CI hosts with
# EIGRP_UUT_SCENARIO_TIMEOUT or --scenario-timeout.
DEFAULT_SCENARIO_TIMEOUT = 180.0


def positive_seconds(value: str) -> float:
    try:
        seconds = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("scenario timeout must be a number") from exc
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("scenario timeout must be finite and > 0")
    return seconds


def execute_scenario(command: list[str], timeout: float) -> tuple[int, str, bool]:
    """Bound a scenario *and its children*; retain stdout on timeout.

    Each scenario has its own POSIX session so a stalled runner and its UUT
    children can be terminated together without disturbing the suite launcher.
    """
    proc = subprocess.Popen(command, cwd=PROJECT_ROOT, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            start_new_session=True)
    try:
        output, _ = proc.communicate(timeout=timeout)
        return proc.returncode, output, False
    except subprocess.TimeoutExpired as exc:
        # TimeoutExpired.stdout may contain bytes even with text=True. Save
        # this partial output explicitly: a second communicate() after SIGTERM
        # is not guaranteed to return it on every Python/platform version.
        partial = exc.stdout or b""
        if isinstance(partial, bytes):
            partial = partial.decode("utf-8", errors="replace")
        # Terminate the entire UUT process group, not just its Python parent.
        # communicate() after a timeout retains the output already received.
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            output, _ = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                output, _ = proc.communicate(timeout=5)
            except subprocess.TimeoutExpired as last:
                # An independently reparented child can retain stdout open
                # after the runner is killed. Keep any buffered diagnostics
                # and advance to the next scenario instead of hanging here.
                output = last.stdout or b""
                if proc.stdout:
                    proc.stdout.close()
                if proc.poll() is None:
                    proc.kill()
                proc.wait(timeout=2)
        if isinstance(output, bytes):
            output = output.decode("utf-8", errors="replace")
        if partial and not output.startswith(partial):
            output = partial + output
        return 124, output, True


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario-timeout", type=positive_seconds,
                        default=os.environ.get("EIGRP_UUT_SCENARIO_TIMEOUT", str(DEFAULT_SCENARIO_TIMEOUT)),
                        help="wall-clock watchdog for each scenario (seconds, default: 180; "
                             "env: EIGRP_UUT_SCENARIO_TIMEOUT)")
    args = parser.parse_args(argv)
    try:
        timeout = positive_seconds(str(args.scenario_timeout))
    except argparse.ArgumentTypeError as exc:
        parser.error(str(exc))
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
        try:
            returncode, output, timed_out = execute_scenario(command, timeout)
        except OSError as exc:
            returncode, output, timed_out = 1, f"scenario launcher failed: {exc}\n", False
        with log_path.open("w", encoding="utf-8") as log:
            log.write(f"test case: {name}\n")
            log.write(f"scenario: {rel_scenario}\n")
            log.write(f"command: {shlex.join(display_command)}\n")
            rerun = ["./tools/python", display_arg(RUNNER), display_arg(topology), display_arg(scenario), "--verbose", "--report"]
            log.write(f"rerun: {shlex.join(rerun)}\n")
            log.write(f"exit status: {returncode}\n")
            if timed_out:
                log.write(f"TIMEOUT: scenario exceeded {timeout:g}s; "
                          "terminated UUT process group\n")
            log.write("\n===== runner/debug output =====\n")
            log.write(output or "")
            if output and not output.endswith("\n"):
                log.write("\n")
        if returncode:
            failures.append((name, log_path))
            print(f"  FAIL{' (TIMEOUT)' if timed_out else ''}: see "
                  f"{log_path.relative_to(PROJECT_ROOT)}", file=sys.stderr)
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
