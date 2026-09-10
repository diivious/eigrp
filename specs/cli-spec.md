# EIGRP CLI Specification

Copyright (C) 2026 Donnie V. Savage

This file is a new project design document. New files created for this EIGRP work use Donnie V. Savage as the copyright owner unless stated otherwise.

Existing source files must preserve all prior copyright notices, SPDX identifiers, and author history. Refactoring an existing file is not permission to remove earlier authorship.

## 1. Purpose

This document owns the EIGRP CLI, VTY, show, clear, and debug command surface for the `eigrpd` project.

It extends `design-spec.md`; function/module naming follows `code-conventions.md`, while this file owns the CLI/VTY command surface, management flow, retention, and command-specific target requirements.

## 2. Authority

CLI authority follows the project authority order defined in `design-spec.md`:

1. Donnie V. Savage.
2. RFC 7868 for protocol behavior unless intentionally clarified or superseded by Donnie V. Savage.
3. Cisco named-mode command references for user-facing CLI shape where they do not conflict with this project.
4. FRR only for build compatibility, daemon integration, VTY plumbing, clippy generation, and library/API compatibility.
5. Existing `eigrpd` code only where it does not conflict with this specification.

## 3. Classic and Named-Mode CLI Direction

Both EIGRP router entry forms are valid and must remain installed concurrently:

```text
router eigrp <asn>
router eigrp <name>
```

`router eigrp <asn>` enters the classic/legacy numeric-AS configuration mode.
`router eigrp <name>` enters named mode; the named process is a parent container and its protocol context is established by an address-family and autonomous-system configuration.

The CLI grammar must preserve type-based disambiguation: numeric values in the valid ASN range select the numeric-AS command, while non-numeric words select the named-mode command.

The existing classic/legacy command surface is compatibility code and is frozen for this work:

- preserve existing classic configuration commands
- preserve existing classic show/clear commands
- do not add new classic configuration commands
- do not add new `show ip eigrp ...` commands
- do not add new `clear ip eigrp ...` commands
- do not remove or regress existing legacy behavior while adding named-mode commands

All new CLI implementation work is named-mode work.

## 4. Named-Mode Command Scope

The named-mode CLI should implement EIGRP commands that configure, control, observe, or debug EIGRP protocol behavior, including:

- address-family configuration
- EIGRP interface behavior
- topology and route management
- route filtering and routing policy attachment
- metrics and summarization
- neighbors and timers
- redistribution
- protocol show commands
- protocol clear commands
- protocol debug commands
- EIGRP technical-support output

A command appearing in a Cisco guide is not automatically an EIGRP command. Commands shown only to prepare the host platform or an example environment must not be added to EIGRP CLI. For example, `ip vrf` is a platform routing command, not an EIGRP command. Commands owned by another host CLI mode, such as route-map, key-chain, or VRF configuration, remain owned by that host subsystem; EIGRP integrates with those objects through its northbound boundary rather than reinstalling their CLI under EIGRP.

The following command classes are explicitly excluded from the named-mode CLI for this implementation:

- `eigrp stub`
- `stub`
- `eigrp upgrade-cli`
- SAF / Service Advertisement Framework commands, including `ipv4-sf`, `service-family`, and their submodes
- platform-dependent BFD commands
- platform-dependent MTR commands
- Cisco router-to-radio/VMI commands such as `dampening-change`, `dampening-interval`, and the platform-specific `eigrp interface` form
- platform test commands
- Cisco plugin management commands such as `show eigrp plugins`
- NSF / graceful-restart configuration commands for this pass

NSF/graceful-restart CLI exclusion does not mean the EIGRP protocol restart behavior is permanently excluded. Protocol restart support may be required later; the host restart/switchover trigger is a platform integration concern and the CLI can be added when that work is undertaken.

`show eigrp tech-support` is considered useful EIGRP operational output and should remain available.

### 4.1 Named-Mode Configuration Hierarchy

Named mode supports both IPv4 and IPv6 address families:

