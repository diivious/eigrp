# OpenEIGRP FRR Integration Specification

Copyright (C) 2026 Donnie V. Savage

The host-neutral public C boundary is defined by `../../specs/public-api.md`; this document maps FRR onto that contract.

## 1. Scope

This document defines how the FRR host adapter integrates FRR with the
portable OpenEIGRP core.

It is deliberately FRR-specific.  The generic host contract belongs in
`../../specs/platform-integration.md`; portable protocol behavior belongs in
`../../eigrp/specs/` and `../../eigrp/code/`.

This specification owns:

- FRR process startup and shutdown wiring;
- the FRR northbound/configuration path into portable semantic targets;
- Zebra/interface/router-ID notifications into portable runtime events;
- FRR event-loop, timer, work-queue, socket and multicast services;
- IPv4 and IPv6 packet receive/transmit call flows;
- EIGRP route installation and withdrawal through Zebra;
- redistributed route delivery from Zebra into the portable core;
- FRR policy/key-chain service calls;
- FRR show/debug/log presentation boundaries;
- the datatype normalization performed by the FRR shim.

It does **not** define DUAL, metric calculation, successor selection, RTP,
packetization, TLV semantics, neighbor protocol decisions, or topology
ownership.  Those remain portable EIGRP behavior.

## 2. Naming and layer terminology

**OpenEIGRP** is the implementation project. **EIGRP** is the routing protocol.
Existing C identifiers remain `eigrp_*` / `EIGRP_*`.

In this document:

- **FRR** means FRR-owned libraries and objects: Zebra/zclient, `struct
  interface`, `struct prefix`, event-loop objects, YANG/NB objects, VTY,
  route-map objects, key chains, logging, and VRFs.
- **shim** means code under `frr/code/`.
- **core** means portable code under `eigrp/code/`.
- **northbound** means FRR configuration/administrative input flowing toward
  the portable core.
- **southbound** means portable core requests for host mechanisms flowing
  through the FRR shim.

The direction names describe ownership, not call-stack nesting.  For example,
Zebra redistribution is an FRR-originated callback that enters the core through
the RIB contract even though `eigrp_rib.h` is also used by the core to install
routes into Zebra.

## 3. Architectural boundary

The required ownership boundary is:

```text
                        FRR HOST
+-----------------------------------------------------------+
| YANG / NB / VTY | Zebra | event loop | sockets | policy  |
+-----------------+-------+------------+---------+---------+
          |           |          ^          ^        ^
          v           v          |          |        |
+-----------------------------------------------------------+
|                    FRR SHIM                               |
| northbound       zebra       southbound       policy      |
| datatype normalization / lifecycle adaptation             |
+-----------------------------------------------------------+
          |           |          ^          ^        ^
          v           v          |          |        |
+-----------------------------------------------------------+
|                 PORTABLE OPENEIGRP CORE                   |
| config | instance | interface | neighbor | DUAL | RTP/RIB |
+-----------------------------------------------------------+
```

The shim may translate representation and lifecycle. It must not make protocol
decisions that should be identical on FRR, BIRD/BSD, Unix, or another host.

### 3.1 Public portable contracts used by FRR

The intended integration surface is centered on:

```text
eigrp/code/eigrp.h
eigrp/code/eigrp_cli.h
eigrp/code/eigrp_mgnt.h
eigrp/code/eigrp_rib.h
eigrp/code/eigrp_sys.h
```

FRR adapter code converts FRR-native values to EIGRP-owned values before
calling portable targets, and converts portable values to FRR-native values
before calling FRR APIs.

Current FRR code still includes some portable private headers. That is existing
integration debt, not permission to enlarge the boundary. New crossings should
use the public contract or extend it explicitly.

## 4. FRR shim file ownership

