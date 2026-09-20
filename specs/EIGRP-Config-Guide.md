# EIGRP config and EXEC guide

Classic and named. IPv4 and IPv6.

> **Reference role:** This is a command/operations reference assembled from public Cisco material. It is not the project architecture, implementation-status matrix, or protocol authority. Project behavior is governed by `design-spec.md`, `cli-spec.md`, and RFC 7868. Commands described here may be outside this project's implementation scope. In particular, EIGRP Stub routing is explicitly out of scope and must not be implemented or tested from this document. Host-dependent features such as BFD remain host/integration concerns unless separately implemented by the project.

Facts come from public Cisco IOS/IOS-XE guides and the command reference. Protocol bits also from RFC 7868. Wording and labs here are mine. Not a Cisco doc. Check `?` on your image before you paste anything.

Left out: IPX, IGRP, and other retired protocol families. Commands that remain in the project target surface, including IPv4 `auto-summary`, are covered where relevant.

---

## 1. Intro

EIGRP is a distance-vector IGP with DUAL. Peers send prefix vectors. DUAL installs a successor. If a neighbor's advertised metric (RD) is strictly less than your best metric (FD), that neighbor is a feasible successor and you can fail over without a query flood.

Cisco documents two CLIs for the same protocol.

| Style | Start | Where knobs live |
|---|---|---|
| Classic (AS mode) | `router eigrp <as>` or `ipv6 router eigrp <as>` | Process + interface |
| Named (virtual instance) | `router eigrp <name>` then an address-family with an AS | AF, `af-interface`, `topology base` |

Instance name is local. AS number is what must match on the wire. Classic and named will neighbor if AS, K-values, auth, and family match.

Named mode has three layers (Cisco doc 200156):

1. Address-family: `network`, static neighbor, router-id, shutdown, logging
2. `af-interface`: hello/hold, auth, passive, split-horizon, summary, bandwidth-percent (Cisco also documents BFD on supported platforms)
3. `topology base`: variance, maximum-paths, redistribute, distribute-list, offset-list, distance, default-metric

`router eigrp NAME` by itself does nothing. You need an address-family + AS, and that AF has to be `no shutdown`.

| Item | Usual default |
|---|---|
| AD internal / external | 90 / 170 |
| Hello / hold on Ethernet | 5s / 15s |
| IPv4 group | 224.0.0.10 |
| IPv6 group | FF02::A |
| IP protocol | 88 |
| Equal-cost paths | 4 |
| K-values | 1 0 1 0 0 (named adds K6=0) |

K-values must match or you get no adjacency. Hello/hold do not have to match.

IPv4 and IPv6 are separate families. Fixing one does not fix the other.

---

## 2. Common

### 2.1 Config

#### Start it

```text
! Classic IPv4
router eigrp 100

! Classic IPv6
ipv6 router eigrp 100

! Named. Still idle until AF + AS exist.
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
 address-family ipv6 unicast autonomous-system 100
```

#### No shutdown

Classic IPv6 and named AFs can sit shut. A lot of IPv6 classic processes come up down.

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  no shutdown
 address-family ipv6 unicast autonomous-system 100
  no shutdown

ipv6 router eigrp 100
 no shutdown
```

#### Router-id

Always 32-bit. Cisco IPv6 EIGRP will not run without one. Set it on IPv4 too so it does not float off an interface.

```text
router eigrp 100
 eigrp router-id 192.0.2.1

ipv6 router eigrp 100
 eigrp router-id 192.0.2.1

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  eigrp router-id 192.0.2.1
 address-family ipv6 unicast autonomous-system 100
  eigrp router-id 192.0.2.11
```

#### Hello and hold

Classic puts the AS on the interface command. Named puts timers under `af-interface`. `af-interface default` is the template. A specific interface overrides it.

```text
interface GigabitEthernet0/0
 ip hello-interval eigrp 100 2
 ip hold-time eigrp 100 6
 ipv6 hello-interval eigrp 100 2
 ipv6 hold-time eigrp 100 6

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  af-interface default
   hello-interval 5
   hold-time 15
  exit-af-interface
  af-interface GigabitEthernet0/0
   hello-interval 2
   hold-time 6
  exit-af-interface
