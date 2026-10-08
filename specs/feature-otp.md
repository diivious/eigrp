# OpenEIGRP EIGRP Over the Top (OTP)
## Product Requirements and Engineering Design

Copyright (C) 2026 Donnie V. Savage

---

# 1. Product Overview

## 1.1 EIGRP across an existing WAN

EIGRP Over the Top, commonly called **OTP**, extends an EIGRP routing domain across an arbitrary routed IP network.

Consider an enterprise with several locations connected through a service-provider network, private WAN, routed data-center fabric, or other IP transport:

```text
                           IP WAN
                 +-----------------------+
                 |                       |
          +------+-------+-------+-------+------+
          |              |               |
        CE-A           CE-B            CE-C
          |              |               |
       Site A          Site B          Site C
      10.1/16         10.2/16         10.3/16
```

The WAN already knows how to deliver packets between the WAN-facing addresses of CE-A, CE-B, and CE-C. The enterprise wants the three sites to participate in one EIGRP routing domain without requiring the WAN provider to carry the enterprise's EIGRP routes or participate in the enterprise routing design.

OTP uses the existing network as an **underlay** and builds an EIGRP overlay above it.

The underlay has one essential job: provide IP reachability between OTP endpoints.

EIGRP then distributes the enterprise routes independently of the underlay:

```text
                  Enterprise EIGRP domain

                    CE-A ------- CE-B
                      \           /
                       \         /
                         CE-C


                     Existing IP WAN

                    CE-A ------- CE-B
                      \           /
                       \         /
                         CE-C
```

This gives the enterprise one end-to-end EIGRP domain while leaving the WAN transport independent of the EIGRP topology. Cisco describes OTP in the same terms: EIGRP operates across private or public WAN transport without requiring the provider to transfer the enterprise routes.

NetworkLessons provides a useful operational description of the same model: the remote customer routers only need routed reachability to one another, while EIGRP OTP provides the overlay routing relationship.

## 1.2 Control plane and data plane

OTP separates the routing control plane from the user-data plane.

The **control plane is EIGRP**. Remote peers exchange EIGRP Hellos, Updates, Queries, Replies, acknowledgments, and SIA packets using routed unicast IP.

The **data plane is LISP encapsulation**. When EIGRP selects a route whose destination is across the OTP overlay, the host forwarding plane encapsulates the user packet toward the selected remote endpoint.

Cisco defines OTP using exactly this relationship: EIGRP distributes the routes between the customer-edge routers and LISP carries traffic across the WAN.

Conceptually:

```text
                      CONTROL PLANE

                         EIGRP

                  CE-A -----------> CE-B
                       routed IP


                       DATA PLANE

                         LISP

              Site A ==============> Site B
```

The EIGRP control packet itself does not need an overlay tunnel. It is simply routed to the configured remote EIGRP peer through the underlay.

Data traffic follows the route learned by EIGRP and uses LISP to reach the remote forwarding endpoint.

## 1.3 Remote EIGRP peers

A normal EIGRP neighbor is discovered or configured on an EIGRP-enabled connected interface.

An OTP **remote peer** is reached through the routed underlay.

Cisco exposes this relationship through the named-mode neighbor syntax:

```text
neighbor { <ipv4-address> | <ipv6-address> } <interface>
    remote <maximum-hops>
    lisp-encap [ <lisp-id> ]
```

For example:

```text
neighbor 10.0.0.1 gigabitethernet 0/0/1 remote 2 lisp-encap 1
```

The address identifies the remote EIGRP peer. The interface supplies the local interface context. `remote` enables routed remote-neighbor behavior, and `maximum-hops` controls the allowed routed distance. `lisp-encap` enables the OTP dataplane and optionally supplies its LISP ID.

Once the remote adjacency is established, the normal EIGRP machinery continues above it:

```text
                    routed unicast transport
                              |
                              v
                       neighbor state
                              |
                              v
                             RTP
                              |
                              v
                  Update / Query / Reply
                              |
                              v
                             DUAL
                              |
                              v
                       topology / RIB
```

Remote-neighbor support therefore extends the transport reach of an EIGRP adjacency without creating a second routing protocol.

## 1.4 Multipoint forwarding

The defining dataplane property of OTP is that it is **multipoint**.

If CE-A learns routes through three remote endpoints:

```text
10.10.0.0/16 -> RLOC A
10.20.0.0/16 -> RLOC B
10.30.0.0/16 -> RLOC C
```

the desired forwarding model is:

```text
                       LISP forwarding context
                               |
                 +-------------+-------------+
                 |             |             |
                 v             v             v
              RLOC A        RLOC B        RLOC C
                 |             |             |
                 v             v             v
              10.10/16      10.20/16      10.30/16
```

The routing table selects the remote endpoint.

Adding another remote site therefore adds routing and forwarding state. It does not create another point-to-point tunnel that must be separately configured and maintained.

This property is critical to OTP scalability and must be preserved by the OpenEIGRP host implementation.

## 1.5 EIGRP Route Reflectors

A small OTP deployment can use directly configured remote peers. As the number of sites grows, maintaining a full control-plane mesh becomes undesirable.

An **EIGRP Route Reflector**, or **E-RR**, provides a scalable control-plane topology:

