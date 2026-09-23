# EIGRP Architecture and Development Specification

Copyright (C) 2026 Donnie V. Savage

Existing source files must preserve prior copyright notices, SPDX identifiers,
and author history. Refactoring an existing file is not permission to remove
prior authorship.

## 1. Purpose

This document is the contributor specification for portable EIGRP core code.
It answers one question: what does a developer need to know before changing
`eigrpd/` or changing behavior that belongs to EIGRP itself?

Process, style, AI use, and how to send a diff are in `../CONTRIBUTING.md`.
Read that before you generate a patch. This file owns design rules. It does
not own review process.

It owns:

- protocol and design authority;
- repository and module ownership;
- portable versus host-specific boundaries;
- EIGRP-owned data and result contracts;
- source, file, and public symbol naming;
- instance, address-family, and runtime ownership;
- configuration-to-core rules;
- packet/TLV safety rules;
- testing, refactor, and delivery rules.

Detailed protocol subsystems are described separately:

- `dual.md` describes how this implementation uses the DUAL state machine;
- `rtp-spec.md` describes packetization and Reliable Transport Protocol behavior;
- `rfc7868.md` identifies the protocol specification used by this project;
- `integration-spec.md` defines the public black-box contract for host platforms;
- `EIGRP-Config-Guide.md` is the operator configuration and EXEC guide;
- `refactor-work.md` is the bounded pre-production parking lot.

## 2. Authority

Protocol and design authority is, in order:

1. Donnie V. Savage's explicit project design decisions.
2. RFC 7868 for EIGRP protocol behavior.
3. FRR or BIRD only for host/integration contracts required by those projects.
4. Existing implementation code where it does not conflict with the above.

FRR and BIRD are host frameworks. Neither defines EIGRP protocol behavior.

Behavior outside RFC 7868 must be identifiable as an implementation detail with
no wire/protocol effect, an intentional clarification or extension, or a defect
that needs correction. Host-framework convenience is not protocol authority.

## 3. Repository ownership

```text
eigrp/
  eigrpd/         portable/common EIGRP protocol code
  frr/            FRR-specific adapters/integration
    patch/        managed FRR-wide changes
    test/         FRR-native integration/UUT material
  bird/           reserved BIRD adapter location
  test/
    build/        lightweight compile-smoke harness
    common/       shared host-independent fixtures
    portable/     host-independent tests
  specs/          project specifications and protocol references
  tools/          host staging/build/UUT/packaging orchestration
```

The project tree is canonical. A staged FRR `eigrpd/` directory is generated
output, not a second source tree.

FRR staging assembles portable source plus FRR adapter files into the FRR tree.
Changes outside FRR's staged daemon tree are exceptional and must be carried as
managed patches under `frr/patch/`. Managed patches are applied idempotently.
If a patch does not apply cleanly and is not already applied, stop for source
review. Do not fuzz or force it.

## 4. Core and platform boundary

Portable protocol modules use EIGRP-owned APIs and data structures. Host-native
objects stop at platform adapters.

```text
host CLI/configuration
  -> host management/front end
  -> platform configuration adapter
  -> EIGRP-owned semantic API
  -> portable EIGRP modules
  -> public host service / RIB contract
  -> platform adapter
  -> host runtime, sockets, RIB, policy, event loop
```

The platform adapter must convert host/system objects, values, and events to or from EIGRP-owned types, abstract host/system calls and services, and provide only the host mechanisms portable EIGRP requires. This boundary must prevent EIGRP behavior from being reimplemented separately for each host platform. It does not own protocol decisions that should be identical on every platform.

If FRR, BIRD, macOS, and another host should reach the same decision from equivalent
normalized inputs, that decision belongs in `eigrpd/`.

### 4.1 Boundary validation

External data is validated and normalized at the boundary where it enters EIGRP. Common protocol code must not repeatedly defend against the same malformed host input after that boundary has accepted it. Once a
required vector or normalized EIGRP value passes that boundary, portable code
may treat the contract as an invariant.

Do not scatter required-vector NULL checks through core consumers. A missing required callback is a binding/programming error and is diagnosed at bind time. Real
optional state, such as an unarmed timer, remains explicitly optional.