```

#### Passive

Announces the connected prefix. Sends no Hellos. Takes no neighbor. Named: under `af-interface`.

```text
router eigrp 100
 passive-interface default
 no passive-interface GigabitEthernet0/0

ipv6 router eigrp 100
 passive-interface default
 no passive-interface GigabitEthernet0/0

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  af-interface default
   passive-interface
  exit-af-interface
  af-interface GigabitEthernet0/0
   no passive-interface
  exit-af-interface
```

#### Auth

Classic: interface + key chain. MD5 is the common classic method. Named: under `af-interface`. Current IOS-XE can do HMAC-SHA-256. Both sides need the same method and key.

```text
key chain EIGRP-KEYS
 key 1
  key-string 0 rotate-me

interface GigabitEthernet0/0
 ip authentication mode eigrp 100 md5
 ip authentication key-chain eigrp 100 EIGRP-KEYS
 ipv6 authentication mode eigrp 100 md5
 ipv6 authentication key-chain eigrp 100 EIGRP-KEYS

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  af-interface GigabitEthernet0/0
   authentication mode hmac-sha-256
   authentication key-chain EIGRP-KEYS
  exit-af-interface
```

Run `authentication mode ?`. Some images want a password on that line instead of a key chain.

#### Split-horizon, next-hop-self, bandwidth-percent

Classic: on the interface. Named: `af-interface`. Only kill split-horizon on a multipoint hub that has to reflect spoke routes.

```text
interface Tunnel0
 ip split-horizon eigrp 100
 ip bandwidth-percent eigrp 100 50
 ip next-hop-self eigrp 100

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  af-interface Tunnel0
   no split-horizon
   bandwidth-percent 50
   no next-hop-self
  exit-af-interface
```

#### Manual summary

Classic: `ip summary-address eigrp` or `ipv6 summary-address eigrp` on the face toward the rest of the AS. Named: `summary-address` under `af-interface`. Router installs a local discard for the aggregate.

```text
interface GigabitEthernet0/0
 ip summary-address eigrp 100 10.10.0.0 255.255.0.0
 ipv6 summary-address eigrp 100 2001:DB8:10::/48

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  af-interface GigabitEthernet0/0
   summary-address 10.10.0.0 255.255.0.0
  exit-af-interface
```

#### Stub — Cisco reference only, project out of scope

> The EIGRP Stub routing feature is explicitly out of scope for this project. The following Cisco syntax is retained only as external command-reference context; it is not an implementation or test requirement.

Spoke tells neighbors "do not query me." Default advertise set is connected + summary. Keywords: `connected`, `summary`, `static`, `redistributed`, `receive-only`, `leak-map`. Put this on the spoke, not the hub.

```text
router eigrp 100
 eigrp stub connected summary

ipv6 router eigrp 100
 eigrp stub connected summary

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  eigrp stub connected summary
```

Some images hide stub under `topology base`. Ask `?`.

#### Variance, maximum-paths, traffic-share

Named: `topology base`. Variance will not install a path that failed feasibility.

```text
router eigrp 100
 maximum-paths 4
 variance 2
 traffic-share balanced

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  topology base
   maximum-paths 4
   variance 2
  exit-af-topology
```

#### Automatic summary (IPv4)

Cisco retains `auto-summary` as an IPv4 topology control; modern IOS/IOS-XE defaults it off. In named mode it is entered under `topology base`. IPv6 has no classful automatic-summary equivalent.

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  topology base
   auto-summary
  exit-af-topology
```

Use `no auto-summary` to restore the disabled state.

#### Metric weights, default-metric, offset-list

Named `metric weights` has K6. Change K-values everywhere in the same window. `default-metric` is bandwidth, delay, reliability, load, MTU. `offset-list` slaps a constant on matching prefixes.