```text
router eigrp <name>
 address-family ipv4 unicast [vrf <name>] autonomous-system <asn>
  ...
 exit-address-family
 address-family ipv6 unicast [vrf <name>] autonomous-system <asn>
  ...
 exit-address-family
exit
```

The IPv4 and IPv6 CLI should share grammar and implementation paths wherever the command semantics are address-family independent. The address family is data, not a reason to duplicate an otherwise identical command implementation.

A representative named-mode configuration hierarchy is:

```text
router eigrp <name>
 address-family <ipv4|ipv6> unicast [vrf <name>] autonomous-system <asn>
  network ...                         # IPv4 only where documented
  eigrp router-id <address>
  neighbor ...
  af-interface <default|interface>
   bandwidth-percent ...
   hello-interval ...
   hold-time ...
   passive-interface
   authentication ...
   next-hop-self
   split-horizon
   summary-address ...
   shutdown | no shutdown
  exit-af-interface
  topology base
   auto-summary
   default-information ...
   default-metric ...
   distance eigrp ...
   maximum-prefix ...
   metric ...
   offset-list ...
   redistribute ...
   summary-metric ...
   timers active-time ...
   traffic-share balanced
   variance ...
  exit-af-topology
  shutdown | no shutdown
 exit-address-family
exit
```

The exact command grammar, legal argument ranges, defaults, address-family applicability, command mode, and documented `no` forms should follow the Cisco named-mode command reference unless this project specification intentionally differs. A command valid for named IPv4 is not automatically valid for named IPv6 merely because the surrounding CLI hierarchy is shared.

The initial implementation may map named-mode CLI onto current FRR/YANG storage where suitable storage already exists. That is an implementation bridge, not a protocol or UX dependency.

## 5. CLI-to-Core Boundary

`eigrp_cli.[c|h]` and `eigrp_vty.[c|h]` are the explicit FRR-facing user interaction contract for EIGRP. They are adapter/front-end code, not portable EIGRP core modules.

`eigrp_cli` owns configuration command syntax, parser/mode behavior, construction/submission of FRR management transactions, and running-configuration interaction. `eigrp_vty` owns operational VTY interaction such as show, clear, debug, command installation, selection arguments, and user-facing presentation. FRR-native CLI/VTY/YANG types are legal within those front-end files.

For configuration that changes EIGRP protocol state, the committed management boundary is `eigrp_northbound.c`. The required path is:

```text
CLI parse
  -> retain/commit configuration through FRR management/YANG
  -> eigrp_northbound callback
  -> normalize host values into EIGRP-owned data
  -> call the real EIGRP feature target function
  -> receive an EIGRP-owned structured result
  -> FRR front end renders/logs that result
```

A CLI handler may normalize textual input to build the management transaction. It must not submit a northbound change and then independently mutate the same portable EIGRP runtime object. Runtime application of committed configuration belongs to the northbound callback and the real EIGRP target it invokes.

Operational VTY commands that do not represent retained configuration may call an appropriate EIGRP operational target after normalizing their arguments. They must still obey the same portable type and structured-result boundary.

FRR management and runtime objects stop at the adapter boundary. Portable core APIs must not accept FRR-native objects such as:

```c
struct vty *;
struct stream *;
struct interface *;
struct event *;
struct lyd_node *;
```

Zebra/YANG callback objects and other host-specific structures are subject to the same rule even when not listed above.

A temporary EIGRP wrapper around a host object is not the long-term data model merely because it has an `eigrp_` name. Common EIGRP objects must eventually contain the EIGRP data itself rather than depend on the layout of an FRR object. This is especially important for a future BSD build where FRR structures may not exist.

Address-family-aware objects must carry their own AF/type and address data. For example, a route/configuration object used by both IPv4 and IPv6 must identify the address family and contain the corresponding normalized address/prefix representation.

## 6. One Real Target Per Command/Feature

Every named-mode command that reaches common EIGRP logic must terminate at the real EIGRP target for that command/feature. The target name follows the human-navigation module convention in `code-conventions.md`, not the CLI mode hierarchy.

