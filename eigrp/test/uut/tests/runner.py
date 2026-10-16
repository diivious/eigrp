from pathlib import Path
import subprocess, sys

def test_live_runner_smoke():
    uut=Path(__file__).resolve().parents[1]
    result=subprocess.run([sys.executable,str(uut/"run.py"),str(uut/"examples/two-router.yaml"),str(uut/"examples/two-router-neighbor-route.yaml")],text=True,capture_output=True,timeout=25)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS: two-router-framework-smoke" in result.stdout


def test_neighbor_up_requires_actual_up_state():
    import importlib.util
    spec = importlib.util.spec_from_file_location("eigrp_uut_run", Path(__file__).resolve().parents[1] / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    runner = module.Runner({}, {}, Path("."))

    class FakeNode:
        def __init__(self, state):
            self.neighbor_state = state

        def state(self):
            return {"neighbors": [{"address": "10.15.0.5", "interface": "eth15",
                                    "state": self.neighbor_state, "reliable_q": 0}]}

    peer = FakeNode("Pending")
    runner.nodes["r1"] = peer
    check = {"uut": "r1", "neighbor": "10.15.0.5", "interface": "eth15"}
    assert not runner.neighbor_ok(check)
    assert runner.neighbor_ok({**check, "state": "present"})
    peer.neighbor_state = "Up"
    assert runner.neighbor_ok(check)
    assert runner.neighbor_ok({**check, "state": "up", "reliable_q": 0})
    assert not runner.neighbor_ok({**check, "state": "down"})


def test_neighbor_absence_fails_while_peer_is_up():
    """Regression: SIA teardown must not pass while the target peer still exists."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("eigrp_uut_run", Path(__file__).resolve().parents[1] / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    runner = module.Runner({}, {}, Path("."))

    class FakeNode:
        neighbors = [{"address": "10.23.0.2", "interface": "eth32", "state": "Up"}]

        def state(self):
            return {"neighbors": self.neighbors}

    peer = FakeNode()
    runner.nodes["r3"] = peer
    absence = {"uut": "r3", "neighbor": "10.23.0.2", "interface": "eth32", "state": "down"}
    assert not runner.neighbor_ok(absence)
    peer.neighbors = [{"address": "10.23.0.2", "interface": "eth32", "state": "Pending"}]
    assert not runner.neighbor_ok(absence)
    peer.neighbors = []
    assert runner.neighbor_ok(absence)


def test_sia_failure_requires_absence_and_timeout_reason():
    import importlib.util
    spec = importlib.util.spec_from_file_location("eigrp_uut_run", Path(__file__).resolve().parents[1] / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    scenario = module.validate_scenario(module.load_yaml(Path(__file__).resolve().parents[1]
                                                         / "active-topology/scenario-sia-failure.yaml"))
    steps = scenario["steps"]
    assert any("validate-neighbor" in step and
               step["validate-neighbor"].get("state") == "down" and
               step["validate-neighbor"]["uut"] == "r3" and
               step["validate-neighbor"]["neighbor"] == "10.23.0.2"
               for step in steps)
    events = [step["validate-event"]["contains"] for step in steps if "validate-event" in step]
    assert any("SIA active timer expired: 10.4.0.0/24" in event for event in events)
    assert any("Peer 10.23.0.2/32: UP -> DOWN" in event and
               "reason stuck in active" in event for event in events)


def test_sia_teardown_event_must_follow_timer_expiry():
    import importlib.util
    spec = importlib.util.spec_from_file_location("eigrp_uut_run", Path(__file__).resolve().parents[1] / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    runner = module.Runner({}, {}, Path("."))

    class FakeNode:
        def __init__(self, events):
            self.events = events

        def cmd(self, command, until=None, timeout=3):
            assert command == "EVENTS"
            return self.events

    expiry = "SIA active timer expired: 10.4.0.0/24 ifindex 4 queries 1"
    removal = "Peer 10.23.0.2/32: UP -> DOWN ifindex 4 reason stuck in active"
    check = {"uut": "r3", "contains": removal, "after": expiry}
    runner.nodes["r3"] = FakeNode([removal, expiry])  # newest first
    assert runner.event_ok(check)
    runner.nodes["r3"].events = [expiry, removal]  # wrong temporal order
    assert not runner.event_ok(check)
    runner.nodes["r3"].events = [removal]  # missing timeout evidence
    assert not runner.event_ok(check)


def test_live_neighbor_absence_must_fail_when_adjacency_is_up(tmp_path):
    """Negative-control UUT: an established adjacency cannot satisfy 'down'."""
    import yaml
    uut = Path(__file__).resolve().parents[1]
    scenario = {
        "schema": "eigrp-uut-test/v1", "topology": "two-router.yaml",
        "name": "negative-control-neighbor-absence",
        "steps": [
            {"start": "all"},
            {"validate-neighbor": {"uut": "r1", "neighbor": "10.0.0.2",
                                   "interface": "eth0", "state": "up", "timeout": 7}},
            {"validate-neighbor": {"uut": "r1", "neighbor": "10.0.0.2",
                                   "interface": "eth0", "state": "down", "timeout": 0.5}},
        ],
    }
    path = tmp_path / "negative-control.yaml"
    path.write_text(yaml.safe_dump(scenario), encoding="utf-8")
    result = subprocess.run([sys.executable, str(uut / "run.py"),
                             str(uut / "examples/two-router.yaml"), str(path)],
                            text=True, capture_output=True, timeout=20)
    assert result.returncode != 0, result.stdout + result.stderr
    assert "step 3 (validate-neighbor) failed" in result.stderr, result.stderr
    assert "validate-neighbor failed:" in result.stderr, result.stderr


def test_packet_delivery_oracle_rejects_a_drop_and_wrong_mark():
    """Regression A2/A3: an RTP drop is not a delivered QUERY/REPLY."""
    import importlib.util
    import pytest
    spec=importlib.util.spec_from_file_location("uut_strict",Path(__file__).resolve().parents[1]/"run.py")
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    runner=module.Runner({}, {}, Path("."))

    class Broker:
        journal=[{"event":"sent","source_uut":"r1","destination_uut":None,
                  "opcode":"reply","prefixes":["10.4.0.0/24"]},
                 {"event":"dropped","source_uut":"r1","destination_uut":"r2",
                  "opcode":"reply","prefixes":["10.4.0.0/24"]}]

    runner.broker=Broker();runner.packet_marks={"loss":0}
    packet={"source_uut":"r1","destination_uut":"r2","opcode":"reply",
            "prefix":"10.4.0.0/24","since":"loss","event":"delivered","min":1}
    assert not runner.packet_ok(packet)
    assert runner.packet_ok({**packet,"event":"dropped"})
    assert runner.packet_ok({**packet,"event":"any","min":0,"max":0,"prefix":"10.5.0.0/24"})
    assert not runner.packet_ok({**packet,"event":"any","min":0,"max":0})
    with pytest.raises(module.SpecError,match="unknown packet mark"):
        runner.packet_ok({**packet,"since":"not-created"})
    with pytest.raises(module.SpecError,match="event is required"):
        runner.packet_ok({k:v for k,v in packet.items() if k!="event"})


def test_route_oracle_rejects_unknown_and_table_incompatible_filters():
    import importlib.util
    import pytest
    spec=importlib.util.spec_from_file_location("uut_strict_route",Path(__file__).resolve().parents[1]/"run.py")
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    runner=module.Runner({}, {}, Path("."))

    class Node:
        def state(self):
            return {"rib":[{"prefix":"10.4.0.0/24","next_hop":"10.1.0.2","metric":7,"distance":90}],
                    "source":[],"paths":[],"topology":[]}

    runner.nodes={"r1":Node()}
    base={"uut":"r1","prefix":"10.4.0.0/24","table":"rib"}
    assert runner.route_ok(base)
    for bad in ({"next_hops":"WRONG"},{"table":"bogus"},{"rd":0}):
        with pytest.raises(module.SpecError):runner.route_ok({**base,**bad})


def test_retransmission_and_retry_oracles_reject_unknown_phase_marks():
    import importlib.util
    import pytest
    spec=importlib.util.spec_from_file_location("uut_marks",Path(__file__).resolve().parents[1]/"run.py")
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    runner=module.Runner({}, {}, Path("."))

    class Broker:
        journal=[{"event":"sent","source_uut":"r1","opcode":"update","sequence":7}]

    runner.broker=Broker();runner.packet_marks={"established":0}
    for name in ("retransmission_ok", "retry_ok"):
        check=getattr(runner,name)
        with pytest.raises(module.SpecError,match="unknown packet mark"):
            check({"since":"not-a-phase"})


def test_phase_checkpoints_preserve_bounded_events_and_state(tmp_path):
    """Older event rings must remain available after a later ring overwrite."""
    import importlib.util
    spec=importlib.util.spec_from_file_location("uut_checkpoint", Path(__file__).resolve().parents[1]/"run.py")
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    runner=m.Runner({}, {}, tmp_path)
    class Node:
        stderr=[]
        p=type("Process", (), {"poll":lambda self:None})()
        events=["EVENTLOG|capacity=2|count=2", "event-before-bounce", "older-event"]
        state=["NBR|address=10.0.0.2|state=Up"]
        def cmd(self, command, until=None, timeout=3):
            return list(self.events if command=="EVENTS" else self.state)
    class Broker:
        journal=[{"event":"sent","opcode":"query","sequence":3}]
    node=Node();runner.nodes={"r1":node};runner.broker=Broker()
    runner.checkpoint("before-bounce")
    node.events=["EVENTLOG|capacity=2|count=2", "event-after-bounce", "new-event"]
    node.state=["NBR|address=10.0.0.2|state=Down"]
    runner.broker.journal.append({"event":"delivered","opcode":"reply","sequence":4})
    runner.checkpoint("after-bounce")
    report=runner.diagnostics()
    assert "event-before-bounce" in report and "event-after-bounce" in report
    assert "NBR|address=10.0.0.2|state=Up" in report
    assert "NBR|address=10.0.0.2|state=Down" in report
    assert report.count("event-before-bounce")==1
    assert "broker journal [0:1]" in report and "broker journal [1:2]" in report


def test_stable_convergence_rejects_transient_satisfaction():
    """A one-sample convergence success must not qualify a flapping prefix."""
    import importlib.util
    import pytest
    spec=importlib.util.spec_from_file_location("uut_stable", Path(__file__).resolve().parents[1]/"run.py")
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    runner=m.Runner({}, {}, Path("."))
    checks=[True,True,False,True,True,False,True,False]
    def flapping(_):
        return checks.pop(0) if checks else False
    runner.route_ok=flapping
    check={"timeout":0.7,"stable_for":0.4,"expect":[{"route":{"uut":"r1","prefix":"10.0.0.0/24"}}]}
    with pytest.raises(AssertionError,match="stable_for=0.4"):
        runner.step("validate-convergence",check)
    runner.route_ok=lambda e:True
    runner.step("validate-convergence", {**check,"timeout":0.7,"stable_for":0.2})


def test_report_packet_marks_automatically_capture_phase_snapshots(tmp_path):
    import importlib.util
    spec=importlib.util.spec_from_file_location("uut_auto_phase",Path(__file__).resolve().parents[1]/"run.py")
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    runner=m.Runner({}, {}, tmp_path, report=True)
    class Broker:
        journal=[{"event":"sent","opcode":"query"}]
    runner.broker=Broker()
    calls=[]
    runner.checkpoint=lambda name:calls.append(name)
    runner.step("packet-mark","after-failure")
    assert runner.packet_marks["after-failure"]==1
    assert calls==["packet-mark:after-failure"]
    runner.report=False
    runner.step("packet-mark","during-recovery")
    assert calls==["packet-mark:after-failure"]


def test_recovered_link_send_errors_cannot_pass_negative_packet_check():
    """Unexpected delivery errors on a recovered transit link are test failures."""
    import importlib.util
    spec=importlib.util.spec_from_file_location("uut_errors",Path(__file__).resolve().parents[1]/"run.py")
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    runner=m.Runner({}, {}, Path("."))
    class Broker:
        journal=[{"event":"sent","source_uut":"r1","source_interface":"eth0"},
                 {"event":"send-error","source_uut":"r1","source_interface":"eth0"}]
    runner.broker=Broker();runner.packet_marks={"recovered":0}
    expected={"source_uut":"r1","source_interface":"eth0","since":"recovered",
              "event":"send-error","min":0,"max":0}
    assert not runner.packet_ok(expected)
    runner.broker.journal.pop()
    assert runner.packet_ok(expected)


def test_broker_send_error_is_an_automatic_failure_negative_control():
    import importlib.util, pytest
    spec=importlib.util.spec_from_file_location("uut_guard",Path(__file__).resolve().parents[1]/"run.py")
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    runner=module.Runner({}, {}, Path("."))
    class Journal:
        journal=[{"event":"delivered","opcode":"hello"}]
    runner.broker=Journal()
    runner.unexpected_wire_errors()
    runner.broker.journal.append({"event":"send-error","source_uut":"r1","source_interface":"eth12"})
    with pytest.raises(AssertionError,match="unexpected wire transmit errors"):
        runner.unexpected_wire_errors()


def test_terminal_stop_closes_wire_before_sequential_router_teardown(monkeypatch, tmp_path):
    """A healthy K6 fabric must not fail because STOP closes peers in sequence."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("uut_terminal_shutdown", Path(__file__).resolve().parents[1] / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    order = []

    class FakeBroker:
        error = None
        journal = []
        ready = type("Ready", (), {"wait": lambda self, timeout: True})()
        closed = False

        def __init__(self, *args):
            pass

        def start(self):
            order.append("wire-start")

        def close(self):
            if not self.closed:
                self.closed = True
                order.append("wire-close")

    broker = FakeBroker()

    class FakeNode:
        def __init__(self, name):
            self.name = name

        def stop(self):
            order.append(f"stop-{self.name}")
            if not broker.closed:
                # Reproduce an UPDATE sent to an already stopped peer.
                broker.journal.append({"event": "send-error", "source_uut": "r5",
                                       "destination_uut": "r2", "opcode": "update"})

    monkeypatch.setattr(module, "Broker", lambda *args: broker)
    runner = module.Runner({}, {"steps": [{"stop": "all"}]}, tmp_path)
    runner.build = lambda: None
    runner.configs = lambda: {}
    runner.nodes = {"r2": FakeNode("r2"), "r5": FakeNode("r5")}
    runner.run()
    assert order[:4] == ["wire-start", "wire-close", "stop-r2", "stop-r5"]
    assert not broker.journal


def test_terminal_stop_still_fails_on_real_transmit_errors(monkeypatch, tmp_path):
    """Quiescing the broker must not hide send-errors from the live test."""
    import importlib.util
    import pytest
    spec = importlib.util.spec_from_file_location("uut_terminal_negative", Path(__file__).resolve().parents[1] / "run.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    order = []

    class FakeBroker:
        error = None
        ready = type("Ready", (), {"wait": lambda self, timeout: True})()
        journal = [{"event": "send-error", "source_uut": "r5",
                    "destination_uut": "r2", "opcode": "update"}]

        def __init__(self, *args):
            pass

        def start(self):
            pass

        def close(self):
            order.append("wire-close")

    class FakeNode:
        def stop(self):
            order.append("router-stop")

    monkeypatch.setattr(module, "Broker", FakeBroker)
    runner = module.Runner({}, {"steps": [{"stop": "all"}]}, tmp_path)
    runner.build = lambda: None
    runner.configs = lambda: {}
    runner.nodes = {"r2": FakeNode()}
    runner.diagnostics = lambda: "<negative-control diagnostics>"
    with pytest.raises(RuntimeError, match="unexpected wire transmit errors"):
        runner.run()
    assert order.index("wire-close") < order.index("router-stop")
