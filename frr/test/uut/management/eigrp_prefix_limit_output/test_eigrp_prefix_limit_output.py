# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""FRR operational presentation contract for portable EIGRP prefix-limit state."""
from pathlib import Path

ROOT = next(p for p in Path(__file__).resolve().parents if (p / 'eigrpd' / 'code').is_dir() or (p / 'eigrp' / 'code').is_dir())
NAMED = ROOT / 'frr/code/eigrp_frr_named.c'
CONSOLE = ROOT / 'frr/code/eigrp_frr_console.c'
VTY = ROOT / 'frr/code/eigrp_vty.c'


def test_accounting_renders_portable_state_without_synthetic_restart_values():
    source = NAMED.read_text()
    start = source.index('static eigrp_result_t eigrp_vty_accounting_state_render(')
    stop = source.index('static eigrp_result_t eigrp_vty_traffic_context_render(', start)
    code = source[start:stop]
    for field in ('state->status', 'state->restart_count', 'state->restart_seconds',
                  'state->reset_seconds', 'state->prefix_count'):
        assert field in code
    assert 'eigrp_statistics_accounting_iterate(' in code
    assert 'Restart fields are n/a' not in code
    assert 'States: A-Adjacency, P-Pending, D-Down' in code


def test_neighbor_and_timer_views_render_portable_penalty_state():
    named = NAMED.read_text()
    classic = CONSOLE.read_text()
    vty = VTY.read_text()
    for field in ('state->limit_restarts', 'state->limit_restart_seconds',
                  'state->limit_reset_seconds'):
        assert field in named
    assert 'EIGRP_TIMER_STATE_LIMIT_RESTART' in named
    assert 'EIGRP_TIMER_STATE_LIMIT_RESET' in named
    assert 'eigrp_timer_state_iterate(' in named
    assert 'eigrp_nbr_limit_seconds(' in classic
    assert 'nbr->limit_state != EIGRP_NBR_LIMIT_NONE' in vty
    assert 'eigrp_vty_accounting_context_render(' in named
    assert 'eigrp_vty_tech_support_context(' in named
