# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for DUAL event-log coverage and timestamp formatting.

from pathlib import Path
import re


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
FSM = ROOT / "eigrp" / "code" / "eigrp_fsm.c"
EVENTLOG = ROOT / "eigrp" / "code" / "eigrp_eventlog.c"
SYS_H = ROOT / "eigrp" / "code" / "eigrp_sys.h"
UNIX_SYS = ROOT / "unix" / "code" / "eigrp_unix_sys.c"
FRR_SB = ROOT / "frr" / "code" / "eigrp_southbound.c"
UUT_NODE = ROOT / "eigrp" / "test" / "uut" / "node.c"


def read(path: Path) -> str:
    return path.read_text()


def test_event_log_uses_wall_clock_milliseconds():
    eventlog = read(EVENTLOG)
    assert "entry->timestamp = eigrp_sys_wallclock_msec();" in eventlog
    assert "entry->timestamp = eigrp_sys_monotime_msec();" not in eventlog
    assert "uint64_t eigrp_sys_wallclock_msec(void);" in read(SYS_H)
    assert "CLOCK_REALTIME" in read(UNIX_SYS)
    assert "uint64_t eigrp_sys_wallclock_msec(void)" in read(FRR_SB)


def test_event_log_preamble_and_dual_text_match_operational_format():
    eventlog = read(EVENTLOG)
    uut = read(UUT_NODE)
    assert '"%H:%M:%S"' in eventlog
    assert '"%s.%03u %s"' in eventlog
    assert '"DUAL state change %s Old: %s New: %s"' in eventlog
    assert 'printf("%u  %s\\n",n,b);' in uut
    assert "EVENT|number=%u|message=%s" not in uut


def test_every_dual_state_assignment_is_immediately_logged():
    source = read(FSM)
    assignments = list(re.finditer(r"(?:prefix|msg->prefix)->state\s*=(?!=)", source))
    assert len(assignments) == 7

    for assignment in assignments:
        tail = source[assignment.end(): assignment.end() + 420]
        assert "EIGRP_EVENTLOG_OPCODE_DUAL_STATE_CHANGE" in tail


def test_dual_state_log_uses_family_neutral_destination():
    source = read(FSM)
    eventlog = read(EVENTLOG)
    calls = re.findall(
        r"eigrp_eventlog_msg_add\([^;]+EIGRP_EVENTLOG_OPCODE_DUAL_STATE_CHANGE,[^;]+\);",
        source,
        flags=re.DOTALL,
    )
    assert len(calls) == 7
    for call in calls:
        assert "->destination" in call
    assert "eigrp_prefix_snprintf(addr, sizeof(addr), &entry->addr);" in eventlog
