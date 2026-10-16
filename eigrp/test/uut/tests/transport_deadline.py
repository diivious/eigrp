# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Donnie V. Savage
"""Timed command transport regressions for the portable UUT harness."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import time

import pytest


def uut_module():
    path = Path(__file__).resolve().parents[1] / "run.py"
    spec = importlib.util.spec_from_file_location("uut_transport_deadline", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def child_node(module, source):
    node = module.Node("r1", None, None)
    node.p = subprocess.Popen([sys.executable, "-u", "-c", "print('READY', flush=True)\n" + source],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, bufsize=1)
    node._start_stdout_reader()
    assert node._readline(5) == "READY"
    return node


def cleanup(node):
    if node.p.poll() is None:
        node.p.kill()
    node.p.communicate(timeout=3)


def test_multiline_command_reads_textio_buffer_without_spurious_timeout():
    module = uut_module()
    node = child_node(module, "import sys\n"
                              "for line in sys.stdin:\n"
                              " for i in range(250): print('ROW|%d' % i)\n"
                              " print('END', flush=True)\n")
    try:
        rows = node.cmd("STATE", until="END", timeout=3)
        assert len(rows) == 250
        assert rows[0] == "ROW|0"
        assert rows[-1] == "ROW|249"
    finally:
        cleanup(node)


def test_multiline_command_times_out_after_first_line():
    """Regression: an incomplete STATE cannot block indefinitely at readline()."""
    module = uut_module()
    node = child_node(module, "import sys,time\n"
                              "for line in sys.stdin:\n"
                              " print('ROW|first', flush=True)\n"
                              " time.sleep(3)\n"
                              " print('END', flush=True)\n")
    try:
        started = time.monotonic()
        with pytest.raises(TimeoutError, match="response timeout"):
            node.cmd("STATE", until="END", timeout=.3)
        assert time.monotonic() - started < 2
    finally:
        cleanup(node)


def test_multiline_command_reports_eof_before_end():
    module = uut_module()
    node = child_node(module, "import sys\n"
                              "sys.stdin.readline()\n"
                              "print('ROW|first', flush=True)\n")
    try:
        with pytest.raises(RuntimeError, match="closed command output"):
            node.cmd("STATE", until="END", timeout=3)
    finally:
        cleanup(node)


def test_single_line_command_times_out_with_no_response():
    module = uut_module()
    node = child_node(module, "import sys,time\n"
                              "sys.stdin.readline()\n"
                              "time.sleep(3)\n")
    try:
        with pytest.raises(TimeoutError, match="response timeout"):
            node.cmd("UP eth0", timeout=.3)
    finally:
        cleanup(node)