| File | Responsibility |
|---|---|
| `frr/code/eigrp_main.c` | FRR daemon lifecycle, signals, event-loop bootstrap |
| `frr/code/eigrp_northbound.c` | FRR NB/YANG commit callbacks and normalization to portable semantic targets |
| `frr/code/eigrp_northbound_ipv4.c` | IPv4-specific northbound value conversion |
| `frr/code/eigrp_northbound_ipv6.c` | IPv6-specific northbound value conversion |
| `frr/code/eigrp_cli_named.c` | FRR named-mode CLI/parser surface |
| `frr/code/eigrp_cli_classic.c` | FRR classic CLI/parser surface |
| `frr/code/eigrp_southbound.c` | common FRR host-service implementation: events, timers, work queues, sockets, policy/key services and RIB forwarding |
| `frr/code/eigrp_southbound_ipv4.c` | IPv4 raw socket, multicast and packet I/O |
| `frr/code/eigrp_southbound_ipv6.c` | IPv6 socket, multicast and packet I/O |
| `frr/code/eigrp_zebra.c` | Zebra registration, route install/delete, redistribution input, router-ID and interface-address callbacks |
| `frr/code/eigrp_frr.c` | FRR-to-EIGRP and EIGRP-to-FRR datatype normalization |
| `frr/code/eigrp_policy.c` | FRR route-map/distribute policy integration |
| `frr/code/eigrp_vrf.c` | FRR VRF lifecycle/identity glue |
| `frr/code/eigrp_vty.c` / `eigrp_dump.c` | FRR show/debug presentation |
| `frr/code/eigrp_log.c` | FRR logging sink for portable logging |
| `frr/code/eigrp_zebra.h`, `eigrp_frr.h`, internal adapter headers | FRR-local adapter declarations; not portable APIs |

## 5. Process startup and teardown

### 5.1 Startup

The relevant startup sequence in `frr/code/eigrp_main.c` is:

```text
FRR daemon entry
  -> frr_init()
       returns FRR event_loop *master
  -> eigrpd_event = master
  -> eigrp_init()                         [portable core]
  -> FRR error / VRF setup
  -> eigrp_sys_runtime_init()             [FRR host runtime services]
  -> eigrp_rib_init()
       -> eigrp_zebra_init()
            -> zclient_new(... eigrp_handlers ...)
            -> zclient_init(... ZEBRA_ROUTE_EIGRP ...)
            -> zebra_connected = eigrp_zebra_connected
  -> debug / VTY / classic CLI / named CLI initialization
  -> eigrp_sys_policy_init()
```

The FRR event loop must exist before portable runtime initialization can
schedule host-backed events.

### 5.2 Zebra reconnect

Zebra can disconnect independently of the EIGRP process. On reconnect:

```text
Zebra reconnect
  -> eigrp_zebra_connected()
       -> zclient_send_reg_requests(default VRF)
       -> eigrp_instance_vrf_iterate(...)
            -> zclient_send_reg_requests(each non-default EIGRP VRF)
       -> eigrp_rib_routes_replay()       [portable core]
            -> replay selected installed EIGRP routes
            -> eigrp_rib_route_add()
                 -> FRR shim / Zebra
```

The replay request originates from the FRR lifecycle callback, but the portable
core remains authoritative for which EIGRP routes currently exist and should be
reinstalled. The shim must not walk private DUAL descriptors to reconstruct the
RIB.

### 5.3 Teardown

Host teardown must cancel host events, stop Zebra integration, release FRR
policy state, close sockets and then release portable runtime state according to
the daemon lifecycle. Host objects must not remain referenced by portable code
after their adapter lifetime ends.

## 6. Northbound configuration flow

FRR parsing/YANG retention is not protocol implementation. A committed command
must terminate at its real portable EIGRP target.

### 6.1 Named-mode configuration

Representative flow:

```text
vtysh / frr-reload / northbound client
  -> FRR parser + frr-eigrpd YANG tree
  -> FRR NB callback in eigrp_northbound.c
       NB_EV_VALIDATE / PREPARE / ABORT: FRR transaction handling
       NB_EV_APPLY:
         read FRR lyd_node values
         normalize AF / VRF / AS / address / interface identity
         resolve portable config/runtime context
  -> portable semantic target
       eigrp_named_config_create/delete()
       eigrp_af_config_create/delete()
       eigrp_*_update()/set()/reset()/add()/remove()
  -> owning portable module
  -> EIGRP_RESULT_*
  -> FRR NB status mapping
```