Wire packets are themselves untrusted external input. Header, length, TLV, prefix,
address-family, and semantic validation stays in the receive/decode path before
native data is consumed by DUAL, topology, or other protocol modules.

### 4.2 Northbound rule

A host configuration adapter receives committed host configuration, normalizes
host values, calls the real EIGRP feature target, and translates
`eigrp_result_t` for the host.

A CLI must not submit a management transaction and then separately mutate the
same portable state.

### 4.3 Southbound/system rule

Portable EIGRP reaches host services only through the public integration
contract described by `integration-spec.md`. Host objects such as FRR VTY,
libyang, Zebra routes, FRR events/interfaces/streams, and equivalent BIRD
objects do not cross into core APIs.

Standard networking primitives such as `AF_INET`, `AF_INET6`, `in_addr`,
`in6_addr`, and `sockaddr` are not host-framework leaks by themselves.

## 5. EIGRP-owned data model

Portable AF-aware objects carry explicit address-family identity and normalized
EIGRP address/prefix data. Public/core boundaries use EIGRP-owned types such as:

```text
eigrp_instance_t
eigrp_interface_t
eigrp_neighbor_t
eigrp_address_t
eigrp_prefix_t
eigrp_metrics_t
eigrp_result_t
```

IPv4 and IPv6 share a public/core target when their protocol semantics are the
same. AF-specific vectors or functions are used only where protocol behavior or
wire/runtime mechanics genuinely differ.

Do not confuse these domains:

```text
DUAL destination descriptor
DUAL path descriptor
EIGRP learned route information
public RIB install snapshot
host RIB route
kernel route
```

## 6. Feature targets and structured results

Every configuration or operational feature terminates at its own real EIGRP
semantic target. The host adapter must call the real EIGRP semantic target rather
than another CLI surface or a generic dispatcher. Do not add generic CLI stubs, generic not-configured handlers,
generic not-implemented dispatchers, or unrelated command routers.

An incomplete feature still owns the correct target and may return
`EIGRP_RESULT_NOT_IMPLEMENTED` there. This keeps the architecture stable while
runtime capability is filled in.

Portable semantic results include success, not implemented, invalid argument,
not found, conflict, unsupported capability, and internal failure. The core
returns the semantic result. The platform decides how to display or log it.

Valid retained configuration is not discarded merely because runtime
application reports a missing capability.

## 7. Human-navigation naming

A developer who knows the protocol area should be able to predict the source
file and symbol prefix to search.

Normal pattern:

```text
eigrp_<module>.c
eigrp_<module>.h
eigrp_<module>_<object>_<action>()
```

Examples:

```c
eigrp_query_receive();
eigrp_query_send();
eigrp_interface_create();
eigrp_interface_delete();
eigrp_interface_shutdown_set();
eigrp_interface_shutdown_reset();
eigrp_instance_parent_create();
eigrp_instance_address_family_delete();
eigrp_metric_variance_set();
eigrp_metric_variance_reset();
```

The module/function prefix normally aligns. Object/detail follows the module
name. The action normally appears last.

Do not derive portable names from Cisco CLI or YANG nesting. A command under
`topology base` that changes metric behavior belongs in `eigrp_metric_*`, not
in `eigrp_topology_*` simply because of command placement.

### 7.1 Grouped modules

Avoid needless file proliferation. Closely related small feature families may
share a source file while retaining distinct searchable prefixes. The model is:

```text
eigrp_filter.c
  eigrp_distribute_*
  eigrp_offset_*
```

Do not mechanically rename those functions to `eigrp_filter_*`.

### 7.2 Action verbs

Use the verb that matches ownership and semantics:

```text
create / delete   object lifecycle
add / remove      collection membership or protocol relationship
set / reset       retained configuration and its no/default form
init / finish     module/subsystem initialization
start / stop      runtime operation lifecycle
enable / disable  literal capability/runtime state
attach / detach   ownership/binding relationship
install / remove  route or host-state installation
send / receive    protocol messages
encode / decode   wire conversion
parse / build     representation construction
validate          semantic/wire validation
calculate         derived-value computation
find / lookup     object retrieval
walk              enumeration
clear             operational state, counters, or neighbors
```

