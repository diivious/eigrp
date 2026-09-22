# EIGRP Configuration and EXEC Guide

Copyright (C) 2026 Donnie V. Savage

## 1. Purpose

This is the user guide for the EIGRP command surface provided by this project.
It documents configuration, show, clear, and debug commands. Core architecture
belongs in `design-spec.md`; platform integration belongs in
`integration-spec.md`.

Two configuration styles are available:

| Style | Entry command | Main configuration location |
|---|---|---|
| Classic IPv4 | `router eigrp <as> [vrf <name>]` | router mode plus interface commands |
| Named | `router eigrp <name>` | address-family, `af-interface`, and `topology base` |

The named parent name is local. The autonomous-system number is the EIGRP
protocol identity used by the address family.

## 2. Classic IPv4 configuration

### 2.1 Create the process

```text
router eigrp 100
```

A VRF may be selected on the router command:

```text
router eigrp 100 vrf BLUE
```

Remove the process with the matching `no router eigrp ...` form.

### 2.2 Router ID

```text
router eigrp 100
 eigrp router-id 192.0.2.1
```

Reset the configured router ID with:

```text
no eigrp router-id
```

### 2.3 Network participation

Classic network configuration uses an IPv4 prefix:

```text
router eigrp 100
 network 10.0.0.0/8
 network 192.0.2.0/24
```

The matching `no network ...` form removes participation.

### 2.4 Static neighbor

```text
router eigrp 100
 neighbor 192.0.2.2
```

Use `no neighbor 192.0.2.2` to remove it.

### 2.5 Passive interfaces

```text
router eigrp 100
 passive-interface eth1
 no passive-interface eth0
```

### 2.6 DUAL and metric controls

```text
router eigrp 100
 timers active-time 180
 variance 2
 maximum-paths 4
 metric weights 0 1 0 1 0 0
```

The corresponding reset forms are:

```text
no timers active-time
no variance
no maximum-paths
no metric weights
```

`timers active-time disabled` is also accepted.

### 2.7 Filtering

Access-list form:

```text
router eigrp 100
 distribute-list FILTER-IN in
 distribute-list FILTER-OUT out eth0
```

Prefix-list form:

```text
router eigrp 100
 distribute-list prefix PL-IN in
 distribute-list prefix PL-OUT out eth0
```

Use the matching `no distribute-list ...` form to remove the filter.

### 2.8 Redistribution

```text
router eigrp 100
 redistribute connected
 redistribute static metric 100000 10 255 1 1500
 redistribute ospf metric 100000 10 255 1 1500 route-map OSPF-EIGRP
```

The installed parser accepts these source protocol names:

```text
kernel connected local static rip ospf isis bgp nhrp vnc babel openfabric
```

Use `no redistribute <protocol>` to remove a source.

## 3. Classic IPv4 interface configuration

Classic interface commands are entered in the host interface configuration
mode.

### 3.1 Bandwidth and delay

```text
interface eth0
 eigrp bandwidth 1000000
 delay 10
```

Reset with:

```text
no eigrp bandwidth
no delay
```

### 3.2 Hello and hold timers

```text
interface eth0
 ip hello-interval eigrp 5
 ip hold-time eigrp 15
```

Reset with the matching `no` form.

### 3.3 Authentication

```text
interface eth0
 ip authentication mode eigrp 100 md5
 ip authentication key-chain eigrp 100 EIGRP-KEYS
```

The authentication mode parser accepts `md5` and `hmac-sha-256`.

### 3.4 Manual summary

```text
interface eth0
 ip summary-address eigrp 100 10.10.0.0/16
```

Remove it with the matching `no ip summary-address eigrp ...` form.

## 4. Named configuration model

Named configuration begins with a local parent and one or more address
families:

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  ...
 exit-address-family
 address-family ipv6 unicast autonomous-system 100
  ...
 exit-address-family
```

The optional `unicast` keyword may be omitted. A VRF can be selected before the
AS number:

```text
address-family ipv4 unicast vrf BLUE autonomous-system 100
```

A named parent may contain both IPv4 and IPv6 address-family configuration.
The address family must be enabled with `no shutdown` when it is intended to
run.

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  no shutdown
```

`shutdown` administratively disables the selected address family.

## 5. Named address-family commands

### 5.1 IPv4 network participation

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  network 10.0.0.0 0.255.255.255
  network 192.0.2.1 0.0.0.0
```

Use the matching `no network ...` form to remove a network statement.

### 5.2 Router ID

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  eigrp router-id 192.0.2.1
```

The same 32-bit router-ID form is used under an IPv6 named address family.

### 5.3 Static neighbors

IPv4:

```text
neighbor 192.0.2.2 eth0
```

IPv6 named configuration accepts an IPv6 neighbor form:

```text
neighbor 2001:db8::2 eth0
```

Remove a configured neighbor with the corresponding `no neighbor ...` form.

### 5.4 Neighbor description and maximum-prefix

