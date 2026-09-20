# EIGRP Process and Address-Family Runtime Model

Copyright (C) 2026 Donnie V. Savage

## 1. Purpose

This document defines ownership and identity for named EIGRP parents,
address-family configuration, protocol runtime instances, and receive-path
demultiplexing.

It extends `design-spec.md` and uses the naming rules in
`code-conventions.md`.

## 2. Named parent and address-family ownership

Named EIGRP is modeled as a local parent configuration object containing one or
more address-family protocol contexts.

```text
router eigrp <name>
  address-family <afi> [vrf <vrf>] autonomous-system <asn>
```

`router eigrp <name>` owns the named parent and all child retained
configuration. Removing the parent removes every child address family and tears
down each bound runtime before freeing retained state.

Each configured address family owns one protocol context identified locally by:

```text
{name, address-family, VRF, AS}
```

The protocol context binds to an `eigrp_instance_t` runtime through the
EIGRP-owned southbound lifecycle API. Child configuration consumes that binding;
network, interface, topology, filter, redistribution, metric, neighbor, and
summary targets must not independently create or discover another EIGRP process.

Removing an address family tears down its runtime binding before the child
configuration is freed.

## 3. Runtime identity versus local name

The named parent name is local configuration ownership. It is not an on-wire
identity.

Wire acceptance is based on the protocol context, including:

- receiving VRF/socket context;
- packet address family;
- receiving interface;
- EIGRP AS number;
- VRID/topology identity where applicable.

The AS number is mandatory for protocol acceptance. A packet that cannot map to
one enabled, unambiguous local protocol context is discarded.

Configuration must not create an ambiguous receive identity for the same
`{VRF, AF, AS, interface, VRID}` context. The host/northbound layer rejects the
conflict or the receive path treats it as invalid; it must never select a named
parent by arbitrary lookup order.

## 4. Address-family runtime capability

A named address family always has a control/configuration context. Packet/RIB
operation is capability-gated by the runtime.

`eigrp_instance_t::data_path_ready` expresses whether that address family may
perform packet, adjacency, interface-I/O, packetizer, and RIB operations.

When `data_path_ready` is true, the runtime may own:

- EIGRP socket receive/send processing;
- self/neighbor runtime state;
- participating interfaces;
- packetizer work queue;
- per-interface output queues;
- reliable-transport/retransmit state;
- route installation/removal state.

When `data_path_ready` is false:

- retained configuration still exists;
- configuration-only targets operate normally;
- runtime-dependent targets return an EIGRP structured capability/not-implemented
  result;
- no socket, receive loop, self-neighbor, packetizer, Hello transmission,
  multicast membership, topology advertisement, or RIB installation is started
  for that AF.

This is a permanent architectural capability boundary, not a second
configuration model. IPv6 named configuration uses the same ownership model as
IPv4 even when the IPv6 packet data path is disabled in a given build.

## 5. Configuration retention and runtime application

Configuration and runtime ownership are separate.

The FRR path is:

```text
CLI
  -> FRR YANG/northbound retained configuration
  -> frr/eigrp_northbound.c
  -> normalized EIGRP-owned context/value
  -> feature target
  -> eigrp_result_t
```

A valid configuration transaction is not rolled back merely because the target
reports a missing runtime capability unless the command itself is semantically
invalid. This keeps running-config/writeback independent from data-path feature
completeness.

Portable configuration and route objects identify their address family
explicitly and contain normalized EIGRP address/prefix data. Shared targets are
preferred when IPv4 and IPv6 protocol semantics are identical.

## 6. Host boundary

Portable runtime/process code uses EIGRP-owned data and services.

Host services are reached through `eigrp_southbound.[c|h]`. FRR Zebra/RIB
implementation remains under `frr/eigrp_zebra.[c|h]` behind the southbound
contract. FRR CLI, YANG, VTY, Zebra, event, interface, and equivalent BIRD
objects must not become parameters or fields of the portable process/worker
contract.

The BIRD adapter implements the same lifecycle and runtime-service contract
without changing the portable process model.

## 7. Receive-path demultiplexing

The host receive path provides enough normalized context to identify the target
EIGRP instance. The portable receive path then validates the EIGRP header and
selects the address-family runtime.

Conceptually:

```text
host packet receive
  -> normalize receiving VRF/interface/address-family
  -> decode EIGRP fixed header
  -> validate AS/VRID/version/opcode as required
  -> lookup one matching enabled eigrp_instance_t
  -> dispatch packet to that instance
```

The local named process string is never used as a wire demultiplexing key.

## 8. Runtime instance navigation

`eigrp_instance_t` is the portable protocol runtime context. Configuration
ownership for named parents/address families also lives in the instance module,
so new public lifecycle/configuration APIs use the `eigrp_instance_*` namespace.

Legacy low-level runtime allocation functions that remain in `eigrpd.c` are
implementation debt rather than a competing naming model. Their bounded
pre-production consolidation is tracked in `refactor-work.md`; do not add new
public lifecycle APIs under the legacy names.

## 9. Lifecycle ordering

Creation order:

```text
named parent
  -> address-family retained state
  -> resolve host VRF/context through adapter
  -> create/bind eigrp_instance_t runtime
  -> apply retained child configuration
  -> start data path only when capability and shutdown state permit
```

Deletion order:

```text
stop address-family data path
  -> detach interfaces/neighbors/timers/queues
  -> remove host RIB/runtime state
  -> unbind runtime from address-family config
  -> free child retained state
  -> free named parent when no longer configured
```

Teardown must not leave child objects referring to a destroyed runtime or host
adapter object.
