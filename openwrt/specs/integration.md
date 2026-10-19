# OpenEIGRP OpenWrt integration specification

Copyright (C) 2026 Donnie V. Savage

## 1. Scope

This document defines the planned concrete integration contract between the
portable OpenEIGRP core and the native OpenWrt host shim under `openwrt/`.

It answers the OpenWrt-specific question that `../specs/platform-integration.md`
leaves host-neutral: how OpenWrt host facilities should map onto the public
OpenEIGRP interfaces.

The authoritative portable public API remains:

```text
eigrp/code/eigrp.h
eigrp/code/eigrp_cli.h
eigrp/code/eigrp_mgnt.h
eigrp/code/eigrp_rib.h
eigrp/code/eigrp_sys.h
```

OpenWrt-private declarations stay under `openwrt/code/` and must not leak into
portable headers.

## 2. Architecture overview

```text
+---------------------------------------------------------------------+
| OpenWrt host                                                        |
|                                                                     |
|  UCI      procd      ubus/CLI      netifd      netlink   kernel RIB |
+----+---------+-----------+------------+------------+----------+-----+
     |         |           |            |            |          |
     +---------+-----------+------------+------------+----------+
                                   |
                                   v
+---------------------------------------------------------------------+
| openwrt/code/                                                       |
|                                                                     |
|  config   lifecycle   sys runtime   interface   packet I/O   RIB    |
|  policy   management  timers/fds    state       v4/v6 proto88 routes|
+-------------------------------+-------------------------------------+
                                |
                                v
+---------------------------------------------------------------------+
| portable OpenEIGRP core                                             |
|                                                                     |
| DUAL | topology | RTP | metrics | TLVs | neighbor logic | mgmt      |
+---------------------------------------------------------------------+
```

The shim is a **translation boundary**, not another protocol engine.

## 3. Source ownership map

Expected OpenWrt source families:

```text
openwrt/code/eigrp_owrt_runtime.c
    runtime initialization, timers, event scheduling, fd readiness,
    work queues, monotonic time, shutdown serialization

openwrt/code/eigrp_owrt_interface.c
    link and address inventory, netifd/netlink observations,
    normalized interface notifications into the core

openwrt/code/eigrp_owrt_packet.c
    IPv4/IPv6 raw socket lifecycle, multicast membership,
    packet send/receive and receive metadata normalization

openwrt/code/eigrp_owrt_rib.c
    Linux kernel route installation/removal, redistribution ingress,
    reconnect/replay

openwrt/code/eigrp_owrt_config.c
    UCI/config parsing and semantic calls into eigrp_cli.h

openwrt/code/eigrp_owrt_policy.c
openwrt/code/eigrp_owrt_keychain.c
    host-side policy and authentication material lookup

openwrt/code/eigrp_owrt_mgnt.c
    read-only local management presentation using eigrp_mgnt.h snapshots

openwrt/code/eigrp_owrt_daemon.c
    foreground procd-managed daemon and Unix signal lifecycle
```

## 4. What the shim owns, and what it must not own

### 4.1 OpenWrt-owned responsibilities

- daemon lifecycle on OpenWrt;
- event loop integration, timers, and fd readiness;
- default VRF/interface identity resolution;
- raw IPv4/IPv6 packet sockets and multicast joins;
- interface and address discovery/updates;
- kernel route installation/removal and redistribution subscriptions;
- UCI parsing, retained configuration, reload, and writeback;
- key and policy lookups;
- management presentation through ubus or CLI.

### 4.2 Portable-core responsibilities

- DUAL and topology-table semantics;
- successor and feasible-successor selection;
- metric calculation rules;
- neighbor state-machine decisions;
- RTP reliable-transport behavior;
- TLV and packet semantics.

## 5. Boundary rule

Only EIGRP-owned public types may cross into `eigrp/code/`.

```text
Allowed across boundary:
  eigrp prefixes, addresses, metrics, route snapshots,
  interface runtime state, result codes, opaque EIGRP identities

Forbidden across boundary:
  UCI section objects
  ubus request objects
  netifd interface structs
  netlink socket message types
  Linux route structs
  OpenWrt event-loop or service structs
```

## 6. Startup and shutdown

### 6.1 Startup flow