```text
neighbor 192.0.2.2 description branch-router
neighbor 192.0.2.2 maximum-prefix 5000 80 warning-only
neighbor maximum-prefix 20000 80 warning-only
```

The address-family-wide maximum-prefix form also accepts `dampened`,
`reset-time`, `restart`, and `restart-count` options.

### 5.5 Neighbor logging

```text
eigrp log-neighbor-changes
eigrp log-neighbor-warnings
eigrp log-neighbor-warnings 10
```

Use the matching `no` form to restore the default.

### 5.6 Metric weights

The shared metric command is valid in named address-family context:

```text
metric weights 0 1 0 1 0 0 0
```

TOS must be 0. K6 is optional. Use `no metric weights` to restore defaults.

## 6. Named `af-interface`

Enter interface-specific EIGRP configuration with:

```text
af-interface default
```

or:

```text
af-interface eth0
```

Leave the submode with:

```text
exit-af-interface
```

Remove a retained interface block with the corresponding `no af-interface ...`
form.

### 6.1 Bandwidth and delay

```text
bandwidth-percent 50
bandwidth 1000000
delay 10
```

### 6.2 Hello and hold timers

```text
hello-interval 5
hold-time 15
```

### 6.3 Authentication

MD5 with a key chain:

```text
authentication mode md5
authentication key-chain EIGRP-KEYS
```

HMAC-SHA-256 configuration form:

```text
authentication mode hmac-sha-256 0 secret
```

The encryption-type field accepts 0 or 7.

### 6.4 Passive, next-hop-self, and split horizon

```text
passive-interface
next-hop-self
split-horizon
```

Each has a matching `no` form.

A common named deployment is passive by default with explicit transit links:

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  af-interface default
   passive-interface
  exit-af-interface
  af-interface eth0
   no passive-interface
  exit-af-interface
```

### 6.5 Manual summary

IPv4:

```text
summary-address 10.10.0.0 255.255.0.0
summary-address 10.10.0.0 255.255.0.0 90
summary-address 10.10.0.0 255.255.0.0 90 leak-map SUMMARY-LEAK
```

IPv6 named configuration:

```text
summary-address 2001:db8:10::/48
summary-address 2001:db8:10::/48 90
```

Use the matching `no summary-address ...` form to remove the summary.

## 7. Named `topology base`

Enter topology configuration with:

```text
topology base
```

Leave with:

```text
exit-af-topology
```

### 7.1 Variance and maximum paths

```text
variance 2
maximum-paths 4
```

These shared command parsers dispatch to the named topology target when entered
under named topology mode.

### 7.2 Active timer

```text
timers active-time 180
timers active-time disabled
```

### 7.3 IPv4 automatic summary

```text
auto-summary
```

Use `no auto-summary` to restore the default disabled state.

### 7.4 Default information and default metric

```text
default-information in
default-information out
default-information in POLICY
default-metric 100000 10 255 1 1500
```

Use the matching `no` form to reset each value.

### 7.5 Administrative distance

```text
distance eigrp 90 170
```

### 7.6 Maximum-prefix controls

Topology prefix limit:

```text
maximum-prefix 10000 80 warning-only
```

Redistribution prefix limit:

```text
redistribute maximum-prefix 5000 80 warning-only
```

The full grammar also supports `dampened`, `reset-time`, `restart`, and
`restart-count` options.

### 7.7 Metric controls

```text
metric maximum-hops 100
metric holddown
traffic-share balanced
```

Use the matching `no` form to restore the default.

### 7.8 Event log size

```text
eigrp event-log-size 1000
```

### 7.9 Distribute lists

```text
distribute-list FILTER-IN in
distribute-list FILTER-OUT out eth0
distribute-list prefix PL-IN in
distribute-list prefix PL-OUT out eth0
```

The shared parser resolves the retained path from the current named topology
context. Use the matching `no distribute-list ...` form to remove it.

### 7.10 Offset list

```text
offset-list OFFSET-ACL in 256000
offset-list OFFSET-ACL out 256000 eth0
```

### 7.11 Redistribution

```text
redistribute connected
redistribute static metric 100000 10 255 1 1500
redistribute ospf metric 100000 10 255 1 1500 route-map OSPF-EIGRP
```

The shared parser dispatches to the named redistribution target in named
topology mode.

### 7.12 Summary metric

IPv4:

```text
summary-metric 10.10.0.0 255.255.0.0 100000 10 255 1 1500
summary-metric 10.10.0.0 255.255.0.0 distance 90
```

IPv6 named configuration:

```text
summary-metric 2001:db8:10::/48 100000 10 255 1 1500
summary-metric 2001:db8:10::/48 distance 90
```

## 8. Named configuration example

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  eigrp router-id 192.0.2.1
  network 10.0.0.0 0.255.255.255
  no shutdown
  af-interface default
   passive-interface
  exit-af-interface
  af-interface eth0
   no passive-interface
   hello-interval 5
   hold-time 15
  exit-af-interface
  topology base
   maximum-paths 4
   variance 2
  exit-af-topology
 exit-address-family
```

