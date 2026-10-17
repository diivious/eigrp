# SPDX-License-Identifier: ISC
"""Compile and execute TRANSMIT's deferred multicast ordering helpers."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"


def test_rtp_deferred_multicast_ordering():
    source = Path(__file__).with_suffix(".c")
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DEIGRP_DISABLE_SNMP",
             "-pthread", "-ffunction-sections", "-fdata-sections", f"-I{CODE}"]
    gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    with tempfile.TemporaryDirectory() as folder:
        binary = Path(folder) / "rtp_order"
        subprocess.run(["cc", *flags, str(source), gc_flag, "-o", str(binary)],
                       check=True)
        subprocess.run([str(binary)], check=True)
