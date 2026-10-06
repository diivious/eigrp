# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage

from pathlib import Path


ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())
CODE = ROOT / "eigrp" / "code"


def function_body(text: str, name: str) -> str:
    start = text.index("void " + name + "(")
    brace = text.index("{", start)
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[brace : pos + 1]
    raise AssertionError(f"unterminated function {name}")


def test_p2_route_decoder_failure_requires_stream_progress():
    for filename, function in (
        ("eigrp_update.c", "eigrp_update_receive"),
        ("eigrp_query.c", "eigrp_query_receive"),
        ("eigrp_reply.c", "eigrp_reply_receive"),
    ):
        body = function_body((CODE / filename).read_text(), function)
        decoder = body.index("(nbr->decoder)(eigrp, nbr, pkt, length)")
        guard = body[decoder : decoder + 500]

        assert "if (!route)" in guard
        assert "pkt->getp == tlv_start" in guard
        assert "return;" in guard
