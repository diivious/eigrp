# OpenEIGRP OpenWrt daemon and operational management (OW-09)

Copyright (C) 2026 Donnie V. Savage

## Scope and naming

The foreground executable `openeigrpd` is a real OpenWrt host for the portable
OpenEIGRP core. OpenWrt source and symbol names use `eigrp_owrt_*`; exported
portable host callbacks retain their EIGRP-owned `eigrp_sys_*` names. The
routing protocol implementation is not duplicated in the service layer.

## Lifecycle

```text
       OpenWrt procd                  openeigrpd
            |                             |
   /etc/init.d/openeigrp --check -------->| libuci load + schema validation
            |                             |
     procd supervised launch ------------>| block termination/reload signals
            |                             | init host scheduler, policy, RIB
            |                             | init portable core and receive worker
            |                             | semantic UCI apply
            |                             | start kernel RIB watcher
            |                             | start Linux interface watcher
            |                             | create /var/run/openeigrpd.sock (0600)
            |                             v
            |                     poll signalfd + local client socket
            |                             |
      reload trigger ---- SIGHUP -------->| load + validate, then replace
            |                             | preserve old config on parse failure
            |                             | best-effort semantic rollback otherwise
            |                             |
      stop ------------ SIGTERM --------->| close management socket
            |                             | stop interface / RIB watchers
            |                             | terminate portable core + host runtime
            |                             | unbind policy, free UCI snapshot
            v                             v
          procd                     process exits
```

A foreground service enables native procd crash detection and restart. The
runtime is not daemonized a second time. On startup the configuration is
validated before opening a protocol socket or modifying a route. A failed
service start exits nonzero; `procd` can report this as a service failure.

The input is `/etc/config/openeigrp`. `openeigrpd --check` validates that file
without starting the EIGRP core or touching the routing table. `SIGHUP`
reloads a new validated snapshot using the OW-08 semantic API, with OW-08's
explicitly best-effort rollback limitation. Reload is serialized against
scheduler callbacks; the portable core owns named instances, AFs and topology.

## Operational console

```text
openeigrpctl status | interfaces | neighbors | topology | traffic | events
        |
        v
/var/run/openeigrpd.sock (AF_UNIX, mode 0600)
        |
        v
  eigrp_owrt_mgnt.c (read-only formatter)
        |
        v
     eigrp_mgnt.h public iterators
        |
        v
     portable EIGRP-owned snapshots
```

No CLI command writes configuration, routes or socket settings. The command
socket is owned by the daemon, recreated on restart, and removed on clean
shutdown. A stale socket is removed only when it is known to be a socket;
other filesystem objects must not be overwritten. No custom ubus RPC object
is installed by OW-09. OpenWrt exposes generic procd state via its own ubus
service interface and `/etc/init.d/openeigrp status`.

The operational console returns text records suitable for terminal use and
script capture. It is not an authentication or remote network interface;
local filesystem permissions are the access control. Never widen the socket
permissions merely to expose diagnostics. Sensitive UCI secrets are not
included in management output.

## Packaging

`openwrt/feed/openeigrp/Makefile` stages the executable and client built from
`openwrt/code/`, retains the public core/runtime archives for development,
installs the init script and private UCI configuration, and declares the
`libuci` dependency. procd logs stdout/stderr to OpenWrt's logging service.

## Acceptance boundaries

The native `daemon-uut.sh` test links the **real** portable core and host
library, replacing only the UCI file reader with a test-only loader. It
checks the executable link, validation, foreground lifetime, read-only IPC,
reload behavior, signal cleanup, and socket permissions. It does not claim
that two OpenWrt nodes exchange Hellos or that real UCI/procd is available
on this host. Those are OW-09 device acceptance and OW-10 topology tests.

Local tests remain independently callable; `tools/openwrt.sh --uut` is the
standard orchestration entry point.