Concrete named parent creation:

```text
FRR NB APPLY
  -> eigrpd_named_create()
       -> yang_dnode_get_string(... "name")
       -> eigrp_named_config_create(name)
  <- EIGRP_RESULT_SUCCESS / error
  <- NB_OK / NB_ERR_INCONSISTENCY
```

Concrete named AF creation:

```text
FRR NB APPLY
  -> eigrpd_named_address_family_create()
       -> normalize afi / vrf / asn
       -> eigrp_af_config_create(name, afi, vrf, asn)
```

The same rule applies to child commands. FRR owns syntax, YANG paths and
transaction objects; portable code owns retained EIGRP semantics and live
runtime effects.

### 6.2 Classic configuration

Classic CLI may use a different FRR parser surface, but it must converge on the
same portable feature targets where the feature semantics are the same.

```text
FRR classic VTY command
  -> eigrp_cli_classic.c
  -> normalize FRR values/context
  -> portable eigrp_* semantic target
  -> portable configuration/runtime behavior
```

Do not create an FRR-only behavior path merely because the command is classic
syntax.

### 6.3 Thread dispatch after a config call

A portable target may determine that mutation belongs on an EIGRP instance
thread. In that case the dispatch remains portable:

```text
FRR NB callback
  -> portable eigrp_*_update()
       -> eigrp_instance_thread_dispatch_needed()
       -> eigrp_instance_message_call(...)
       -> instance-owned portable message processor
       -> actual state mutation
  <- structured result
  <- FRR NB result
```

FRR should not know the private message type or reproduce the dispatch logic.

## 7. Host event-loop and timer flow

Portable EIGRP owns the callback and protocol meaning. FRR owns the mechanism
that wakes it.

`frr/code/eigrp_southbound.c` wraps each FRR `struct event` in an
EIGRP-owned opaque `eigrp_event_t` adapter object.

### 7.1 Immediate event

```text
portable core
  -> eigrp_sys_event_add(&owner, callback, arg)
       [frr/code/eigrp_southbound.c]
       -> eigrp_southbound_event_prepare()
            cancel previous owner event if present
            bind EIGRP callback + arg
       -> event_add_event(eigrpd_event,
                          eigrp_southbound_event_run, adapter, ...)

FRR event loop later fires
  -> eigrp_southbound_event_run(host_event)
       clear owner
       free FRR adapter wrapper
       -> callback(arg)                    [portable core]
```

### 7.2 Timer

```text
portable timer owner
  -> eigrp_sys_timer_add(&owner, callback, arg, delay_msec)
  -> event_add_timer_msec(FRR event loop, ...)
  ...time elapses...
FRR event loop
  -> eigrp_southbound_event_run()
  -> portable callback(arg)
```

`eigrp_sys_timer_remaining_seconds()` maps the portable query to FRR's
`event_timer_remain_second()`.

### 7.3 Cancellation

```text
portable core
  -> eigrp_sys_event_cancel(&owner)
       -> owner = NULL
       -> event_cancel(&host_event) if armed
       -> free adapter wrapper
```

The FRR adapter must preserve one-owner/one-event semantics. It must not retain
or fire an EIGRP callback after portable cancellation.

## 8. Socket-read event and packet ingress

Packet receive is a two-stage crossing: FRR provides readiness and raw socket
metadata; portable code owns EIGRP packet validation/demux/protocol handling.

### 8.1 Read readiness

At instance startup or re-arm:

```text
portable core: eigrp_packet_read registration
  -> eigrp_sys_read_add(&eigrp->t_read, eigrp,
                        eigrp_packet_read, eigrp)
       -> find FRR socket for this eigrp_instance_t
       -> event_add_read(FRR loop, eigrp_southbound_event_run, ... fd ...)
```

When the descriptor is readable:

```text
FRR event loop
  -> eigrp_southbound_event_run()
  -> eigrp_packet_read(eigrp)              [portable]
       -> immediately re-arm one-shot read with eigrp_sys_read_add()
       -> allocate portable packet-input object/stream
       -> eigrp->af_vectors.packet_receive(...)
```

### 8.2 IPv4 receive

```text
eigrp_packet_read()                         [portable]
  -> eigrp_ipv4_packet_receive()            [portable AF vector]
       -> eigrp_sys_packet_receive()
            -> eigrp_sys_ipv4_packet_receive()
                 [frr/code/eigrp_southbound_ipv4.c]
                 -> recvmsg(FRR-owned raw socket)
                 -> validate IPv4 header framing
                 -> extract src / dst / ingress ifindex
                 -> if_lookup_by_index(ifindex, instance VRF)
                 -> normalize ingress_vrf_id
                 -> fill eigrp_packet_rx_meta_t
       <- EIGRP-owned address + metadata values
  -> locate portable eigrp_intf_t
  -> eigrp_process_packet_submit(input)
       -> process-owned receive/demux thread
       -> portable packet/RTP/neighbor/DUAL processing
```

FRR does not decode EIGRP opcodes or TLVs in the receive shim.

### 8.3 IPv6 receive

```text
eigrp_packet_read()                         [portable]
  -> eigrp_ipv6_packet_receive()            [portable AF vector]
       -> eigrp_sys_packet_receive()
            -> eigrp_sys_ipv6_packet_receive()
                 [frr/code/eigrp_southbound_ipv6.c]
                 -> recvmsg()
                 -> read sockaddr_in6 source
                 -> read IPV6_PKTINFO destination + ifindex
                 -> resolve ingress VRF from FRR interface
                 -> fill normalized EIGRP metadata
       <- normalized packet
  -> portable receive/demux path
```

IPv6 link-local scope and FRR `in6_pktinfo` are host concerns. Neighbor, RTP,
TLV and DUAL behavior after normalization remain portable.

## 9. Packet egress

Portable code constructs the EIGRP payload and decides multicast/unicast,
reliability, sequencing, pacing and retransmission. FRR only sends the bytes on
the requested interface.

### 9.1 Common call flow

```text
portable packet/RTP/packetizer code
  -> eigrp->af_vectors.packet_send(eigrp, ei, packet)
       -> eigrp_ipv4_packet_send() or eigrp_ipv6_packet_send()
            -> eigrp_sys_packet_send(eigrp, ei, destination,
                                     payload, length)
                 -> AF-specific FRR send function
                      -> sendmsg()
  <- host send result
```

### 9.2 IPv4 egress

`eigrp_sys_ipv4_packet_send()`:

```text
portable payload + eigrp_intf_t + EIGRP IPv4 destination
  -> FRR socket lookup for instance
  -> read normalized local interface address/ifindex
  -> for multicast: set requested outgoing multicast interface
  -> construct host IPv4 header / IP_PKTINFO
  -> sendmsg()
```

The shim may construct the host/network header required by FRR/OS socket
semantics. It must not alter the portable EIGRP payload semantics.

### 9.3 IPv6 egress

`eigrp_sys_ipv6_packet_send()`:

```text
portable payload + eigrp_intf_t + EIGRP IPv6 destination
  -> FRR socket lookup
  -> find interface link-local source in FRR VRF/interface state
  -> set scope/ifindex and IPV6_PKTINFO
  -> for multicast: select outgoing multicast interface
  -> sendmsg()
```

The need to select an FRR link-local address is adapter behavior; the EIGRP
packet remains portable.

## 10. Multicast membership

Portable interface lifecycle requests EIGRP multicast participation; the FRR
shim maps that request onto socket options.

```text
portable interface up/down
  -> eigrp_intf_multicast_update()
  -> eigrp_sys_* multicast service
       -> IPv4: IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP
                 224.0.0.10 on interface address/ifindex
       -> IPv6: IPV6_JOIN_GROUP / IPV6_LEAVE_GROUP
                 ff02::a on ifindex
```

