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
- `code-conventions.md`
- `refactor-work.md`
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

This repository is a project repository, not just the FRR daemon directory. Canonical source is organized by ownership:

```text
eigrp/eigrpd/       portable/common EIGRP source
eigrp/frr/          FRR-specific adapters, integration payload, patches, and FRR tests
eigrp/bsd/          future BSD-specific adapters/integration
eigrp/test/         portable/common/build-smoke tests
eigrp/specs/        design and protocol specifications
eigrp/tools/        platform build/install/UUT orchestration
```

FRR staging assembles the daemon tree from both common and FRR-specific project source:

```text
eigrp/eigrpd/*      -> frr/eigrpd/
eigrp/frr/*         -> frr/eigrpd/ as appropriate
eigrp/frr/test/*    -> frr/tests/eigrpd/
eigrp/frr/patch/*   -> managed patches applied at the FRR repository root
```

The expected workflow is driven by `tools/frr.sh` / `tools/frr-install.sh`; canonical project files are not moved as a side effect of staging. FRR-wide patches must be applied idempotently and must fail on unrecognized source drift rather than being silently fuzzed or forced. Patch application is explicit: `tools/frr.sh --patch` is the only driver action that applies managed FRR-wide patches. Install, build, check, configure, and UUT actions may restage project source and tests but must not apply or modify FRR-wide patches.

The project must remain build-compatible with FRR while keeping common EIGRP code independent of FRR-native types and lifecycle assumptions.

## 4. FRR Boundary Rule

This project must not pollute or leak FRR-specific assumptions into portable EIGRP logic. FRR is the current host and integration target.

Canonical ownership is:

```text
eigrp/eigrpd/    portable/common EIGRP code
eigrp/frr/       FRR-specific adapters/integration and managed FRR patches
```

FRR-specific source may use FRR-native libraries and objects where required. Portable/common EIGRP source must not depend on FRR-native CLI, VTY, YANG, Zebra, event, stream, or interface objects at its public/core boundaries.

Changes outside the staged FRR `eigrpd/` tree are exceptional. When required, they must be explicit managed patches under `eigrp/frr/patch/`, applied by the project tooling with idempotent forward/reverse detection. A patch that is neither applicable nor already applied must stop so source drift can be reviewed; do not silently fuzz or force it.

FRR-wide changes should be kept as small as practical and must not alter unrelated FRR behavior.

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

The host-facing files have distinct responsibilities. `eigrp_cli.[c|h]` and `eigrp_vty.[c|h]` are the FRR user/management front ends. They own command syntax, mode transitions, VTY presentation, show/clear/debug presentation, running-configuration interaction, and submission into FRR management machinery. They are allowed to use FRR-native CLI, VTY, YANG, and management objects.

`eigrp_northbound.c` is the FRR management-to-EIGRP adapter. It owns the committed configuration/application boundary: host/YANG management state is translated into normalized EIGRP-owned values and then applied through the real EIGRP feature target functions. CLI/VTY front ends are not themselves the portable core boundary and must not bypass the northbound application path by directly mutating portable EIGRP runtime state after a management transaction.

`eigrp_southbound.[c|h]` is the EIGRP-to-host runtime adapter. It isolates EIGRP core from host runtime and operating-system behavior, including event queues, work queues, timers, sockets, interface state, and host framework callbacks.

`eigrp_zebra.[c|h]` is FRR-specific southbound/RIB integration. Zebra-specific route, interface, and RIB objects may remain in that adapter. They must not leak into portable EIGRP target APIs merely because Zebra is the current host.

The boundary is therefore:

```text
user / management
  -> eigrp_cli / eigrp_vty
  -> FRR management / YANG machinery
  -> eigrp_northbound
  -> normalized EIGRP-owned APIs and core state
  -> eigrp_southbound / eigrp_zebra
  -> FRR / operating system runtime
```

Core EIGRP modules must not call FRR `work_queue`, `struct event`, zebra, VTY, YANG callbacks, or future BSD APIs directly. Core modules call EIGRP-owned abstractions such as `eigrp_work_queue_enqueue()`. The FRR implementation of those abstractions lives in the appropriate adapter module; a future BSD implementation replaces the host adapters without changing packetizer, DUAL, topology, metric, neighbor, or TLV core logic.

### 5.2 Normalized Management Boundary

