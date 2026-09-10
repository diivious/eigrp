# EIGRP Pre-Production Refactor Work

Copyright (C) 2026 Donnie V. Savage

## Purpose

This file is the parking lot for architectural and naming cleanup that should be revisited before production release but should not interrupt active RFC 7868/named-mode feature work unless it becomes a functional blocker.

Items here are design decisions or cleanup work, not permission to refactor unrelated code during a feature change.

## 1. DUAL Topology Descriptor Naming

### Current model

The DUAL topology database has a destination-level descriptor and a set of path-level descriptors beneath it:

```text
prefix_descriptor
    route_descriptor
    route_descriptor
    ...
```

Cisco terminology historically maps these to DNDB/NDB and DRDB/RDB concepts.

### Reason for review

The words `prefix` and `route` are readable, but `route` is overloaded by the host platform, Zebra/RIB integration, kernel routing, and DUAL topology processing. Cisco's DNDB/DRDB abbreviations have several practical advantages:

- they are established EIGRP terminology visible in real-world debugging;
- they are concise and easy to type;
- they are highly searchable;
- they distinguish DUAL topology objects from platform routes;
- they make the one-prefix-to-many-routes relationship explicit to experienced EIGRP developers/operators.

The current `prefix_descriptor` / `route_descriptor` terminology also intentionally provides some separation from Cisco's historical implementation naming.

### Decision to make before production

Evaluate at least these alternatives against the completed code:

```text
A. eigrp_topology_prefix_* / eigrp_topology_route_*
B. eigrp_topology_prefix_descriptor_* / eigrp_topology_route_descriptor_*
C. eigrp_topology_dndb_* / eigrp_topology_drdb_*
D. hybrid naming, with descriptive C types and DNDB/DRDB navigation/debug names
```

Review actual usage across:

```text
eigrp_topology
eigrp_fsm / DUAL processing
query/update/reply processing
packet/TLV encode-decode
show/debug/dump output
southbound/Zebra route installation
tests
```

Do not decide this from naming aesthetics alone. The final convention should make it immediately clear whether an operation acts on a DUAL destination object, one DUAL path descriptor, or a host/RIB route.

### Current rule

Do not perform rename-only churn here during named-mode implementation. Preserve existing names unless a functional change requires touching them, and avoid adding new ambiguous bare `route` APIs.

## 2. Instance / Process Lifecycle Navigation

The runtime protocol context is already represented by `eigrp_instance_t`, but lifecycle functions are currently distributed across legacy files and names such as `eigrp_new()`, `eigrp_get()`, `eigrp_finish()`, and `eigrp_lookup()`.

Before production, review whether runtime/process ownership should be consolidated so a developer debugging an EIGRP process/thread can predictably search:

```text
eigrp_instance.c
eigrp_instance_*
```

This review should be coordinated with named-mode address-family runtime binding. A named parent is configuration ownership; each configured AF/VRF/AS protocol context must map cleanly to the runtime instance/worker model.

Do not perform a rename-only migration until the runtime ownership model is clear.

## 3. Portable `main()` / Platform Lifecycle Boundary

`eigrp_main.c` should remain the portable executable/process entry point. FRR-specific daemon startup, privilege, event-loop, VTY/YANG registration, signal, and host lifecycle code should eventually move behind a narrow platform lifecycle contract.

Potential direction:

```text
eigrpd/eigrp_main.c       portable main/process orchestration
eigrpd/eigrp_platform.h   narrow lifecycle contract
frr/eigrp_platform.c      FRR implementation
bsd/eigrp_platform.c      future BSD implementation
```

Do not allow `eigrp_platform.h` to become a generic dumping ground. Timers, work queues, sockets, route installation, CLI, and other services should stay behind their existing northbound/southbound/module-specific contracts where appropriate.

## 4. Core-to-Zebra Leakage Audit

Common/core files still need a pre-production audit for direct Zebra/RIB calls and includes. Portable topology/network/neighbor/runtime code should invoke EIGRP-owned southbound operations rather than directly depending on `eigrp_zebra_*`.

The audit should distinguish clearly among:

```text
DUAL prefix descriptor
DUAL route descriptor/path
EIGRP learned route as protocol information
host/Zebra RIB route
kernel route
```

This work should follow the topology descriptor naming decision where practical so the boundary becomes clearer rather than merely renamed.

## 5. Portability Grooming

`eigrp_snmp.[ch]` and `eigrp_vrf.[ch]` remain in the common tree for now. Revisit their ownership when the BSD/native portability work begins. Do not move them simply for directory symmetry while FRR named-mode functionality is still being completed.

## 6. Naming Consistency Audit

Before production, perform a focused navigation/naming audit using `code-conventions.md`:

- filename/module and `eigrp_<module>_` prefix normally align;
- object/detail narrows after the module name;
- action normally appears last;
- configuration `set/reset` functions are explicit and searchable;
- generic CLI/not-implemented dispatchers do not exist;
- grouped-module exceptions such as filter/distribute/offset remain intentional;
- no alias wrappers remain after approved renames.

This should be a bounded pre-production refactor, not a broad rewrite of otherwise stable code.