```text
                            E-RR
                       /      |      \
                      /       |       \
                   CE-A     CE-B      CE-C
```

Each CE forms an EIGRP relationship with the E-RR. The E-RR receives routes from its clients and reflects them to the other clients.

The crucial behavior is that the E-RR preserves the originating next hop.

Assume CE-B advertises:

```text
10.20.0.0/16
next hop = CE-B
```

The control path may be:

```text
CE-B -----------> E-RR -----------> CE-A
```

but CE-A should still learn:

```text
10.20.0.0/16
next hop = CE-B
```

The resulting data path is direct:

```text
                        E-RR
                       /    \
                      /      \
                   CE-A      CE-B
                     \        ^
                      \      /
                       \====/
                         LISP
```

Cisco requires the E-RR interface to disable `next-hop-self` so the original next hop is preserved; Cisco specifically notes that traffic otherwise flows through the E-RR. Split horizon is also disabled because routes learned from one remote neighbor must be reflected through the same source interface to other remote neighbors.

The E-RR listener syntax is:

```text
remote-neighbors source <interface>
    unicast-listen
    lisp-encap [ <lisp-id> ]
```

Cisco documents the corresponding E-RR configuration sequence as:

```text
af-interface <interface>
    no next-hop-self
    no split-horizon

remote-neighbors source <interface>
    unicast-listen
    lisp-encap [ <lisp-id> ]
```


This topology exposes one of the most important OTP design rules:

```text
Remote EIGRP peer
    = where EIGRP control packets are exchanged

EIGRP next hop
    = where traffic for a selected route is forwarded

OTP RLOC
    = LISP endpoint derived from the selected EIGRP next hop
```

The remote neighbor and the forwarding RLOC can have the same address in a simple two-router deployment, but they represent different concepts.

The implementation must preserve that distinction.

## 1.6 LISP dataplane

For OpenEIGRP OTP, `lisp-encap` means **actual LISP encapsulation on the wire**.

The standard LISP data packet consists of an outer RLOC IP header, UDP, an eight-byte LISP data header, and the original IP packet. RFC 9300 assigns UDP destination port 4341 to LISP data packets and defines the RLOC-based outer header and LISP Instance ID encoding.

Conceptually:

```text
+------------------------------------------------+
| Outer IP                                       |
| source      = local RLOC                       |
| destination = selected remote RLOC             |
+------------------------------------------------+
| UDP                                            |
+------------------------------------------------+
| LISP data header                               |
| Instance ID                                    |
+------------------------------------------------+
| Original IP packet                             |
+------------------------------------------------+
```

OpenEIGRP implements the LISP dataplane profile required for Cisco OTP interoperability.

Cisco OTP packet captures are part of the implementation evidence and test corpus. They establish the exact OTP use of optional LISP header fields, UDP source-port behavior, checksum behavior, TTL handling, DSCP/ECN handling, and other details where the general LISP specification permits choices.

The result is a deliberately small LISP implementation focused on OTP forwarding.

## 1.7 Operator experience

OTP should appear to the operator as an EIGRP routing feature, not as a tunnel-management feature.

For a CE, the operator describes the remote EIGRP relationship:

```text
router eigrp savage
 address-family ipv4 autonomous-system 4453
  neighbor 192.0.2.1 <interface> remote 10 lisp-encap 4453
```

For an E-RR, the operator describes where remote peers are accepted and enables the EIGRP forwarding behavior required for reflection:

```text
router eigrp savage
 address-family ipv4 autonomous-system 4453

  af-interface <interface>
   no next-hop-self
   no split-horizon

  remote-neighbors source <interface>
      unicast-listen
      lisp-encap 4453
```

From this configuration OpenEIGRP creates the necessary runtime behavior:

```text
configuration
      |
      v
remote EIGRP adjacency
      |
      v
route exchange
      |
      v
DUAL selects path
      |
      v
next hop identifies RLOC
      |
      v
host LISP dataplane programmed
      |
      v
direct site-to-site forwarding
```

There is no operator-created mesh of point-to-point tunnels.

---

# 2. Engineering Design

## 2.1 Design objective

OTP begins as EIGRP control-plane state and terminates as host forwarding state.

The architecture must preserve that separation:

```text
+-----------------------------------------------------+
|                Portable OpenEIGRP                   |
|                                                     |
| remote neighbors                                    |
| RTP                                                 |
| topology                                            |
| DUAL                                                |
| path selection                                      |
| EIGRP next hop / OTP RLOC                           |
+-------------------------+---------------------------+
                          |
                          | normalized intent
                          v
+-----------------------------------------------------+
|                   Host integration                  |
|                                                     |
| FRR or BIRD adapter                                 |
| route translation                                   |
| forwarding-context lifecycle                        |
| host capability                                     |
+-------------------------+---------------------------+
                          |
                          v
+-----------------------------------------------------+
|                Native host dataplane                |
|                                                     |
| FIB selection                                       |
| LISP encapsulation                                  |
| LISP decapsulation                                  |
| multipath                                           |
| underlay recursion                                  |
| MTU handling                                        |
| physical forwarding                                |
+-----------------------------------------------------+
```