```text
router eigrp 100
 metric weights 0 1 0 1 0 0
 default-metric 100000 10 255 1 1500
 offset-list OFFSET-ACL in 256000 GigabitEthernet0/0

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  metric weights 0 1 0 1 0 0 0
  metric rib-scale 128
  topology base
   default-metric 100000 10 255 1 1500
   offset-list OFFSET-ACL in 256000 GigabitEthernet0/0
  exit-af-topology
```

Named uses 64-bit wide metrics. Seed `1 1 1 1 1` can overflow the RIB. Use real bandwidth and delay.

#### Redistribute and filter

Give EIGRP a seed metric for foreign protocols. Named: `redistribute` and `distribute-list` under `topology base`. Classic IPv6 filter is `distribute-list prefix-list`. Cisco's IPv6 EIGRP guide says no route-map on that classic list.

```text
router eigrp 100
 redistribute ospf 1 metric 10000 100 255 1 1500 route-map OSPF-TO-EIGRP
 distribute-list prefix CORE-IN in GigabitEthernet0/0

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  topology base
   redistribute ospf 1 metric 10000 100 255 1 1500 route-map OSPF-TO-EIGRP
   distribute-list prefix CORE-IN in GigabitEthernet0/0
  exit-af-topology
```

#### Static neighbor, logging, and Cisco BFD reference

`neighbor` forces unicast Hellos (NBMA). Leave neighbor-change logs on. Cisco documents named BFD under `af-interface`, with interface-owned BFD timers, and documents EIGRP IPv6 BFD in named mode. BFD is host-dependent integration and is not a portable EIGRP-core requirement in this project.

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  neighbor 192.0.2.2 GigabitEthernet0/0
  eigrp log-neighbor-changes
  eigrp log-neighbor-warnings
  af-interface GigabitEthernet0/0
   bfd
  exit-af-interface
```

#### Bandwidth and delay

EIGRP reads configured `bandwidth` and `delay`, not negotiated Ethernet speed. Change those if the defaults do not match the design.

### 2.2 EXEC

Two show families. Address-family form works for classic and named. Old `show ip eigrp` / `show ipv6 eigrp` still work.

```text
show eigrp protocols
show ip protocols
show ipv6 protocols
show running-config | section router eigrp
```

`show eigrp protocols` dumps every instance: AS, K-values, router-id, variance, distance.

| Need | AF form | Old form |
|---|---|---|
| Interfaces | `show eigrp address-family ipv4 interfaces` | `show ip eigrp interfaces` |
| Neighbors | `show eigrp address-family ipv4 neighbors` | `show ip eigrp neighbors` |
| Topology | `show eigrp address-family ipv4 topology` | `show ip eigrp topology` |
| Counters | `show eigrp address-family ipv4 traffic` | `show ip eigrp traffic` |
| IPv6 | swap `ipv4` for `ipv6` | `show ipv6 eigrp ...` |

Add `detail` when the parser allows it.

Neighbor fields that matter: Hold should reset. Uptime should grow. Q Cnt should stay 0. RTO stuck at 5000 plus retries in `detail` means ACKs are not coming back.

Topology: P = done. A = queries out. Each `via` is `(FD/RD)`. `topology all-links` shows paths that failed feasibility.

```text
clear ip eigrp neighbors
clear ip eigrp neighbors GigabitEthernet0/0
clear ip eigrp 100 neighbors 192.0.2.2
clear ipv6 eigrp neighbors
clear eigrp address-family ipv4 neighbors
clear eigrp address-family ipv6 neighbors
clear ip eigrp topology
clear ipv6 eigrp topology
```

Clear drops the adjacency and prefixes from that peer. Pin it to an interface or address in production.

```text
debug eigrp packets hello
debug eigrp packets update
debug eigrp packets terse
debug eigrp neighbors
debug ip eigrp
debug ipv6 eigrp
undebug all
```

Cisco debug help lists hello, update, query, reply, ack, SIA-query, SIA-reply. Ignore `ipxsap`. Packet debug is heavy. Keep it short.

### 2.3 Examples

#### C1. Dual-stack named core, passive default

```text
ipv6 unicast-routing
!
interface Loopback0
 ip address 192.0.2.1 255.255.255.255
 ipv6 address 2001:DB8::1/128
