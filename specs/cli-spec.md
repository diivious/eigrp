# EIGRP CLI Specification

Copyright (C) 2026 Donnie V. Savage

Existing source files must preserve prior copyright notices, SPDX identifiers,
and author history.

## 1. Purpose

This document defines the EIGRP CLI/VTY contract for configuration, show,
clear, and debug operations. It owns command scope, mode placement, management
flow, configuration retention, `no` behavior, and the boundary between FRR CLI
objects and portable EIGRP feature targets.

Architecture is defined by `design-spec.md`; portable symbol naming is defined
by `code-conventions.md`.

## 2. Authority

CLI shape follows, in order:

1. Donnie V. Savage's explicit project decisions.
2. RFC 7868 where the command controls protocol behavior.
3. Cisco named-mode command references for user-facing syntax and placement
   where they do not conflict with this project.
4. FRR only for parser, VTY, YANG/northbound, and host integration mechanics.

A Cisco example command is not automatically an EIGRP project command. Host
VRF, route-map, key-chain, BFD, and similar platform configuration remains in
the owning host subsystem unless EIGRP defines a protocol-specific attachment
command.

## 3. Classic and named entry forms

Both router entry forms remain valid:

```text
router eigrp <asn>
router eigrp <name>
```

`router eigrp <asn>` selects the existing classic numeric-AS surface.
`router eigrp <name>` creates/selects a named parent whose protocol contexts are
its address families.

The parser preserves type-based disambiguation: a valid numeric ASN selects the
classic command; a non-numeric name selects named mode.

The existing classic surface is compatibility code:

- preserve existing classic configuration and operational commands;
- do not add a second classic implementation of a feature added in named mode;
- do not remove or regress classic behavior while implementing named mode;
- converge classic and named behavior below the host boundary where the
  protocol operation is the same.

Named mode is the canonical surface for new EIGRP configuration work.

Named mode covers the complete applicable classic EIGRP protocol feature set
unless a feature is explicitly excluded by this specification. Cisco named-mode
documentation determines named placement and grammar where documented; a
feature may move into address-family, `af-interface`, or `topology base` rather
than reproduce classic syntax literally.

## 4. Named-mode scope

Named mode covers EIGRP-owned protocol configuration and operations, including:

- IPv4 and IPv6 unicast address-family configuration;
- interface EIGRP behavior;
- topology and metric controls;
- filtering and routing-policy attachment;
- summarization;
- neighbor policy and timers;
- redistribution;
- protocol show/clear/debug output;
- EIGRP technical-support output.

The following are not project EIGRP CLI requirements:

- EIGRP Stub routing (`eigrp stub` / `stub`) — explicitly out of scope;
- `eigrp upgrade-cli` conversion tooling;
- SAF/service-family commands such as `ipv4-sf` and `service-family`;
- Cisco plugin-management commands such as `show eigrp plugins`;
- Cisco router-to-radio/VMI and platform test commands;
- host-owned commands whose behavior belongs to another subsystem.

Host-dependent integrations such as BFD or MTR are adapter capabilities, not a
reason to put host-native objects into portable EIGRP APIs. A host may expose
an EIGRP attachment command when that integration is implemented cleanly in the
host adapter.

`show eigrp tech-support` is an EIGRP operational command and remains part of
the named operational surface.

## 5. Named configuration hierarchy

Named mode is organized as:

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

The named parent is local configuration ownership. The address family plus VRF
and AS defines the protocol context. The local process name is not carried on
the wire.

The normal command ownership hierarchy is:

```text
address-family
  network ...                         IPv4 only
  eigrp router-id ...
  neighbor ...
  neighbor ... description ...
  neighbor ... maximum-prefix ...
  neighbor maximum-prefix ...
  eigrp log-neighbor-changes ...
  eigrp log-neighbor-warnings ...
  af-interface <default|interface>
    bandwidth ...
    bandwidth-percent ...
    delay ...
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
    auto-summary                     IPv4 semantics only
    default-information ...
    default-metric ...
    distance eigrp ...
    maximum-prefix ...
    maximum-paths ...
    metric holddown ...
    metric maximum-hops ...
    metric weights ...
    eigrp event-log-size ...
    distribute-list ...
    offset-list ...
    redistribute ...
    redistribute maximum-prefix ...
    summary-metric ...
    timers active-time ...
    traffic-share balanced
    variance ...
  exit-af-topology
  shutdown | no shutdown
exit-address-family
```

Command placement follows Cisco named-mode behavior where documented, but the
portable target namespace follows the owning EIGRP module rather than the CLI
nesting.