```text
procd starts daemon
    -> openwrt runtime init
    -> eigrp_sys host services become available
    -> RIB and policy/key services initialize
    -> eigrp_init()
    -> eigrp_process_receive_start()
    -> load retained UCI configuration
    -> apply semantic eigrp_cli.h targets
    -> discover interfaces and addresses
    -> notify interface state to portable core
    -> join multicast / open sockets on participating interfaces
    -> normal protocol operation
```

### 6.2 Shutdown flow

```text
procd stop/restart
    -> stop accepting new config and packet input
    -> remove or quiesce configured runtime contexts
    -> cancel host events and fd callbacks
    -> withdraw/cleanup host-side packet and RIB state
    -> eigrp_terminate()
    -> release OpenWrt runtime resources
```

The key acceptance rule is that no host callback may outlive the portable
object it references.

## 7. Configuration flow

```text
UCI or CLI mutation
    -> OpenWrt config frontend parses host syntax
    -> normalize strings, numbers, addresses, names, IDs
    -> call owning eigrp_cli.h semantic target
    -> map eigrp_result_t back to host-facing result
    -> retain or write back config as required
```

The configuration frontend must not implement protocol semantics itself.

## 8. Interface lifecycle flow

```text
netifd/netlink event
    -> build normalized eigrp_intf_runtime_state_t
    -> eigrp_sys_intf_update() / down() / remove() / addr_update()
    -> portable interface participation logic
    -> adjacency and packet-path changes as determined by core
```

The OpenWrt shim owns **how** interface state is observed. The portable core
owns **what EIGRP does** with it.

## 9. Packet receive and transmit flow

### 9.1 Receive

```text
Linux raw socket packet (protocol 88)
    -> OpenWrt packet adapter identifies AF, VRF, ifindex, src/dst metadata
    -> eigrp_sys_*packet_receive envelope to portable core
    -> portable decode / validation / neighbor / RTP handling
```

### 9.2 Transmit

```text
portable packet/RTP logic
    -> eigrp_sys_*packet_send
    -> OpenWrt packet adapter selects socket/interface destination
    -> kernel transmit
```

The shim must not reinterpret reliable transport semantics, packetization, or
TLV meaning.

## 10. Routing-table flow

### 10.1 Install/remove selected routes

```text
portable route selection
    -> normalized eigrp_rib_route
    -> OpenWrt RIB adapter translates to Linux route operation
    -> kernel RIB update
```

### 10.2 Redistribution ingress

```text
host route change
    -> OpenWrt RIB adapter normalizes source route
    -> public redistribution ingress API
    -> portable policy/metric/topology logic
```

## 11. Management flow

```text
ubus or CLI show request
    -> OpenWrt management adapter
    -> eigrp_mgnt.h snapshot / iterator APIs
    -> format as JSON, text, or structured output
```

Management output should come from public snapshot APIs, not from direct access
to private portable structures.

## 12. Minimum implementation sequence

```text
1. setup + package skeleton
2. runtime services
3. interface inventory
4. packet sockets
5. kernel RIB integration
6. UCI configuration and writeback
7. procd and ubus management
8. QEMU/device UUT
9. container image and release automation
```

This sequence matches the staged task list in `../TASKS.md`.

### OW-07 kernel RIB boundary

Linux route ownership and notifications live in `eigrp_owrt_rib.c`. The
portable `eigrp_rib.h` exchange uses normalized EIGRP prefixes, next hops,
route sources and result codes. Linux rtnetlink headers and route messages
remain exclusively in the OpenWrt shim. See [rib.md](rib.md) for flows and
limitations. A service-process lifecycle comes in OW-09.

## OW-08: retained configuration boundary

```text
OpenWrt /etc/config/openeigrp
              |
              v
         libuci reader
              |
              v
  eigrp_owrt_config_validate()
              |
              v
  eigrp_owrt_config_replace()
              |
      +-------+-------+
      v               v
 eigrp_cli.h      eigrp_owrt_policy.c
 (real targets)    (EIGRP-owned service callbacks)
      |               |
      +-------+-------+
              v
       Portable OpenEIGRP
```

Only the normalized EIGRP public values cross into the core. The OW-09 daemon
will own invocation order and synchronize reloads with operational callbacks;
there is no service activated by the OW-08 archive-only package.

## OW-09 deployed lifecycle

The foreground daemon, local operational socket and procd init script are
implemented as described in [daemon.md](daemon.md). A custom ubus API remains
a future presentation layer; the daemon exposes read-only state through
`openeigrpctl` and leaves generic service state to procd.
