# Named-Mode CLI Linkage Audit Record

Copyright (C) 2026 Donnie V. Savage

> **Non-normative audit record.** The durable CLI/target rules from this review
> are defined in `cli-spec.md` and `code-conventions.md`. This file may be
> removed once the audit history is no longer useful.

## Scope

This audit verified that named-mode commands do not use classic FRR parser or
northbound callbacks as their protocol implementation merely to claim reuse.
The required convergence point is below the host boundary in EIGRP-owned
behavior.

EIGRP Stub was excluded because the EIGRP Stub feature is outside project scope.

## Final linkage rule

For a named command with a classic equivalent:

```text
classic front end --\
                    -> EIGRP-owned semantic target/processor
named front end   ---/
```

When the existing classic endpoint is host-owned and cannot cleanly call the
same public target, it may remain separate at the host boundary while both paths
converge on EIGRP-owned runtime state/processing below that boundary.

Named mode must not:

- call a classic FRR CLI callback as protocol behavior;
- import FRR VTY/YANG/interface/Zebra objects into a portable target;
- copy a classic `TODO` or non-implementation as its backend;
- replace a real feature target with a generic not-implemented command stub.

When no working classic runtime behavior exists, the named command still owns a
real EIGRP feature target and returns a structured capability result from that
target.

## Target ownership verified by the audit

The reviewed named surface terminates in EIGRP-owned feature families for:

```text
instance/address-family lifecycle
router ID and shutdown
network participation
interface configuration and timers
neighbor configuration/policy/logging
authentication
summary configuration
metrics and topology controls
filter/distribute/offset configuration
redistribution
event log/statistics/status
show/clear operational state
```

The exact symbol inventory is intentionally not frozen in this audit record.
Public names are governed by `code-conventions.md` and may be normalized by an
approved refactor without requiring this file to track point-in-time callback
counts.

## Documentation rule

FRR-facing command and northbound callbacks should make the following easy to
trace in source:

```text
command syntax/mode
  -> retained XPath/management node where applicable
  -> northbound callback
  -> normalized EIGRP-owned target
```

Operational commands without retained configuration should likewise identify
the EIGRP-owned state/clear/debug target they invoke.

## Regression rule

A named command is correctly linked when a reviewer can follow the command to
its host management callback and then to the real EIGRP feature target without
crossing back into another CLI surface or leaking host-native objects into the
portable API.
