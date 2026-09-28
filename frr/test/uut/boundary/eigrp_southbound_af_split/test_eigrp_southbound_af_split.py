# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrpd" / "code").is_dir())


def read(path: str) -> str:
    return (ROOT / path).read_text()


def test_frr_southbound_is_split_by_address_family():
    common = read("frr/code/eigrp_southbound.c")
    ipv4 = read("frr/code/eigrp_southbound_ipv4.c")
    ipv6 = read("frr/code/eigrp_southbound_ipv6.c")
    subdir = read("frr/code/subdir.am")

    assert "int eigrp_sys_packet_send(" in common
    assert "bool eigrp_sys_packet_receive(" in common
    assert "eigrp_sys_ipv4_packet_send(" not in common.split("int eigrp_sys_packet_send(")[0]
    assert "int eigrp_sys_ipv4_packet_send(" in ipv4
    assert "IP_PKTINFO" in ipv4
    assert "int eigrp_sys_ipv6_packet_send(" in ipv6
    assert "IPV6_PKTINFO" in ipv6
    assert "eigrpd/eigrp_southbound_ipv4.c" in subdir
    assert "eigrpd/eigrp_southbound_ipv6.c" in subdir


def test_southbound_private_declarations_use_public_opaque_types():
    internal = read("frr/code/eigrp_southbound_internal.h")

    assert '#include "eigrp.h"' in internal
    assert '#include "eigrpd/code/eigrpd.h"' not in internal
    assert '#include "eigrpd/code/eigrp_interface.h"' not in internal
    assert '#include "eigrpd/code/eigrp_sys.h"' not in internal
