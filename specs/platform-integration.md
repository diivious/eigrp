# OpenEIGRP Platform Integration Specification

Copyright (C) 2026 Donnie V. Savage

## 1. Scope

This document defines the generic contract for integrating the portable
OpenEIGRP implementation into a routing platform.

It is intentionally host-neutral. FRR-specific integration belongs under
`frr/`; BIRD/BSD-specific integration belongs under `bird/`; standalone Unix
host integration belongs under `unix/`.

The concrete FRR implementation, including packet, event, interface, RIB,
redistribution, policy, and configuration call flows, is specified in
`frr/specs/integration.md`.

The standalone macOS/Linux implementation, including runtime scheduling,
interface/RIB host models, virtual-wire packet I/O, logical segments, and Unix
core/shim call flows, is specified in `unix/specs/integration.md`.

A platform integrator should be able to understand the required boundary from
this document and the public headers without learning private DUAL, topology,
packetizer, TLV, or RTP structures.

The complete public C API contract, including every callable symbol, public
structure, callback, ownership rule, and which side implements each function, is
defined in `specs/public-api.md`.

The exact C declarations are authoritative in:

```text
eigrp/code/eigrp.h
eigrp/code/eigrp_cli.h
eigrp/code/eigrp_mgnt.h
eigrp/code/eigrp_rib.h
eigrp/code/eigrp_sys.h
```

Feature-specific public headers may supplement the base contract when needed.

## 2. Naming

OpenEIGRP is the implementation project. EIGRP is the protocol.

The public API intentionally retains `eigrp_*` C symbols and `EIGRP_*`
constants. Integrating OpenEIGRP does not require a platform to rename those
symbols or the `router eigrp` protocol surface.

## 3. Integration model

The adapter is a translation boundary, not another EIGRP implementation.

```text
+-------------------+      +-------------------+      +-------------------+
| host platform     |      | adapter           |      | portable core     |
|                   |      |                   |      |                   |
| config / CLI      +----->+ eigrp_cli.h       +----->+ semantic targets  |
| show / telemetry  +<-----+ eigrp_mgnt.h      +<-----+ runtime snapshots |
| RIB               +<---->+ eigrp_rib.h       +<---->+ route state       |
| timers / sockets  +<-----+ eigrp_sys.h       +<-----+ protocol runtime   |
+-------------------+      +-------------------+      +-------------------+
```

The adapter translates host-native identities, configuration objects, RIB
objects, events, timers, sockets, and policy results into OpenEIGRP-owned values
and calls the portable API.

It does not reimplement:

- DUAL;
- successor or feasible-successor selection;
- metric calculation;
- neighbor state-machine decisions;
- topology ownership;
- packetization;
- RTP acknowledgment/retransmission logic;
- TLV semantics.

## 4. Public header roles

### 4.1 `eigrp.h`

Common public values and opaque identities.

It defines the cross-boundary vocabulary used by the other public headers,
including result codes, address-family values, OpenEIGRP address/prefix types,
metric values, VRF/interface identifiers, and opaque runtime identities.

A host may use the public accessors declared here. It must not cast opaque
objects to private structures.

### 4.2 `eigrp_cli.h`

Host-to-core configuration and administrative operations.

Use this API after parsing and normalizing host configuration. It owns semantic
operations such as instance/address-family lifecycle, network participation,
interface configuration, neighbors, authentication, summaries, metrics,
topology settings, filtering, redistribution, timers, event-log operations,
and debug controls.

The host CLI or YANG tree is not the public OpenEIGRP API. The semantic call is.

### 4.3 `eigrp_mgnt.h`

Core-to-host management and observation snapshots.

Use this API for show commands, telemetry, event-log presentation, statistics,
protocol status, timers, interface state, neighbor state, and topology state.

A host should not traverse private portable structures to implement show output
when a public management snapshot exists.

### 4.4 `eigrp_rib.h`

Bidirectional routing-table exchange.

The portable core uses this contract to request installation/removal of selected
EIGRP routes. The host uses the same boundary to feed redistributed source-route
changes into OpenEIGRP.

Host-native route objects do not cross this boundary.

### 4.5 `eigrp_sys.h`

Host runtime/system services used by the portable core.

This includes event scheduling, timers, work queues, monotonic time, socket and
multicast services, packet I/O, VRF resolution, interface discovery, router-ID
services, policy evaluation, authentication-key lookup, and host lifecycle
notifications.

The platform implements the host side of these services using native mechanisms.

## 5. Public versus private types

