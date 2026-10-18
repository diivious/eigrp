# SPDX-License-Identifier: ISC
"""Execute RFC 7868 4.3 unknown-QUERY topology admission regression."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"


def test_unknown_query_topology():
    with tempfile.TemporaryDirectory() as folder:
        binary = Path(folder) / "unknown_query"
        flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DEIGRP_DISABLE_SNMP",
                 "-O0", "-pthread", "-ffunction-sections", "-fdata-sections", f"-I{CODE}"]
        gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
        subprocess.run(["cc", *flags, str(Path(__file__).with_suffix(".c")),
                        *(str(CODE / f) for f in ["eigrp_topology.c", "eigrp_neighbor.c", "eigrp_metric.c", "eigrp_table.c", "eigrp_list.c", "eigrp_prefix.c", "eigrp_eventlog.c", "eigrp_instance.c", "eigrp_log.c"]),
                        gc_flag,
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