Northbound code is responsible for translating committed host management objects and configuration values into EIGRP-owned data before invoking portable EIGRP logic.

The required configuration application path is:

```text
CLI / management input
  -> retain/commit host configuration
  -> eigrp_northbound callback
  -> normalize into EIGRP-owned data
  -> real EIGRP feature target function
  -> EIGRP-owned structured result
  -> host adapter renders/logs the result
```

A CLI command may perform syntax normalization needed to construct the host management transaction, but the portable runtime side effect belongs to the northbound callback or a portable target called by it. The CLI must not perform a second direct runtime mutation after submitting the same configuration change through northbound management.

Portable EIGRP target functions must not receive a host object merely recast or typedefed with an `eigrp_` name. A wrapper is acceptable only when it owns or exposes a stable EIGRP-defined representation independent of the host implementation.

Examples of host objects that must stop at the adapter boundary include FRR `struct vty`, `struct stream`, `struct interface`, `struct event`, YANG/libyang nodes and callback objects, Zebra-native objects, and equivalent host-specific management/runtime structures.

Address-family-neutral EIGRP objects should carry an explicit AF/type and normalized address/prefix data so the same core API can operate on IPv4 or IPv6 where the protocol semantics are otherwise identical.

### 5.3 Adapter Header Rule

Do not create adapter headers merely for naming symmetry. `eigrp_northbound.h` is required only if another module needs a genuine exported northbound API. FRR/YANG callback registration may continue to be exposed through the existing YANG ownership where appropriate, while callback implementation details remain private to `eigrp_northbound.c`.

## 6. Production-Ready Code Only

No new legacy code, alias code, or temporary compatibility layers may be introduced.

Existing classic EIGRP CLI that is explicitly preserved by `cli-spec.md` is a compatibility exception and must not be expanded as part of named-mode development. Preserving that existing command surface does not authorize new legacy wrappers, duplicate implementations, or compatibility aliases in the EIGRP core.

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

## 7. Function and Module Naming Rule

The primary purpose of the naming convention is human code navigation. A developer who knows the EIGRP protocol area should be able to predict the source file to inspect and the function prefix to search without already knowing the implementation.

The normal pattern is:

```text
eigrp_<module>.c
eigrp_<module>.h
eigrp_<module>_<object>_<action>()
```

The module name and function prefix should normally align. The object/detail narrows the operation and the action normally appears last.

Correct examples:

```c
/* eigrp_query.c */
eigrp_query_receive();
eigrp_query_send();

/* eigrp_interface.c */
eigrp_interface_create();
eigrp_interface_delete();

/* eigrp_metric.c */
eigrp_metric_calculate();
eigrp_metric_weights_set();
eigrp_metric_weights_reset();
eigrp_metric_variance_set();
eigrp_metric_variance_reset();
```

Do not invert the action and object merely to make a phrase read like English:

```c
eigrp_metric_set_variance();     /* avoid */
eigrp_create_neighbor();         /* avoid */
eigrp_decode_packet();           /* avoid */
```

Do not derive portable function names from the CLI/YANG hierarchy. A command that appears beneath `topology base` does not automatically belong to the `eigrp_topology_*` namespace. The function namespace follows the implementation/protocol module a human would reasonably search.

File proliferation must be balanced against naming consistency. Closely related small feature families may share a file when splitting them would create trivial modules. Existing `eigrp_filter.c` containing `eigrp_distribute_*` and `eigrp_offset_*` is an intentional model: the searchable feature prefixes remain distinct even though their implementations are grouped. Exceptions should remain uncommon and defensible.

See `code-conventions.md` for the detailed convention.

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

Static/private helpers should normally follow the same module navigation prefix. A private shared helper may use an internal operation enum when that reduces duplicated implementation, but the public feature targets remain explicit and searchable.

Do not create broad naming exceptions merely because a function is private.

### 7.3 Rename Rule

Do not create giant rename-only commits unless explicitly approved.

Apply naming cleanup to:

- new functions
- touched functions
- materially refactored functions
- functions moved into a clearer module boundary

When a function is renamed, update callers directly. Do not leave alias wrappers.

### 7.4 Feature Target Function Rule

Management and CLI work must terminate in the real EIGRP function that owns the requested feature. Do not create a generic CLI stub, generic `not configured`, generic `not implemented`, or unrelated-command dispatcher.

