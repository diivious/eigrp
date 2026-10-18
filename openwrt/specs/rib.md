# OW-07: Linux kernel RIB boundary

Copyright (C) 2026 Donnie V. Savage

## Ownership and interfaces

The OpenWrt RIB adapter is `openwrt/code/eigrp_owrt_rib.c` with pure Linux
rtnetlink translation in `eigrp_owrt_rib_netlink.c`. Private declarations
are in `eigrp_owrt_rib.h`. It implements the host side of the existing public
`eigrp_rib.h` API. None of the Linux netlink objects, route sockets, route
cache records, or platform thread types are passed to portable EIGRP.

```text
          portable EIGRP core (RFC 7868)
             topology / DUAL / metric
                       |
             public eigrp_rib.h
                       |
             +---------+----------+
             |                    |
        selected routes      subscriptions /
        add/del/replay       route source events
             |                    |
             v                    v
    eigrp_owrt_rib.c  <----> owned route/source snapshots
             |                    |
    eigrp_owrt_rib_netlink.c (Linux rtnetlink encoding/decoding)
             |                    |
             v                    v
   RTM_NEWROUTE / DELROUTE   GETROUTE dump + route watch
             |                    |
             +---------> Linux main routing table
```

`eigrp_rib_route_add()` installs or replaces a selected route by issuing a
synchronous rtnetlink request and checking the **kernel ACK**. A newly owned
route uses `NLM_F_CREATE|NLM_F_EXCL`, so the first install does not overwrite
an unrelated existing route with the same Linux routing key. An updated route
uses replacement for an existing owned key. A changed priority first withdraws
the previous key; rollback is attempted if installation fails. An unsuccessful
kernel request is **not** recorded as successfully installed.

`eigrp_rib_route_del()` withdraws only prefixes recorded as owned by the given
EIGRP instance. `eigrp_rib_instance_delete()` also attempts to withdraw its
outstanding routes, and `eigrp_rib_finish()` cleans up the remaining entries.
The kernel marks installed routes with `RTPROT_EIGRP` (192), and host state
retains the owning EIGRP instance. This prevents the adapter from adopting
arbitrary external routes and from redistributing its own protocol routes.

### Supported and explicitly limited semantics

- IPv4 and IPv6 unicast FIB routes, default VRF, Linux **main** table only.
- Single and multipath next hops. `RTA_MULTIPATH` represents ECMP; the
  8-bit Linux nexthop hop weight saturates at 256 (0 maps to 1).
- Public EIGRP scalar metric is mapped to Linux route priority, saturating
  at `UINT32_MAX`. EIGRP administrative distance is **not** equivalent to
  Linux route priority. No claim of kernel-enforced administrative distance.
- Route tags are not exported as kernel realms: `RTA_FLOW` is not a safe
  lossless representation of arbitrary 32-bit EIGRP tags.
- Source subscriptions with route-instance **0** map these Linux `rtm_protocol`
  codes: KERNEL (connected), STATIC, RIP, OSPF, ISIS, BGP. Other route-instance
  values and EIGRP self-redistribution are explicitly `UNSUPPORTED`.
- A source route must satisfy the portable core's seed metric rules. The host
  does **not** invent redistribution metric vectors for static or learned routes.
  Configure an explicit seed metric or default metric where required.
- Kernel route replace operations are protected by locally retained ownership
  and the initial exclusive add, but Linux does not offer an atomic
  compare-and-swap by owner. Concurrent external programs rewriting exactly the
  same prefix/priority can still race. Multi-process EIGRP ownership and VRFs
  are outside this phase.

## Source-route lifecycle and replay

```text
RTM_GETROUTE (v4 then v6)
            |
    decode into owned-value snapshots
            |
       compare old/new
            |
   +--------+----------------+
   |                         |
withdraw removed/changed   introduce added/changed
   |                         |
 eigrp_rib_redist_del()   eigrp_rib_redist_add()
              \           /
               \         /
         portable AF redistribution rules
```

The route monitor subscribes to `RTMGRP_IPV4_ROUTE` and
`RTMGRP_IPV6_ROUTE` before its initial dump. Notifications cause a new
**authoritative** v4/v6 route dump, rather than trusting a possibly incomplete
notification stream. A failed dump does not replace the prior source snapshot.
Refreshing and synthetic test reconciliation share the same delta logic.

Only routes matching an explicit `eigrp_rib_redistribute_add()` subscription
are sent to an EIGRP AF. Subscriptions can be removed independently and their
remaining source routes are withdrawn. Notifications are serialized through the
OW-04 protocol lock, and are processed without holding the RIB state mutex.

When the kernel reports deletion of an owned EIGRP route, its cached
`present` bit is cleared. An authoritative route dump on watcher restart
also detects entries missing from the FIB and invalidates them. The watcher calls the existing portable
`eigrp_rib_routes_replay()` function, which re-evaluates installed EIGRP
successors and can reinstall the selected route with an exclusive add.
An unchanged local route must not falsely bypass this replay merely because
its EIGRP metric/next-hop state matches its prior value.

## Lifecycle

```text
 eigrp_sys_runtime_init()
 eigrp_init()
 eigrp_owrt_rib_start()  # subscribed dump, watcher thread
       |  periodic event-driven refresh/route replay
 eigrp_owrt_rib_stop()   # signal, join, close descriptors
 eigrp_terminate()       # RIB cleanup, then runtime finish internally
```

The daemon in OW-09 will own this exact ordering. No daemon or host service
process is invented by OW-07. Startup/stop are idempotent; watcher errors are
visible and require an explicit stop/restart, not silent false success.

## Testing

Component and toolchain tests:

```sh
bash openwrt/test/recipe-test.sh
bash openwrt/test/runtime-test.sh
```

Linux unit under test, with read-only kernel route dump and deterministic
IPv4/IPv6 encoding, ECMP, notification, subscribe/unsubscribe, and restart
cases:

```sh
bash openwrt/test/rib-uut.sh
```

An **optional privileged, isolated namespace UUT** actually inserts,
updates and withdraws kernel routes. It changes no routes in the host's
main namespace and requires `iproute2`, root and `CAP_NET_ADMIN`:

```sh
sudo bash openwrt/test/rib-uut.sh --kernel
```

The OpenWrt ARM64 cross-build (`./tools/openwrt.sh --build` after a package
clean), actual router route exchange, route race/ownership, and restart on an
OpenWrt device are separate acceptance gates. No privileged live kernel tests
were run while producing this increment.

## Test naming convention

Use `*-test.sh` for tools, compilation, recipes and individual component
checks. Use `*-uut.sh` for tests involving a running unit, real host interfaces,
packet path, kernel RIB, or protocol scenarios. The previous test script names have been replaced without
compatibility wrappers. Clean up the former filenames
when applying a ZIP increment to an existing working tree.
