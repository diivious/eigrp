# EIGRP CLI Specification

Copyright (C) 2026 Donnie V. Savage

This file is a new project design document. New files created for this EIGRP work use Donnie V. Savage as the copyright owner unless stated otherwise.

Existing source files must preserve all prior copyright notices, SPDX identifiers, and author history. Refactoring an existing file is not permission to remove earlier authorship.

## 1. Purpose

This document owns the EIGRP CLI, VTY, show, clear, and debug command surface for the `eigrpd` project.

It extends `design-spec.md` and keeps implementation-facing CLI notes out of the general development design spec.

## 2. Authority

CLI authority follows the project authority order defined in `design-spec.md`:

1. Donnie V. Savage.
2. RFC 7868 for protocol behavior unless intentionally clarified or superseded by Donnie V. Savage.
3. Cisco named-mode command references for user-facing CLI shape where they do not conflict with this project.
4. FRR only for build compatibility, daemon integration, VTY plumbing, clippy generation, and library/API compatibility.
5. Existing `eigrpd` code only where it does not conflict with this specification.

## 3. Classic and Named-Mode CLI Direction

Both EIGRP router entry forms are valid and must be installed concurrently:

```text
router eigrp <asn>
router eigrp <name>
```

`router eigrp <asn>` enters the classic/legacy numeric-AS configuration mode.
`router eigrp <name>` enters named mode; the EIGRP protocol instance is created when an address-family and autonomous-system are configured.

The CLI grammar must preserve type-based disambiguation: numeric values in the valid ASN range select the numeric-AS command, while non-numeric words select the named-mode command. Do not remove either command surface when modifying the other.

Classic interface-style commands may coexist with their named-mode equivalents while compatibility is required, including:

```text
ip hello-interval eigrp <asn> <seconds>
ip hold-time eigrp <asn> <seconds>
ip summary-address eigrp <asn> <prefix>
ip authentication mode eigrp <asn> ...
ip authentication key-chain eigrp <asn> ...
```

The named-mode user-facing configuration model is:

```text
router eigrp <name>
 address-family ipv4 unicast [vrf <name>] autonomous-system <asn>
  network <address> [wildcard]
  network <prefix/len>
  eigrp router-id <address>
  af-interface <interface>
   hello-interval <seconds>
   hold-time <seconds>
   delay <tens-of-microseconds>
   passive-interface
   authentication mode <md5|hmac-sha-256>
   authentication key-chain <name>
   no shutdown
  exit-af-interface
  topology base
   variance <value>
   maximum-paths <value>
   redistribute ...
  exit-af-topology
 exit-address-family
exit
```

The initial implementation may map named-mode CLI onto the current FRR/YANG storage where the storage model already exists. That is an implementation bridge, not a protocol or UX direction.

Commands present in Cisco named mode but not backed by current implementation logic must be installed as explicit stubs instead of being silently absent. Stub commands must return success and log/debug that the command is not configured yet.

IPv6 address-family configuration is a named-mode command surface requirement, but it remains a stub until the IPv6 EIGRP data path is implemented.

## 4. Named-Mode VTY and Debug CLI Direction

`eigrp_vty.c` owns named-mode operational commands only.

Classic operational commands must not be installed as the active user-facing surface:

```text
show ip eigrp ...
clear ip eigrp ...
```

The active operational command surface is:

```text
show eigrp address-family ipv4 [vrf <name>] [<asn>] neighbors ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] interfaces ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] topology ...
clear eigrp address-family ipv4 [vrf <name>] [<asn>] neighbors ...
```

VTY command function names follow the command mode first:

```text
show_eigrp_neighbor_cmd
show_eigrp_interface_cmd
show_eigrp_topology_cmd
clear_eigrp_neighbor_cmd
```

Do not use module/object/action names for VTY command objects such as:

```text
eigrp_vty_show_neighbor_cmd
eigrp_vty_clear_neighbor_cmd
```

Debug command coverage must include the packet/testing surface even when backing logic is partial:

```text
debug eigrp packet ...
debug eigrp transmit ...
debug eigrp event ...
debug eigrp timers
debug eigrp neighbor
```

When a debug or show command is present but not fully backed by implementation logic, it must log/debug a clear "not configured yet" message rather than silently missing from the CLI.