FRR/OS socket calls are host-owned. The decision that an EIGRP interface should
participate is core-owned.

## 11. Interface/address notification flow

FRR learns interface-address state from Zebra. The shim converts the FRR object
to a portable runtime snapshot and the core distributes it to matching EIGRP
instances.

### 11.1 Address add/change snapshot

```text
Zebra ZEBRA_INTERFACE_ADDRESS_ADD
  -> eigrp_zebra_interface_address_add()
       -> zebra_interface_address_read()
       -> struct connected / struct interface
       -> eigrp_frr_interface_state_import()
            struct prefix -> eigrp_prefix_t
            ifindex / name / type / operative / bandwidth / mtu
            secondary flag
       -> eigrp_sys_intf_update(vrf_id, &state)      [portable entry]
            -> iterate portable instances in VRF
            -> eigrp_instance_intf_update_event_enqueue(instance, state)
                 -> instance event queue
                 -> EIGRP_AF_EVENT_INTF_UPDATE processing
                 -> eigrp_network_intf_update()/interface runtime handling
```

The shim reports host state; it does not decide whether an EIGRP `network`
statement matches the address. That decision remains in portable network logic.

### 11.2 Address removal

```text
Zebra ZEBRA_INTERFACE_ADDRESS_DELETE
  -> eigrp_zebra_interface_address_delete()
       -> zebra_interface_address_read()
       -> eigrp_frr_prefix_import()
       -> eigrp_sys_intf_addr_update(vrf_id, ifindex, removed,
                                     EIGRP_INTERFACE_REMOVE_HOST)
            -> enqueue address-update event to matching instances
            -> instance thread verifies matching runtime interface/address
            -> eigrp_intf_runtime_delete(... reason ...)
```

### 11.3 Interface state ownership

FRR may provide operative state, bandwidth, MTU and addresses. The portable core
owns resulting EIGRP interface creation/removal, hello lifecycle, multicast
participation, neighbor effects, metric refresh and topology consequences.

## 12. Router-ID notification flow

```text
Zebra ZEBRA_ROUTER_ID_UPDATE
  -> eigrp_zebra_router_id_update()
       -> zebra_router_id_update_read()
       -> update FRR adapter's router_id_zebra
       -> eigrp_process_routerid_cb(vrf_id)          [portable]
            -> iterate EIGRP instances in VRF
            -> eigrp_instance_event_enqueue(
                   EIGRP_AF_EVENT_ROUTERID_UPDATE)
            -> instance thread handles router-ID change
```

`eigrp_sys_router_id_get()` is the opposite-direction service used by portable
code to query the current normalized FRR router ID.

## 13. EIGRP route installation into Zebra

The portable topology code decides which route is installable. The shim only
converts `eigrp_rib_route_t` into `zapi_route`.

### 13.1 Route add

Representative current flow:

```text
portable topology / route-selection result
  -> build eigrp_rib_route_t
       prefix
       route type / admin distance
       metric / tag
       one or more eigrp_rib_nexthop_t
  -> eigrp_rib_route_add(eigrp, &route)               [public RIB boundary]
       [implemented by FRR southbound adapter]
       -> eigrp_zebra_route_add()
            -> eigrp_frr_prefix_export()
            -> zapi_route_init()
            -> api.type     = ZEBRA_ROUTE_EIGRP
            -> api.instance = EIGRP AS number
            -> api.vrf_id   = EIGRP VRF
            -> normalize nexthops to FRR NEXTHOP_TYPE_*
            -> zclient_route_send(ZEBRA_ROUTE_ADD, ...)
  -> Zebra / kernel RIB
```

DUAL descriptors and private topology objects must not cross into
`eigrp_zebra.c`.

### 13.2 Route delete

```text
portable topology determines route is no longer installable
  -> eigrp_rib_route_del(eigrp, &prefix)
       -> eigrp_zebra_route_del()
            -> eigrp_frr_prefix_export()
            -> zapi_route { type=EIGRP, instance=AS, vrf, prefix }
            -> zclient_route_send(ZEBRA_ROUTE_DELETE, ...)
```

