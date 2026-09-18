# Audit Step 5: Portable Address-Family / Host Boundary Sweep

Copyright (C) 2026 Donnie V. Savage

## Purpose

This audit records the remaining address-family and host-framework coupling in
`eigrpd/` after the packet, route-TLV, network/interface-participation, and
summary/auto-summary boundary work.

The portability rule for this sweep is deliberately narrow:

- standard system networking definitions that are common to the target hosts
  are not considered leaks merely because they are not EIGRP-owned;
- FRR- or BIRD-specific objects and services must not leak across the portable
  EIGRP boundary;
- EIGRP-owned types are introduced only where EIGRP has its own semantics or
  where the host representations differ in a way that must be normalized.

Accordingly, these are acceptable in common code when they are the natural
representation for the operation:

```text
AF_INET / AF_INET6
struct in_addr / struct in6_addr
struct sockaddr / struct sockaddr_in / struct sockaddr_in6
inet_ntop / inet_pton
INADDR_* / IPV6_*
```

Their presence is still useful during an audit because a surrounding operation
may be host-service coupling, but the definitions themselves are not a reason to
invent an EIGRP replacement type.

`eigrp_addr_t` therefore continues to use `AF_INET` / `AF_INET6` as its runtime
family identity and `struct in_addr` / `struct in6_addr` for the address payload.
The EIGRP configuration/wire abstractions (`eigrp_address_t`, `eigrp_prefix_t`,
and EIGRP TLV AFI values) remain separate where they carry EIGRP semantics.

## Cleanup completed by this sweep

The sweep found a concrete packet-runtime issue unrelated to type ownership:
several IPv4 multicast/hello packet constructors populated `packet->dst.ip.v4`
without setting `packet->dst.afi`.  `eigrp_ipv4_packet_send()` validates the
runtime destination family before sending, so a zero-initialized family could
cause an otherwise valid packet to be rejected.

The affected IPv4 packet constructors now set:

```c
packet->dst.afi = AF_INET;
```

before storing the IPv4 destination.  No new address-family abstraction was
introduced.

The topology-prefix migration is also complete.
`eigrp_prefix_descriptor_t::destination` and
`eigrp_route_descriptor_t::dest` now use `eigrp_prefix_t`; Update, Query,
Reply/SIA, packetizer, interface-connected-route, TLV1/TLV2, topology lookup,
and Zebra route-install call paths carry the native EIGRP prefix.  The temporary
route-prefix AF codec wrappers and topology import/export bridges were removed,
and protocol processing no longer allocates FRR prefixes with
`prefix_ipv4_new()`.  The FRR `route_table` storage key remains a localized
topology implementation detail until the later host-runtime boundary work.
The deferred prefix/route descriptor naming decision is unchanged.

## Remaining boundary findings

### 1. FRR interface/runtime objects in portable modules

Primary files include:

```text
eigrpd/eigrp_structs.h
eigrpd/eigrp_interface.c
eigrpd/eigrp_interface.h
eigrpd/eigrp_filter.c
eigrpd/eigrp_network.c
eigrpd/eigrp_network.h
```

Common data structures and APIs still expose FRR objects such as `struct
interface`, `struct vrf`, FRR prefix-list objects, FRR route-map objects, event
objects, and FRR interface lookup helpers.  Those are genuine host-framework
coupling because BIRD will provide different interface/configuration/runtime
objects.

This should be removed by EIGRP-owned southbound/configuration contracts rather
than by replacing standard IP address types.

### 2. FRR route-map/filter integration

Primary files include:

```text
eigrpd/eigrp_routemap.c
eigrpd/eigrp_routemap.h
eigrpd/eigrp_filter.c
eigrpd/eigrp_structs.h
```

These paths directly depend on FRR route-map, prefix-list, distribute-list, and
interface objects/callback signatures.  BIRD policy integration will use BIRD
objects, so these dependencies belong behind EIGRP-owned policy/filter
interfaces with FRR implementations under `frr/`.

### 3. FRR event-loop and daemon integration

Primary files include:

```text
eigrpd/eigrpd.h
eigrpd/eigrp_structs.h
eigrpd/eigrp_main.c
eigrpd/eigrp_packet.c
eigrpd/eigrp_update.c
eigrpd/eigrp_filter.c
```

Portable structures and public functions still expose FRR `struct event`,
`struct event_loop`, Zebra daemon state, and event macros.  Event scheduling is a
host service and must eventually sit behind an EIGRP-owned abstraction so the
BIRD implementation can use the BIRD event loop without leaking BIRD objects
into common APIs.

### 4. Route/RIB integration

Direct Zebra/RIB dependencies remain part of the broader core-to-Zebra audit in
`specs/refactor-work.md`.  Learned EIGRP route state must stay distinct from the
host RIB representation, and route installation/removal should terminate at a
southbound operation implemented by the host adapter.

### 5. Standard socket/address code

POSIX/BSD socket and address definitions are not portability defects by
construction.  Socket *service ownership* should only be moved when the actual
lifecycle, event-loop, interface, or host-routing integration differs between
FRR and BIRD.  Do not add wrapper types around `AF_INET`, `AF_INET6`, `in_addr`,
`in6_addr`, or `sockaddr*` solely for naming symmetry.

### 6. SNMP portability debt

`eigrpd/eigrp_snmp.c` remains tied to the current host integration.  The existing
pre-production portability plan already defers SNMP ownership.  This sweep does
not move SNMP merely for directory symmetry.

## Step-5 cleanup order

The remaining work should be done in this order:

1. **Topology prefix representation — complete** — DUAL/topology
   destination/path prefixes now use `eigrp_prefix_t`; temporary route-prefix
   bridges and protocol-side `prefix_ipv4_new()` call sites are removed.
2. **Interface/runtime boundary** — remove FRR `struct interface`, VRF/event-loop,
   and interface-discovery objects from portable public APIs where practical,
   using the existing EIGRP southbound boundary.
3. **Policy/filter boundary** — isolate FRR route-map/prefix-list/distribute-list
   objects under the FRR adapter while keeping EIGRP filter decisions portable.
4. **RIB/southbound residuals** — complete the direct Zebra/RIB call audit and
   route-install abstraction already tracked by `refactor-work.md`.
5. **Deferred portability grooming** — SNMP and other explicitly deferred host
   integration work.

IPv6 runtime/data-path implementation is not part of this sweep.