The integration boundary has two categories of public data.

### 5.1 Public value/snapshot types

These are copied values safe to exchange across the boundary, for example
addresses, prefixes, metrics, route snapshots, management snapshots, packet
receive metadata, and normalized interface state.

Their ownership rules are defined by the public header and call contract.

### 5.2 Opaque portable objects

Some APIs identify a portable runtime with an opaque pointer type such as an
EIGRP instance or interface runtime.

A host may retain the identity only for the documented lifetime. It must not
read private fields or infer implementation layout.

### 5.3 Forbidden cross-boundary types

Portable public APIs must not expose:

- FRR VTY/YANG/Zebra/interface/event/stream objects;
- BIRD configuration/protocol/channel/table/event-loop objects;
- platform socket wrappers;
- private DUAL/topology/neighbor/packetizer structures;
- host policy object pointers.

If integration requires one of those objects, translate it in the adapter.

## 6. Initialization and shutdown

A host integration should follow this lifecycle ordering.

### 6.1 Start

```text
1. host runtime/event loop exists
2. initialize eigrp_sys host services
3. initialize eigrp_rib host services
4. initialize policy services if used
5. call eigrp_init()
6. bind host configuration/admin to eigrp_cli.h
7. begin feeding interface, RIB, and packet events
```

The portable runtime must not receive callbacks that depend on an uninitialized
host service.

### 6.2 Stop

```text
1. stop accepting new host configuration and packet input
2. disable/delete configured OpenEIGRP runtime contexts
3. cancel host callbacks that reference those contexts
4. remove RIB/runtime integration state
5. call eigrp_terminate()
6. finish policy, RIB, and system services
```

Exact host sequencing may differ, but no scheduled callback may outlive the
portable object it references.

## 7. Configuration flow

A host configuration path has four responsibilities:

```text
parse host syntax
    -> normalize host representation
    -> call owning eigrp_cli.h target
    -> map eigrp_result_t to host-facing outcome
```

The adapter should not duplicate protocol validation that already belongs in a
semantic OpenEIGRP target.

Configuration retention is distinct from runtime capability. A configuration
may need to remain writable even when the real target reports that its runtime
behavior is not yet implemented.

Classic and named host syntax should converge on the same semantic target when
they represent the same EIGRP property.

## 8. Interface lifecycle flow

The host owns interface discovery and operating-system state. OpenEIGRP owns
what EIGRP does with that state.

A typical flow is:

```text
host interface/address event
    -> adapter creates normalized eigrp_intf_runtime_state / identity
    -> eigrp_sys_intf_* notification
    -> portable interface/neighbor/network logic
```

Do not call private neighbor or topology routines directly from the platform
because an interface changed.

## 9. Packet receive flow

```text
host socket receives packet
    -> adapter identifies VRF/interface/source metadata
    -> eigrp_sys_packet_receive() or AF-specific envelope
    -> portable packet validation/decoding
    -> neighbor/RTP/protocol processing
```

The host adapter owns socket mechanics. The portable core owns EIGRP packet
semantics.

The receive boundary must supply enough normalized metadata for the portable
core to identify the correct runtime and interface without reading host-native
objects.

## 10. Packet transmit flow

```text
portable packet/RTP logic
    -> eigrp_sys_packet_send() or AF-specific envelope
    -> adapter chooses host socket/interface mechanics
    -> host transmits bytes
```

The adapter may report host send success/failure. It must not decide whether an
EIGRP packet should be reliable, retransmitted, acknowledged, split, or
packetized differently.

## 11. RIB installation flow

```text
portable route selection
    -> normalized eigrp_rib_route
    -> host-side eigrp_rib_route_install/remove implementation
    -> host RIB API
```

The RIB adapter converts route representation only. Administrative decisions
that are part of EIGRP route selection remain portable.

The host must be able to recover from a RIB reconnect/restart by replaying the
portable selected-route state through the public RIB contract.

## 12. Redistribution flow

```text
host source-route event
    -> adapter normalizes source protocol, prefix, metric/nexthop attributes
    -> eigrp_rib_route_add/del or current public ingress API
    -> portable redistribution/policy/metric logic
    -> topology and update generation
```

Host route-map or policy objects remain on the host side. The portable core may
request a semantic policy decision through `eigrp_sys.h` and receive a
normalized result.

## 13. Policy and authentication services

The host may own policy databases and key-chain storage.

OpenEIGRP requests only the operation it needs, such as:

- filter evaluation;
- redistribution route-map evaluation;
- summary leak-map evaluation;
- authentication key lookup.