### 13.3 Route-owner notifications

FRR may deliver `ZEBRA_ROUTE_NOTIFY_OWNER`. The adapter may consume host
lifecycle/status information, but must not duplicate portable route-selection
logic based on that notification.

## 14. Zebra redistribution into OpenEIGRP

Redistribution is the reverse RIB direction: FRR supplies source routes; the
portable core decides EIGRP redistribution semantics.

### 14.1 Subscription

Portable configuration:

```text
portable redistribution config
  -> eigrp_rib_redistribute_add(eigrp, source)
       [FRR shim]
       -> eigrp_zebra_redistribute_update()
            -> normalize EIGRP source to Zebra AFI/type/instance/VRF
            -> retain FRR adapter subscription state
            -> zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD, ...)
```

Removal follows the same path to `ZEBRA_REDISTRIBUTE_DELETE` once no EIGRP
consumer still uses that FRR subscription.

### 14.2 Route event from Zebra

```text
Zebra ZEBRA_REDISTRIBUTE_ROUTE_ADD / DEL
  -> eigrp_zebra_redistribute_route()
       -> zapi_route_decode()
       -> eigrp_zebra_redist_route_import()
            FRR prefix      -> eigrp_prefix_t
            FRR route type  -> eigrp_redist_protocol_t
            instance        -> eigrp route-instance value
            metric/tag      -> portable fields
            nexthop         -> eigrp_rib_nexthop_t
            connected/interface-static interface values
                              -> normalized EIGRP vector metric inputs
       -> locate subscribed EIGRP runtimes by VRF + AF + source
       -> suppress reflection of this same EIGRP runtime's own route
       -> ADD: eigrp_rib_redist_add(runtime, &route)
          DEL: eigrp_rib_redist_del(runtime, &route)
            [portable redistribution module]
            -> portable route-map/filter/default-metric processing
            -> portable topology/external route behavior
```

The FRR shim decides only whether the host event belongs to a subscribed
runtime and how to normalize host attributes. It must not calculate EIGRP
external route preference, DUAL state, feasibility or successor selection.

## 15. Policy and key-chain service flow

Some portable features require host-owned services rather than host state
notifications.

### 15.1 Route/filter evaluation

```text
portable EIGRP policy use
  -> eigrp_sys_filter_evaluate(...) or
     eigrp_sys_redistribute_route_map_evaluate(...)
       [frr/code/eigrp_southbound.c]
       -> frr/code/eigrp_policy.c
       -> FRR access-list/prefix-list/route-map APIs
  <- normalized EIGRP_FILTER_* / EIGRP_RESULT_* values
```

FRR owns route-map/access-list objects. Portable code owns what an accept/deny
result means to EIGRP.

### 15.2 Policy change notification

```text
FRR distribute/policy update hook
  -> eigrp_policy.c
  -> normalize/replace portable filter snapshot as required
  -> eigrp_sys_policy_runtime_update()                [portable]
  -> EIGRP runtime refresh/resync behavior
```

The host detects that its policy object changed; the portable core owns the
protocol consequences.

### 15.3 Authentication key lookup

```text
portable authentication code
  -> eigrp_sys_auth_key_lookup(keychain_name, ...)
       -> FRR keychain_lookup()
       -> key_lookup_for_send()
       -> copy normalized key id/material to caller buffer
  <- portable authentication logic computes/validates EIGRP authentication
```

FRR key-chain objects do not cross into portable authentication code.

## 16. Work-queue adaptation

Where portable code uses the host work-queue service:

```text
portable core
  -> eigrp_sys_work_queue_create(eigrp, name, workfunc, deletefunc)
       -> FRR work_queue_new(...)
       -> adapter retains portable callbacks
  -> eigrp_sys_work_queue_enqueue(queue, data)
       -> work_queue_add(...)

FRR worker fires
  -> eigrp_work_queue_host_run()
       -> portable workfunc(queue, data)
       -> EIGRP_WORK_QUEUE_SUCCESS / REQUEUE / BLOCKED
       -> map to FRR WQ_SUCCESS / WQ_REQUEUE / WQ_QUEUE_BLOCKED
```

