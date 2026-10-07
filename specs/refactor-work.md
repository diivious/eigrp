# EIGRP Pre-Production Refactor Work

Copyright (C) 2026 Donnie V. Savage

## Purpose

This file contains only unresolved work that is intentionally deferred from
normal feature changes. Completed work is removed rather than retained as
history.

The items here are bounded reviews or current capability/hardening gaps. They do
not override `specs/architecture.md`, `specs/code-conventions.md`, or the owning
protocol specification.

## 1. DUAL topology descriptor naming

The topology database has a destination-level descriptor with one or more
neighbor/path descriptors:

```text
prefix_descriptor
    route_descriptor
    route_descriptor
    ...
```

Cisco historical terminology maps these concepts to DNDB/NDB and DRDB/RDB.
Final source/API naming remains intentionally parked until a coordinated
pre-production review because `route` is also used for host/RIB objects.

Review actual usage across:

```text
eigrp_topology
DUAL/FSM processing
query/update/reply/SIA processing
packetizer and TLV codecs
show/debug/event output
southbound RIB installation
tests
```

Until that review:

- preserve `prefix_descriptor` / `route_descriptor` names;
- do not perform rename-only churn;
- do not introduce new ambiguous bare `route` APIs;
- DNDB/DRDB terminology may appear in comments or diagnostics when it improves
  EIGRP navigation.

## 2. Bounded naming consistency review

Before production, perform one finite navigation/naming review against
`specs/code-conventions.md`.

The review should verify that:

- module/file and public symbol prefixes normally align;
- narrow object/detail follows the module name and the action appears last;
- public configuration actions use the intended `set/reset`, `add/remove`, or
  `create/delete` vocabulary;
- operational actions use `clear` only for operational state;
- grouped-module exceptions remain deliberate;
- no obsolete aliases or temporary compatibility wrappers remain without an
  approved migration reason.

Do not combine this review with protocol feature work. Produce a finite rename
set and review it as a separate refactor.

## 3. Current capability boundaries

These are current, explicit boundaries rather than placeholder feature targets.

### 3.1 Public result enum

`EIGRP_RESULT_NOT_IMPLEMENTED` remains defined for public API stability, but the
portable production source has no feature target that returns it. Valid options
that exceed an active runtime/host capability use `EIGRP_RESULT_UNSUPPORTED` or
another precise structured result.

### 3.2 HMAC-SHA-256 encryption type 7

Named HMAC-SHA-256 accepts encryption type 7 configuration text for retention
and writeback. The portable core does not decode Cisco type-7 text and never
uses encoded text as HMAC key material. Applying type 7 to a live runtime is an
unsupported capability until a deliberate decoder/key-handling design is added.

### 3.3 Maximum-prefix advanced controls

Neighbor, topology, and redistribution maximum-prefix admission enforce the
configured maximum, threshold, and `warning-only` behavior. The grammar also
retains `dampened`, `reset-time`, `restart`, and `restart-count`; those advanced
restart/dampening controls are not active runtime behavior and return
`EIGRP_RESULT_UNSUPPORTED` when requested on a live runtime.

### 3.4 Host integration boundaries

- The FRR integration exposes the current unicast address-family and base
  topology command model.
- The standalone Unix host supports the default VRF and does not provide a
  native ACL/prefix-list/route-map or key-chain database.
- The `bird/` tree is the BIRD/BSD integration boundary; no BIRD runtime adapter
  is present in the current repository.

These host limits must not become portable EIGRP protocol prohibitions.

EIGRP Stub runtime behavior remains outside project scope.

## 4. Remaining receive-path resource hardening

The packet parser performs framing, length, authentication, source, and TLV
validation before protocol processing. Remaining hardening work should be small,
targeted, and accompanied by crafted-packet tests.

### 4.1 Neighbor-object admission bound

A valid on-link Hello can create a neighbor object before an adjacency reaches
UP. Prefix limits bound learned route growth but do not provide a process-wide
or interface-wide cap on neighbor objects. Before production exposure on large
shared LANs, decide whether the host/core contract needs an explicit neighbor
admission limit and define the operational behavior when it is exceeded.

### 4.2 Unknown Hello TLVs

Unknown Hello TLVs are skipped so future/optional TLVs do not break adjacency
interoperability. Keep that behavior, but preserve the rule that framing and
known mandatory TLVs are validated before an unknown TLV can drive unbounded
state allocation or looping.

### 4.3 Parser regression coverage

Keep regression tests around the existing safety properties rather than
rewriting working codecs:

- prefix bit lengths are bounded before address copies;
- packet/TLV lengths are checked against remaining bytes;
- authentication TLV framing is bounded before digest processing;
- packet receive metadata is validated before checksum/TLV access;
- stream copies are bounded by available data;
- IPv4 and IPv6 address formatting remains family-correct and reentrant enough
  for diagnostic call sites.
