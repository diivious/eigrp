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
