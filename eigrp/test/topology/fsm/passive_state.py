# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Source-level guards for RFC 7868 DUAL PASSIVE-state behavior.

from pathlib import Path


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
FSM = ROOT / "eigrp" / "code" / "eigrp_fsm.c"


def read(path: Path) -> str:
    return path.read_text()


def test_passive_query_metric_change_queues_one_reply():
    source = read(FSM)
    start = source.rindex("int eigrp_fsm_event_keep_state(")
    end = source.index("int eigrp_fsm_event_lr(", start)
    keep = source[start:end]

    passive_start = keep.index("if (prefix->state == EIGRP_FSM_STATE_PASSIVE)")
    query_start = keep.index("if (msg->packet_type == EIGRP_OPC_QUERY)", passive_start)
    passive_update = keep[passive_start:query_start]
    query_reply = keep[query_start:]

    # Metric maintenance while PASSIVE must not enqueue its own QUERY reply.
    # The common QUERY block below is the single reply point for PASSIVE.
    assert "eigrp_reply_send(eigrp, msg->adv_router" not in passive_update
    assert query_reply.count(
        "eigrp_reply_send(eigrp, msg->adv_router, prefix);"
    ) == 1
