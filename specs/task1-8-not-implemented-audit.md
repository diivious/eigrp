# TASK1-8 — Final `EIGRP_RESULT_NOT_IMPLEMENTED` audit

Copyright (C) 2026 Donnie V. Savage

## Scope

This audit covers production source under `eigrpd/code`, `frr/code`, and
`unix/code`, plus test/harness occurrences elsewhere.  It classifies every
production occurrence as a feature/capability boundary or propagation/commit
handling.  EIGRP Stub remains out of scope and is not implemented or tested.

## Corrections made by TASK1-8

- `eigrp_instance_parent_shutdown_update()` now retains parent shutdown state,
  stops child AF runtimes, and on reset restarts only children whose own AF
  shutdown state permits startup.
- New AFs created beneath a shut named parent no longer start their runtime.
- Base `eigrp_topology_create()` / `eigrp_topology_delete()` maintain real
  retained base-topology state and succeed idempotently. Portable topology APIs
  no longer reject non-base TIDs merely because current host CLIs expose only base.
- Multiprotocol TLV encode/decode preserves non-base TIDs instead of filtering
  them at the packet boundary.
- Runtime-only manual-summary mutation now returns `NOT_FOUND`, not
  `NOT_IMPLEMENTED`; retained summary configuration belongs to the AF config.
- Current FRR named operational grammar does not expose the multicast
  address-family selector. Portable state types retain the selector for future or
  alternate integrators.
- The stale broad incomplete-feature table in `refactor-work.md` was replaced by
  the current boundary description.

## Production result

Production feature targets under `eigrpd/code`, `frr/code`, and `unix/code` no
longer return or tolerate `EIGRP_RESULT_NOT_IMPLEMENTED`. The enum remains part
of the public structured-result vocabulary, and exhaustive result consumers such
as the FRR CLI renderer still handle it defensively.

Current capability boundaries use normal semantic results instead:

- Current FRR/BIRD CLI surfaces do not expose the multicast address-family. A
  multicast state request reaching the portable state API returns `UNSUPPORTED`;
  the portable selector remains available for a future/alternate integrator.
- FRR named HMAC-SHA256 exposes only encryption type 0. A direct portable
  request for type 7 returns `UNSUPPORTED` before retained or runtime state is
  mutated because no portable type-7 decoder exists.
- Non-base topology IDs are not a capability error in portable EIGRP. The
  common topology and multiprotocol TLV paths preserve the supplied TID even
  though current FRR/BIRD CLIs expose only `topology base`.

Completed runtime paths no longer tolerate `NOT_IMPLEMENTED` as a committable
or ignorable result. This includes AF lifecycle, distribute-list, redistribution,
FRR northbound configuration, Zebra redistribution, and Unix configuration-file
application. FRR result rendering no longer treats `NOT_IMPLEMENTED` as CLI
success.

## Test/harness-only occurrences

Occurrences under `eigrpd/test`, `frr/test`, and `unix/test` are assertions or
harness behavior.  The Unix configuration harness deliberately supplies an
unimplemented host callback and is not production protocol behavior.

## Stub audit

No EIGRP Stub runtime behavior is introduced by TASK1-8.  External/reference
configuration-guide mentions remain documentation only, consistent with
`design-spec.md` and `cli-spec.md`.

## Validation performed for this cleanup

- Portable pytest suite: 134/134 PASS.
- Production-source audit: no feature target returns or tolerates `EIGRP_RESULT_NOT_IMPLEMENTED`; the FRR CLI renderer retains an exhaustive enum case.
- FRR compile smoke reaches the existing harness/API mismatch in
  `eigrp_zebra.c`: the harness `struct zapi_nexthop` has no `weight` member.
  The remaining mismatch is outside this cleanup's result-handling changes.
