# OpenEIGRP OpenWrt configuration and management specification

Copyright (C) 2026 Donnie V. Savage

## 1. Scope

This document describes the planned OpenWrt configuration, retained state,
reload, and management model for the OpenEIGRP shim.

The guiding rule is simple: OpenWrt owns configuration presentation and service
lifecycle; portable OpenEIGRP owns the semantic targets.

## 2. Configuration architecture

```text
+---------------------------------------------------------------+
| UCI files                                                     |
|   /etc/config/openeigrp                                       |
+---------------------------+-----------------------------------+
                            |
                            v
+---------------------------------------------------------------+
| OpenWrt config adapter                                         |
|                                                               |
|  parse UCI   validate host syntax   normalize values          |
|  retain cfg  writeback/no-forms     map result codes          |
+---------------------------+-----------------------------------+
                            |
                            v
+---------------------------------------------------------------+
| eigrp_cli.h semantic targets                                  |
+---------------------------+-----------------------------------+
                            |
                            v
+---------------------------------------------------------------+
| Portable OpenEIGRP runtime                                    |
+---------------------------------------------------------------+
```

## 3. Intended host-facing pieces

The OpenWrt shim is expected to use:

- **UCI** for retained configuration;
- **procd** for service start/stop/reload lifecycle;
- **ubus** and/or a lightweight CLI for operational queries;
- public `eigrp_mgnt.h` snapshots for operational state presentation.

## 4. Configuration ownership rules

### 4.1 The OpenWrt layer owns

- UCI section layout and option names;
- config-file read and writeback mechanics;
- service reload triggers;
- host-side validation of UCI syntax shape;
- mapping `eigrp_result_t` to host-visible outcomes.

### 4.2 The portable core owns

- whether a semantic EIGRP operation is valid;
- how a setting changes runtime behavior;
- protocol defaults and reset semantics;
- feature support vs `EIGRP_RESULT_NOT_IMPLEMENTED`.

## 5. Configuration flow

```text
uci commit / service reload / ubus mutation
    -> config adapter parses UCI state
    -> normalize addresses, ASNs, timers, names, booleans
    -> call the real eigrp_cli.h target for each setting
    -> collect structured results
    -> retain config even when runtime behavior is incomplete
```

## 6. Proposed logical model

The exact UCI schema will be refined during implementation, but the logical
shape should align with named-mode EIGRP rather than with host-specific object
leakage.

```text
config process 'savage'
    option name 'savage'

config address_family
    option process 'savage'
    option afi 'ipv4'
    option asn '4453'
    option shutdown '0'
    option router_id '10.0.0.1'

config network
    option process 'savage'
    option afi 'ipv4'
    option asn '4453'
    option prefix '10.0.0.0/8'

config interface
    option process 'savage'
    option afi 'ipv4'
    option asn '4453'
    option ifname 'lan'
    option hello_interval '5'
    option hold_time '15'
```

This is an architectural sketch, not a frozen final schema.

## 7. Named-mode ordering expectations

The intended configuration sequence follows the existing project development
order:

```text
1. router eigrp savage / IPv4 AF AS 4453
2. IPv6 AF AS 4453
3. multiple AS under one named parent
4. distinct case-sensitive named parents
```

The OpenWrt config layer should preserve that same semantic ordering when it
replays retained UCI configuration at startup or reload time.

## 8. Reload and writeback behavior

### 8.1 Reload

```text
UCI change
    -> procd reload_service()
    -> config adapter diffs or replays retained config
    -> call semantic add/update/reset/remove functions
    -> preserve service continuity where possible
```

### 8.2 Writeback

```text
host-side config mutation
    -> semantic call succeeds
    -> config adapter updates retained UCI state
    -> commit/write as policy requires
```

A failed semantic operation must not be silently written as if it succeeded.

## 9. Result handling

The OpenWrt config layer must preserve distinctions such as:

- success;
- invalid input;
- not found;
- already exists;
- unsupported runtime behavior;
- `EIGRP_RESULT_NOT_IMPLEMENTED`.

This is important because configuration retention may still be required even
when runtime support is incomplete.

## 10. Management and operational visibility

```text
show / status / telemetry request
    -> ubus method or CLI frontend
    -> eigrp_mgnt.h snapshot readers/iterators
    -> present neighbors, interfaces, topology, routes, traffic, event log
```

### Management presentation layers

```text
+-------------------------+
| ubus methods / CLI      |
+------------+------------+
             |
             v
+-------------------------+
| OpenWrt mgmt adapter    |
|   JSON/text formatting  |
+------------+------------+
             |
             v
+-------------------------+
| eigrp_mgnt.h snapshots  |
+-------------------------+
```

The management adapter must not walk private portable structures directly.

## 11. procd lifecycle

```text
boot/start
    -> procd launches daemon
    -> daemon loads retained UCI config
    -> applies semantic configuration

reload
    -> procd signals reload
    -> daemon re-reads UCI and applies deltas/replay

stop
    -> daemon shuts down cleanly
    -> callbacks and sockets are canceled
```

## 12. Acceptance checkpoints

- retained configuration survives restart;
- reload applies mutations without stale runtime state;
- documented reset/no forms restore defaults;
- IPv6 config remains writable even before all runtime pieces exist;
- operational views use public management APIs only.
