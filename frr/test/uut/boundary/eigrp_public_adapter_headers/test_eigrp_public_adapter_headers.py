# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "frr" / "code").is_dir())
FRR = ROOT / "frr" / "code"


def test_small_frr_boundary_headers_do_not_pull_private_portable_headers():
    forbidden = (
        '"eigrp_types.h"', '"eigrp_structs.h"', '"eigrpd.h"',
        '"eigrp_neighbor.h"', '"eigrp_interface.h"', '"eigrp_topology.h"',
    )
    for name in ("eigrp_frr.h", "eigrp_northbound.h", "eigrp_frr_policy.h"):
        text = (FRR / name).read_text()
        for token in forbidden:
            assert token not in text, f"{name} still includes {token}"