OpenEIGRP decides where traffic belongs.

The host forwarding implementation moves the packets.

The EIGRP process must therefore participate when control-plane state changes rather than once for every forwarded data packet.

## 2.2 OTP state follows the path

An OTP forwarding endpoint belongs to the selected EIGRP path.

Assume an Update produces:

```text
destination     10.20.0.0/16
next hop        192.0.2.20
LISP instance   4453
```

The topology relationship is:

```text
                     10.20.0.0/16
                           |
                    route descriptor
                           |
                           v
                  next hop 192.0.2.20
                           |
                           v
                    OTP RLOC
                           |
                           v
                   LISP IID 4453
```

When DUAL selects that path as successor, the forwarding request carries the same relationship into the host.

Conceptually:

```text
install
    destination = 10.20.0.0/16
    forwarding  = LISP
    RLOC        = 192.0.2.20
    instance    = 4453
```

A successor change may therefore change the RLOC without changing the destination.

This state belongs with the path rather than with the EIGRP neighbor that happened to deliver the Update. That is what allows E-RR reflection to work correctly.

## 2.3 Logical forwarding context

OpenEIGRP needs a logical object representing an OTP LISP forwarding context.

It is conceptually similar to Cisco's multipoint LISP dataplane:

```text
                         LISP instance
                              |
            +-----------------+-----------------+
            |                 |                 |
            v                 v                 v
         RLOC A            RLOC B            RLOC C
```

The context is shared across compatible OTP paths.

Its identity is expected to include enough information to distinguish the forwarding domain, such as address family, routing/VRF context, and LISP Instance ID.

The portable object describes the EIGRP forwarding requirement. It does not expose Linux LWT objects, eBPF handles, FRR nexthops, BIRD routes, TUN file descriptors, or another host-native representation.

The host adapter owns that translation.

## 2.4 Configuration and creation flow

Enabling LISP encapsulation on a remote relationship drives the complete OTP lifecycle:

```text
neighbor ... remote ... lisp-encap ...
                    |
                    v
          normalized configuration
                    |
                    v
          remote-peer state created
                    |
                    v
       LISP forwarding context acquired
                    |
                    v
        host dataplane made available
                    |
                    v
         unicast EIGRP peer started
```

Forwarding contexts are shared when their forwarding identity is compatible.

If several remote peers use the same OTP forwarding context, configuring the additional peers adds EIGRP relationships rather than additional point-to-point dataplanes.

The runtime context remains available while selected routes or configured peers require it.

## 2.5 Remote EIGRP packet flow

Remote EIGRP control traffic uses ordinary underlay forwarding:

```text
                         eigrpd
                            |
                       unicast Hello
                            |
                            v
                     host IP output
                            |
                            v
                       underlay FIB
                            |
                            v
                           NIC
                            |
                            +---------> remote peer
```

The reverse path is:

```text
remote peer
     |
     v
    NIC
     |
     v
host protocol-88 input
     |
     v
EIGRP receive path
     |
     v
neighbor / RTP / topology
```

The existing EIGRP RTP and DUAL machinery continues above that transport.

Remote-neighbor support therefore needs to remove connected-neighbor assumptions from the transport path while keeping the protocol machinery common.

## 2.6 Route-reflector flow

The E-RR case is the reference topology for validating the design.

Assume:

```text
                         E-RR
                       /      \
                    CE-A      CE-B
```

CE-B sends:

```text
10.20.0.0/16
next hop = CE-B
```

The E-RR reflects it without rewriting the next hop:

```text
 CE-B                     E-RR                     CE-A
   |                        |                        |
   | 10.20/16, NH=CE-B      |                        |
   +----------------------->|                        |
                            | 10.20/16, NH=CE-B      |
                            +----------------------->|
```

CE-A's resulting control and data relationships are:

```text
control peer = E-RR

destination  = 10.20.0.0/16
next hop     = CE-B
OTP RLOC     = CE-B
```

Data therefore bypasses the E-RR:

```text
                    E-RR
                   /    \
                  /      \
               CE-A      CE-B
                 \        ^
                  \      /
                   \====/
                    LISP
```

Any implementation that derives the LISP endpoint from the EIGRP neighbor object rather than the route next hop fails this design.

## 2.7 Transmit dataplane

After OpenEIGRP installs the route, ordinary packets no longer involve EIGRP processing.

A packet for `10.20.1.100` follows:

```text
packet
dst 10.20.1.100
        |
        v
host FIB lookup
        |
        v
10.20.0.0/16 selected
        |
        v
selected OTP path
        |
        +-- RLOC 192.0.2.20
        +-- IID 4453
        |
        v
native LISP encapsulation
        |
        v
outer packet
dst 192.0.2.20
        |
        v
underlay FIB lookup
        |
        v
physical output
```

This produces two independent routing decisions.

The inner lookup chooses the EIGRP OTP path and therefore the RLOC.

The outer lookup chooses how the underlay reaches that RLOC.

If the underlay path changes while the RLOC remains reachable, the OTP route does not need to reconverge.

## 2.8 Receive dataplane

Inbound traffic follows the complementary operation:

```text
physical input
      |
      v
packet for local RLOC
      |
      v
LISP dataplane
      |
      +-- validate header
      +-- identify forwarding context / IID
      +-- decapsulate
      |
      v
original inner IP packet
      |
      v
normal host forwarding
```

The recovered packet becomes a normal packet in the appropriate forwarding context.

The host may deliver it locally or forward it toward another interface according to its FIB.

## 2.9 Multipath

OTP must retain normal EIGRP multipath behavior.

Two selected paths can have different RLOCs:

```text
                      10.20.0.0/16
                           |
                  +--------+--------+
                  |                 |
                  v                 v
               RLOC A            RLOC B
```

Each installed nexthop retains the RLOC belonging to that EIGRP path.

The native host multipath mechanism selects the forwarding nexthop. LISP encapsulation then uses the RLOC belonging to that selected nexthop.

OTP therefore does not introduce an independent flow-hashing or load-sharing algorithm above the host FIB.

## 2.10 Production fast path

Production transit traffic must remain in the host dataplane:

```text
                    CONTROL

                     eigrpd
                       |
                       | programs
                       v
                 forwarding state


                      DATA

                    packet
                       |
                       v
                   host FIB
                       |
                       v
                 LISP dataplane
                       |
                       v
                      NIC
```

This is a product requirement rather than a later optimization.

A production router must not require `eigrpd` to receive, encapsulate, decapsulate, or retransmit each transit data packet.

This preserves the performance model expected of a routing platform and leaves the host free to use kernel forwarding, NIC acceleration, or hardware offload where available.

## 2.11 Linux dataplane direction

On Linux, **LWT/BPF** is the preferred mechanism to investigate first.

LWT/BPF is not an OTP wire protocol. It is a Linux forwarding mechanism that may allow the kernel to implement the required LISP transformation while packets remain in the native forwarding path.

The intended relationship is:

```text
OpenEIGRP
     |
     | install route + RLOC + IID
     v
Linux forwarding state
     |
     v
LWT/BPF
     |
     | constructs actual LISP packet
     v
underlay
```

If Linux provides a cleaner native mechanism that satisfies the same contract, the host shim may use it. The observable OTP behavior remains LISP.

## 2.12 LISP interoperability profile

The general LISP data format is defined by RFC 9300. OTP uses that dataplane in a more constrained environment because EIGRP already supplies the forwarding relationship that identifies the remote endpoint.

The OpenEIGRP implementation should therefore establish a specific **OTP LISP interoperability profile**.

That profile is derived from two authorities:

Cisco OTP behavior determines interoperability requirements.

The applicable LISP data-plane specification determines field encoding and protocol behavior where Cisco follows standard LISP.

Before the dataplane implementation is considered stable, representative Cisco OTP traffic must be captured and converted into permanent test fixtures.

Those captures should establish the exact values and behaviors OpenEIGRP must reproduce for outer addressing, LISP flags, Instance ID, UDP source selection, UDP checksum handling, TTL or hop-limit treatment, DSCP/ECN behavior, DF/fragmentation interaction, and IPv4/IPv6 combinations.

This avoids designing a general LISP implementation when the engineering requirement is a precise OTP dataplane.

## 2.13 MTU

Encapsulation increases packet size.

For standard IPv4 LISP data encapsulation, the base overhead consists of an IPv4 outer header, UDP, and the eight-byte LISP header. An IPv6 outer header increases that overhead.

The production host implementation must present correct forwarding behavior at the effective path MTU.

The responsibility belongs to the dataplane because the dataplane knows the actual outer address family and forwarding mechanism.

The tests must specifically exercise DF behavior, fragmentation where appropriate, ICMP Packet Too Big handling, and packets immediately above and below the effective tunnel MTU.

---

# 3. Host Requirements, Limitations, and Exclusions

## 3.1 Required host capability

An OTP-capable production host provides a programmable multipoint forwarding service.

From OpenEIGRP's perspective, the fundamental request is:

```text
Install destination P
using LISP forwarding context L
with remote RLOC R
and LISP instance I.
```

The observable result is:

```text
packet matching P
      |
      v
host selects path
      |
      v
LISP encapsulation toward R
      |
      v
ordinary underlay forwarding
```

For receive traffic, the host recognizes the corresponding LISP dataplane traffic, identifies the forwarding context, removes the encapsulation, and returns the original packet to normal IP forwarding.

The host must support many remote RLOCs through the same logical forwarding context.

It must also preserve route-specific RLOC information through ECMP, replacement, withdrawal, and reconvergence.

## 3.2 Host shim boundary

OpenEIGRP specifies the forwarding semantics and wire protocol.

The host shim supplies the mechanism.

That boundary is:

```text
                   OpenEIGRP

            prefix / RLOC / IID / LISP
                       |
                       v
              OpenEIGRP host API
                       |
        --------------------------------
                    host shim
        --------------------------------
                       |
                       v
            native forwarding facility
```

FRR and BIRD may expose completely different APIs for route installation, event handling, interface discovery, and lifecycle management. Those differences remain below this boundary.

The host may likewise use different internal packet-forwarding technology.

What remains invariant is that `lisp-encap` produces and consumes the defined LISP OTP dataplane.

## 3.3 Linux / FRR production target

