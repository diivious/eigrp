# OW-05: OpenWrt interface discovery and lifecycle

Copyright (C) 2026 Donnie V. Savage

## Scope and current boundary

Implemented in `openwrt/code/eigrp_owrt_interface.c`,
`eigrp_owrt_interface_state.c` and `eigrp_owrt_interface.h`. All OpenWrt-owned
symbols use `eigrp_owrt_*`. The portable core's existing public entry points
remain `eigrp_sys_interface_walk()` and `eigrp_sys_intf_*()`.

This is a **real Linux route-netlink integration**, not a mocked interface
list. OW-05 produces normalized state and lifecycle notifications; it does not
create EIGRP packet sockets, join multicast groups, or install routes.

## Information flow

```text
           Linux kernel interface + IPv4/IPv6 state
                              |
                              v
         +------------------------------------------+
         | NETLINK_ROUTE                            |
         | RTMGRP_LINK / IPV4_IFADDR / IPV6_IFADDR |
         +--------------------+---------------------+
                              | notifications
                              v
            +---------------------------------------+
            | eigrp_owrt_interface_monitor          |
            | recv link/address change or overflow |
            +-----------------+---------------------+
                              | refresh
                              v
            +---------------------------------------+
            | RTM_GETLINK + RTM_GETADDR             |
            | read complete normalized snapshot    |
            +-----------------+---------------------+
                              | old/new comparison
                              v
            +---------------------------------------+
            | eigrp_owrt_interface_snapshot_emit    |
            | add/change, down, remove, addr delete |
            +-----------------+---------------------+
                              | public EIGRP API
                              v
    +----------------------------------------------------+
    | eigrp_sys_intf_update()                            |
    | eigrp_sys_intf_down() / remove() / addr_update()  |
    +-------------------------+--------------------------+
                              |
                              v
                   eigrp/code/ (portable)
```

An independent `eigrp_sys_interface_walk(instance, callback, arg)` performs a
fresh kernel dump, filters by the instance's address family, and invokes the
caller for each address. The callback receives borrowed state valid only for
the call. No Linux netlink objects cross the public boundary.

## Normalization

| Linux source | EIGRP normalized value |
|---|---|
| `ifi_index` | `eigrp_ifindex_t` |
| `IFLA_IFNAME` | interface name string |
| `IFLA_MTU` | MTU |
| `IFF_UP` and `IFF_RUNNING` | carrier/operative state |
| `IFF_MULTICAST` | whether participation is possible |
| `IFA_LOCAL` or `IFA_ADDRESS` | IPv4/IPv6 address |
| `ifa_prefixlen` | prefix length |
| `IFA_FLAGS` / `IFA_F_SECONDARY` | secondary IPv4 or temporary IPv6 address |
| `/sys/class/net/<name>/speed` | reported Mbps converted to kb/s |

For links without a usable speed (many bridges, tunnels, and virtual
interfaces), use an explicit 100000 kb/s fallback. This is an inventory input,
not a metric calculation; portable EIGRP owns metric behavior. Current
`operative` requires link UP/RUNNING and multicast capability. The interface
type field remains zero, matching the existing portable host-value convention.

Address records are retained using host IP address and prefix length, without
masking the local host bits. Secondary address notifications are preserved in
the host state, although the portable `eigrp_sys_intf_update()` currently
suppresses secondary participation.

## Snapshot lifecycle

```text
First start:
  bind multicast notification socket
       -> full link/address dump
       -> publish initial snapshot and notifications
       -> start asynchronous watcher

Link becomes nonoperative:
  previous UP + new DOWN
       -> eigrp_sys_intf_down()
       -> address metadata updates where needed

Address removed:
  previously present, now absent
       -> eigrp_sys_intf_addr_update(...REMOVE_HOST)

Interface removed or renamed:
  prior ifindex disappears, or same ifindex new name
       -> eigrp_sys_intf_remove(...REMOVE_HOST)
       -> emit newly discovered interface addresses

Stop:
  disable callback publishing
       -> wake/join watcher thread
       -> close netlink/wake descriptors
       -> free snapshot
```

No duplicate notifications are emitted when two snapshots are identical.
Kernel notification overflow causes a full rescan instead of guessing which
event was missed. Incomplete or interrupted netlink dumps are rejected rather
than publishing partial state.

## Daemon lifecycle ordering

```text
OW-06/OW-09 daemon:
    eigrp_sys_runtime_init()
    eigrp_init()
    eigrp_owrt_interface_start()
    [configuration and packet/RIB initialization]

Shutdown:
    stop packet ingress
    eigrp_owrt_interface_stop()    <-- join before destroying core
    eigrp_terminate()             <-- finishes runtime
```

The watcher thread calls EIGRP-owned lifecycle APIs while holding
`eigrp_owrt_runtime_enter()/leave()` so the runtime callback thread and
host-side notifications cannot mutate portable state simultaneously.
Do not call `eigrp_owrt_interface_stop()` from a watcher callback.

## Present limitations

- The adapter currently publishes only the default VRF. Linux VRF device
  mapping and per-VRF notification dispatch require a future explicit design.
- Netifd lifecycle is observed through Linux link/address changes. Direct
  netifd/ubus subscriptions are not required for the existing notifications.
- A dead watcher stops dispatching and logs an error. A supervising daemon
  should stop and restart the interface adapter after a watcher failure.
- These libraries have no daemon entry point yet. Socket and RIB consumers
  are added in OW-06 and OW-07; procd wiring follows in OW-09.

## Validation

```sh
bash openwrt/test/interface-smoke.sh    # Linux only, no root needed
bash openwrt/test/runtime-smoke.sh
bash openwrt/test/recipe-smoke.sh
./tools/openwrt.sh --build              # OpenWrt target cross-build
```

The deterministic test covers first scan, no-op rescan, secondary address
addition, link-down/up, address deletion, rename, link removal, AF-filtered
live enumeration, refresh, stop, and restart. An actual link-flap/route test
on an OpenWrt UUT is a separate OW-10 acceptance gate.
