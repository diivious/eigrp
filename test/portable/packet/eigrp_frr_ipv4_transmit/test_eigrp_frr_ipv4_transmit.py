from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def function_body(source: str, name: str) -> str:
    marker = f"{name}("
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    raise AssertionError(f"unterminated function {name}")


def test_transmit_path_preserves_selected_interface_to_host_send():
    packet = read("eigrpd/eigrp_packet.c")
    ipv4 = read("eigrpd/eigrp_ipv4.c")

    write = function_body(packet, "eigrp_packet_write")
    send = function_body(ipv4, "eigrp_ipv4_packet_send")

    assert "eigrp->af_vectors.packet_send(eigrp, ei, packet)" in write
    assert "eigrp_sys_ipv4_packet_send(" in send
    assert "eigrp, ei, &destination" in send


def test_frr_ipv4_send_pins_each_packet_to_selected_ifindex():
    southbound = read("frr/eigrp_southbound.c")
    send = function_body(southbound, "eigrp_sys_ipv4_packet_send")

    assert "ifindex = eigrp_interface_ifindex(ei);" in send
    assert "if (!ifindex)" in send
    assert "IP_PKTINFO" in send
    assert "pktinfo->ipi_ifindex = ifindex;" in send
    assert "pktinfo->ipi_spec_dst" in send
    assert "msg.msg_control = control;" in send
    assert "sendmsg(socket->fd, &msg, flags)" in send


def test_multicast_send_does_not_ignore_interface_selection_failure():
    southbound = read("frr/eigrp_southbound.c")
    send = function_body(southbound, "eigrp_sys_ipv4_packet_send")

    assert "eigrp_sys_multicast_interface_set(eigrp, ei) < 0" in send
    assert "(void)eigrp_sys_multicast_interface_set(eigrp, ei)" not in send


def test_packet_debug_distinguishes_failed_send_from_send_attempt():
    debug = read("eigrpd/eigrp_debug.c")
    send_debug = function_body(debug, "eigrp_debug_packet_send")

    assert "if (send_result < 0)" in send_debug
    assert "EIGRP: Send failed" in send_debug
