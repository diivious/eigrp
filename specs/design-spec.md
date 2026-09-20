# EIGRP Architecture and Development Specification

Copyright (C) 2026 Donnie V. Savage

Existing source files must preserve prior copyright notices, SPDX identifiers,
and author history. Refactoring an existing file is not permission to remove
prior authorship.

## 1. Purpose

This document defines the architectural rules for the EIGRP implementation in
this repository. It owns repository boundaries, portability, management/runtime
separation, common data ownership, module responsibilities, protocol-safe packet
handling, testing expectations, and delivery rules.

Focused specifications extend this document:

- `code-conventions.md` — source/module/function naming.
- `cli-spec.md` — classic/named CLI and management behavior.
- `process-spec.md` — named parent, address-family, and runtime ownership.
- `packetizing-spec.md` — DUAL-to-packet pipeline and reliable transport.
- `refactor-work.md` — deliberately deferred pre-production cleanup.

`EIGRP-Config-Guide.md` and `EIGRP-Named-Mode.md` are reference notes, not
implementation authority.

## 2. Authority

Protocol and design authority is, in order:

1. Donnie V. Savage's explicit project design decisions.
2. RFC 7868 for EIGRP protocol behavior.
3. FRR or BIRD only for host/integration contracts required by those projects.
4. Existing implementation code where it does not conflict with the above.

FRR and BIRD are host frameworks. Neither defines EIGRP protocol behavior.

Behavior outside RFC 7868 must be identifiable as one of:

- implementation detail with no wire/protocol effect;
- intentional protocol clarification;
- intentional protocol extension;
- defect requiring correction.

Do not silently adopt host-framework behavior as EIGRP protocol behavior.

## 3. Repository ownership

Canonical ownership is:

```text
eigrp/
  eigrpd/         portable/common EIGRP protocol code
  frr/            FRR-specific adapters/integration
    patch/        managed FRR-wide changes
    test/         FRR-native integration/UUT material
  bird/           BIRD-specific adapters/integration
  test/
    build/        lightweight compile-smoke harness
    common/       shared host-independent fixtures
    portable/     host-independent tests
  specs/          architecture and protocol implementation specifications
  tools/          host staging/build/UUT/packaging orchestration
```

The project tree is canonical. A staged FRR `eigrpd/` directory is a generated
projection, not a second source tree.

FRR staging assembles:

```text
eigrpd/* + frr daemon adapter files -> FRR/eigrpd/
frr/test/*                          -> FRR/tests/eigrpd/
```

Changes outside FRR's staged `eigrpd/` tree are exceptional. They are carried
as managed patches under `frr/patch/`, applied in explicit dependency order,
and must be idempotent. If a patch is neither cleanly applicable nor recognized
as already applied, patching stops for source-drift review. Do not fuzz or force
managed patches.

## 4. Portability boundary

Portable protocol modules must use EIGRP-owned APIs and data structures. Host
objects stop at host adapters.

The architectural flow is:

```text
host CLI/configuration
  -> host front end
  -> host management transaction
  -> EIGRP northbound adapter
  -> normalized EIGRP-owned configuration/state
  -> EIGRP protocol modules
  -> EIGRP southbound contract
  -> host runtime/RIB/socket/event implementation
```

For FRR:

```text
frr/eigrp_cli_*.c / frr/eigrp_vty.c
  -> FRR YANG/northbound machinery
  -> frr/eigrp_northbound.c
  -> eigrpd/*
  -> eigrpd/eigrp_southbound.h
  -> frr/eigrp_southbound.c
  -> frr/eigrp_zebra.c / FRR runtime APIs
```

The BIRD adapter implements equivalent host services under `bird/` without
changing portable DUAL, topology, metric, packetizer, TLV, neighbor, network,
or reliable-transport APIs.

### 4.1 Northbound responsibilities

The host northbound adapter:

- receives committed host configuration;
- converts host/YANG values to EIGRP-owned types;
- resolves host-owned objects by stable EIGRP inputs such as names/IDs;
- invokes the real EIGRP feature target;
- translates the EIGRP structured result into host management semantics.

The CLI parser may normalize text to construct a host transaction. It must not
submit that transaction and then perform a second direct mutation of the same
portable runtime state.

### 4.2 Southbound responsibilities

The EIGRP southbound contract owns requests from portable protocol code to host
runtime services, including:

- runtime instance lifecycle;
- event/read/write scheduling and timers;
- work queues;
- socket creation and multicast operations;
- interface discovery/state refresh;
- policy/filter evaluation;
- redistribution subscriptions;
- route installation/removal;
- host RIB lifecycle.

Portable modules must not directly call FRR `work_queue`, `struct event`,
Zebra, VTY, libyang, interface, or equivalent BIRD APIs.

`frr/eigrp_zebra.[c|h]` is an FRR-private RIB adapter behind the southbound
boundary. Portable code does not call `eigrp_zebra_*()` directly and does not
construct Zebra route objects.

### 4.3 Host objects that must not cross into portable APIs

Examples include:

```text
FRR struct vty
FRR struct stream
FRR struct interface
FRR struct event
FRR/YANG/libyang callback objects
Zebra zapi_route / zapi_nexthop
FRR route-map/access-list/prefix-list objects
BIRD configuration/routing-table/event-loop/interface/socket/timer objects
```

A wrapper with an `eigrp_` name is not portable if its representation or
lifecycle still depends on a host object's layout.

Standard socket/networking primitives such as `AF_INET`, `AF_INET6`, `in_addr`,
`in6_addr`, and `sockaddr` are not host-framework leaks by themselves.

## 5. EIGRP-owned data model

Portable address-family-aware objects carry explicit AF identity and normalized
EIGRP address/prefix data.

Use EIGRP-owned types at protocol boundaries, including:

```text
eigrp_instance_t
eigrp_interface_t
eigrp_neighbor_t
eigrp_prefix_t
eigrp_address_t / eigrp_addr_t
eigrp_metrics_t / metric value structures
eigrp_result_t
```

IPv4 and IPv6 share a target/API when the protocol semantics are identical.
Create AF-specific implementation functions only when the protocol behavior or
wire/runtime operation is genuinely AF-specific.

Host/RIB route objects are not DUAL topology objects. Keep these concepts
separate:

```text
DUAL prefix descriptor
DUAL route/path descriptor
EIGRP learned routing information
EIGRP southbound route-install snapshot
host RIB route
kernel route
```

## 6. Feature target and result contract

Every configuration or operational feature terminates at its own real EIGRP
feature target. Do not create generic CLI stubs, generic `not configured`
handlers, generic `not implemented` dispatchers, or unrelated-command routers.

Examples:

```c
eigrp_metric_variance_set(...);
eigrp_metric_variance_reset(...);
eigrp_redistribute_add(...);
eigrp_redistribute_remove(...);
eigrp_neighbor_clear(...);
```

An incomplete feature still owns its real target. That target may return
`EIGRP_RESULT_NOT_IMPLEMENTED` without changing the CLI/northbound architecture.

Portable operations return EIGRP-owned structured results capable of
representing at least:

```text
success
not implemented
invalid input/configuration
not found
conflict
unsupported address family/capability
internal failure
```

The portable core determines semantic result. Host adapters decide how that
result is rendered through CLI, logs, or host management APIs. Portable core
code must not call `vty_out()` merely to report a result.

For retained configuration, host configuration is committed before runtime
application. A missing runtime capability does not silently discard valid
configuration. This rule is especially important for named IPv6 configuration
while its data path is capability-gated.

## 7. Source/module naming

Human navigation is the primary naming goal. The normal pattern is:

```text
eigrp_<module>.c
eigrp_<module>.h
eigrp_<module>_<object>_<action>()
```

The module/function prefix normally aligns, the object/detail narrows the
operation, and the action appears last.

Portable function names follow the protocol/module owner, not Cisco CLI or YANG
nesting. A command under `topology base` that changes metric behavior belongs in
the metric namespace.

Closely related small feature families may share a file while retaining their
predictable public prefixes. `eigrp_filter.c` containing `eigrp_distribute_*`
and `eigrp_offset_*` is the model.

Public action naming and rename discipline are defined by
`code-conventions.md`.

## 8. Production code rule

Do not add unapproved compatibility debt.

Forbidden without an explicit migration reason:

```text
legacy_* or old_* replacement paths
alias wrappers for renamed internal APIs
parallel old/new protocol implementations
generic compatibility dispatchers
temporary host-object leakage into portable APIs
```

Existing classic EIGRP CLI is retained as a compatibility surface as defined by
`cli-spec.md`; that does not authorize duplicate portable implementations.

When replacing an internal API, update callers directly unless a separately
approved compatibility contract requires otherwise.

## 9. Header ownership

### 9.1 `eigrp_types.h`

Owns broadly reusable primitives, forward declarations, opaque types, callback
typedefs, and basic EIGRP type aliases.

### 9.2 `eigrp_structs.h`

Owns shared EIGRP structures whose layouts are required by multiple portable
modules. It must not become a general dumping ground.

Keep structure definitions private to an owning `.c` file or module header when
other modules do not require the layout.

### 9.3 Module headers

Each module header exports only the public API and public datatypes needed by
other modules for that subsystem.

Host adapter headers export only genuine adapter contracts. Do not create
headers merely for naming symmetry.

## 10. Protocol module ownership

The stable ownership model is:

```text
instance      named/config/runtime instance ownership
network       configured network participation
interface     EIGRP interface state and interface-scoped behavior
neighbor      neighbor lifecycle/state/policy
metric        classic/wide metric calculation and metric configuration
summary       manual/automatic summary behavior
filter        distribute/offset filtering feature families
redistribute  redistribution configuration and portable state
topology      DUAL topology database records/lookup/route selection ownership
fsm           DUAL state transitions
query         QUERY behavior
reply         REPLY behavior
siaquery      SIA-QUERY behavior
siareply      SIA-REPLY behavior
update        UPDATE behavior and initialization/resync update walking
packetizer    DUAL/topology work-to-packet orchestration
packet        packet buffer/header/checksum/reliable send queue mechanics
tlv1          classic route TLV wire codec
tlv2          multiprotocol/wide route TLV wire codec
auth          authentication behavior and packet authentication data
eventlog      EIGRP event-log state
statistics    protocol counters/accounting
status        protocol/tech-support state presentation inputs
southbound    portable core-to-host runtime/RIB contract
```