!
interface GigabitEthernet0/0
 description PEER
 ip address 192.0.2.17 255.255.255.252
 ipv6 address 2001:DB8:0:17::1/64
!
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  af-interface default
   passive-interface
  exit-af-interface
  af-interface GigabitEthernet0/0
   no passive-interface
  exit-af-interface
  network 192.0.2.1 0.0.0.0
  network 192.0.2.16 0.0.0.3
  eigrp router-id 192.0.2.1
  eigrp log-neighbor-changes
  no shutdown
 exit-address-family
 address-family ipv6 unicast autonomous-system 100
  af-interface default
   passive-interface
  exit-af-interface
  af-interface GigabitEthernet0/0
   no passive-interface
  exit-af-interface
  eigrp router-id 192.0.2.1
  no shutdown
 exit-address-family
```

#### C2. Neighbor missing

1. Interface up/up. Ping the peer (IPv4, or IPv6 link-local).
2. `show eigrp protocols`: AS, K-values, router-id, AF not shut.
3. `show eigrp address-family ipv4 interfaces detail` and the IPv6 twin: passive, auth, peers=0?
4. ACL/CoPP allowing proto 88.
5. Diff both configs.
6. Then `debug eigrp packets hello` for a few seconds.

K-value and auth mismatches usually log with no debug. Silent missing Hellos is usually passive, ACL, VRF, or wrong interface.

#### C3. OSPF into EIGRP with a tag

```text
ip prefix-list OSPF-ONLY seq 10 permit 10.20.0.0/16 le 24
!
route-map OSPF-TO-EIGRP permit 10
 match ip address prefix-list OSPF-ONLY
 set tag 120
route-map OSPF-TO-EIGRP deny 20
!
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  topology base
   default-metric 100000 10 255 1 1500
   redistribute ospf 1 route-map OSPF-TO-EIGRP
  exit-af-topology
```

Tag inbound so the other edge can deny that tag on the way back.

---

## 3. IPv4

### 3.1 Config

#### Classic: `network` matches interface IPs

`network A.B.C.D [wildcard]` does not "advertise that prefix as typed." It turns on interfaces whose primary IPv4 address falls in range. Type the wildcard. `0.0.0.0` is one host.

```text
router eigrp 100
 eigrp router-id 192.0.2.1
 network 192.0.2.1 0.0.0.0
 network 10.0.0.0 0.255.255.255
 eigrp log-neighbor-changes
 maximum-paths 4
```

#### Named

Same `network` idea, under the IPv4 AF. Topology knobs under `topology base`. Do not skip `no shutdown`.

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  network 192.0.2.1 0.0.0.0
  network 10.0.0.0 0.255.255.255
  eigrp router-id 192.0.2.1
  no shutdown
  topology base
   maximum-paths 4
  exit-af-topology
 exit-address-family
```

#### VRF

Named puts VRF on the AF line after VRF + RD exist. Each VRF is its own table and AS.

```text
vrf definition BRANCH
 rd 65000:10
 address-family ipv4
 exit-address-family
!
router eigrp CORE
 address-family ipv4 unicast vrf BRANCH autonomous-system 110
  network 10.10.0.0 0.0.255.255
  eigrp router-id 192.0.2.10
  no shutdown
 exit-address-family
```

#### Classic interface knobs

```text
interface GigabitEthernet0/0
 ip bandwidth-percent eigrp 100 75
 ip hello-interval eigrp 100 5
 ip hold-time eigrp 100 15
 ip authentication mode eigrp 100 md5
 ip authentication key-chain eigrp 100 EIGRP-KEYS
 ip summary-address eigrp 100 10.10.0.0 255.255.0.0
 ip next-hop-self eigrp 100
```

#### Default toward remotes

Two usual methods: `redistribute static` of `0.0.0.0/0` through a route-map, or `summary-address 0.0.0.0 0.0.0.0` on the hub interface. Summary installs a local discard. Hub still needs its own default.

#### Distance and prefix cap

