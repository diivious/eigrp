# FRR shim ownership and AF-selective dependency audit

Status: Task 13-3 second-pass audit after the Tasks 14-15 northbound/southbound AF split.

This audit is intentionally bounded. It records dependencies that prevent an eventual
`common + IPv4` integration build without changing runtime semantics or introducing
conditional address-family builds in this task.

## Six-module ownership result

The FRR shim remains split into:

- `frr/eigrp_northbound.c`: address-family-neutral named-mode normalization,
  FRR northbound registration, and common host-to-EIGRP management helpers.
- `frr/eigrp_northbound_ipv4.c`: classic IPv4 northbound callbacks and IPv4
  host-value conversion.
- `frr/eigrp_northbound_ipv6.c`: IPv6 host-value conversion currently needed by
  common named operational handling.
- `frr/eigrp_southbound.c`: address-family-neutral FRR service implementations
  plus current AF dispatch points.
- `frr/eigrp_southbound_ipv4.c`: IPv4 socket options, multicast membership,
  interface address selection, and packet I/O.
- `frr/eigrp_southbound_ipv6.c`: IPv6 socket options, multicast membership,
  link-local source selection, and packet I/O.

Every function in the six implementation files was reviewed by what it inherently
knows: IPv4, IPv6, or neither. The only clear ownership errors suitable for this
bounded pass were two helpers left in common northbound but used exclusively by
classic IPv4 callbacks. `eigrp_northbound_ipv4_redistribute_metrics_get()` and
`eigrp_northbound_ipv4_interface_lookup_host()` now live in the IPv4 module and are
`static`.

No common implementation duplicated between the IPv4 and IPv6 modules was found
that should be pulled back into common in this pass. Similar multicast and packet-I/O
functions are family implementations, not accidental duplication: their socket
options, source-address rules, ancillary data, and membership structures differ.

The common modules still contain AF selection/dispatch. Those sites are recorded
below rather than hidden behind new compatibility wrappers. The long-term rule
remains that vectors are the normal common-to-AF mechanism; direct common-to-AF
symbol references are blockers to selective linkage.

## Header and declaration audit

- `eigrp_northbound_internal.h` now exposes only declarations that actually cross
  northbound translation units. IPv4-only helper declarations were removed after
  moving those helpers into the IPv4 module.
- `eigrp_southbound_internal.h` needs only public opaque EIGRP identities/results for
  its cross-file declarations. It no longer includes private `eigrpd.h` or
  `eigrp_interface.h`, nor the broader `eigrp_sys.h` contract.
- `eigrp_northbound_ipv6.c` only performs an IPv6 host-address copy today. Its prior
  broad collection of protocol, Zebra, CLI, policy, and FRR northbound includes was
  unnecessary and has been reduced to the public EIGRP value contract plus the FRR
  private declaration header and standard IPv6/string definitions.
- The IPv4 northbound and both AF southbound implementations still include private
  portable headers because they dereference portable runtime objects. Removing those
  private-layout dependencies is not a header-cleanup exercise; it requires extending
  the approved public integration contracts/accessors and is therefore recorded as a
  later redesign below.

## IPv4-only readiness

Target considered here: link common integration plus IPv4 implementation objects,
while omitting IPv6 implementation objects. No conditional build is implemented.

