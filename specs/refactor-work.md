# EIGRP Pre-Production Refactor Work

Copyright (C) 2026 Donnie V. Savage

## Purpose

This file holds two kinds of work.

Sections 1-3 are naming and architecture items parked until a coordinated
review. Do not use those as an excuse to rename half the tree in a feature
PR.

Section 4 is incomplete feature work. The public target exists, config is
retained, and the runtime path still returns `NOT_IMPLEMENTED`. A contributor
can pick one of those items, implement it, and send a PR.

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

This review should produce a finite rename set and be committed separately from
protocol feature changes.

## 4. Incomplete feature targets

These targets keep retained configuration and return
`EIGRP_RESULT_NOT_IMPLEMENTED` when the live path is missing. That is
intentional. Do not add a generic stub dispatcher. Finish the real target.

IPv6 named config uses the same targets. IPv6 packet I/O is a separate
capability gate, listed last.

| Target | What works today | What is still missing |
|---|---|---|
| `eigrp_auth_mode_set` HMAC-SHA-256 with a named direct password | config retained; MD5 runtime works; classic HMAC via key-chain works | named direct-password key material and receive validation |
| `eigrp_offset_add` / `eigrp_offset_remove` | config retained | apply offset into metric processing |
| `eigrp_instance_parent_shutdown_set` / `_reset` | config retained | parent-wide runtime shutdown semantics |
| `eigrp_instance_distance_set` / `_reset` | config retained | RIB administrative-distance application |
| `eigrp_interface_bandwidth_percent_set` / `_reset` | config retained | live pacing application |
| `eigrp_interface_next_hop_self_set` / `_reset` | config retained | live packet-path application |
| `eigrp_metric_traffic_share_balanced_set` / `_reset` | config retained | forwarding/runtime application |
| `eigrp_summary_create` / `_delete` | config retained | advertise/withdraw manual summaries |
| `eigrp_summary_auto_set` / `_reset` | config retained | auto-summary runtime |
| `eigrp_summary_metric_set` / `_reset` | config retained | use the override when originating a summary |
| `eigrp_neighbor_maximum_prefix_set` / `_reset` / `_all_*` | config retained | enforce the prefix limit on a neighbor |
| `eigrp_topology_create` / `_delete` | config/identity retained | non-base topology runtime |
| `eigrp_topology_default_information_set` / `_reset` | config retained | originate/accept default by that policy |
| `eigrp_topology_maximum_prefix_set` / `_reset` | config retained | enforce the topology prefix limit |
| `eigrp_rib_redistribute_add` / FRR `eigrp_zebra_redistribute_update` | Zebra subscribe happens | install source routes into topology and apply route-map |
| IPv6 data path | named IPv6 config/writeback | proto-88 IPv6 socket, multicast, and packet I/O |
| `eigrp_init` / `eigrp_terminate` | FRR main calls them from `eigrpd.h` | move process init into the public `eigrp.h` contract |

Rules for Section 4 work:

- Keep the existing target. Do not invent a parallel API.
- Keep valid retained config even while you fill in runtime.
- Add or extend a test that fails if the path goes back to `NOT_IMPLEMENTED`.
- IPv6 packet I/O is one item. Do not invent a second integration model for it.
- Stub routing stays out of scope. Do not add it here.

Show/state walkers also return `NOT_IMPLEMENTED` when `data_path_ready` is
false. That is the IPv6/capability gate, not a separate missing show API.