```text
router eigrp 100
 distance eigrp 90 170

router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  maximum-prefix 5000 80
```

### 3.2 EXEC

```text
show ip protocols
show eigrp protocols
show ip eigrp interfaces
show ip eigrp interfaces detail
show eigrp address-family ipv4 interfaces detail
show ip eigrp neighbors
show ip eigrp neighbors detail
show eigrp address-family ipv4 neighbors detail
show ip eigrp topology
show ip eigrp topology 10.10.1.0/24
show ip eigrp topology all-links
show ip eigrp traffic
show ip route eigrp
show ip route 10.10.1.0 255.255.255.0
```

| Command | Tells you |
|---|---|
| `show ip protocols` | AS, K-values, router-id, network list, filters, redistribution |
| `show ip eigrp interfaces [detail]` | Enabled ifs, peer count, timers, auth |
| `show ip eigrp neighbors [detail]` | Adjacency, Q-count, stub, retries |
| `show ip eigrp topology [prefix]` | FD/RD, successor vs FS |
| `show ip eigrp traffic` | Hello/update/query volume |
| `show ip route eigrp` | RIB (`D` internal, `D EX` external) |

```text
clear ip eigrp neighbors GigabitEthernet0/0
clear ip eigrp 100 neighbors 192.0.2.18
debug eigrp packets hello
debug ip eigrp
undebug all
```

### 3.3 Examples

#### V4-1. Two-router classic

```text
! R1
interface Loopback0
 ip address 10.1.1.1 255.255.255.255
interface GigabitEthernet0/0
 ip address 10.0.0.1 255.255.255.252
router eigrp 100
 eigrp router-id 10.1.1.1
 network 10.1.1.1 0.0.0.0
 network 10.0.0.0 0.0.0.3

! R2
interface Loopback0
 ip address 10.2.2.2 255.255.255.255
interface GigabitEthernet0/0
 ip address 10.0.0.2 255.255.255.252
router eigrp 100
 eigrp router-id 10.2.2.2
 network 10.2.2.2 0.0.0.0
 network 10.0.0.0 0.0.0.3
```

R1 should show `D 10.2.2.2/32` via 10.0.0.2.

#### V4-2. Named edge summary

```text
router eigrp CORE
 address-family ipv4 unicast autonomous-system 100
  af-interface GigabitEthernet0/0
   summary-address 10.10.0.0 255.255.0.0
  exit-af-interface
  af-interface Loopback0
   passive-interface
  exit-af-interface
  network 10.10.0.0 0.0.255.255
  network 10.0.0.0 0.0.0.3
  eigrp router-id 10.10.0.1
  no shutdown
 exit-address-family
```

Core sees one `/16`. Edge shows Null0 for that aggregate.

#### V4-3. Stub spoke, hub default summary — Cisco reference only

> EIGRP Stub is outside project scope. This example is retained only to explain the external Cisco command reference.

```text
! Spoke
router eigrp 100
 eigrp router-id 10.9.9.9
 network 10.9.0.0 0.0.255.255
 eigrp stub connected summary

! Hub facing spokes
interface GigabitEthernet0/1
 ip summary-address eigrp 100 0.0.0.0 0.0.0.0
```

#### V4-4. Classic talking to named

`router eigrp 100` and `address-family ipv4 unicast autonomous-system 100` will neighbor. Name `CORE` never goes on the wire. Auth and K-values still have to match.

---

## 4. IPv6

### 4.1 Config

Cisco "IPv6 Routing: EIGRP Support" for classic:

- Turn it on per interface with `ipv6 eigrp <as>`. No `network` statement.
- Need a 32-bit router-id.
- Process starts shut. `no shutdown` required.
- Global unicast not required. Link-local is enough to speak.
- Classic filter is `distribute-list prefix-list`.

Also need `ipv6 unicast-routing` or the box will not forward IPv6.

#### Classic minimum

