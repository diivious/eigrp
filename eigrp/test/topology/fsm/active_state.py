# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for RFC 7868 DUAL ACTIVE-state invariants.

from pathlib import Path
import re


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
FSM = ROOT / "eigrp" / "code" / "eigrp_fsm.c"
SPEC = ROOT / "eigrp" / "specs" / "dual.md"

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
        assert "eigrp_topology_successors_read" not in body
        assert "listnode_head(successors)" not in body


def test_route_metric_updates_are_still_recorded_before_fsm_event_selection():
    body = function_body(read(FSM), "eigrp_fsm_event_select")

    assert "change = eigrp_topology_update_distance(msg);" in body
    assert "msg->change = change;" in body


def test_dual_spec_documents_active_state_freeze_rule():
    spec = read(SPEC)
    spec_words = " ".join(spec.split())

    assert "## 6. Active-state freeze invariant" in spec
    assert "destination-level successor, FD" in spec_words
    assert "reported distance, selected distance" in spec_words
    assert "While Active" in spec


def test_active_query_origin_is_retained_for_deferred_reply_without_successor_lookup():
    source = read(FSM)
    structs = read(ROOT / "eigrp" / "code" / "eigrp_structs.h")
    topology = read(ROOT / "eigrp" / "code" / "eigrp_topology.c")

    assert "eigrp_nbr_t *query_origin" in structs

    q_fcn = source[source.rindex("int eigrp_fsm_event_q_fcn("):source.rindex("int eigrp_fsm_event_keep_state(")]
    qact = source[source.rindex("int eigrp_fsm_event_qact("):]
    lr = source[source.rindex("int eigrp_fsm_event_lr("):source.rindex("int eigrp_fsm_event_dinc(")]
    lr_fcs = source[source.rindex("int eigrp_fsm_event_lr_fcs("):source.rindex("int eigrp_fsm_event_lr_fcn(")]

    assert "prefix->query_origin = msg->adv_router;" in q_fcn
    assert "msg->prefix->query_origin = msg->adv_router;" in qact

    for body in (lr, lr_fcs):
        assert "eigrp_topology_successors_read" not in body
        assert "assert(successors)" not in body
        assert "prefix->query_origin" in body
        assert "eigrp_reply_send(eigrp, prefix->query_origin, prefix);" in body
        assert "prefix->query_origin = NULL;" in body

    assert "if (pe->query_origin == nbr)" in topology
    assert "pe->query_origin = NULL;" in topology


def test_active_non_successor_query_replies_with_current_rib_metric():
    source = read(FSM)

    helper_start = source.index("static void eigrp_fsm_active_non_successor_reply_send(")
    helper_end = source.index("int eigrp_fsm_event_keep_state(", helper_start)
    helper = source[helper_start:helper_end]
    keep_start = source.rindex("int eigrp_fsm_event_keep_state(")
    keep_end = source.index("int eigrp_fsm_event_lr(", keep_start)
    keep = source[keep_start:keep_end]

    assert "EIGRP_ROUTE_DESCRIPTOR_INTABLE_FLAG" in helper
    assert "route->distance == EIGRP_MAX_METRIC" in helper
    assert "reply_route->metric = prefix->reported_metric;" in helper
    assert "EIGRP_PACKETIZER_WORK_F_OWN_ROUTE" in helper
    assert "EIGRP_PACKETIZER_WORK_F_POISON" in helper

    assert "msg->packet_type == EIGRP_OPC_QUERY" in keep
    assert "EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG" in keep
    assert "eigrp_fsm_active_non_successor_reply_send(" in keep


def test_active_successor_query_is_deferred_and_updates_query_origin_state():
    source = read(FSM)
    select_start = source.index("eigrp_fsm_event_select(eigrp_fsm_action_message_t *msg)")
    select_end = source.index("int eigrp_fsm_event(", select_start)
    select = source[select_start:select_end]
    keep_start = source.rindex("int eigrp_fsm_event_keep_state(")
    keep_end = source.index("int eigrp_fsm_event_lr(", keep_start)
    keep = source[keep_start:keep_end]
    qact_start = source.rindex("int eigrp_fsm_event_qact(")
    qact = source[qact_start:]

    # RFC 7868 section 3.5 step 5: a QUERY from the current successor while
    # ACTIVE remains part of the existing diffusing computation.  It updates
    # the query-origin/history state and is not answered immediately.
    active2 = select[select.index("case EIGRP_FSM_STATE_ACTIVE_2:"):
                     select.index("case EIGRP_FSM_STATE_ACTIVE_3:")]
    active3 = select[select.index("case EIGRP_FSM_STATE_ACTIVE_3:"):]

    for body in (active2, active3):
        assert "msg->packet_type == EIGRP_OPC_QUERY" in body
        assert "EIGRP_ROUTE_DESCRIPTOR_SUCCESSOR_FLAG" in body
        assert "return EIGRP_FSM_EVENT_QACT;" in body

    # KEEP_STATE may reply immediately only to an ACTIVE query from a
    # non-successor.  A successor query must not take an immediate REPLY path.
    assert "eigrp_fsm_active_non_successor_reply_send(" in keep
    assert "else\n\t\t\teigrp_reply_send_route" not in keep

    # QACT records the successor-originated query and maps the state to oij2.
    assert "msg->prefix->state = EIGRP_FSM_STATE_ACTIVE_2;" in qact
    assert "msg->prefix->query_origin = msg->adv_router;" in qact