The host returns normalized results or key material according to the public
contract. Private host policy/key objects never become portable dependencies.

## 14. Scheduling and time

Portable protocol code may request events, timers, reads, writes, work-queue
execution, and time through `eigrp_sys.h`.

The adapter maps these onto the host event loop.

The host scheduler must preserve required callback ordering and cancellation
semantics. It must not reinterpret protocol timer meaning.

Use monotonic time for elapsed protocol intervals. Wall-clock time is for
presentation/logging where required.

## 15. Management flow

A show/telemetry implementation should use public management snapshots:

```text
host show/telemetry request
    -> eigrp_mgnt.h read/iterate API
    -> public snapshot values
    -> host formatting/encoding
```

Presentation belongs to the host. Protocol state ownership remains portable.

If the management API lacks a required piece of state, add a public semantic
snapshot/accessor rather than reaching into private structs from the host.

## 16. Result handling

Portable semantic calls use `eigrp_result_t` where the caller must distinguish
outcomes.

The host adapter maps those values to its own CLI/YANG/error mechanism at the
boundary.

Do not return host-native status codes through portable APIs.

A real feature target may return `EIGRP_RESULT_NOT_IMPLEMENTED`. The adapter
must preserve that distinction from invalid input or a generic host error.

## 17. Optional capabilities

A host may not support every optional OpenEIGRP capability immediately.

Optional integration should follow these rules:

1. capability absence is explicit;
2. the base public contract remains buildable;
3. configuration required for retention is not silently discarded;
4. unsupported runtime behavior reports a structured result;
5. host-specific capability checks stay in the adapter or a public feature
   contract, not scattered through protocol modules.

IPv4 and IPv6 use the same architectural integration model even when the host
implements their socket mechanics differently.

## 18. Minimum host implementation

A new host needs, at minimum:

- portable `eigrp/code/*.c` objects, except any intentionally replaced host
  implementation point such as a logging sink;
- implementations of the required `eigrp_sys.h` host services;
- implementations of the required `eigrp_rib.h` host services;
- a configuration/admin front end that calls `eigrp_cli.h`;
- management/show integration through `eigrp_mgnt.h` if the product exposes
  operational state.

Do not compile FRR adapter source into a non-FRR host or BIRD adapter source into
a non-BIRD host.

## 19. Bring-up sequence

A practical integration order is:

### Phase 1 — compile boundary

- compile the public headers in the host environment;
- implement required system/RIB symbols;
- verify no private portable headers are needed.

### Phase 2 — runtime basics

- initialize/terminate cleanly;
- provide event/timer/time services;
- resolve VRF and interface identity;
- open/close packet sockets.

### Phase 3 — configuration

- create/delete classic and/or named contexts;
- retain and write configuration;
- exercise documented reset/no forms;
- map structured results correctly.

### Phase 4 — adjacency

- feed interface/address events;
- join multicast where required;
- receive/transmit Hellos;
- establish and tear down neighbors.

### Phase 5 — routing

- install/remove selected EIGRP routes;
- feed redistribution source routes;
- validate RIB reconnect replay.

### Phase 6 — management

- expose interfaces, neighbors, topology, timers, traffic, event log, and status
  using public management APIs.

### Phase 7 — teardown

- delete runtime contexts;
- cancel events/work queues;
- close sockets;
- verify no host callback references freed portable state.

## 20. Integration acceptance criteria

A platform integration is architecturally acceptable when:

1. the host builds against public OpenEIGRP headers rather than private module
   headers;
2. host-native objects stop at the adapter;
3. protocol decisions remain in `eigrp/code/`;
4. configuration commands reach real semantic feature targets;
5. management output consumes public snapshots;
6. packet I/O uses the system boundary rather than direct host calls in core;
7. route installation and redistribution use the RIB boundary;
8. lifecycle cancellation prevents callbacks into freed state;
9. IPv4/IPv6 differences are represented only where genuinely required;
10. host-specific code and tests remain under the host directory.

## 21. Platform-specific documentation

This file intentionally does not document FRR YANG paths, Zebra callbacks,
BIRD channels, or Unix test-shim internals.

Use:

- `frr/README.md` and `frr/specs/` for FRR-specific mapping;
- `bird/README.md` and `bird/specs/` for BIRD/BSD-specific mapping;
- `unix/README.md` and `unix/specs/integration.md` for the standalone Unix host;
- `specs/EIGRP-Config-Guide.md` for the user-visible EIGRP semantic command
  surface that host front ends should represent.