```text
ipv6 unicast-routing
!
interface Loopback0
 ipv6 address 2001:DB8::1/128
 ipv6 eigrp 100
!
interface GigabitEthernet0/0
 ipv6 address 2001:DB8:0:17::1/64
 ipv6 eigrp 100
!
ipv6 router eigrp 100
 eigrp router-id 192.0.2.1
 no shutdown
 eigrp log-neighbor-changes
```

#### Named

Create the IPv6 AF and `no shutdown` it. That turns on every capable interface. Access SVIs will speak unless you set `af-interface default` passive and then exempt transit links. `shutdown` under one `af-interface` keeps that if out.

```text
ipv6 unicast-routing
!
router eigrp CORE
 address-family ipv6 unicast autonomous-system 100
  eigrp router-id 192.0.2.1
  no shutdown
  af-interface default
   passive-interface
  exit-af-interface
  af-interface GigabitEthernet0/0
   no passive-interface
  exit-af-interface
  af-interface GigabitEthernet0/2
   shutdown
  exit-af-interface
 exit-address-family
```

#### Classic interface knobs

```text
interface GigabitEthernet0/0
 ipv6 hello-interval eigrp 100 5
 ipv6 hold-time eigrp 100 15
 ipv6 summary-address eigrp 100 2001:DB8:10::/48
 ipv6 bandwidth-percent eigrp 100 75
 ipv6 authentication mode eigrp 100 md5
 ipv6 authentication key-chain eigrp 100 EIGRP-KEYS
```

#### Named interface and topology

```text
router eigrp CORE
 address-family ipv6 unicast autonomous-system 100
  af-interface GigabitEthernet0/0
   hello-interval 5
   hold-time 15
   summary-address 2001:DB8:10::/48
   authentication mode hmac-sha-256
   authentication key-chain EIGRP-KEYS
   bfd
  exit-af-interface
  topology base
   variance 2
   maximum-paths 4
   default-metric 100000 10 255 1 1500
  exit-af-topology
 exit-address-family
```

#### Classic prefix filter

```text
ipv6 prefix-list V6-CORE-IN seq 10 permit 2001:DB8::/32 le 64
!
ipv6 router eigrp 100
 distribute-list prefix-list V6-CORE-IN in GigabitEthernet0/0
```

#### IPv6 VRF-lite

Named only, per Cisco.

```text
vrf definition BRANCH
 rd 65000:10
 address-family ipv6
 exit-address-family
!
router eigrp CORE
 address-family ipv6 unicast vrf BRANCH autonomous-system 110
  eigrp router-id 192.0.2.10
  no shutdown
 exit-address-family
```

#### BFD for IPv6 EIGRP — Cisco/host reference

> This section describes Cisco host integration. BFD is not a portable EIGRP-core requirement for this project.

Cisco documents this in named mode: the AF must be up, timers live on the interface, and `bfd` is entered under `af-interface`.

```text
interface GigabitEthernet0/0
 ipv6 address 2001:DB8:0:17::1/64
 bfd interval 50 min_rx 50 multiplier 3
!
router eigrp CORE
 address-family ipv6 unicast autonomous-system 100
  eigrp router-id 192.0.2.1
  af-interface GigabitEthernet0/0
   bfd
  exit-af-interface
```

### 4.2 EXEC

```text
show ipv6 protocols
show eigrp protocols
show ipv6 eigrp interfaces
show ipv6 eigrp interfaces detail
show eigrp address-family ipv6 interfaces detail
show ipv6 eigrp neighbors
show ipv6 eigrp neighbors detail
show eigrp address-family ipv6 neighbors detail
show ipv6 eigrp topology
show ipv6 eigrp topology 2001:DB8:10::/48
show ipv6 eigrp traffic
show ipv6 route eigrp
show ipv6 route 2001:DB8:10::/48
```

Neighbor and topology `via` lines use link-locals. That is normal. Classic `show ipv6 protocols` lists interfaces. Named often does not, so use the AF interface command.

```text
clear ipv6 eigrp neighbors
clear ipv6 eigrp neighbors GigabitEthernet0/0
clear eigrp address-family ipv6 neighbors
clear ipv6 eigrp topology
debug ipv6 eigrp
debug eigrp packets hello
undebug all
```

