#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Regression tests for the bounded 2048-peer operational snapshot reader."""

import importlib.util
import ipaddress
import queue
from pathlib import Path
import unittest
from unittest.mock import Mock

path = Path(__file__).with_name("neighbor_2048.py")
spec = importlib.util.spec_from_file_location("eigrp_neighbor_2048", path)
scale = importlib.util.module_from_spec(spec)
spec.loader.exec_module(scale)
PEERS, R1 = scale.PEERS, scale.R1


class SnapshotTests(unittest.TestCase):
    @staticmethod
    def reader(lines):
        node = R1.__new__(R1)
        node.proc = Mock()
        node.proc.poll.return_value = None
        node.errors = []
        node.responses = queue.Queue()
        for line in lines:
            node.responses.put(line)
        return node

    def test_full_neighbor_and_accounting_snapshot(self):
        # STATE emits both NBR and ACCOUNT for every runtime neighbor.
        addresses = [str(ipaddress.IPv4Address(0x0A000002 + index))
                     for index in range(PEERS)]
        lines = ["STATE|instance=r1|asn=4453",
                 f"INTF|name=eth0|peer_count={PEERS}|output_queue=0|reliable_queue=0"]
        lines.extend(f"NBR|address={address}|state=UP" for address in addresses)
        lines.extend(f"ACCOUNT|address={address}|status=A|restart_count=0"
                     for address in addresses)
        lines.extend(("TOPO|prefix=10.0.0.0/16", "TRAFFIC|sent_hello=1", "END"))
        node = self.reader(lines)

        visible, up, queues = node.snapshot(timeout=0.01)
        self.assertEqual(visible, set(addresses))
        self.assertEqual(up, set(addresses))
        self.assertEqual(queues, (PEERS, 0, 0))
        self.assertTrue(node.responses.empty())
        node.proc.stdin.write.assert_called_once_with("STATE\n")

    def test_unterminated_snapshot_remains_bounded(self):
        node = self.reader(["STATE|instance=r1|asn=4453"] * (PEERS * 2 + 200))
        with self.assertRaisesRegex(AssertionError, "STATE did not terminate"):
            node.snapshot(timeout=0.01)


if __name__ == "__main__":
    unittest.main()