Prefer separate public `set/reset`, `add/remove`, and `create/delete` functions
over a public generic operation enum. Private helpers may share implementation.

Avoid vague public verbs such as `process`, `handle`, `do`, `run`, or `manage`
when a protocol-specific action is available.

### 7.3 Address-family naming

Do not duplicate public IPv4/IPv6 APIs when one AF-aware EIGRP object correctly
represents both. AF-specific behavior belongs in AF implementation vectors when
appropriate.

### 7.4 Rename discipline

Apply the convention to new APIs and to materially touched APIs. Do not create
rename-only churn during unrelated feature work. Approved renames update callers
directly. Do not leave alias wrappers without an explicit migration reason.

The topology descriptor naming decision remains parked in `refactor-work.md`.
Until then preserve `prefix_descriptor` and `route_descriptor`, avoid ambiguous
new bare `route` APIs, and do not perform rename-only churn.

### 7.5 Public API granularity

A public symbol represents a semantic action, not every possible value of an
attribute used by that action. If several functions have the same contract and
differ only by a level, category, codec family, time unit, or similar selector,
prefer one public API with an EIGRP-owned typed selector or one canonical unit.

Keep separate public functions when ownership, lifecycle, side effects, argument
contracts, or protocol meaning differ. `set/reset`, `add/remove`, and
`create/delete` remain distinct semantic actions and are not collapsed merely to
reduce symbol count.

## 8. Instance, address-family, and runtime ownership

A named EIGRP parent is a local configuration object containing one or more
address-family protocol contexts.

```text
router eigrp <name>
  address-family <afi> [vrf <vrf>] autonomous-system <asn>
```

The parent name is local configuration identity. It is never an on-wire EIGRP
identity.

Each configured address family owns one context identified locally by:

```text
{name, address-family, VRF, AS}
```

The address-family configuration binds to one `eigrp_instance_t` runtime. Child
network, interface, topology, filter, redistribution, metric, neighbor, and
summary targets consume that binding. They must not independently discover or
create a second runtime.

### 8.1 Receive identity

Packet acceptance is based on protocol context, including receiving VRF/socket,
packet address family, receiving interface, AS number, and topology/VRID where
applicable. The local named-parent string is not a receive demultiplexing key.

Configuration must not create an ambiguous receive identity for the same
`{VRF, AF, AS, interface, topology}` context.

### 8.2 Runtime capability

A configured address family always has configuration/control identity. Runtime
packet/RIB operation may be capability-gated. `data_path_ready` records whether
that AF may perform packet, adjacency, interface-I/O, packetizer, RTP, and RIB
operations.

Configuration and runtime ownership are separate. Configuration may be retained
when the data path is unavailable.

When `data_path_ready` is false, retained configuration remains valid but packet,
adjacency, interface-I/O, packetizer/RTP, multicast, and RIB data-path work does
not start for that address family. IPv6 named configuration uses the same
ownership model as IPv4 even when its runtime data path is capability-gated.

### 8.3 Lifecycle ordering

Creation:

```text
named parent
  -> address-family retained state
  -> resolve host VRF/context
  -> create/bind eigrp_instance_t
  -> apply retained child configuration
  -> start data path when capability and shutdown state permit
```

Deletion:

```text
stop address-family data path
  -> detach interfaces/neighbors/timers/queues
  -> remove host RIB/runtime state
  -> unbind runtime from retained AF configuration
  -> free child retained state
  -> free parent when removed
```

Teardown must not leave child objects referring to destroyed runtime or platform
objects.

## 9. CLI/configuration to core contract

Classic and named command surfaces may differ in syntax and placement, but when
they represent the same protocol operation they converge on the same EIGRP-owned
semantic behavior below the host boundary. Classic and named configuration surfaces
therefore converge on the same EIGRP-owned semantic behavior below the host boundary. Named mode is the canonical surface for new configuration work. Named mode covers the complete applicable classic EIGRP protocol feature set except features explicitly excluded by this specification.

```text
classic front end --\
                    -> EIGRP-owned target/processor
named front end   ---/
```

