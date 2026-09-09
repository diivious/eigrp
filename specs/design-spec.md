# EIGRP Development Design Specification

Copyright (C) 2026 Donnie V. Savage

This file is a new project design document. New files created for this EIGRP work use Donnie V. Savage as the copyright owner unless stated otherwise.

Existing source files must preserve all prior copyright notices, SPDX identifiers, and author history. Refactoring an existing file is not permission to remove earlier authorship.

## 1. Purpose

This document is the governing development design specification for the `eigrpd` project.

It defines the rules for code structure, naming, ownership boundaries, portability, packet encoding, testing direction, and FRR integration.

This document is not a replacement for RFC 7868. RFC 7868 remains the protocol reference unless Donnie V. Savage intentionally clarifies, extends, or supersedes a behavior for this implementation.

Future focused specifications may be added as the design gets deeper, including:

- `testing-spec.md`
- `tlv-spec.md`
- `packetizing-spec.md`
- `cli-spec.md`
- `rfc-current.md`

Those documents must not conflict with this file unless they explicitly update it.

## 2. Authority Order

Development authority follows this order:

1. Donnie V. Savage, original EIGRP protocol developer and author of RFC 7868.
2. RFC 7868, unless intentionally clarified, extended, or superseded by Donnie V. Savage for this project.
3. FRR only for build compatibility, daemon integration, and library/API compatibility.
4. Existing `eigrpd` code only where it does not conflict with this design specification.

FRR is not the protocol authority for this project. FRR is the current host framework.

When a requested or implemented behavior is not part of RFC 7868, that must be called out so it can be reviewed as one of these cases:

- intentional implementation detail
- intentional protocol clarification
- intentional protocol extension
- possible design mistake

If the behavior clarifies or updates the RFC, the note should eventually be captured in `rfc-current.md`.

## 3. Repository Role

This repository is a project repository, not just the FRR daemon directory.

The daemon source is maintained under:

```text
eigrp/eigrpd/
```

and is staged as a drop-in replacement for:

```text
frr/eigrpd/
```

FRR-native EIGRP tests are maintained under:

```text
eigrp/test/frr/
```

and are staged into:

```text
frr/tests/eigrpd/
```

The expected workflow is:

```sh
git clone https://github.com/frrouting/frr.git frr
git clone git@github.com:diivious/eigrp.git eigrp
cd eigrp
tools/frr.sh --all --frr-root ../frr
```

The project must remain build-compatible with FRR when `eigrp/eigrpd/` is copied into `frr/eigrpd/`.

## 4. FRR Boundary Rule

This project must not pollute or leak into FRR.

Default rule:

```text
Allowed:
- change files under frr/eigrpd
- update eigrpd/subdir.am when needed
- use FRR libraries and APIs from EIGRP adapter code

Not allowed by default:
- require FRR-wide source changes
- change FRR behavior outside eigrpd
- add FRR global assumptions into portable EIGRP logic
- create hidden dependencies on FRR internals outside adapter modules
```

Any change outside `frr/eigrpd` requires explicit approval.

FRR adapter code may use FRR-native objects directly where required. Portable EIGRP logic should not.

## 5. Portability Direction

The code is currently hosted inside FRR, but the design should keep a future BSD port practical.

This does not mean wrapping every FRR object immediately. It means keeping the boundary clean.

Preferred model:

```text
FRR/BSD management -> eigrp_northbound -> EIGRP core -> eigrp_southbound -> FRR/BSD runtime
```

```text
EIGRP core logic      -> EIGRP-owned types and APIs
FRR adapter modules   -> FRR objects, zclient, vty, event loop, zebra integration
Future BSD adapter    -> BSD route/socket/process integration
```

FRR-specific code may be ugly where necessary. EIGRP core code must stay clean.

### 5.1 Northbound and Southbound Boundary

`eigrp_northbound` isolates EIGRP core from host management, configuration, CLI, show, debug, and management API behavior.

`eigrp_southbound` isolates EIGRP core from host runtime and operating-system behavior, including event queues, work queues, timers, sockets, interface state, route installation, and host framework callbacks.

Core EIGRP modules must not call FRR `work_queue`, `struct event`, zebra, VTY, or future BSD APIs directly. Core modules call EIGRP-owned abstractions such as `eigrp_work_queue_enqueue()`. The FRR implementation of those abstractions lives in `eigrp_southbound.[c|h]`; a future BSD implementation replaces only the southbound and northbound adapter files.

## 6. Production-Ready Code Only

