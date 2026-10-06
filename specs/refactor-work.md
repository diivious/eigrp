# EIGRP Pre-Production Refactor Work

Copyright (C) 2026 Donnie V. Savage

## Purpose

This file holds three kinds of work.

Sections 1-2 are naming and architecture items parked until a coordinated
review. Do not use those as an excuse to rename half the tree in a feature
PR.

Section 3 is incomplete feature work. The public target exists, config is
retained, and the runtime path still returns `NOT_IMPLEMENTED`. A contributor
can pick one of those items, implement it, and send a PR.

Section 4 is packet-path and parser hardening. P0-P2 are the items worth
doing before trusting an unauthenticated LAN image. P3 and below are
low-value cleanup that can wait.

Completed work comes out of this file. Historical audit notes do not belong
here.

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

Before production, choose one final source/API navigation convention after
reviewing real usage across:

```text
eigrp_topology
DUAL/FSM processing
query/update/reply/SIA processing
packetizer and TLV codecs
show/debug/dump output
southbound RIB installation
tests
```

Candidate naming families include:

```text
eigrp_topology_prefix_* / eigrp_topology_route_*
eigrp_topology_prefix_descriptor_* / eigrp_topology_route_descriptor_*
eigrp_topology_dndb_* / eigrp_topology_drdb_*
```

The decision must optimize human navigation and make it immediately clear
whether an operation acts on a DUAL destination, a DUAL path, or a host/RIB
route.

Until the review:

- preserve `prefix_descriptor` / `route_descriptor` names;
- do not perform rename-only churn;
- do not introduce new ambiguous bare `route` APIs;
- DNDB/DRDB terminology may appear in comments/debug output where it improves
  EIGRP understanding.

## 2. Naming consistency pass

Before production, perform one bounded navigation/naming review against
`design-spec.md`:

- module/file and public symbol prefixes normally align;
- object/detail follows the module name;
- action appears last;
- configuration uses the intended `set/reset`, `add/remove`, or
  `create/delete` semantics;
- operational actions use `clear` only where appropriate;
- generic CLI/not-implemented dispatchers do not exist;
- grouped-module exceptions remain intentional;
- no obsolete alias wrappers remain after approved renames.

The public namespace should name the EIGRP object a developer is navigating to,
not mechanically repeat the implementation module.  A detail that has no useful
protocol meaning outside its owning area may still be the object-level namespace.
For example, variance is a metric behavior, but `eigrp_metric_variance_update()`
obscures the object/action pattern; the bounded naming pass should move that family
toward `eigrp_variance_*`.  Apply the same test to other compound public names
rather than preserving module prefixes by habit.

This review should produce a finite rename set and be committed separately from
protocol feature changes.

## 3. Intentional incomplete capability boundaries

The broad feature-completion list that previously lived here became stale as the
feature work landed.  TASK1-8 audits the remaining `EIGRP_RESULT_NOT_IMPLEMENTED`
paths individually in `task1-8-not-implemented-audit.md`.  Do not use this section
as a backlog of already-completed features.

The legacy `metric holddown` configuration surface has been removed; it is not an EIGRP feature target. Named HMAC-SHA-256 encryption type
7 also remains a capability boundary because the project has no portable type-7
decoder; encoded configuration text must never be used as the HMAC key.

Protocol runtimes no longer carry a per-instance data-path readiness flag. AF
support is an image/host integration capability, and a created runtime requires
its packet/RIB services rather than exposing a config-only runtime mode. Current
FRR/BIRD CLI surfaces expose only the unicast address-family and base topology.
Those host CLI limits must not become portable-core prohibitions: topology IDs
remain protocol identities carried by the common API and multiprotocol TLVs.

Stub routing stays out of scope.

## 4. Packet-path and parser hardening

This is an on-link protocol. Anyone on the LAN can send proto 88. Auth-off
is the common case today. Remaining P2 items should be completed before production use.

A PR should add a crafted-packet test for the hole it closes. Do not "harden"
by rewriting the codec.

### P2. Resource and loop abuse

#### 5.10 No cap on neighbors or topology entries

Each accepted Hello can `calloc` a neighbor. Each route TLV can `calloc` a
descriptor. Prefix-limit config still returns `NOT_IMPLEMENTED`. On-link
flood is memory and CPU.

Fix: enforce max-neighbors and max-prefix on the receive path, not just
retained config. That is the same work as Section 3 max-prefix items.

#### 5.11 Unknown Hello TLVs are skipped after the neighbor exists

Combined with 5.3, junk Hellos still allocate.

#### 5.12 Update/Query/Reply decoder loop

The loop keeps calling the decoder while `endp > getp`. TLV1/TLV2 abort to
`endp` on unknown type, so they advance. A future codec that returns NULL
without moving `getp` hangs the read callback.

Fix: if decoder returns NULL and `getp` did not move, drop the packet.

#### 5.13 `eigrp_print_addr()` is `inet_ntoa` of IPv4 only

Static buffer, IPv6-wrong. Two prints in one log can alias. Not an
overflow. Logging lie.

### P3. Low value. Already in decent shape. Pick up later if you want.

These are not holes to burn a release on. They can still be cleaned up.

- IPv4/IPv6 prefix decode already caps bitlen and checks remaining before
  `stream_get`. Keep that pattern. Add tests so it cannot regress.
- FRR `recvmsg` already requires `ip_v == 4`, sane `ip_hl`, and
  `ip_len == recv size`. Keep a portable contract test so a new shim cannot
  drop those checks.
- Auth TLV framing walk already rejects `tlv_length < 4` or
  `tlv_length > remaining`. Keep it. Do not rewrite it.
- Debug dump prefix printer already checks length before copy.
- `eigrp_stream_get()` already caps the copy to available bytes. The
  problem is the untyped `getc`/`getw`/`getl` wrappers in 5.7, not `get()`.
- TLV1/TLV2 `tlv_start + length` is safe today because `length <= remaining`
  and `size_t` is wide. Do not churn it. If you copy that walk, keep the
  check.
- `eigrp_header.tlv[0]` is typed as `char *` and used as bytes. Ugly, not
  an overflow by itself. Cleanup only.

### P4. Later hygiene. Not security-critical.

- Replace `inet_ntoa` in `eigrp_print_addr()` with a caller-supplied buffer
  that handles IPv4 and IPv6.
- Stop ignoring `pktlen` once 5.2 clamps the stream. Pass one bound and use
  it.
- Document that SHA-256 config may be retained while receive stays
  `NOT_IMPLEMENTED`. That is already a Section 3 auth item.