Named mode must not call a classic FRR CLI callback as protocol behavior.
Likewise classic mode must not become a second portable implementation.

Every supported configuration feature implements its applicable `no` form. The no form
removes explicit retained configuration or restores the defined default and
calls the same EIGRP feature family as the positive form.

Configuration values normally use `set/reset`, collection relationships use
`add/remove`, and owned objects use `create/delete`.

The host parser, YANG/libyang node, VTY object, interface object, route-map
object, or equivalent platform representation never becomes an argument to a
portable semantic target.

Operational show/state is exported through public management snapshots or
walkers. Clear/debug/admin actions call real EIGRP operational targets after
host argument normalization.

Where named EXEC grammar includes the `multicast` selector, it selects EIGRP's
Multicast Address Family (MAF), VRID `0x0001`. It is not normal EIGRP packet
multicast transport. The project configuration/runtime model is unicast; a MAF
request remains an explicit unsupported/not implemented semantic result until a
MAF design exists.

For FRR, the normal ownership is:

```text
frr/eigrp_cli_classic.[c|h]  classic configuration front end
frr/eigrp_cli_named.[c|h]    named configuration and named EXEC front end
frr/eigrp_vty.[c|h]          classic operational VTY surface
frr/eigrp_northbound.c       committed FRR configuration -> EIGRP adapter
```

### 9.1 Operational output conventions

EIGRP-owned operational presentation follows the established Cisco EIGRP
troubleshooting form where the implementation has the corresponding real state.
The presentation reference is:

```text
https://www.cisco.com/c/en/us/support/docs/ip/enhanced-interior-gateway-routing-protocol-eigrp/118974-technote-eigrp-00.html
```

This applies to EIGRP-owned headings, field labels, topology terminology,
neighbor-change reason text, and packet-debug wording. Host timestamps, logging
prefixes, parser decoration, and other framework-owned presentation remain host
owned.

Do not create protocol state or implement an incomplete capability solely to
fill a presentation field. If a value is not maintained by the backend, omit or
mark it unavailable. detailed topology/vector-metric output is not synthesized
from partial state merely to match an example.

## 10. Header ownership

`eigrp.h`, `eigrp_cli.h`, `eigrp_mgnt.h`, `eigrp_rib.h`, and `eigrp_sys.h` are
public platform integration headers. Their contract is documented by
`integration-spec.md`.

Private/core headers own only what portable modules need internally.
`eigrp_types.h` contains broadly reusable internal primitives and declarations.
`eigrp_structs.h` contains shared internal layouts required across several core
modules. Module headers export the minimum internal API needed by peer modules.

Do not create headers merely for symmetry and do not promote a private type to a
public header because one host currently finds it convenient.

## 11. Protocol module ownership

```text
instance      named/config/runtime ownership
network       configured network participation
interface     EIGRP interface state and interface behavior
neighbor      neighbor lifecycle/state/policy and per-neighbor transport values
metric        classic/wide metrics and metric configuration
summary       manual/automatic summary behavior
filter        distribute/offset feature families
redistribute  redistribution configuration and portable state
topology      DUAL topology records, lookup, route selection
fsm           DUAL state transitions
query         QUERY semantics
reply         REPLY semantics
siaquery      SIA-QUERY semantics
siareply      SIA-REPLY semantics
update        UPDATE semantics and adjacency init/resync table walk
packetizer    DUAL/topology work-to-packet orchestration
packet        packet buffer/header/checksum/output/RTP mechanics
tlv1          classic route TLV wire codec
tlv2          multiprotocol/wide route TLV wire codec
auth          packet authentication behavior and state
eventlog      EIGRP event-log state
statistics    protocol counters/accounting
status        management/tech-support state inputs
```

Platform-specific CLI, configuration, policy, RIB, event-loop, socket, and host
lifecycle implementations remain outside portable core.

## 12. DUAL ownership and invariants

### 12.1 DUAL FSM Active-State Invariant

The topology database currently uses a destination-level `prefix_descriptor`
with one or more neighbor/path `route_descriptor` objects. Cisco DNDB/NDB and
DRDB/RDB terminology may be useful in comments and debug output, but the final
source/API naming decision is deferred to `refactor-work.md`.

