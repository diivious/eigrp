from pathlib import Path
import subprocess, sys

def test_live_runner_smoke():
    uut=Path(__file__).resolve().parents[1]
    result=subprocess.run([sys.executable,str(uut/"run.py"),str(uut/"examples/two-router.yaml"),str(uut/"examples/two-router-neighbor-route.yaml")],text=True,capture_output=True,timeout=25)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS: two-router-framework-smoke" in result.stdout
