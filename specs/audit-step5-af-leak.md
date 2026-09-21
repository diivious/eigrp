# Portable Address-Family / Host Boundary Audit Record

Copyright (C) 2026 Donnie V. Savage

> **Non-normative audit record.** The durable architecture from this audit is
> defined in `design-spec.md` and `refactor-work.md`. This file may be removed
> once the audit history is no longer useful.

## Scope

This audit verified the boundary between portable `eigrpd/` protocol code and
host-framework services after address-family, interface/runtime, policy/filter,
and RIB cleanup.

Standard POSIX/socket networking types are not host-framework leaks by themselves.
Portable code may use natural system networking definitions such as:

```text
AF_INET / AF_INET6
struct in_addr / struct in6_addr
struct sockaddr*
inet_ntop / inet_pton
```

EIGRP-owned address/prefix types remain required where the object carries EIGRP
configuration, topology, or wire semantics.

## Final findings

### Interface/runtime boundary

Portable interface/runtime state uses EIGRP-owned identifiers and objects.
FRR interface discovery, VRF lookup, event scheduling, raw-socket/multicast
operations, and host interface lifecycle live behind the FRR southbound adapter.

Portable runtime state is not stored in FRR `struct interface` objects.

### Prefix representation

DUAL/topology destination and path descriptors use `eigrp_prefix_t` for native
prefix data. Update, Query/Reply/SIA, packetizer, TLV, connected-route, topology
lookup, and route-install paths carry EIGRP-owned prefixes across portable
boundaries.

The topology descriptor naming decision remains separately parked in
`refactor-work.md`.

### Policy/filter boundary

Portable filter state stores EIGRP-owned policy names/decisions rather than FRR
access-list, prefix-list, route-map, or distribute-list objects.

Policy evaluation follows:

```text
eigrp_filter_prefix_apply()
  -> eigrp_southbound_filter_evaluate()
  -> FRR policy adapter
  -> eigrp_filter_decision_t
```

Host policy lookup, evaluation, callbacks, and change notifications remain in
FRR integration code.

### RIB boundary

Portable topology/DUAL code does not construct Zebra RIB objects or call the
Zebra adapter directly.

Route installation/removal follows:

```text
DUAL/topology
  -> eigrp_southbound_route_install()/remove()
  -> FRR southbound adapter
  -> FRR Zebra adapter
  -> Zebra RIB
```

The handoff contains only EIGRP-owned prefix and next-hop snapshot data.

### Remaining portability work

The host-boundary cleanup has also removed the former FRR utility-storage,
process-bootstrap, SNMP, and VRF ownership dependencies from portable `eigrpd/`.
Portable lists, prefix tables, packet streams, logging, allocation, checksum, and
crypto are EIGRP-owned; FRR process bootstrap, VRF/SNMP integration, and VTY/debug
presentation live under `frr/`.

The remaining pre-production portability/refactor work is limited to the parked
topology descriptor and runtime lifecycle naming reviews in `refactor-work.md`.

## Regression rule

New portable APIs must not reintroduce FRR or BIRD management, event-loop,
interface, policy, RIB, or route-table objects merely because one host already
provides a convenient implementation type. Normalize at the adapter boundary
and keep protocol state EIGRP-owned.