Ping the peer link-local and name the egress if: `ping FE80::2 GigabitEthernet0/0`.

### 4.3 Examples

#### V6-1. Classic pair

```text
! R1
ipv6 unicast-routing
interface Loopback0
 ipv6 address 2001:DB8:1::1/128
 ipv6 eigrp 100
interface GigabitEthernet0/0
 ipv6 address 2001:DB8:17::1/64
 ipv6 eigrp 100
ipv6 router eigrp 100
 eigrp router-id 10.1.1.1
 no shutdown

! R2
ipv6 unicast-routing
interface Loopback0
 ipv6 address 2001:DB8:2::2/128
 ipv6 eigrp 100
interface GigabitEthernet0/0
 ipv6 address 2001:DB8:17::2/64
 ipv6 eigrp 100
ipv6 router eigrp 100
 eigrp router-id 10.2.2.2
 no shutdown
```

`show ipv6 eigrp neighbors` should list FE80:: on Gi0/0. R1 `show ipv6 route eigrp` should have `2001:DB8:2::2/128` as `D`. Empty table: add router-id, `no shutdown`, and make sure a link-local exists.

#### V6-2. Named edge summary

```text
ipv6 unicast-routing
!
router eigrp CORE
 address-family ipv6 unicast autonomous-system 100
  eigrp router-id 10.10.0.1
  no shutdown
  af-interface default
   passive-interface
  exit-af-interface
  af-interface GigabitEthernet0/0
   no passive-interface
   summary-address 2001:DB8:10::/48
  exit-af-interface
 exit-address-family
```

#### V6-3. Named IPv6 on every SVI

After conversion, access VLANs form neighbors. IPv6 AF turned on every capable if. Fix: `af-interface default` / `passive-interface`, then `no passive-interface` only on core links.

#### V6-4. Classic IPv4 + named IPv6 on one box

Fine during a move. `show eigrp protocols` lists both. Remove leftover `ipv6 router eigrp` once IPv6 lives only in named mode.

```text
router eigrp 100
 eigrp router-id 192.0.2.1
 network 10.0.0.0 0.255.255.255
!
router eigrp CORE
 address-family ipv6 unicast autonomous-system 100
  eigrp router-id 192.0.2.1
  no shutdown
  af-interface default
   passive-interface
  exit-af-interface
  af-interface GigabitEthernet0/0
   no passive-interface
  exit-af-interface
 exit-address-family
```

---

## 5. Where to type it

| Knob | Classic IPv4 | Classic IPv6 | Named |
|---|---|---|---|
| Start | `router eigrp <as>` | `ipv6 router eigrp <as>` | `router eigrp <name>` + AF |
| Enable if | `network` | `ipv6 eigrp <as>` | `network` (v4), AF up (v6) |
| Router-id | process | process | AF |
| Passive | process | process | `af-interface` |
| Timers / auth / summary | interface | interface | `af-interface` |
| Variance / redistrib / offset | process | process | `topology base` |
| Auto-summary | process (Cisco IPv4 reference) | n/a | `topology base` (IPv4) |
| Stub | Cisco reference only; project out of scope | Cisco reference only; project out of scope | Cisco reference only; project out of scope |
| Shutdown | delete process | `shutdown` on process | `shutdown` on AF |

Neighbor-down reasons from Cisco trouble notes: hold expired, interface down, manually cleared, K-value mismatch, auth fail, SIA, not on a common subnet.

---

## 6. Sources

Restated from:

- Configure EIGRP Named Mode (Cisco 200156)
- IP Routing: EIGRP Configuration Guide (IOS 15 / IOS-XE)
- IPv6 Routing: EIGRP Support
- EIGRP IPv6 Configuration Example (Cisco 113267). Labs here use different addresses.
- BFD Support for EIGRP IPv6
- EIGRP Stub Routing
- Cisco IOS IP Routing: EIGRP Command Reference
- RFC 7868 (DUAL, metrics, proto 88, multicast groups)

Use the command reference for your train if anything fights the parser.