## 9. Classic IPv4 EXEC commands

### 9.1 Interfaces

```text
show ip eigrp interfaces
show ip eigrp interfaces detail
show ip eigrp interfaces eth0 detail
show ip eigrp vrf BLUE interfaces
```

### 9.2 Neighbors

```text
show ip eigrp neighbors
show ip eigrp neighbors detail
show ip eigrp neighbors eth0 detail
show ip eigrp vrf BLUE neighbors
```

The detailed view includes adjacency and transport state such as hold time,
queue count, SRTT, RTO, sequence, and retry information where maintained.

### 9.3 Topology

```text
show ip eigrp topology
show ip eigrp topology all-links
show ip eigrp topology 100
show ip eigrp topology 10.10.1.0/24
show ip eigrp vrf BLUE topology
```

`all-links` includes paths beyond the normal successor/feasible-successor view.

### 9.4 Events

```text
show ip eigrp events
show ip eigrp vrf BLUE events
```

### 9.5 Clear neighbors

```text
clear ip eigrp neighbors
clear ip eigrp neighbors eth0
clear ip eigrp neighbors 192.0.2.2
clear ip eigrp neighbors soft
clear ip eigrp neighbors eth0 soft
clear ip eigrp neighbors 192.0.2.2 soft
```

A VRF may be added after `eigrp`.

### 9.6 Clear events

```text
clear ip eigrp events
clear ip eigrp vrf BLUE events
```

## 10. Named EXEC commands

Named operational commands use an explicit address-family selector.

### 10.1 Interfaces

```text
show eigrp address-family ipv4 interfaces
show eigrp address-family ipv4 100 interfaces detail
show eigrp address-family ipv4 vrf BLUE interfaces eth0 detail
```

### 10.2 Neighbors

```text
show eigrp address-family ipv4 neighbors
show eigrp address-family ipv4 100 neighbors detail
show eigrp address-family ipv4 vrf BLUE neighbors eth0 detail
```

### 10.3 Topology

```text
show eigrp address-family ipv4 topology
show eigrp address-family ipv4 topology all-links
show eigrp address-family ipv4 topology 100 10.10.1.0/24
```

### 10.4 Accounting, events, timers, and traffic

```text
show eigrp address-family ipv4 accounting
show eigrp address-family ipv4 events
show eigrp address-family ipv4 timers
show eigrp address-family ipv4 traffic
```

The optional VRF and AS selectors narrow the requested EIGRP context.

### 10.5 Protocol summary and technical support

```text
show eigrp protocols
show eigrp tech-support
```

### 10.6 Clear topology

```text
clear eigrp ipv4 topology
clear eigrp 100 ipv4 topology
clear eigrp ipv4 topology 10.10.1.0/24
clear eigrp 100 vrf BLUE ipv4 topology 10.10.1.0/24
```

The IPv4 network/mask form is also accepted:

```text
clear eigrp ipv4 topology 10.10.1.0 255.255.255.0
```

### 10.7 Clear neighbors

```text
clear eigrp address-family ipv4 neighbors
clear eigrp address-family ipv4 100 neighbors
clear eigrp address-family ipv4 neighbors eth0
clear eigrp address-family ipv4 neighbors 192.0.2.2
clear eigrp address-family ipv4 neighbors soft
```

### 10.8 Clear events

```text
clear eigrp address-family ipv4 events
clear eigrp events
```

## 11. Debug commands

The EIGRP debug surface includes packet, transmit, event, timer, FSM, neighbor,
notification, and address-family scoped debugging.

Examples:

```text
debug eigrp packet hello
debug eigrp packet update send detail
debug eigrp packet query receive
debug eigrp transmit ack packetize
debug eigrp event detail
debug eigrp timers
debug eigrp fsm
debug eigrp neighbor siatimer
debug eigrp notifications rib
debug eigrp address-family ipv4 100
debug eigrp address-family ipv4 100 neighbor 192.0.2.2
```

Disable a category with the matching `no debug eigrp ...` command.

Packet categories accepted by the parser are:

```text
siaquery siareply ack hello probe query reply request retry terse update all
```

Packet debug may also be restricted to `send` or `receive` and may request
`detail`.

## 12. Operational interpretation

For neighbor output, useful transport fields include:

- **Hold**: remaining neighbor hold time;
- **SRTT**: smoothed round-trip time for reliable packets;
- **RTO**: retransmission timeout;
- **Q**: queued reliable work/packets as presented by the implementation;
- **Seq**: reliable sequence state.

For topology output:

- **P** means Passive;
- **A** means Active;
- successor and feasible-successor state come from DUAL/topology;
- `all-links` exposes additional known paths.

When troubleshooting adjacency formation, verify the selected AS/VRF/AF,
router ID, participating interface, passive state, authentication, interface
reachability, and protocol 88 packet flow before debugging DUAL or the RIB.