FRR/Linux is the first production target.

The Linux implementation must keep transit traffic in the kernel/native forwarding path. LWT/BPF should be evaluated first because it offers a programmable route-associated packet path without requiring the EIGRP daemon to become a userspace packet forwarder.

The prototype must prove that the selected EIGRP nexthop/RLOC and LISP Instance ID can reach the encapsulation path cleanly and that normal underlay routing occurs after encapsulation.

FRR remains the routing-stack integration layer. Zebra may participate in programming the Linux FIB, but the design should first use existing FRR and Linux extension mechanisms.

If stock FRR cannot represent forwarding information required by the proven dataplane design, any FRR-wide modification follows the project's managed-patch model under `frr/patch/`.

## 3.4 Linux / BIRD

A future BIRD/Linux implementation uses the same OpenEIGRP OTP semantics and the same LISP interoperability profile.

BIRD's route and lifecycle APIs may require a different shim from FRR, while the Linux forwarding mechanism underneath may be shared or independently programmed as appropriate.

The routing-stack integration is allowed to differ.

The configured feature and packets on the wire are not.

## 3.5 BSD / BIRD production target

BSD support targets BIRD and uses BSD-native forwarding infrastructure.

A production BSD implementation must provide the same essential properties as the Linux production dataplane: multipoint route-to-RLOC forwarding, native LISP encapsulation and decapsulation, ECMP-compatible path selection, normal underlay recursion, and transit forwarding outside `eigrpd`.

The implementation becomes supported when the BSD host shim can provide those semantics using an appropriate native fast path.

A BSD environment that does not yet provide the necessary dataplane reports the OTP runtime capability as unsupported while preserving configuration according to the project's normal capability-boundary rules.

## 3.6 macOS development target

macOS is a development, education, interoperability, and lab environment.

It must use the same EIGRP control-plane behavior and the same LISP wire protocol so that a macOS OpenEIGRP instance can participate in meaningful protocol testing against Linux, BSD, and Cisco.

Because macOS is not a production routing target, its dataplane may use a userspace packet mechanism if that is the practical host implementation.

The reduced forwarding performance of such an implementation is acceptable for development use.

## 3.7 LISP scope

OTP requires the portion of LISP needed to forward OTP data traffic.

EIGRP supplies the route and the remote forwarding endpoint. The LISP component consumes that information to construct and decapsulate the corresponding data packets.

The initial implementation therefore covers the OTP data path: RLOC-based outer addressing, UDP LISP data transport, the LISP data header, Instance ID handling, encapsulation, and decapsulation.

General LISP mapping services such as Map-Server, Map-Resolver, Map-Request, Map-Reply, or a general LISP mapping database are outside this feature because EIGRP supplies the forwarding mapping used by OTP.

## 3.8 Initial feature scope

The first production target is named-mode IPv4 OTP on FRR/Linux.

It includes configured remote peers, routed unicast EIGRP transport, `maximum-hops`, `lisp-encap`, LISP Instance ID handling, multipoint route-to-RLOC forwarding, Cisco-compatible LISP encap/decap, E-RR remote-neighbor listening, preserved next-hop behavior, split-horizon behavior required for reflection, ECMP, route withdrawal, and normal reconvergence.

After the IPv4 implementation is complete and interoperable, IPv6 follows the project's normal named-mode development order.

Security Group Tag propagation is a separate extension and is not required for the initial OTP implementation.

---

# 4. Test Strategy and Test Cases

## 4.1 Testing philosophy

OTP has several independent behaviors that must work together.

A ping between two sites proves very little by itself. The packet could have taken the wrong next hop, traversed the route reflector, used a userspace forwarding path, or succeeded despite incorrect route state.

The test strategy therefore follows the architecture from configuration to adjacency, topology, route installation, LISP wire packet, and finally forwarding performance.

Cisco interoperability is a release criterion rather than an optional compatibility exercise.

## 4.2 Remote-neighbor control plane

The first test topology contains two OpenEIGRP routers separated by at least one routed hop.

The test configures a remote neighbor with `maximum-hops` and verifies that unicast Hellos cross the underlay, the adjacency forms, RTP reaches steady state, Updates are exchanged, acknowledgments operate normally, and the adjacency is removed correctly when reachability disappears.

The test is then repeated at the configured hop boundary and beyond it to validate `maximum-hops`.

Authentication tests exercise the same remote relationship with each supported EIGRP authentication mode.

The success condition is that remote transport behaves as a transport variation beneath the normal neighbor/RTP/topology machinery.

## 4.3 Path and RLOC preservation

A portable test injects an OTP-learned route:

```text
prefix       10.20.0.0/16
next hop     192.0.2.20
LISP IID     4453
```

and drives the path through topology processing and DUAL selection.

The resulting normalized RIB request must still identify:

```text
destination  10.20.0.0/16
encap        LISP
RLOC         192.0.2.20
IID          4453
```

A successor change to another path must produce the corresponding new RLOC.

This test belongs in portable coverage because losing the next hop before the host boundary is a protocol architecture failure, not a host-integration bug.

## 4.4 Peer-versus-RLOC test

The mandatory architectural topology is:

```text
                         E-RR
                        /    \
                     CE-A    CE-B
```

