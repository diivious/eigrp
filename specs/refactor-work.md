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

## 3. Portable `main()` / platform lifecycle boundary

The end-state repository requires portable protocol code to be usable with FRR
and BIRD. Review the remaining host bootstrap responsibilities in
`eigrp_main.c`, `eigrpd.c`, `eigrp_vrf.[ch]`, and host adapter files.

The desired boundary is a narrow platform lifecycle contract for process
bootstrap/termination only. It must not become a generic adapter dumping ground.

A possible shape is:

```text
eigrpd/eigrp_main.c       portable process orchestration
eigrpd/eigrp_platform.h   narrow process/platform lifecycle contract
frr/eigrp_platform.c      FRR implementation
bird/eigrp_platform.c     BIRD implementation
```

Timers, work queues, sockets, route installation, CLI, policy, and packet I/O
remain behind their existing module/southbound contracts rather than moving into
one catch-all platform API.

## 4. Generic container and packet-buffer dependencies

Portable code still uses several FRR/lib-style utility representations in its
internal implementation, notably `route_table`/`route_node`, `struct list`, and
`struct stream`. These are not Zebra RIB objects, but they are still portability
dependencies when their concrete host-library types appear in public portable
interfaces or shared structure layouts.

Before the BIRD build is considered native, establish EIGRP-owned container
and packet-buffer contracts so portable module APIs do not require FRR utility
object layouts. The review should decide whether to:

- retain an existing implementation behind an opaque EIGRP-owned contract;
- replace it with project-owned prefix/list/packet-buffer implementations; or
- use another host-independent implementation shared by both adapters.

In particular, `eigrp_stream_t` must not remain a typedef alias whose public
contract is an FRR `struct stream`. Packet/TLV modules may keep efficient stream
semantics, but the end-state portable API and owned runtime structures must use
an EIGRP-owned representation.

Do not confuse this utility-storage cleanup with the already-established
Zebra/RIB southbound boundary.

## 5. SNMP and VRF ownership

`eigrp_snmp.[ch]` and `eigrp_vrf.[ch]` require a focused ownership review before
production/BIRD integration.

The review should determine which behavior is:

- portable EIGRP protocol state;
- host-management integration;
- FRR-specific lifecycle/presentation;
- optional platform functionality.

Move code only when the ownership boundary is clear; do not move files for
directory symmetry.

## 6. Naming consistency pass

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