IPv4 and IPv6 share parser/target paths when the semantics are genuinely the
same. A command valid for IPv4 is not automatically valid for IPv6. IPv4
`network` and classful auto-summary semantics are examples of AF-specific
behavior.

### 5.1 Mode-changing command navigation

Mode-changing commands resolve from the current EIGRP hierarchy before the
command is applied. They may move back to an existing parent, but they do not
create missing parent context.

- `router eigrp ...` is a top-level configuration command. If it is entered
  from an EIGRP submode, return to top-level configuration first and then select
  the requested classic or named process.
- `address-family ...` requires an existing named `router eigrp <name>` parent.
  If it is entered from another address-family, `af-interface`, or topology
  submode under that named process, return to the named parent first and then
  select the requested address-family. It is not valid from top-level
  configuration or from a classic numeric-AS process.
- `af-interface ...` and `topology base` require an existing named
  address-family. If either is entered from another submode under that same
  address-family, return to the address-family first and then enter the
  requested submode.
- `exit-address-family`, `exit-af-interface`, and `exit-af-topology` return to
  the real parent context represented by the EIGRP XPath hierarchy.

## 6. `no` forms and retained configuration

Every supported configuration feature implements its applicable `no` form.

The `no` form must:

- remove explicit retained configuration or restore the defined default;
- invoke the same EIGRP feature family as the positive form;
- remain independently testable through mutation and running-config writeback;
- not depend on a generic CLI reset dispatcher.

Public target verbs follow `code-conventions.md`: configuration values normally
use `set/reset`, collection relationships use `add/remove`, and owned objects
use `create/delete`.

Valid configuration is retained even when a runtime capability is incomplete.
The runtime target may return `EIGRP_RESULT_NOT_IMPLEMENTED`; that result does
not erase a successfully committed configuration node.

This retention rule applies equally to IPv4 and IPv6 named configuration.

## 7. CLI-to-core boundary

FRR-facing ownership is:

```text
frr/eigrp_cli_classic.[c|h]  classic configuration parser/front end
frr/eigrp_cli_named.[c|h]    named configuration and named operational front end
frr/eigrp_vty.[c|h]          classic operational VTY surface
frr/eigrp_northbound.c       committed FRR/YANG config -> EIGRP adapter
```

Configuration flow is:

```text
CLI parse
  -> construct/submit FRR management change
  -> FRR commits retained configuration
  -> eigrp_northbound callback
  -> normalize to EIGRP-owned values
  -> call the real EIGRP feature target
  -> receive eigrp_result_t
  -> FRR renders/logs the result
```

The CLI must not submit a northbound change and then independently mutate the
same portable EIGRP state.

Operational commands that are not retained configuration may call an EIGRP
operational target directly after normalizing host arguments, but portable APIs
still receive EIGRP-owned values and return EIGRP-owned results.

FRR objects such as `struct vty`, `struct interface`, `struct event`, libyang
nodes, and Zebra objects do not cross into portable feature APIs.

## 8. One real target per feature

Every command reaching portable logic terminates at the real owning EIGRP
feature target. Do not create or use generic targets such as:

```text
eigrp_cli_not_configured(...)
eigrp_cli_stub_function(...)
eigrp_not_implemented_command(...)
```

An incomplete runtime feature still has its real target and returns a structured
result there. Shared private helpers below the public target are allowed.

Examples of intended target shape:

```c
eigrp_metric_variance_set(...);
eigrp_metric_variance_reset(...);
eigrp_redistribute_add(...);
eigrp_redistribute_remove(...);
eigrp_neighbor_clear(...);
```

Classic and named front ends must not call one another as a substitute for
sharing protocol behavior. Convergence belongs below the host boundary.

## 9. Incomplete capability behavior

Portable result semantics distinguish at least:

```text
success
not implemented
invalid argument/configuration
not found
conflict
unsupported address family/capability
internal failure
```

The portable target never calls `vty_out()` merely to explain failure. FRR
renders the structured result.

A configuration command may therefore succeed as retained configuration while
its runtime application reports `NOT_IMPLEMENTED`. This is the required model
for configuration that is architecturally valid but whose data-path operation
is capability-gated.

A partially implemented show command displays all real data available from its
backend. Missing fields do not justify replacing the entire command with an
empty generic stub.

## 10. Named operational commands

Named operational commands use the Cisco address-family-oriented shape where
applicable:

```text
show eigrp address-family ipv4 [vrf <name>] [<asn>] interfaces ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] neighbors ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] topology ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] traffic ...
show eigrp address-family ipv4 [vrf <name>] [<asn>] timers ...
show eigrp address-family ipv6 [vrf <name>] [<asn>] ...
clear eigrp address-family <ipv4|ipv6> [vrf <name>] [<asn>] neighbors ...
show eigrp protocols
show eigrp tech-support
```

Do not install `show eigrp plugins`.

### 10.1 Multicast address-family selector

Where the Cisco named show grammar includes the `multicast` selector, that token
means EIGRP's Multicast Address Family (MAF), using VRID `0x0001`. It does not mean
ordinary IPv4 multicast packet transport or group membership.

The project configuration/runtime model is unicast. A parsed MAF operational
request must remain explicitly unsupported/not implemented through the
EIGRP-owned state request/target boundary. Do not reinterpret the token as
normal packet multicast state.

### 10.2 Debug surface

Named debug coverage is address-family aware. Supported protocol debug forms
include the installed EIGRP debug families, for example:

```text
debug eigrp packet ...
debug eigrp transmit ...
debug eigrp event [detail]
debug eigrp timers
debug eigrp fsm
debug eigrp neighbor ...
debug eigrp notifications ...
debug eigrp address-family <ipv4|ipv6> [vrf <name>] [<asn>] ...
```

Address-family scoped forms own named route/neighbor/notification/summary
debugging where the classic surface does not provide named context.

Legacy IPX-SAP and EIGRP Stub selectors are not valid debug categories for this
project.

Debug commands use real operational targets and EIGRP-owned results. FRR debug
command objects may retain host naming conventions because they are adapter
objects, not portable APIs.

### 10.3 Operational output conventions

For EIGRP-owned operational presentation that is already implemented, use the
Cisco EIGRP troubleshooting output as the display-format reference:

```text
https://www.cisco.com/c/en/us/support/docs/ip/enhanced-interior-gateway-routing-protocol-eigrp/118974-technote-eigrp-00.html
```

This is a presentation rule, not a protocol-capability rule. It applies to
EIGRP-owned headings, capitalization, field labels, topology terminology,
neighbor-change reason text, and packet-debug wording where the implementation
has the corresponding real state.

Examples include:

```text
IP-EIGRP neighbors for process <AS>
H   Address ... Interface ... Hold Uptime SRTT RTO Q Seq

IP-EIGRP Topology Table for AS(<AS>)/ID(<router-id>)
Codes: P - Passive, A - Active, U - Update, Q - Query, R - Reply,
       r - reply Status, s - sia Status

P <prefix>, <n> successors, FD is <metric>
P <prefix>, 0 successors, FD is Inaccessible
P <prefix>, <n> successors, FD is <metric>, serno <serial>  # all-links

EIGRP: Sending <PACKET> on <interface> ...
EIGRP: Received <PACKET> on <interface> ...
  AS <asn>, Flags <flags>, Seq <sequence>/<ack>
```

Neighbor-change messages use the established EIGRP reason wording when the
corresponding transition exists, such as `holding time expired`,
`peer restarted`, `new adjacency`, `K-value mismatch`, `Interface Goodbye
received`, and `peer graceful-restart`.

Host/framework-owned decoration is outside this rule. FRR/BIRD timestamps,
logging prefixes, VTY wrappers, command parser conventions, and other host
presentation that EIGRP does not own do not need to imitate IOS.

Do not create protocol state or implement an incomplete capability solely to
fill an IOS field. If SRTT, ACTIVE-state detail, event timestamps, queue detail,
or another value is not maintained by the current backend, retain an honest
unavailable/omitted representation until the owning capability is implemented.
Similarly, detailed topology/vector-metric output is not synthesized from
partial data merely to match an IOS example.

## 11. VTY/DEFPY object naming

FRR command object/function names follow the command surface and FRR parser
conventions, for example:

```text
show_eigrp_neighbor_cmd
show_eigrp_interface_cmd
show_eigrp_topology_cmd
clear_eigrp_neighbor_cmd
```

Do not force portable core naming rules onto `DEFPY` command variables. The
portable targets reached after argument normalization follow
`code-conventions.md`.

## 12. Validation contract

For every named configuration command, tests must cover the applicable subset
of:

- positive parse/commit;
- running-config writeback;
- value mutation;
- documented `no` form;
- parent/address-family/interface/topology mode placement;
- IPv4/IPv6 applicability;
- multiple AS contexts;
- case-sensitive named parents;
- runtime result when a capability is intentionally incomplete.

Parser/configuration retention must be validated before a failure is attributed
to runtime worker or packet processing code.
