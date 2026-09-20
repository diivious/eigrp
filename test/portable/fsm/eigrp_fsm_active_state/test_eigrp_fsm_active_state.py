# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for RFC 7868 DUAL ACTIVE-state invariants.

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[4]
FSM = ROOT / "eigrpd" / "eigrp_fsm.c"
SPEC = ROOT / "specs" / "design-spec.md"

ACTIVE_STATE_FUNCTIONS = (
    "eigrp_fsm_event_nq_fcn",
    "eigrp_fsm_event_q_fcn",
    "eigrp_fsm_event_dinc",
    "eigrp_fsm_event_lr_fcn",
    "eigrp_fsm_event_qact",
)

FROZEN_DESTINATION_FIELDS = (
    "fdistance",
    "rdistance",
    "distance",
    "reported_metric",
)


def read(path: Path) -> str:
    return path.read_text()


def function_body(source: str, name: str) -> str:
    match = re.search(rf"(?:^|\n)(?:static\s+[^\n]+\n)?int\s+{name}\(", source)
    if not match:
        match = re.search(rf"(?:^|\n)static\s+enum\s+[^\n]+\n{name}\(", source)
    assert match, f"missing function {name}"

    start = match.start()
    next_function = re.search(r"\n(?:static\s+[^\n]+\n)?int\s+\w+\(|\nstatic\s+enum\s+[^\n]+\n\w+\(", source[start + 1 :])
    if not next_function:
        return source[start:]
    return source[start : start + 1 + next_function.start()]


def assignment_to(field: str) -> re.Pattern[str]:
    return re.compile(rf"(?:prefix|msg->prefix)->{field}\s*=")


def test_active_state_handlers_do_not_rewrite_destination_fd_rd_or_metric():
    source = read(FSM)

    for function in ACTIVE_STATE_FUNCTIONS:
        body = function_body(source, function)
        for field in FROZEN_DESTINATION_FIELDS:
            assert not assignment_to(field).search(body), f"{function} rewrites {field} while ACTIVE"


def test_active_state_handlers_do_not_recompute_successor_list_to_adopt_metric():
    source = read(FSM)

    for function in ACTIVE_STATE_FUNCTIONS:
        body = function_body(source, function)
        assert "eigrp_topology_get_successor" not in body
        assert "listnode_head(successors)" not in body


def test_route_metric_updates_are_still_recorded_before_fsm_event_selection():
    body = function_body(read(FSM), "eigrp_get_fsm_event")

    assert "change = eigrp_topology_update_distance(msg);" in body
    assert "msg->change = change;" in body


def test_design_spec_documents_active_state_freeze_rule():
    spec = read(SPEC)
    spec_words = " ".join(spec.split())

    assert "DUAL FSM Active-State Invariant" in spec
    assert "successor selection, Feasible Distance, destination reported distance" in spec_words
    assert "While Active" in spec