Examples of real target namespaces include:

```c
eigrp_metric_variance_set(...);
eigrp_metric_variance_reset(...);
eigrp_redistribute_add(...);
eigrp_redistribute_remove(...);
eigrp_neighbor_clear(...);
eigrp_packet_debug(...);
```

A target whose runtime body is incomplete still exists under its real module/feature name and may return the structured EIGRP `not implemented` result. Implementing the feature later must extend that target rather than redesign the CLI call path.

Prefer separate public functions for distinct actions such as `set/reset`, `add/remove`, and `create/delete`. An internal helper may combine those operations when useful.

IPv4 and IPv6 do not require separate target functions when one AF-aware EIGRP object can represent both correctly. Duplicate AF-specific functions should exist only when the behavior is genuinely address-family specific.

## 8. Preferred Action Verbs

Use precise action verbs and normally place the action last.

Preferred verbs include:

```text
create / delete        object lifecycle
add / remove           keyed collection membership
set / reset            retained configuration and return-to-default
init / start / stop    lifecycle phases
read / write
send / receive
encode / decode
parse / build / validate
calculate / update
find / lookup
insert / walk / dump
clear                   operational state/counters/neighbors, not normal config reset
```

Avoid vague verbs unless there is no better protocol-specific action:

```text
process
handle
do
run
manage
check
```

`reset` is preferred for a configuration `no` operation that restores a default. `clear` should normally mean an operational action such as clearing neighbors, counters, or protocol state.

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

### 9.1 EIGRP Result Types

Portable EIGRP APIs that can fail or report incomplete support should return an EIGRP-owned structured result/status rather than reducing every outcome to a generic boolean success/failure value.

The result model must be able to distinguish conditions such as:

```text
success
not implemented
invalid input/configuration
not found
conflict
unsupported AF/capability
internal failure
```

The EIGRP core determines the semantic result. Northbound/southbound adapters decide how that result is rendered through VTY, logs, management APIs, or a future BSD host interface.

Core code must not call `vty_out()` or otherwise depend on FRR just to communicate an operation result.

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

### 11.1 DUAL Topology Descriptor Terminology

The DUAL topology database owns two related descriptor object classes:

```text
prefix_descriptor    destination/prefix-level topology object
route_descriptor     one path/neighbor-derived route for that prefix
```

Cisco historically used DNDB/NDB and DRDB/RDB terminology for these concepts. That terminology is concise, familiar in EIGRP debugging, and avoids collision with a host platform's broader use of `route`. The current descriptive names also provide useful separation from Cisco's historical implementation naming.

The final source/API naming convention for these descriptor blocks is deliberately deferred to the pre-production review in `refactor-work.md`. Until then:

- do not perform rename-only churn in this area;
- preserve current names unless a functional change requires touching them;
- avoid introducing new ambiguous bare `route` APIs where a topology descriptor, prefix, Zebra RIB route, or kernel route is actually meant;
- comments/debug output may identify the DNDB/DRDB terminology where that improves understanding.

Topology remains the owning protocol module for DUAL topology database operations. The exact final lifecycle/member verbs (`create/delete`, `add/remove`, etc.) should follow the actual ownership relationship when the pre-production naming review is performed.

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
cli-spec.md          - CLI/VTY/debug command surface, modes, retention, and target rules
code-conventions.md  - human-navigation function/module naming conventions
refactor-work.md     - deferred pre-production architectural/naming cleanup
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
- make -C test/build
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
- FRR staging assembles frr/eigrpd from portable `eigrpd/` plus FRR-specific `frr/` source.
- FRR-wide changes are exceptional and must be managed under `frr/patch/`.
- Do not pollute FRR or leak FRR assumptions into EIGRP core logic.
- New/refactored functions follow the navigation-first eigrp_<module>_<object>_<action>() convention, with deliberate grouped-module exceptions documented by code-conventions.md.
- EIGRP APIs use EIGRP-owned normalized types rather than host structs at portable boundaries.
- CLI/management input is normalized before entering EIGRP core logic.
- Each feature calls its real EIGRP target function; no generic CLI stub dispatcher.
- Portable operations return EIGRP-owned structured results rather than only pass/fail.
- Header ownership must stay clear.
- No new legacy code; existing classic CLI is preserved only as defined by cli-spec.md.
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