FRR-only ownership remains under `frr/`:

```text
cli/vty       FRR command parsing and presentation
northbound    FRR committed-config to EIGRP adapter
policy        FRR policy object lookup/evaluation/callback integration
southbound    FRR implementation of portable runtime services
zebra         FRR Zebra/RIB implementation
frr           daemon/platform glue
```

## 11. DUAL topology invariants

### 11.1 Descriptor terminology

The topology database currently uses:

```text
prefix_descriptor    destination-level DUAL topology object
route_descriptor     one neighbor/path descriptor beneath that destination
```

Cisco historical DNDB/NDB and DRDB/RDB terminology remains useful for EIGRP
navigation/debugging. Final source/API naming is deliberately parked in
`refactor-work.md`.

Until that review:

- do not perform rename-only churn;
- do not introduce new ambiguous bare `route` APIs;
- preserve the distinction between topology descriptors and host/RIB routes.

### 11.2 DUAL FSM Active-State Invariant

RFC 7868 requires destination-level state to remain frozen while a destination
is Active.

While Active, protocol handlers may update per-neighbor/path observations and
reply/origin bookkeeping, and may enqueue QUERY/REPLY/SIA work. They must not
change destination-level successor selection, Feasible Distance, destination
reported distance, current destination distance, or destination reported
metric until the transition back to Passive.

This preserves FD as the Feasibility Condition loop-free anchor.

## 12. Packet and TLV rules

Packet encode/decode must be:

- bounds-safe;
- endian-safe;
- debuggable/dumpable;
- separated from topology/route-selection decisions;
- compatible with the host send/receive adapter without making host stream
  objects the protocol data model.

### 12.1 Byte order

Internal EIGRP values use host order unless a type explicitly documents
otherwise. Wire buffers use network order. Every multibyte wire field is
converted at the encode/decode boundary.

### 12.2 Packed structures

Packed structures may describe fixed wire views. They do not replace safe
parsing. Variable-length TLVs, prefixes, attributes, authentication data, and
multiprotocol fields require explicit length/bounds validation.

### 12.3 TLV validation

Every decoder validates:

- minimum TLV header length;
- declared length against remaining packet bytes;
- type-specific minimum size;
- variable-field size/prefix length;
- alignment where the encoding requires it.

Malformed TLVs fail safely and must not advance receive loops into an invalid or
non-progressing state.

### 12.4 Route TLV ownership

`eigrp_tlv1` and `eigrp_tlv2` own their private wire representations. Packetizer,
DUAL, topology, CLI, and reliable transport operate on native EIGRP data and do
not depend on private TLV1/TLV2 structure layouts.

The packetizer architecture is defined in `packetizing-spec.md`.

## 13. CLI and process scope

Classic and named CLI behavior is defined by `cli-spec.md`. CLI/VTY/debug
command-surface rules are owned by `cli-spec.md`.

The named process/address-family runtime model is defined by `process-spec.md`.

EIGRP Stub routing is explicitly outside project scope. Do not implement Stub
runtime behavior, import it from FRR/Cisco code, or add tests that validate the
EIGRP Stub feature. Existing external/reference mentions do not create a project
requirement.

## 14. Testing contract

Tests are separated by dependency ownership:

```text
test/build/       lightweight compile/syntax/prototype smoke
test/common/      host-independent fixtures
test/portable/    host-independent behavior/source-boundary tests
frr/test/         FRR-native integration/UUT tests
bird/...          BIRD-native integration/UUT tests
```

The lightweight smoke harness does not replace a complete FRR build/link.

For source changes, the normal gate is:

```text
1. make test
2. FRR stage/build/link succeeds
3. relevant live/FRR-native UUT coverage succeeds
```

Changes to CLI/configuration require validation of parsing, mutation,
running-config writeback, and applicable `no` forms. Protocol packet changes
require encode/decode/bounds/endian validation appropriate to the changed path.

Do not make portable tests depend on FRR-only types or lifecycle when the
behavior under test is protocol-owned.

## 15. Drift and refactor discipline

Do not refactor merely because a specification describes a cleaner final name.
When code and design differ:

- fix the code when the design rule is intentional and the touched work makes
  the correction appropriate; or
- update the specification when the implementation demonstrates a better
  architecture.

Deferred naming/ownership work belongs in `refactor-work.md`. That file is a
parking lot, not authorization for unrelated broad cleanup during feature work.

## 16. Copyright and authorship

New project files use Donnie V. Savage as copyright owner unless another author
is intentionally identified.

Existing source must preserve its prior copyright, SPDX, and author history.
Substantial new work may add authorship/copyright without removing existing
history.

## 17. Delivery

Requested project changes are delivered as a ZIP rooted at `eigrp/` so they can
be copied into another checkout without inventing alternate paths.

The delivery includes every changed source, specification, test, script,
configuration, and supporting project file required by the change. Local VCS,
build, and cache artifacts are not part of the delivery.