CE-A receives a CE-B route from the E-RR.

At CE-A the test must observe:

```text
control peer = E-RR
route RLOC   = CE-B
```

The route must never inherit the E-RR address simply because the E-RR delivered the Update.

This test directly protects the design property that permits direct site-to-site forwarding.

## 4.5 Cisco LISP fixtures

A small Cisco OTP lab is used to capture real dataplane packets.

The baseline case should use IPv4 enterprise traffic over an IPv4 underlay with a known LISP ID and known EIGRP next hop.

Additional captures expand the profile to multiple flows, several packet sizes, multiple IIDs, and later IPv6.

Each fixture records the configuration that generated it so the expected packet fields are reproducible.

The OpenEIGRP encoder is tested against this profile, and the decoder is tested by feeding it Cisco-generated packets and comparing the recovered inner packet with the original.

These fixtures become permanent UUT material.

## 4.6 Direct Cisco interoperability

The first full integration topology is:

```text
             OpenEIGRP CE ================ Cisco CE
                              OTP/LISP
```

The routers form a remote EIGRP adjacency and advertise local prefixes in both directions.

OpenEIGRP must install the Cisco-learned route with the correct RLOC. Cisco must install the OpenEIGRP-learned route.

Bidirectional traffic is then generated while capturing both sides of the WAN.

The capture must show valid LISP forwarding from OpenEIGRP to Cisco and valid Cisco LISP packets accepted by OpenEIGRP.

The route is withdrawn and restored to verify dataplane lifecycle and reconvergence.

## 4.7 Cisco E-RR interoperability

The primary OTP system test is:

```text
         OpenEIGRP CE ------ Cisco E-RR ------ Cisco CE
```

Both CEs peer with the E-RR.

The Cisco CE advertises a local route. The E-RR reflects it while preserving the originating CE as next hop.

The OpenEIGRP CE must install that originating CE as the RLOC.

Control traffic should show:

```text
OpenEIGRP CE <----> Cisco E-RR <----> Cisco CE
```

while user traffic should show:

```text
OpenEIGRP CE <======================> Cisco CE
                         LISP
```

The E-RR must not become the user-data transit point for the reflected path.

The reciprocal topology, using OpenEIGRP as the E-RR, becomes a completion test when E-RR listener support is implemented.

## 4.8 Multipoint behavior

A single forwarding context is tested with several remote RLOCs:

```text
                         CE-A
                  /       |       \
                 /        |        \
              CE-B      CE-C      CE-D
```

Each remote site advertises multiple prefixes.

Traffic from CE-A toward each site's routes must use the same logical OTP forwarding context while producing the correct route-selected outer RLOC.

Adding CE-D must add remote routing/forwarding state without creating a CE-A-to-CE-D point-to-point tunnel object.

The test is then scaled independently by remote peers, unique RLOCs, and learned prefixes.

## 4.9 ECMP

Two equal-cost OTP successors are installed:

```text
                     10.20.0.0/16
                          |
                 +--------+--------+
                 |                 |
              RLOC A            RLOC B
```

Multiple flows are generated.

The host's normal ECMP mechanism selects a path. Packet capture verifies that the LISP outer destination matches the RLOC associated with the selected path.

The test demonstrates that OTP retains host-native multipath behavior rather than performing separate load balancing in `eigrpd`.

## 4.10 Underlay independence

An OTP route is established while its RLOC is reachable through underlay path A.

The underlay route is then changed to path B without modifying the EIGRP destination or successor.

The OTP route remains selected while its outer LISP packets follow the new underlay path.

This test demonstrates the intended separation between EIGRP overlay convergence and WAN convergence.

## 4.11 MTU

Traffic is generated below, at, and above the effective OTP path MTU.

Tests exercise normal packets and IPv4 packets with DF set. IPv6 Packet Too Big behavior is added with IPv6 OTP support.

The host must produce correct forwarding and signaling behavior without truncation or silent corruption.

The test should also verify that the effective overhead matches the LISP profile actually used by the implementation.

## 4.12 Dataplane validation and malformed input

The receive path is exercised with valid and invalid packets.

Valid traffic covers configured IIDs, expected address families, and supported LISP header combinations.

Negative fixtures cover truncated headers, inconsistent lengths, unknown forwarding contexts, invalid inner packets, and unsupported header combinations.

The receive path must reject malformed traffic deterministically without affecting unrelated forwarding state.

## 4.13 Lifecycle and recovery

The forwarding context and installed paths are observed while peers and routes change.

Coverage includes initial context creation, additional peers sharing the context, successor replacement, route withdrawal, peer failure, underlay failure, route restoration, daemon restart, and RIB reconnect.

After each event, installed dataplane state must correspond to current EIGRP topology state.

No stale RLOC should continue forwarding after its route has been removed.

## 4.14 Fast-path verification

A stable OTP topology is established and then held constant while transit traffic is increased.

The test records throughput, packet rate, latency, host forwarding CPU, and `eigrpd` CPU.

Increasing user traffic should exercise the host dataplane rather than cause corresponding packet processing in the EIGRP daemon.

Where the platform provides hardware or NIC acceleration, the implementation should also record whether the selected mechanism remains eligible for those facilities.

