# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""End-to-end watchdog and failure-log regressions."""
import importlib.util
from pathlib import Path
import sys
import time

import pytest


def suite_module():
    path = Path(__file__).resolve().parents[1] / "run_all.py"
    spec = importlib.util.spec_from_file_location("uut_suite_watchdog", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_watchdog_preserves_partial_output_and_marks_timeout():
    suite = suite_module()
    started = time.monotonic()
    command = [sys.executable, "-u", "-c",
               "import time; print('UUT-DIAGNOSTIC: before stall', flush=True); time.sleep(15)"]
    status, output, timed_out = suite.execute_scenario(command, timeout=2.5)
    assert status == 124
    assert timed_out
    assert "UUT-DIAGNOSTIC: before stall" in output
    assert time.monotonic() - started < 6


def test_watchdog_normal_completion():
    suite = suite_module()
    status, output, timed_out = suite.execute_scenario(
        [sys.executable, "-c", "print('done')"], timeout=3)
    assert status == 0 and output.strip() == "done" and not timed_out


@pytest.mark.parametrize("value", ["0", "-3", "nan", "inf", "-inf", "garbage"])
def test_watchdog_rejects_invalid_timeout(value):
    suite = suite_module()
    with pytest.raises(Exception, match="scenario timeout"):
        suite.positive_seconds(value)


def test_suite_continues_after_timeout_and_keeps_logs(tmp_path, monkeypatch):
    suite = suite_module()
    monkeypatch.setattr(suite, "PROJECT_ROOT", tmp_path)
    monkeypatch.setattr(suite, "LOG_ROOT", tmp_path / "logs")
    scenarios = []
    for name in ("timeout-test", "next-test"):
        folder = tmp_path / name
        folder.mkdir()
        scenario = folder / "scenario.yaml"
        scenario.write_text(f"schema: eigrp-uut-test/v1\nname: {name}\n", encoding="utf-8")
        topology = folder / "topology.yaml"
        topology.write_text("schema: eigrp-uut-topology/v1\n", encoding="utf-8")
        scenarios.append((topology, scenario))
    monkeypatch.setattr(suite, "discover", lambda: scenarios)
    calls = []

    def fake_execute(command, timeout):
        calls.append(command)
        if len(calls) == 1:
            return 124, "partial diagnostic before hang\n", True
        return 0, "PASS: next-test\n", False

    monkeypatch.setattr(suite, "execute_scenario", fake_execute)
    assert suite.main(["--scenario-timeout", "4"]) == 1
    assert len(calls) == 2
    failed_log = (tmp_path / "logs" / "timeout-test.log").read_text()
    assert "TIMEOUT: scenario exceeded 4s" in failed_log
    assert "partial diagnostic before hang" in failed_log
    assert "exit status: 124" in failed_log
    passed_log = (tmp_path / "logs" / "next-test.log").read_text()
    assert "exit status: 0" in passed_log