The work item remains portable-owned data. The shim maps scheduling result
semantics only.

## 17. Show, management and debug flow

FRR presentation must not become a second topology implementation.

```text
vtysh show/debug request
  -> eigrp_vty.c / eigrp_dump.c
  -> eigrp_mgnt.h read/iterate/snapshot API
  -> portable state snapshot
  -> FRR formatting / VTY output
```

Where current code still reaches private portable structures, that should be
treated as integration debt and reduced over time. New show work should use or
extend `eigrp_mgnt.h` rather than directly walking DUAL/topology internals.

Debug enable/disable configuration may enter through portable semantic debug
targets; formatting/sink mechanics remain FRR-specific.

## 18. Logging flow

Portable modules emit EIGRP-owned log severity and text through the portable
logging interface. The FRR build supplies the FRR sink in `frr/code/eigrp_log.c`
so messages enter FRR logging rather than a standalone stderr sink.

```text
portable module
  -> eigrp_log(EIGRP_LOG_*, ...)
  -> FRR logging adapter
  -> zlog / FRR log destination
```

The FRR shim may add host-context diagnostics at a failed boundary conversion
or host API call. Protocol event-log semantics remain portable and must not be
reimplemented as FRR zlog messages.

## 19. Datatype normalization rules

`frr/code/eigrp_frr.c` is the preferred narrow conversion layer for generic FRR
objects.

### 19.1 Prefixes

```text
FRR struct prefix
  -> eigrp_frr_prefix_import()
  -> eigrp_prefix_t

EIGRP eigrp_prefix_t
  -> eigrp_frr_prefix_export()
  -> FRR struct prefix
```

Family and prefix-length validity must be checked at this boundary.

### 19.2 Interfaces

```text
FRR struct interface + struct prefix
  -> eigrp_frr_interface_state_import()
  -> eigrp_intf_runtime_state_t
       interface_name
       ifindex
       EIGRP interface type
       primary/secondary
       operative state
       bandwidth
       mtu
       normalized prefix
```

A portable module must not retain a `struct interface *`.

### 19.3 RIB objects

The Zebra adapter performs explicit conversion between `zapi_route`/
`zapi_nexthop` and `eigrp_rib_route_t`/`eigrp_rib_nexthop_t`. Do not expose
`zapi_route` through portable headers.

## 20. Thread and callback ownership

FRR callbacks often execute on the FRR event-loop thread. Portable EIGRP runtime
mutation may belong on a per-instance worker/AF thread.

Therefore host callbacks should normalize/copy required values and enter a
portable queue/dispatcher rather than hold pointers to ephemeral FRR callback
objects.

Examples already following this pattern:

- interface state: FRR snapshot -> `eigrp_sys_intf_update()` -> instance event;
- address removal -> `eigrp_sys_intf_addr_update()` -> instance event;
- router-ID update -> `eigrp_process_routerid_cb()` -> instance event;
- packet input -> normalized packet object -> `eigrp_process_packet_submit()`;
- configuration targets may use `eigrp_instance_message_call()` internally.

The shim must not directly mutate private DUAL/topology/neighbor state from an
FRR callback.

## 21. Error boundary

Use structured `eigrp_result_t` values across portable interfaces.

The shim translates only at the FRR edge:

```text
FRR input error / unsupported host value
  -> normalize to EIGRP_RESULT_INVALID_ARGUMENT / UNSUPPORTED / ...

portable target result
  -> FRR NB/CLI status, Zebra/log diagnostic, or host callback result
```

Do not hide `EIGRP_RESULT_NOT_IMPLEMENTED` by pretending success in FRR. The
real feature target must remain visible.

For data-plane receive callbacks, malformed host packets or missing required
metadata are dropped at the adapter/packet boundary without introducing
host-specific protocol behavior.

