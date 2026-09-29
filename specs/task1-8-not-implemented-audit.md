# TASK1-8 — Final `EIGRP_RESULT_NOT_IMPLEMENTED` audit

Copyright (C) 2026 Donnie V. Savage

## Scope

This audit covers production source under `eigrpd/code`, `frr/code`, and
`unix/code`, plus test/harness occurrences elsewhere.  It classifies every
production occurrence as a feature/capability boundary or propagation/commit
handling.  EIGRP Stub remains out of scope and is not implemented or tested.

## Corrections made by TASK1-8

- `eigrp_instance_parent_shutdown_update()` now retains parent shutdown state,
  stops capable child AF runtimes, and on reset restarts only children whose own
  AF shutdown state permits startup.  A disabled data path remains a truthful
  child capability result.
- New AFs created beneath a shut named parent no longer start their runtime.
- Base `eigrp_topology_create()` / `eigrp_topology_delete()` now maintain real
  retained base-topology state and succeed idempotently.  Non-base TIDs remain
  unsupported.
- Runtime-only manual-summary mutation now returns `NOT_FOUND`, not
  `NOT_IMPLEMENTED`; retained summary configuration belongs to the AF config.
- FRR named topology MAF requests now cross the EIGRP-owned state-request
  boundary instead of returning a host-side literal `NOT_IMPLEMENTED`.
- The stale broad incomplete-feature table in `refactor-work.md` was replaced by
  the current boundary description.

## Remaining production returns

| File / function | Classification | Authorization / dependency |
|---|---|---|
| `eigrp_auth.c` / `eigrp_auth_mode_update()` | capability boundary | Named HMAC encryption type 7 has no portable decoder. TASK1-4 requires preserving encryption-type representation and forbids treating type-7 text as runtime key material. |
| `eigrp_instance.c` / `eigrp_af_instance_iterate()` | capability boundary | `cli-spec.md` 10.1: MAF/VRID `0x0001` is explicitly unsupported and must terminate at the EIGRP-owned state boundary. |
| `eigrp_instance.c` / `eigrp_af_instance_stop()` and `eigrp_af_instance_start()` | capability boundary | `process-spec.md` 4: when `data_path_ready` is false, runtime-dependent targets return a structured capability/not-implemented result and no packet/RIB runtime starts. |
| `eigrp_interface.c` / `eigrp_intf_state_iterate()` | capability boundary | Same `process-spec.md` `data_path_ready` rule; interface operational state is runtime-dependent. |
| `eigrp_topology.c` / `eigrp_topology_state_iterate()` | capability boundary | Same `process-spec.md` runtime gate; config-only IPv6 may exist without a live topology data path. |
| `eigrp_topology.c` / `eigrp_topology_context_validate()` | capability boundary | TASK1-8 expected boundary: only base topology/TID 0 is implemented; non-base topology IDs are outside the current model. |
| `eigrp_topology.c` / `eigrp_topology_default_information_update()` | documented design dependency | `task1-8-default-information-design-question.md` records missing authoritative semantics. The real target remains; FRR retains committed config. |
| `eigrp_neighbor.c` / `eigrp_nbr_state_iterate()` and `eigrp_nbr_clear()` | capability boundary | `process-spec.md` 4: neighbor/adjacency operations do not run when `data_path_ready` is false. |
| `eigrp_redistribute.c` / add/remove/import paths | capability boundary | `process-spec.md` 4 explicitly gates RIB operations when `data_path_ready` is false; retained configuration remains committed. |
| `eigrp_statistics.c` / `eigrp_statistics_context_validate()` | capability boundary | Runtime statistics are runtime-dependent and therefore follow the `data_path_ready` rule. |
| `eigrp_timer.c` / `eigrp_timer_state_iterate()` | capability boundary | Runtime timer state is unavailable when `data_path_ready` is false. |

## Propagation / committable handling

The other production textual occurrences are not feature targets:
`eigrp_instance.c` aggregates a child capability result while preserving parent
state; `eigrp_filter.c` and `eigrp_redistribute.c` accept/propagate capability
results; `frr/eigrp_northbound.c`, `frr/eigrp_zebra.c`, and
`unix/eigrp_unix_config.c` translate or tolerate the structured result; and
`frr/eigrp_cli_named.c` renders the result enum.  `eigrpd/code/eigrp.h` only
defines the enum value.

## Test/harness-only occurrences

Occurrences under `eigrpd/test`, `frr/test`, and `unix/test` are assertions or
harness behavior.  The Unix configuration harness deliberately supplies an
unimplemented host callback and is not production protocol behavior.

## Stub audit

No EIGRP Stub runtime behavior is introduced by TASK1-8.  External/reference
configuration-guide mentions remain documentation only, consistent with
`design-spec.md` and `cli-spec.md`.

## Validation performed for this delivery

- Portable core build: PASS.
- Task1-8 focused pytest: 5/5 PASS.
- Portable pytest suite: 127/127 PASS.
- YAML scenario runner: scenarios 1-3 PASS; the execution sandbox terminated
  the aggregate gate while scenario 4 (`srtt-rto`) was running because each
  command is limited to about 20 seconds.  This is a gate-execution limitation,
  not a reported scenario assertion failure.
- FRR compile/link smoke: blocked by the existing staged
  `eigrp_policy.c` -> `eigrp_interface.h` dependency: unknown type
  `eigrp_intf_params_t`.  TASK1-8 does not modify either file/type.
