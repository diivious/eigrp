# SPDX-License-Identifier: ISC
"""Fast, deterministic checks for heavy stress-driver controls and topology.

These tests do not replace running the actual EIGRP UUT stress scenarios.
"""
from __future__ import annotations

from pathlib import Path
import importlib.util
import threading
import struct
from unittest.mock import Mock

import pytest

# Collect by explicit sibling path: pytest importlib mode does not place this
# directory on sys.path, while `make scale-*` invokes the driver as a script.
_scale_spec = importlib.util.spec_from_file_location(
    "eigrp_scale_stress_extended", Path(__file__).with_name("stress_extended.py"))
if _scale_spec is None or _scale_spec.loader is None:
    raise ImportError("unable to load scale stress driver")
scale = importlib.util.module_from_spec(_scale_spec)
_scale_spec.loader.exec_module(scale)
base = scale.base


def test_source_prefixes_stay_in_disjoint_test_space():
    assert scale.prefix(0) == "100.0.0.0/32"
    assert scale.prefix(99999) == "100.1.134.159/32"
    with pytest.raises(ValueError):
        scale.prefix(1 << 24)


def test_md5_configuration_applies_authentication_before_interface_up():
    config = scale.router_config("r1", "10.41.0.1", source_count=3, md5=True)
    assert config.count("\nsource ") == 3
    assert config.count("auth-md5 eth0 scale-test-keychain scale-test-only-secret") == 1
    assert config.index("auth-md5 ") < config.index("up eth0")
    assert config.index("redistribute static") < config.index("up eth0")


def test_plain_route_source_does_not_enable_auth():
    config = scale.router_config("r2", "10.41.0.2", source_count=0)
    assert "auth-md5" not in config
    assert "redistribute static" not in config
    assert "source " not in config


def test_mixed_tlv_versions_are_distinct():
    legacy = struct.pack("!HHBBBB", 4, 8, 1, 0, 1, 2)
    modern = struct.pack("!HHBBBB", 4, 8, 1, 0, 2, 2)
    assert legacy != modern
    assert base.checksum(base.packet(5, tlvs=base.PARAMETER + legacy)) == 0
    assert base.checksum(base.packet(5, tlvs=base.PARAMETER + modern)) == 0


def test_uut_command_checks_real_result():
    node = Mock()
    node.name = "r1"
    node.control_lock = threading.Lock()
    node._response.return_value = "RESULT|0"
    scale.command(node, "SOURCE-DEL 100.0.0.0/32")
    node.proc.stdin.write.assert_any_call("SOURCE-DEL 100.0.0.0/32\n")
    node._response.return_value = "RESULT|5"
    with pytest.raises(AssertionError, match="RESULT\\|5"):
        scale.command(node, "SOURCE-DEL 100.0.0.0/32")


def test_mcast_hold_and_release_ack_buffer():
    link = base.SyntheticLink(Path("/unused"), "inject", {"10.0.0.2"}, broker=False)
    link.hold_peer_acks(["10.0.0.2"])
    with link.cv:
        link.held_acks.append(("10.0.0.2", 12))
    assert link.release_held_acks() == 1
    assert link.ack_jobs.get_nowait() == ("10.0.0.2", 12)
    assert not link.held_peers


def test_source_routes_use_host_rib_not_same_interface_reinjection(monkeypatch):
    sent = []
    monkeypatch.setattr(scale, "command", lambda node, text: sent.append(text))
    scale.source_routes_add(object(), 0, 3)
    assert sent == [f"SOURCE-ADD {scale.prefix(i)} 1" for i in range(3)]
    # EIGRP split horizon prevents same-interface UPDATE reflection. The real
    # host RIB must be the source for multicast backpressure qualification.
    assert all(text.startswith("SOURCE-ADD ") for text in sent)


def test_mcast_cr_capacity_selects_cr_or_full_backoff():
    assert scale.mcast_cr_exclusions_fit(8)
    assert scale.mcast_cr_exclusions_fit(289)
    assert not scale.mcast_cr_exclusions_fit(290)
    assert not scale.mcast_cr_exclusions_fit(300)