While Active, per-neighbor/path observations and reply/origin bookkeeping may change, but destination-level successor selection, Feasible Distance, destination reported distance, current destination distance, and destination reported metric stay frozen until the destination returns to Passive.

`dual.md` defines the state-machine model and message interaction in detail.

## 13. Packet, TLV, packetizer, and RTP rules

Packet encode/decode must be bounds-safe, endian-safe, debuggable, and separated
from route-selection decisions.

Internal EIGRP scalar values use host order unless explicitly documented
otherwise. Wire buffers use network order. Every multibyte wire field crosses
an explicit encode/decode boundary.

Packed structs may describe fixed wire views but do not replace safe parsing.
Every TLV decoder validates header length, declared length, type-specific minimum
size, variable fields/prefix length, and required alignment. Malformed input
must fail safely and must never leave a non-progressing decode loop.

`eigrp_tlv1` and `eigrp_tlv2` own private wire representations. DUAL, topology,
packetizer, CLI, and RTP operate on native EIGRP data rather than private TLV
layouts.

`rtp-spec.md` defines route work queueing, packetization, packet lifetime,
interface pacing, reliable transmission, ACK tracking, SRTT/RTO, retry behavior,
and conditional receive.

## 14. Production code rule

Do not add compatibility debt without an explicit migration requirement.
Core changes follow the style already in the file. Do not land whitespace-only
churn or gratuitous cleanup unless repo moderators asked for that cleanup.
See `../CONTRIBUTING.md`.
Forbidden examples include legacy/old replacement paths, alias wrappers for
renamed internal APIs, parallel old/new protocol implementations, generic
compatibility dispatchers, and temporary host-object leakage into portable APIs.

Existing classic CLI is a supported surface. That does not authorize duplicate
portable protocol implementations.

## 15. Testing contract

```text
test/build/       lightweight compile/syntax/prototype smoke
test/common/      host-independent fixtures
test/portable/    host-independent behavior/source-boundary tests
frr/test/         FRR-native integration/UUT tests
bird/test/        reserved BIRD-native integration/UUT tests
```

Normal source-change gate:

```text
1. make test
2. relevant host stage/build/link succeeds
3. relevant host-native/live UUT coverage succeeds
```

CLI/configuration changes validate parsing, mutation, writeback, and applicable
`no` forms. Packet changes validate encode/decode, bounds, endian behavior, and
reliable-transport effects appropriate to the changed path.

Portable tests must not depend on FRR/BIRD types when the behavior under test is
protocol-owned.

## 16. Development order for named mode

Named-mode work is staged deliberately:

1. Make `router eigrp savage` / IPv4 AF AS 4453 pass every applicable command,
   writeback, mutation, and documented `no` form.
2. Do the same for IPv6 AF AS 4453.
3. Test multiple autonomous systems under one named parent.
4. Validate distinct case-sensitive parent names such as `savage` and `SAVAGE`.

Do not attribute a parser/config-retention failure to process/thread runtime code
until the command has actually reached that layer.

## 17. Scope exclusions

EIGRP Stub routing is explicitly outside project scope. Do not implement Stub runtime
behavior, import it from another implementation, or add tests that validate it.
Ordinary test/compiler stub terminology is unrelated.

## 18. Drift and refactor discipline

Do not refactor merely because a specification describes a cleaner final name.
When code and design differ, fix code when the design rule is intentional and
the touched work makes that appropriate, or update the specification when the
implementation demonstrates a better architecture.

Deferred naming/ownership work belongs in `refactor-work.md`. That document is a
parking lot, not authorization for broad unrelated cleanup.

## 19. Copyright and authorship

New project files use Donnie V. Savage as copyright owner unless another author
is intentionally identified. Existing source preserves prior copyright, SPDX,
and author history.

## 20. Delivery

Changes arrive as a GitHub pull request against
https://github.com/diivious/eigrpd

Fork, branch, PR. The PR text states the issue, the change, and the tests
run. Review happens on the PR. Accepted work is merged to master.

Do not send zip files or recursive copies as the contribution path.
Process details are in `../CONTRIBUTING.md`.