## 4.15 Completion criteria

The first production implementation is complete when OpenEIGRP/FRR/Linux can form a Cisco OTP remote adjacency, exchange routes in both directions, preserve the correct route next hop as the LISP RLOC, forward Cisco-compatible LISP traffic bidirectionally, operate correctly through a Cisco E-RR with direct CE-to-CE traffic, use a single multipoint forwarding context for multiple RLOCs, preserve ECMP and underlay independence, and forward transit traffic through the Linux dataplane rather than through `eigrpd`.

---

# 5. Implementation Plan

Section 5 is intended as the engineering handoff. Each task should produce a buildable tree and a testable capability. A developer should be able to take one task without having to redesign the feature described in Sections 1 through 4.

## TASK1 — Remote-peer portable model

The first task establishes the portable representation of an EIGRP remote peer.

The existing neighbor, interface, RTP, topology, and northbound paths should be traced before introducing new APIs. Remote-peer state should extend the existing EIGRP object model rather than create a parallel neighbor subsystem.

The model must retain the remote address, source-interface context, configured maximum hops, and `lisp-encap` configuration including the LISP ID.

Configuration must be retained and writeable even on a host that does not yet provide the OTP dataplane.

**Completion:** named-mode IPv4 configuration can create, retain, display/write, reset, and delete a remote peer with `lisp-encap`, and portable tests can inspect the resulting normalized state.

## TASK2 — Routed EIGRP unicast transport

The second task makes the remote peer operational at the EIGRP control-plane level.

The packet-service path must be extended so EIGRP can send protocol-88 packets through the routed underlay rather than assuming the neighbor is directly connected. The implementation must provide the configured source-interface behavior and the remote-hop semantics required by `maximum-hops`.

Incoming unicast packets must join the existing neighbor/RTP receive path so the normal EIGRP adjacency state machine and reliable transport remain common.

The first host target is FRR/Linux.

**Completion:** two OpenEIGRP routers separated by routed hops establish a remote adjacency and exchange reliable EIGRP Updates without LISP data forwarding.

## TASK3 — OTP path and RLOC propagation

The third task carries OTP forwarding identity through topology processing.

A route learned through an OTP environment must preserve the advertised EIGRP next hop so that the selected path can provide the remote RLOC to the RIB layer.

This task must specifically test the E-RR case where the packet source/control peer differs from the advertised next hop.

The work should fit the current `prefix_descriptor` / `route_descriptor` topology model and avoid unrelated topology naming changes.

**Completion:** a portable test can receive a route from peer A whose next hop is B, select that route through DUAL, and observe a normalized OTP RIB request identifying B as the RLOC.

## TASK4 — Cisco OTP LISP profile

Before selecting a Linux dataplane implementation, establish the exact packet format it must produce.

Create a Cisco OTP lab and capture representative user traffic. Record the Cisco configuration with each fixture.

Use the captures together with the LISP data-plane specification to define the OpenEIGRP OTP profile for outer addressing, UDP behavior, LISP header flags, Instance ID, checksum handling, TTL/hop-limit treatment, DSCP/ECN behavior, and MTU interaction.

Implement a small host-independent LISP codec or packet-description library sufficient to encode and validate these fixtures. The codec should contain packet-format knowledge only; it must not become a userspace forwarding engine.

**Completion:** portable tests can reproduce the Cisco OTP data header profile and decode the Cisco fixtures into the expected inner packets and forwarding context.

## TASK5 — OTP southbound forwarding contract

Define the portable-to-host contract for OTP forwarding.

The contract should express EIGRP intent in EIGRP-owned terms: create or acquire a LISP forwarding context, install or replace a destination with an RLOC and IID, remove that path, and release the forwarding context.

It must support more than one RLOC, ECMP, and independent route lifecycle.

No FRR, Zebra, Linux LWT, BPF, BIRD, BSD, or kernel-native type crosses this API.

A mock implementation should be sufficient to exercise the complete portable route lifecycle.

**Completion:** portable tests can observe the correct sequence of context creation, path installation, RLOC replacement, multipath installation, withdrawal, and context release.

## TASK6 — Linux LISP fast-path prototype

Prototype the production Linux dataplane before binding it deeply to FRR.

LWT/BPF is the first mechanism to evaluate.

The prototype must prove the central forwarding requirement:

```text
Linux FIB selects OTP route
          |
          v
route-selected RLOC and IID
reach native encapsulation path
          |
          v
actual Cisco-compatible LISP packet
          |
          v
normal underlay lookup
```

The receive direction must decapsulate compatible LISP traffic and return the inner packet to normal Linux forwarding.

The prototype must also prove ECMP and underlay recursion.

Transit traffic must remain outside `eigrpd`.

**Completion:** an unmodified or normally extensible Linux host forwards bidirectional OTP-profile LISP traffic in its native dataplane using route-selected RLOCs.

## TASK7 — FRR/Linux dataplane integration

Connect the proven Linux dataplane to OpenEIGRP's FRR adapter.

The adapter maps the normalized OTP southbound contract into FRR/Zebra/Linux operations. Existing FRR interfaces should be used wherever they can express the required state.

