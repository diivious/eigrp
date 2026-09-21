# EIGRP Pre-Production Refactor Work

Copyright (C) 2026 Donnie V. Savage

## Purpose

This file is the bounded parking lot for architectural cleanup that is
intentionally deferred until a pre-production refactor review.

Items here are not development status notes and are not permission to refactor
unrelated code during feature work. A feature change should touch a parked item
only when the parked issue becomes a functional blocker or the user explicitly
approves the refactor.

Completed audits and historical implementation steps do not belong here.

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

## 2. Runtime lifecycle namespace consolidation

Named configuration ownership is already under `eigrp_instance_*`, while some
low-level runtime allocation/destruction entry points remain under legacy names
in `eigrpd.c`, including `eigrp_get()`, `eigrp_lookup()`, and `eigrp_finish()`.

Before production, decide whether the remaining runtime lifecycle should be
consolidated into a predictable `eigrp_instance.c/.h` ownership boundary and
`eigrp_instance_*` namespace.

The review must preserve:

- named parent/address-family ownership;
- classic compatibility behavior;
- AF/VRF/AS runtime identity;
- southbound lifecycle isolation;
- teardown ordering;
- no alias-wrapper compatibility layer after an approved rename.

Do not perform a rename-only migration before that coordinated review.

## 3. Naming consistency pass

Before production, perform one bounded navigation/naming review against
`code-conventions.md`:

- module/file and public symbol prefixes normally align;
- object/detail follows the module name;
- action appears last;
- configuration uses the intended `set/reset`, `add/remove`, or
  `create/delete` semantics;
- operational actions use `clear` only where appropriate;
- generic CLI/not-implemented dispatchers do not exist;
- grouped-module exceptions remain intentional;
- no obsolete alias wrappers remain after approved renames.

This review should produce a finite rename set and be committed separately from
protocol feature changes.
