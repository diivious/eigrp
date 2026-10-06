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


def test_packet_consumer_uses_stream_bound_validator_before_header_access():
    packet=(ROOT/'eigrp'/'code'/'eigrp_packet.c').read_text()
    start=packet.index('static void eigrp_packet_input_process')
    end=packet.index('static void eigrp_instance_message_complete', start)
    body=packet[start:end]
    assert 'endp = eigrp_stream_get_endp(ibuf);' in body
    assert 'eigrp_packet_input_bounds_validate(endp, meta.network_header_length,' in body
    assert body.index('eigrp_packet_input_bounds_validate') < body.index('eigrph = (struct eigrp_header *)eigrp_stream_pnt(ibuf);')


def test_hello_prevalidation_requires_parameter_before_neighbor_allocation():
    hello = (ROOT / "eigrp" / "code" / "eigrp_hello.c").read_text()
    validator = hello[hello.index("static bool eigrp_hello_tlvs_validate"):hello.index("void eigrp_hello_receive")]
    receive = hello[hello.index("void eigrp_hello_receive"):hello.index("static uint8_t eigrp_host_major")]

    assert "bool parameter_seen = false;" in validator
    assert "parameter_seen = true;" in validator
    assert "return parameter_seen;" in validator
    assert receive.index("eigrp_hello_tlvs_validate") < receive.index("eigrp_nbr_create")


def test_instance_condition_wait_keeps_clock_domains_consistent():
    structs = (ROOT / "eigrp" / "code" / "eigrp_structs.h").read_text()
    instance = (ROOT / "eigrp" / "code" / "eigrp_instance.c").read_text()
    packet = (ROOT / "eigrp" / "code" / "eigrp_packet.c").read_text()

    assert "work_cond_monotonic" not in structs
    assert "!defined(__APPLE__)" in instance
    assert "pthread_condattr_setclock(&attr, CLOCK_MONOTONIC)" in instance
    assert "!defined(__APPLE__)" in instance
    assert "#if defined(__APPLE__) || !defined(CLOCK_MONOTONIC)" in packet
    assert "eigrp_instance_wait_deadline" in packet
    assert "clock_gettime(CLOCK_REALTIME, deadline)" in packet