If FRR cannot carry a required forwarding attribute, document the exact missing interface before adding a managed FRR patch. Any FRR-wide patch belongs under `frr/patch/` and must follow the project's idempotent patch-management rules.

**Completion:** a real EIGRP successor change programs the Linux LISP dataplane through the FRR integration, and route removal cleans up the corresponding forwarding state.

## TASK8 — Direct Cisco interoperability

Connect the FRR/Linux OpenEIGRP implementation directly to a Cisco OTP CE.

Verify remote adjacency, route exchange, route withdrawal, and bidirectional user traffic.

Capture OpenEIGRP-generated packets and compare them with the profile established in TASK4. Capture Cisco-generated packets and verify successful OpenEIGRP decapsulation.

Any interoperability discrepancy updates the profile and regression fixtures before further feature work.

**Completion:** OpenEIGRP and Cisco exchange EIGRP routes and forward OTP user traffic bidirectionally without special-case configuration beyond the documented OTP relationship.

## TASK9 — E-RR listener and reflection

Implement the E-RR side of remote-neighbor operation.

The E-RR must accept remote unicast EIGRP peers through the configured source context and reflect routing information according to EIGRP OTP next-hop and split-horizon behavior.

`no next-hop-self` must preserve the originating endpoint as the next hop. `no split-horizon` must permit route advertisement among remote peers sharing the source interface.

The implementation should use the existing EIGRP neighbor, topology, Update generation, and interface-policy machinery wherever possible.

**Completion:** an OpenEIGRP E-RR accepts at least two remote CEs, reflects their routes with the correct next hop and metrics, and produces a direct CE-to-CE LISP data path.

## TASK10 — Cisco E-RR interoperability

Validate OpenEIGRP against Cisco's E-RR behavior in both directions.

First use:

```text
OpenEIGRP CE ------ Cisco E-RR ------ Cisco CE
```

and verify that the Cisco CE becomes the OpenEIGRP route RLOC while the E-RR remains control-plane only.

Then, when OpenEIGRP reflection is ready, use:

```text
Cisco CE ------ OpenEIGRP E-RR ------ Cisco CE
```

and verify equivalent behavior.

Dual-E-RR testing should verify redundant control relationships without changing the direct CE-to-CE dataplane property.

**Completion:** mixed Cisco/OpenEIGRP E-RR topologies retain the correct next hop and forward user traffic directly between endpoints.

## TASK11 — Multipath, lifecycle, and scale

Exercise the production implementation beyond the simple interoperability topology.

Add multiple RLOCs, multiple routes behind each RLOC, ECMP successors, successor changes, peer failures, underlay changes, daemon restart, and RIB reconnect.

Scale peers, RLOCs, routes, and traffic flows independently.

The implementation must retain the multipoint property: growth in remote sites creates forwarding entries, not a corresponding mesh of point-to-point tunnel objects.

Run the fast-path test while scaling traffic to confirm that transit packet rate remains a host dataplane concern.

**Completion:** the Linux implementation is operationally stable under multipoint routing, ECMP, reconvergence, restart, and meaningful route/RLOC scale.

## TASK12 — IPv6 OTP

Extend the proven IPv4 design to IPv6 following the project's named-mode development sequence.

The same remote-peer, topology, RLOC, forwarding-context, and host-contract architecture is reused.

Add the required combinations of IPv6 EIGRP transport, IPv6 destinations, LISP outer addressing, MTU behavior, and Cisco interoperability fixtures.

**Completion:** IPv6 provides the same remote-peer and direct multipoint OTP behavior demonstrated by IPv4.

## TASK13 — BIRD/Linux integration

Implement the BIRD/Linux host adapter against the existing OTP southbound contract.

BIRD-specific configuration, route installation, interface discovery, and event handling remain in the BIRD integration. The EIGRP semantics and LISP profile remain common.

Where appropriate, BIRD/Linux should use the same proven Linux forwarding facility used by FRR/Linux.

**Completion:** BIRD/Linux interoperates with FRR/Linux OpenEIGRP and Cisco OTP using the same `lisp-encap` feature and the same LISP wire behavior.

## TASK14 — BIRD/BSD production dataplane

Evaluate the target BSD forwarding facilities against the established OTP host contract.

Select a native mechanism capable of multipoint route-to-RLOC forwarding and LISP encap/decap without putting production transit traffic through `eigrpd`.

Implement that mechanism below the BIRD/BSD shim.

The Linux backend provides an architectural reference; the Cisco fixtures provide the wire-protocol reference.

**Completion:** BIRD/BSD implements the same `lisp-encap` feature and interoperates with Cisco and OpenEIGRP/Linux.

## TASK15 — macOS development implementation

Add a macOS implementation suitable for development, education, labs, and interoperability testing.

The implementation may use a userspace packet path if required by macOS. It must nevertheless exchange the exact same EIGRP control behavior and LISP dataplane defined by this specification.

This implementation is particularly useful for developer packet captures and testing OpenEIGRP against Linux or Cisco without requiring macOS to satisfy production forwarding-performance requirements.

**Completion:** a macOS OpenEIGRP instance can form a remote EIGRP adjacency and exchange OTP/LISP traffic with a production OpenEIGRP implementation.