No legacy code. No alias code. No temporary compatibility layers.

All new or refactored code must be written as production code.

Forbidden patterns:

```text
legacy_*()
old_*()
compat_*()
alias wrappers for renamed functions
parallel old/new implementations kept alive without an explicit migration reason
```

If code is replaced, update the callers and remove the old path.

This is a hard rule to prevent future cleanup debt.

## 7. Function Naming Rule

For this project, the module name is always:

```text
eigrp
```

All new or refactored EIGRP-owned functions must follow:

```text
eigrp_<object>_<action>()
```

Do not use:

```text
eigrp_<action>_<object>()
```

Correct examples:

```c
eigrp_queue_read();
eigrp_queue_write();
eigrp_packet_encode();
eigrp_packet_decode();
eigrp_neighbor_create();
eigrp_neighbor_delete();
eigrp_metric_calculate();
eigrp_zebra_connected();
```

Incorrect examples:

```c
eigrp_read_queue();
eigrp_write_queue();
eigrp_encode_packet();
eigrp_decode_packet();
eigrp_create_neighbor();
eigrp_calculate_metric();
```

### 7.1 Callback Rule

If the function is implemented by EIGRP code, it uses EIGRP naming even when called by FRR.

FRR may own the caller, registration table, callback signature, or object lifecycle. EIGRP owns the callback function name and implementation.

Example:

```c
void eigrp_zebra_init(void)
{
	zclient = zclient_new(eigrpd_event, &zclient_options_default, eigrp_handlers,
			      array_size(eigrp_handlers));

	zclient_init(zclient, ZEBRA_ROUTE_EIGRP, 0, &eigrpd_privs);
	zclient->zebra_connected = eigrp_zebra_connected;
}
```

`zclient` belongs to FRR. `eigrp_zebra_connected()` belongs to EIGRP.

### 7.2 Static Helper Rule

Static/private helper functions should follow the same naming rule unless there is a strong local readability reason not to.

Do not create broad naming exceptions just because a function is private.

### 7.3 Rename Rule

Do not create giant rename-only commits unless explicitly approved.

Apply naming cleanup to:

- new functions
- touched functions
- materially refactored functions
- functions moved into a clearer module boundary

When a function is renamed, update callers directly. Do not leave alias wrappers.

## 8. Preferred Action Verbs

Use precise action verbs.

Preferred verbs:

```text
create
init
start
stop
read
write
send
receive
encode
decode
parse
build
validate
calculate
update
delete
clear
find
lookup
insert
remove
walk
dump
```

Avoid vague verbs unless there is no better option:

```text
process
handle
do
run
manage
check
```

## 9. Type and Typedef Rules

EIGRP code should prefer EIGRP typedef primitives and EIGRP wrapper types at module boundaries.

Public EIGRP APIs should use EIGRP-owned types for EIGRP-owned objects.

Preferred:

```c
eigrp_instance_t *eigrp;
eigrp_interface_t *ei;
eigrp_neighbor_t *nbr;
eigrp_metrics_t *metric;
eigrp_packet_t *packet;
```

Avoid exposing implementation structs in public EIGRP APIs:

```c
struct eigrp_neighbor *nbr;
struct eigrp_interface *ei;
```

Avoid exposing FRR-native structs in portable EIGRP APIs:

```c
struct interface *ifp;
struct stream *s;
struct event *thread;
```

Exceptions are allowed in FRR adapter modules or where direct FRR integration is the purpose of the function.

## 10. Header Ownership

Headers must have clear ownership.

### 10.1 `eigrp_types.h`

Owns:

- primitive typedefs
- forward declarations
- opaque pointer types
- callback typedefs
- basic EIGRP type aliases needed across modules

This header should be safe to include widely.

### 10.2 `eigrp_structs.h`

Owns:

- shared EIGRP-owned structures
- common internal structures needed by multiple modules
- packet view structs where shared access is truly required

This header must not become a dumping ground.

New structure definitions should go here only when multiple modules require structure details. Otherwise, keep the structure private to the owning `.c` file or the owning module header.

### 10.3 Module Headers

Module headers own exported APIs and exported datatypes for the matching `.c` file or module.

Example:

```text
eigrp_dump.c -> eigrp_dump.h
```

`eigrp_dump.h` should export only dump-related functions and datatypes needed by other files.

## 11. Module Boundaries

Initial module boundary model:

```text
packet      - fixed EIGRP header, checksum, packet framing, packet buffer lifecycle
packetizer  - DUAL/topology work-to-packet scheduling and packet construction orchestration
tlv1        - classic TLV encode/decode
tlv2        - multiprotocol/wide TLV encode/decode
metric      - classic/wide metric math and conversion
neighbor    - neighbor lifecycle and state
interface   - EIGRP interface state and configuration
topology    - topology table records and lookup
fsm         - DUAL state transitions
queue       - packet queue operations
auth        - authentication encode/validate
southbound  - host runtime abstraction: event/work queue, timers, sockets, interfaces, zebra/kernel route hooks
northbound  - host management abstraction: config, CLI/VTY, show/debug, management callbacks
zebra       - FRR/Zebra integration behind the southbound boundary where practical
vty/cli     - CLI and configuration surface behind the northbound boundary where practical
dump        - debug dump and packet/structure print helpers
```

These boundaries are a first pass. They may be refined as code is reviewed.

### 11.1 Topology Object Lifecycle Naming

Topology-owned prefix and route descriptor allocation/free functions belong to the topology module.

Use topology-module names for lifecycle operations:

```c
eigrp_topology_prefix_create();
eigrp_topology_prefix_free();
eigrp_topology_route_create();
eigrp_topology_route_free();
```

Do not use descriptor-first lifecycle names for these objects:

```c
eigrp_prefix_descriptor_new();
eigrp_prefix_descriptor_free();
eigrp_route_descriptor_new();
eigrp_route_descriptor_free();
```

Reason:

```text
- prefix and route descriptors are topology-owned objects
- lifecycle functions should expose the owning module
- this keeps the eigrp_<object>_<action>() rule consistent
- no alias wrappers should be left behind
```

### 11.2 DUAL FSM Active-State Invariant

RFC 7868 defines the Active state as a frozen destination-level computation window.

When a destination enters or remains in Active state, FSM handlers must not update successor selection, Feasible Distance, Reported Distance, current destination distance, or the destination-level reported metric.

Allowed while Active:

- record received neighbor metric information on the route descriptor
- update per-neighbor reported distance and computed distance
- update reply-status/origin-state flags
- enqueue QUERY, REPLY, SIA-QUERY, or SIA-REPLY work

Forbidden while Active:

```text
prefix->fdistance = ...
prefix->rdistance = ...
prefix->distance = ...
prefix->reported_metric = ...
eigrp_topology_update_node_flags(...) for the Active destination
eigrp_update_routing_table(...) for the Active destination
```

Those destination-level fields may be updated only as part of the transition back to Passive. This preserves FD as the loop-free anchor used by the Feasibility Condition.

## 12. Packetization Design Rules

Packet encode/decode must be:

- bounds-safe
- endian-safe
- debuggable
- dumpable
- compatible with FRR stream transmission
- cleanly separated from route/topology selection logic

### 12.1 Internal Host Order vs Wire Order

Internal EIGRP structures use host byte order.

Wire buffers use network byte order.

Every packet field must be converted explicitly at the encode/decode boundary.

No code may assume host byte order equals wire byte order.

### 12.2 Debuggable Packet Buffer Model

Packet construction may encode into an EIGRP-owned packet buffer first, then copy/send the completed buffer through FRR stream APIs.

Reason:

- easier GDB inspection
- easier `eigrp_dump` output
- easier packet validation before transmission
- cleaner packetizer debugging

The packet buffer may be represented by fixed-format structs where safe, but the encoder remains the wire contract.

### 12.3 Packed Struct Rule

Packed wire structs are allowed only for fixed-format packet views.

They are not a substitute for safe parsing.

Variable-length content must be handled with explicit bounds checks, including:

- TLVs
- destination descriptors
- variable-length IPv4/IPv6 prefixes
- extended attributes
- authentication data
- future wide/multiprotocol encodings

No blind cast of untrusted packet bytes may be used as the only validation step.

### 12.4 TLV Rule

TLV parsing must validate:

- minimum header length
- declared TLV length
- remaining packet length
- type-specific minimum length
- variable field length
- alignment requirements where applicable

Malformed TLVs must be rejected safely.

### 12.5 Packetizer Pipeline Direction

The packetizing design will be specified later in `packetizing-spec.md`.

Current intended direction:

```text
rdb/ndb changes
  -> packetizing queue
  -> packetizer
  -> per-interface transmission queue
  -> FRR stream/socket send path
```

Do not overfit this document to that design yet. The future packetizing spec will own the detailed queueing model.

## 13. RFC Handling