Do not create or use a generic CLI stub/dispatcher such as:

```c
eigrp_cli_not_configured(...);
eigrp_cli_stub_function(...);
eigrp_not_implemented_command(...);
```

A command whose runtime behavior is incomplete still gets its real target, for example:

```c
eigrp_metric_variance_set(...);
eigrp_metric_variance_reset(...);
eigrp_redistribute_add(...);
eigrp_redistribute_remove(...);
eigrp_neighbor_clear(...);
```

The target may currently return `EIGRP_RESULT_NOT_IMPLEMENTED`, but it must remain the stable place where that feature is later implemented. Shared private helpers below command-specific targets are allowed.

A CLI `no` form that restores a configured default should normally map to a `*_reset()` target. Operational `clear` commands remain `*_clear()` operations. Keyed collection members may use `add/remove` when that better expresses ownership.

When IPv4 and IPv6 operations have identical semantics, prefer one address-family-aware EIGRP target receiving normalized EIGRP data rather than duplicated AF-specific implementations.

## 7. Incomplete Feature Behavior

A valid named-mode command whose backend is incomplete must still be parsed, accepted, and retained in configuration unless the command is explicitly excluded by this specification.

For configuration commands, the required flow is:

```text
CLI parse
  -> retain normalized configuration
  -> call the real EIGRP target function
  -> target returns a structured EIGRP result
  -> northbound renders the result for the host CLI/logging environment
```

The core must return more information than a boolean pass/fail result. Common EIGRP APIs should use an EIGRP-owned result/status type capable of distinguishing at least:

```text
success
not implemented
invalid argument/configuration
not found
conflict
unsupported address family or capability
internal failure
```

The exact type and enum names are implementation details, but they must be EIGRP-owned and usable outside FRR.

For an unimplemented feature, the feature's target function returns the EIGRP `not implemented` result. The FRR northbound/VTY layer may render that as, for example:

```text
% EIGRP redistribute is not currently supported
```

The core must not call `vty_out()` or otherwise depend on FRR merely to report the result.

Configuration is not discarded because the runtime feature is not implemented. It must remain available to running-config/config writeback so the command does not silently disappear.

IPv6 named-mode configuration follows the same rule. The IPv6 CLI is required now even though the IPv6 data path is not yet implemented. IPv6 CLI input must populate the same EIGRP-owned configuration/route objects with the correct address-family and IPv6 address/prefix data so IPv6 runtime support can be added without replacing the CLI architecture.

## 8. Named-Mode Show, Clear, and Debug Direction

Named-mode operational commands are the active development surface. Existing classic operational commands are preserved but are not expanded.

Named show/clear commands should follow the Cisco named-mode shape where applicable, including forms such as:

```text
show eigrp address-family ipv4 [vrf <name>] [<asn>] neighbors ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] interfaces ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] topology ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] traffic ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] timers ...
show eigrp address-family ipv6 [vrf <name>] [<asn>] ...
clear eigrp address-family <ipv4|ipv6> [vrf <name>] [<asn>] neighbors ...
show eigrp tech-support
```

Do not install `show eigrp plugins`.

A partially implemented show command must display all real information currently available. Missing backend data must not cause the entire command to be replaced by an empty generic stub.

Debug command coverage should include protocol debugging such as:

```text
debug eigrp packet ...
debug eigrp transmit ...
debug eigrp event ...
debug eigrp timers
debug eigrp neighbor
```

Debug commands must use real target functions and structured EIGRP results under the same rules as configuration commands.

## 9. VTY Command Object Naming

VTY command object/function names follow the command surface first because they are adapter objects rather than portable EIGRP core APIs. Examples:

```text
show_eigrp_neighbor_cmd
show_eigrp_interface_cmd
show_eigrp_topology_cmd
clear_eigrp_neighbor_cmd
```

Do not force portable core naming rules onto FRR command objects such as `DEFPY` command variables.

The functions called after CLI normalization are EIGRP core APIs and follow the EIGRP naming and portability rules in `design-spec.md`.