## 22. Required direction matrix

| Flow | FRR origin/target | Shim entry | Portable entry/target |
|---|---|---|---|
| Named config | YANG/NB | `eigrp_northbound.c` | `eigrp_cli.h` semantic targets |
| Classic config | VTY | `eigrp_cli_classic.c` / NB glue | same owning portable targets |
| Packet RX | socket/read event | `eigrp_southbound*.c` | `eigrp_packet_read()` -> packet/RTP/core |
| Packet TX | socket | `eigrp_southbound*.c` | called from packet/RTP/packetizer core |
| Timer/event | FRR event loop | `eigrp_southbound.c` | EIGRP callback supplied by core |
| Work queue | FRR work queue | `eigrp_southbound.c` | EIGRP worker supplied by core |
| Interface add/update | Zebra | `eigrp_zebra.c` + `eigrp_frr.c` | `eigrp_sys_intf_update()` -> instance event |
| Address delete | Zebra | `eigrp_zebra.c` | `eigrp_sys_intf_addr_update()` -> instance event |
| Router ID | Zebra | `eigrp_zebra.c` | `eigrp_process_routerid_cb()` -> instance event |
| RIB install | Zebra target | `eigrp_zebra_route_add()` | request from `eigrp_rib_route_add()` |
| RIB withdraw | Zebra target | `eigrp_zebra_route_del()` | request from `eigrp_rib_route_del()` |
| Redistribute subscribe | Zebra target | `eigrp_zebra_redistribute_update/delete()` | request from portable redistribution config |
| Redistributed route RX | Zebra | `eigrp_zebra_redistribute_route()` | `eigrp_rib_redist_add/del()` |
| Policy evaluation | FRR route-map/list | `eigrp_policy.c` | `eigrp_sys_filter_evaluate()` etc. |
| Auth key lookup | FRR keychain | `eigrp_southbound.c` | `eigrp_sys_auth_key_lookup()` |
| Show/telemetry | VTY/SNMP | `eigrp_vty.c`, `eigrp_dump.c` | `eigrp_mgnt.h` snapshots |
| Logging | FRR zlog target | `eigrp_log.c` | `eigrp_log()` calls from core |

## 23. Integration invariants

Every FRR change must preserve these rules:

1. FRR objects stop at the shim boundary.
2. Portable objects are used for normalized cross-boundary state.
3. DUAL, topology, neighbor, metric, packetizer, TLV and RTP decisions remain
   portable.
4. FRR callbacks do not directly mutate private protocol state when the core
   provides an event/message entry path.
5. Configuration paths terminate at real portable feature targets.
6. RIB install/delete crosses through `eigrp_rib_route_t`/`eigrp_prefix_t`, not
   topology descriptors.
7. Zebra redistribution is normalized before entering the portable
   redistribution module.
8. Packet shims carry bytes and host metadata; they do not decode EIGRP TLVs or
   reproduce reliable transport logic.
9. Host event objects are never exposed as portable timer/event objects.
10. A new FRR integration need should first ask whether the public portable
    contract is missing an operation; it should not be solved by reaching into
    a private portable structure.

## 24. Review checklist for an FRR integration change

Before accepting a change under `frr/`, verify:

- What is the exact FRR callback/API at the host edge?
- Where is the FRR-native value converted into an EIGRP-owned type?
- What exact public/core function receives it?
- If runtime mutation is required, is it queued/dispatched to the owning
  instance thread?
- On the reverse direction, which portable API requests the host service?
- Does the shim translate representation only, or has protocol policy leaked
  into FRR?
- Could the same core operation work unchanged with BIRD/BSD using a different
  shim?
- Are FRR YANG/VTY/Zebra/event/socket types absent from portable public APIs?
- Does error handling preserve the structured EIGRP result rather than hiding
  an incomplete feature?
- Is the appropriate FRR-native test under `frr/test/`, with portable behavior
  tested under `eigrp/test/` where possible?

If the answer to the portability question is no because of protocol behavior,
the behavior is probably in the wrong layer.
