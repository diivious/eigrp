# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"


def compile_and_run(source: str) -> None:
    linker_gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "test.c"
        exe = Path(tmp) / "test"
        src.write_text(source)
        subprocess.run(
            [
                "cc",
                "-std=c11",
                "-D_POSIX_C_SOURCE=200809L",
                "-DEIGRP_DISABLE_SNMP",
                "-ffunction-sections",
                "-fdata-sections",
                f"-I{CODE}",
                str(src),
                linker_gc,
                "-pthread",
                "-o",
                str(exe),
            ],
            check=True,
            cwd=ROOT,
        )
        subprocess.run([str(exe)], check=True, cwd=ROOT)


def test_auth_sequence_replay_and_wrap_rules():
    compile_and_run(
        r'''
#include <arpa/inet.h>
#include <stdint.h>
#include "eigrp_auth.c"

int main(void)
{
    if (eigrp_auth_sequence_newer(htonl(7), htonl(7)))
        return 1;
    if (!eigrp_auth_sequence_newer(htonl(8), htonl(7)))
        return 2;
    if (eigrp_auth_sequence_newer(htonl(6), htonl(7)))
        return 3;
    if (!eigrp_auth_sequence_newer(htonl(0), htonl(0xffffffffU)))
        return 4;
    if (eigrp_auth_sequence_newer(htonl(0xffffffffU), htonl(0)))
        return 5;
    return 0;
}
'''
    )


def test_zero_hold_parameter_tlv_is_rejected_before_neighbor_allocation():
    compile_and_run(
        r'''
#include <stdint.h>
#include "eigrp_hello.c"

int main(void)
{
    uint8_t zero_hold[12] = {
        0x00, 0x01, 0x00, 0x0c,
        0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00
    };
    uint8_t valid_hold[12] = {
        0x00, 0x01, 0x00, 0x0c,
        0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x0f
    };

    if (eigrp_hello_tlvs_validate(NULL, zero_hold, sizeof(zero_hold)))
        return 1;
    if (!eigrp_hello_tlvs_validate(NULL, valid_hold, sizeof(valid_hold)))
        return 2;
    return 0;
}
'''
    )
