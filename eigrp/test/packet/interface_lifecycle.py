# SPDX-License-Identifier: ISC
"""Exercise real TRANSMIT goodbye and output-purge code in a small C fixture."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = next(p for p in Path(__file__).resolve().parents
            if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"


def test_interface_lifecycle_transmit():
    source = Path(__file__).with_suffix(".c")
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DEIGRP_DISABLE_SNMP",
             "-pthread", "-ffunction-sections", "-fdata-sections", f"-I{CODE}"]
    gc_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    with tempfile.TemporaryDirectory() as folder:
        binary = Path(folder) / "interface_lifecycle"
        subprocess.run(["cc", *flags, str(source), str(CODE / "eigrp_stream.c"),
                        str(CODE / "eigrp_list.c"), gc_flag, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


def test_shutdown_is_sent_before_full_interface_queue_purge():
    hello = (CODE / "eigrp_hello.c").read_text()
    interface = (CODE / "eigrp_interface.c").read_text()
    packet = (CODE / "eigrp_packet.c").read_text()

    sender = hello[hello.index("void eigrp_hello_send(eigrp_intf_t *ei,"):
                   hello.index("bool eigrp_hello_send_sequence(")]
    assert "flags & EIGRP_HELLO_GRACEFUL_SHUTDOWN" in sender
    assert "eigrp_packet_goodbye_send(ei, packet);" in sender

    teardown = interface[interface.index("int eigrp_intf_down(eigrp_intf_t *ei)"):
                         interface.index("bool eigrp_intf_is_passive(")]
    assert "eigrp_packet_interface_output_clear(ei);" in teardown
    assert teardown.index("eigrp_packet_interface_output_clear(ei);") < teardown.index("eigrp_nbr_delete(nbr);")

    free = interface[interface.index("void eigrp_intf_free("):
                     interface.index("eigrp_intf_t *eigrp_intf_lookup_by_local_addr(")]
    assert free.index("eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);") < free.index("eigrp_intf_down(ei);")
    assert "eigrp_packet_queue_clear(ei->obuf);" in packet