RFC 7868 remains attached as the standalone protocol reference.

This file governs development and implementation structure.

When implementation work clarifies, updates, or intentionally differs from RFC 7868, capture the difference in one of these places:

```text
design-spec.md       - development/code structure rule
tlv-spec.md          - TLV-specific design
packetizing-spec.md  - packetizing/queueing design
cli-spec.md          - CLI/VTY/debug command surface, naming, modes, and stub rules
rfc-current.md       - current protocol clarification/update against RFC 7868 sections 1-8
```

`rfc-current.md` should contain only the current working protocol clarifications or updates for RFC 7868 sections 1 through 8. It should not copy the full RFC unless explicitly needed.

## 14. Copyright and Authorship Rule

New files created for this project should identify Donnie V. Savage as copyright owner unless another author is intentionally added.

Existing files must preserve prior author and copyright history.

Do not remove prior authors from existing files during refactor work.

When adding substantial new work to an existing file, add the new copyright/authorship notice without deleting existing notices.

## 15. Style Rule

Donnie V. Savage's project style wins.

FRR style should be followed where it improves adoption, readability, and build compatibility, but FRR style is not a hard authority over this project.

Reason:

- FRR is the current host framework.
- BSD may become a future target.
- EIGRP code should remain readable and maintainable independent of one host project's style preferences.

## 16. Testing Direction

Minimum required gate for all commits:

```text
- FRR builds with replacement eigrpd.
- eigrpd daemon links.
```

Target smoke gate:

```text
- vtysh can enter router eigrp mode.
- show ip eigrp neighbor does not crash.
- packet encode/decode changes include dump, capture, or debug validation.
```

Portable compile smoke gate:

```text
- make -C build
- uses narrow FRR stub headers to catch syntax, prototype, and command macro errors
- does not replace the full FRR build/link gate
```

Longer-term test direction:

```text
- Extend FRR's existing test framework where practical.
- Keep EIGRP core packet/metric tests portable enough to be reused outside FRR.
- Avoid tying all tests permanently to FRR-only infrastructure.
```

Suggested future test layers:

```text
build/link tests
  - FRR builds with replacement eigrpd
  - eigrpd daemon links

CLI smoke tests
  - vtysh enters router eigrp mode
  - basic show commands do not crash

packet tests
  - encode known packet
  - decode known packet
  - reject malformed TLV
  - validate checksum handling
  - validate endian conversion

daemon/topology tests
  - one-router config load
  - two-router neighbor formation
  - update/query/reply smoke
  - route withdrawal smoke
```

Build/link is mandatory now. Smoke and packet tests should be added incrementally without blocking early cleanup work.

## 17. Drift Review Rule

Existing code may already satisfy many of these rules.

Do not assume drift. Review first.

When drift is found, either:

- update the code to match this spec, or
- update this spec if the existing code expresses the better design.

The goal is not cosmetic churn. The goal is a clean, production-ready EIGRP codebase.

## 18. Hard Rules Summary

```text
- Donnie V. Savage is the top project/protocol authority.
- RFC 7868 is the protocol reference unless intentionally clarified or superseded.
- FRR is only the build/library/daemon host authority.
- The `eigrpd/` tree is a drop-in replacement for frr/eigrpd.
- Do not require FRR-wide changes by default.
- Do not pollute FRR or leak FRR assumptions into EIGRP core logic.
- New/refactored functions use eigrp_<object>_<action>().
- EIGRP APIs use EIGRP typedef primitives and wrapper types where practical.
- Header ownership must stay clear.
- No legacy code.
- No alias code.
- No temporary compatibility layers.
- Packet encode/decode must be bounds-safe and endian-safe.
- Internal structures are host order; wire buffers are network order.
- Existing copyrights/authors must be preserved.
- Minimum test gate is FRR build plus eigrpd link.
```

## 19. Delivery Package Rule

All updated project materials must be delivered in a zip file.

This includes:

```text
- source code
- specs
- markdown documents
- tests
- scripts
- configuration files
- generated assets
- any other project file changed for the requested work
```

The zip file must include every changed file needed for the update and must preserve the intended project-relative path structure.

Do not provide loose changed files as the primary delivery format unless explicitly requested.

When only one document is updated, that document still must be placed in a zip file.

Project deliveries should preserve the repository-relative layout, including `eigrpd/`, `specs/`, `tools/`, `build/`, and `test/` paths.

CLI/VTY/debug command-surface rules are owned by `cli-spec.md`.

