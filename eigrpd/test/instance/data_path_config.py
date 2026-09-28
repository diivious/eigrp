# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import re

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())
INSTANCE = ROOT / "eigrpd" / "code" / "eigrp_instance.c"
CLI = ROOT / "eigrpd" / "code" / "eigrp_cli.h"


def test_af_create_exposes_explicit_runtime_capability_target():
    header = CLI.read_text()
    source = INSTANCE.read_text()
    assert "eigrp_af_instance_create_with_data_path" in header
    assert re.search(
        r"eigrp_af_instance_runtime_create\(name, af, data_path_ready\)", source
    )
    assert re.search(
        r"eigrp_instance_lookup_or_create_by_af\([^;]+data_path_ready\)",
        source,
        re.DOTALL,
    )


def test_existing_full_data_path_create_keeps_its_semantics():
    source = INSTANCE.read_text()
    assert re.search(
        r"eigrp_af_instance_create\([^)]*\)\s*\{\s*return "
        r"eigrp_af_instance_create_with_data_path\([^;]+true\);",
        source,
        re.DOTALL,
    )
