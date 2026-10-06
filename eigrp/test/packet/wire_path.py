# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
from pathlib import Path
ROOT=next(p for p in Path(__file__).resolve().parents if (p/'eigrp'/'code').is_dir())
def test_both_address_families_use_normal_host_packet_boundary():
    for name in ('eigrp_ipv4.c','eigrp_ipv6.c'):
        text=(ROOT/'eigrp'/'code'/name).read_text()
        assert 'eigrp_sys_packet_send(' in text
        assert 'eigrp_sys_packet_receive(' in text
def test_receive_path_keeps_eigrp_validation_above_host_transport():
    packet=(ROOT/'eigrp'/'code'/'eigrp_packet.c').read_text()
    assert 'eigrp_packet_header_validate(ei, &src, eigrph, length)' in packet
    assert 'eigrp_hello_receive(eigrp, eigrph, &src, ei' in packet