| Area | Classification | Concrete dependency/blocker |
| --- | --- | --- |
| Northbound registration/classic IPv4 callbacks | already separable | Classic callbacks are owned by `eigrp_northbound_ipv4.c`; common registration references those IPv4 callbacks as expected for an IPv4 build. |
| Common named northbound parsing | already separable | Common named callbacks represent both AFs through `eigrp_afi_t` and generic EIGRP values. Parsing an `afi` value is not itself an implementation-object dependency. |
| Named clear-neighbor host address conversion | API/vector dependency | `eigrp_northbound_neighbor_clear_address()` in common directly references both `eigrp_northbound_ipv4_neighbor_address_copy()` and `eigrp_northbound_ipv6_neighbor_address_copy()`. Omitting the IPv6 northbound object therefore leaves an unresolved symbol. The conversion/dispatch needs an AF registration/vector or a generic host-value normalization API before selective linkage. |
| Portable AF vector initialization | API/vector dependency | `eigrpd/eigrpd.c` and `eigrpd/eigrp_instance.c` directly call both `eigrp_ipv4_init()` and `eigrp_ipv6_init()`. Omitting `eigrp_ipv6.c` cannot link until AF vector providers are registered/selected without hard references from common lifecycle code. |
| FRR socket creation/configuration | API/vector dependency | `eigrp_sys_socket_open()` in common southbound chooses `AF_INET`/`AF_INET6` and directly calls both AF socket-configure functions. Socket family configuration must become an AF-provided operation (or equivalent registered capability) before the IPv6 southbound object can be omitted. |
| Multicast interface/join/leave | API/vector dependency | Common southbound `eigrp_sys_multicast_interface_update(EIGRP_SET)`, `eigrp_sys_multicast_join()`, and `eigrp_sys_multicast_leave()` directly reference IPv4 and IPv6 implementations. These are normal AF operations and need vector/registration ownership rather than link-time references from common. |
| Packet send/receive | API/vector dependency | Common southbound `eigrp_sys_packet_send()` and `eigrp_sys_packet_receive()` directly dispatch to `eigrp_sys_ipv4_*` and `eigrp_sys_ipv6_*`. Portable AF vectors already select the family before entering the system boundary, but the system boundary dispatches a second time. Selective linkage requires removing this second hard-wired AF dispatch, not adding conditional stubs. |
| RIB/Zebra route installation | already separable | The public RIB request carries a generic EIGRP prefix/nexthop and Zebra translation handles the request by AF value. No direct dependency on `eigrp_southbound_ipv6.c` or `eigrp_northbound_ipv6.c` is required merely to compile the IPv4 route-install path. Build-time feature selection may still choose which AF values are accepted. |
| Interface discovery/state | requires later redesign | AF southbound code dereferences private `eigrp_interface_t` layout for IPv4 address and IPv6 link-local selection. A strict public-header-only FRR shim needs public interface queries/snapshots or equivalent SYS operations. This is independent of merely omitting IPv6, but it is a remaining integration-boundary leak. |
| Interface multicast lifecycle | runtime dependency | Common portable interface lifecycle invokes generic SYS multicast join/leave. IPv4-only runtime behavior is valid, but the current common SYS implementation has hard references to both AF implementations as described above. |
| Packet/socket startup | runtime dependency | Runtime creation opens the protocol socket before packet processing. An IPv4-only runtime can use only IPv4 behavior, but current initialization reaches common dispatch code that is linked against both AF implementations. |
| Global instance/AF creation | API/vector dependency | Address-family creation initializes vectors through direct `eigrp_ipv4_init()`/`eigrp_ipv6_init()` calls. This is the earliest portable link blocker for omitting an AF implementation object. |
| Shutdown | already separable | Common shutdown closes per-instance sockets and tears down common runtime state without requiring an IPv6-specific shutdown entry point. The multicast-leave path remains subject to the common multicast dispatch dependency above. |
| Build manifests | minor build dependency | `frr/subdir.am` currently lists both AF shim objects unconditionally, as expected before conditional builds exist. Once symbol dependencies are removed, selecting object lists is straightforward build-system work. |
| AF implementation private-header use | requires later redesign | IPv4/IPv6 FRR AF shims include private portable headers to inspect runtime/interface fields. Public integration accessors must replace those layout dependencies before the integration boundary satisfies the five-header rule completely. |

## Direct common-to-AF symbol dependencies to remove later

Northbound:

- `eigrp_northbound_ipv4_neighbor_address_copy()`
- `eigrp_northbound_ipv6_neighbor_address_copy()`

Southbound:

- `eigrp_southbound_ipv4_socket_configure()` / `eigrp_southbound_ipv6_socket_configure()`
- `eigrp_southbound_ipv4_multicast_interface_update(EIGRP_SET)` / IPv6 counterpart
- `eigrp_southbound_ipv4_multicast_join()` / IPv6 counterpart
- `eigrp_southbound_ipv4_multicast_leave()` / IPv6 counterpart
- `eigrp_sys_ipv4_packet_send()` / `eigrp_sys_ipv6_packet_send()`
- `eigrp_sys_ipv4_packet_receive()` / `eigrp_sys_ipv6_packet_receive()`

Portable lifecycle also has direct AF initializer dependencies:

- `eigrp_ipv4_init()`
- `eigrp_ipv6_init()`

These should be solved as one coherent AF-provider/vector design. Conditional weak symbols,
empty IPv6 objects, generic "not implemented" shims, or build-time fake implementations
would only hide the ownership problem and are not recommended.

## Naming and dependency conclusion

The six-module names follow module/object/action navigation closely enough for this
stage. The helpers moved in this pass now carry the `eigrp_northbound_ipv4_*` prefix
and action-last naming. No rename-only churn is warranted elsewhere.

The architecture is clean enough to continue with six modules, but it is not yet
AF-selectively linkable. The principal blocker is not source-file placement: common
lifecycle and FRR SYS dispatch still hold direct symbol dependencies on both AF
providers. The next selective-build task should first define the AF provider/vector
registration boundary, then make build manifests conditional only after common code
no longer names omitted AF implementation symbols.
