# OpenEIGRP BFD
## Product Requirements and Engineering Design

Copyright (C) 2026 Donnie V. Savage

---

# 1. Product Overview

## 1.1 Faster failure detection for EIGRP

EIGRP already knows how to determine whether a neighbor is alive. Hellos maintain the neighbor relationship, the advertised hold time defines how long that relationship may remain silent, and expiration of the hold timer causes EIGRP to remove the neighbor and reconverge.

That mechanism is intentionally conservative. A normal EIGRP hold timer is designed to distinguish a failed neighbor from a temporarily delayed Hello. In networks where sub-second convergence matters, waiting for the EIGRP hold time can be much longer than the forwarding plane needs to determine that the path is unusable.

Bidirectional Forwarding Detection, or **BFD**, provides that faster forwarding-path signal.

With BFD enabled, EIGRP continues to establish and maintain its neighbor relationships in the normal way:

```text
                         EIGRP

                   Hello / Update
                         |
                         v
               neighbor adjacency
                         |
                         v
                        DUAL
```

BFD runs beside EIGRP and monitors the forwarding path to that same neighbor:

```text
                    EIGRP neighbor
                         |
              +----------+----------+
              |                     |
              v                     v

          EIGRP Hello             BFD
          hold timer         fast path monitor
              |                     |
              +----------+----------+
                         |
                         v
                  neighbor liveness
```

When BFD determines that a forwarding path that had been operational has failed, it notifies EIGRP immediately. EIGRP then performs the same neighbor-loss processing it would have performed when its own hold timer expired, but without waiting for that timer.

The result is faster convergence without changing EIGRP's topology algorithm, route selection, Reliable Transport Protocol, or packet format.

Cisco describes BFD-EIGRP in the same way: EIGRP registers with BFD sessions on its routing interfaces and receives forwarding-path failure notifications from BFD. BFD supplies the detection service; EIGRP remains responsible for the routing reaction.

## 1.2 What changes when BFD is enabled

Consider two EIGRP routers connected through a forwarding path:

```text
                  R1 ---------------- R2
                       EIGRP neighbor
```

Without BFD, a silent failure is detected when EIGRP's hold timer expires:

```text
path fails
    |
    v
Hellos stop
    |
    v
hold timer expires
    |
    v
neighbor DOWN
    |
    v
DUAL reconverges
```

With BFD, the forwarding path can be declared failed before the EIGRP hold timer reaches zero:

```text
path fails
    |
    v
BFD detects failure
    |
    v
BFD notifies EIGRP
    |
    v
neighbor DOWN
    |
    v
DUAL reconverges
```

The important point is that the final EIGRP behavior is unchanged. BFD provides an additional, faster reason for the existing neighbor to transition down.

Once the neighbor is gone, normal EIGRP behavior determines what happens next. If a feasible successor is available, DUAL can immediately install it. If no feasible successor exists, the destination enters Active processing and EIGRP sends Queries as usual.

BFD therefore improves **failure detection time**. It does not create a second convergence algorithm.

## 1.3 EIGRP remains responsible for adjacency establishment

BFD does not discover EIGRP neighbors.

EIGRP Hellos still discover or validate the peer, exchange the EIGRP parameters required for adjacency, and establish the EIGRP neighbor relationship. Once EIGRP has an eligible neighbor on a BFD-enabled interface, OpenEIGRP registers that neighbor with the host BFD service.

The relationship is:

```text
                  EIGRP Hello mechanism
                          |
                          v
                 neighbor discovered
                          |
                          v
                  adjacency becomes UP
                          |
                          v
                 request BFD monitoring
                          |
                          v
                BFD monitors the path
```

This also defines how the adjacency returns after a BFD failure.

When an established BFD session reports that the path has gone down, EIGRP removes the neighbor and unregisters its BFD interest. The EIGRP Hello mechanism subsequently rediscovers the peer. Once the EIGRP adjacency is established again, the neighbor is registered with BFD again.

Cisco documents this behavior explicitly for BFD-EIGRP: a BFD peer-down notification causes EIGRP to remove the peer from BFD and tear down the EIGRP relationship; the EIGRP Hello mechanism later performs peer rediscovery and BFD registration again.

Conceptually:

```text
                 established neighbor
                         |
                         v
                      BFD UP
                         |
                    path failure
                         |
                         v
                  BFD peer DOWN
                         |
                         v
                EIGRP neighbor DOWN
                         |
                         v
                BFD request released
                         |
                         v
                  EIGRP Hellos
                         |
                         v
               neighbor rediscovered
                         |
                         v
                 EIGRP adjacency UP
                         |
                         v
                  BFD registered
```

A BFD "Up" event does not create an EIGRP adjacency. EIGRP owns adjacency establishment.

## 1.4 BFD is a shared forwarding-path service

BFD is useful to many routing protocols at the same time.

A physical path between two routers might simultaneously carry EIGRP, BGP, OSPF, or another client of BFD. Running an independent BFD protocol implementation inside every routing daemon would duplicate the same forwarding-path detection work.

The intended architecture is therefore:

```text
                    EIGRP          BGP          OSPF
                      |             |             |
                      +-------------+-------------+
                                    |
                                    v
                            host BFD service
                                    |
                                    v
                         one monitored data path
```

The host BFD implementation owns BFD packets, timers, discriminators, authentication, session sharing, and the BFD state machine.

OpenEIGRP is a **BFD client**. It requests monitoring for an EIGRP neighbor and consumes the resulting state notifications.

This is also the model required by RFC 5882, which describes BFD as a service supplied to client protocols and recommends a single BFD session per data-protocol path even when several applications use it.

It is already the native architecture of both target routing stacks. Modern FRR exposes a common BFD client API backed by `bfdd`. BIRD has a native BFD protocol and an API through which routing protocols request sessions and receive state callbacks.

Using those services keeps BFD protocol machinery below the OpenEIGRP host boundary and gives FRR and BIRD the same EIGRP-visible behavior.

## 1.5 Operator experience

OpenEIGRP follows the EIGRP named-mode model used by current Cisco software.

BFD is an address-family interface property:

```text
router eigrp savage
 address-family ipv4 autonomous-system 4453
  af-interface GigabitEthernet0/0
   bfd
```

The default address-family interface can enable BFD for every participating interface:

```text
router eigrp savage
 address-family ipv4 autonomous-system 4453
  af-interface default
   bfd
```

A specific interface may carry its own effective configuration through the existing named-mode default/interface inheritance model.

The EIGRP `bfd` command enables EIGRP's use of the host BFD service. BFD session timing, authentication, echo behavior, offload, and other BFD-engine properties belong to the host BFD configuration.

This matches the operational separation used by Cisco. EIGRP is configured to register its peers with BFD, while the underlying BFD service supplies the session parameters used to monitor the path.

The operator therefore sees one routing feature:

```text
af-interface
    |
    +-- bfd
```

and one effect:

```text
forwarding-path failure
        |
        v
fast EIGRP neighbor failure
        |
        v
normal DUAL convergence
```

## 1.6 Product objective

The OpenEIGRP BFD feature should make fast failure detection feel like a native extension of EIGRP rather than a second neighbor protocol.

A developer reading the implementation should be able to follow this simple model:

```text
EIGRP creates the neighbor
        |
        v
EIGRP asks the host to monitor that neighbor with BFD
        |
        v
host BFD service reports path state
        |
        v
real BFD failure enters the normal EIGRP neighbor-down path
        |
        v
DUAL responds exactly as it does to any other neighbor loss
```

The feature is complete when this behavior is the same on FRR and BIRD even though the BFD service underneath each routing stack is different.

---

# 2. Engineering Design

## 2.1 Architectural boundary

BFD is a particularly good fit for the OpenEIGRP host abstraction because EIGRP only needs a small semantic service from the host:

```text
"Monitor the forwarding path to this EIGRP neighbor
and tell me when the established path fails."
```

The architecture is:

```text
+-----------------------------------------------------+
|                Portable OpenEIGRP                   |
|                                                     |
| named interface configuration                       |
| neighbor lifecycle                                  |
| RTP                                                 |
| topology                                            |
| DUAL                                                |
| neighbor-down processing                            |
+-------------------------+---------------------------+
                          |
                          | normalized BFD request/state
                          v
+-----------------------------------------------------+
|                 OpenEIGRP host shim                 |
|                                                     |
| create/request host BFD session                     |
| bind neighbor identity                              |
| release request                                     |
| normalize BFD state callback                        |
+-------------------------+---------------------------+
                          |
                          v
+-----------------------------------------------------+
|                    Host BFD service                 |
|                                                     |
| FRR bfdd / libbfd                                   |
| BIRD BFD protocol                                   |
| packet I/O, timers, discriminators, authentication  |
| session sharing, offload, implementation details    |
+-----------------------------------------------------+
```

Portable EIGRP should never receive an FRR `struct bfd_session_params`, a BIRD `struct bfd_request`, or another host-native BFD object.

Likewise, the portable implementation should not open BFD UDP sockets or implement RFC 5880 packet processing. It requests a service and reacts to the service result.

## 2.2 Reference implementation assessment

The supplied `frr-jf1578-eigrpd-bfd.zip` was intended to be used as an implementation reference. The archive available to this design review contains 99 directory entries and no file payload, so a line-by-line source audit is not possible from that artifact.

The historical FRR discussion around Joseph Freivald's EIGRP/BFD work does establish the architecture the implementation was using. The work was based on FRR's BFD protocol-integration API rather than a private EIGRP BFD engine. FRR maintainers noted that the BFD daemon itself had evolved substantially since FRR 5.0, while the protocol-client integration concept remained stable; their recommendation was to adapt the client API calls to current FRR rather than modify or reproduce the BFD daemon.

That architectural decision is correct for OpenEIGRP.

The current FRR tree confirms the modern form of the same model. `lib/bfd.h` provides a routing-protocol client API that creates a session request, supplies local and remote addresses, interface, VRF and hop information, installs or removes the request, and reports current and previous BFD state through a callback. Current FRR OSPF uses that API and reacts to an established `UP -> DOWN` transition by bringing the routing neighbor down.

The current BIRD tree independently validates the same abstraction. BIRD routing protocols use `bfd_request_session()` to request monitoring and receive a callback through `struct bfd_request`; the BIRD BFD protocol owns the actual session.

The implementation described by this specification therefore retains the useful design intent of the Freivald work — EIGRP as a client of the routing stack's BFD service — while targeting the current OpenEIGRP portability boundary and the current FRR/BIRD BFD APIs.

If a complete copy of the historical source becomes available later, it should be reviewed for reusable behavior and test ideas. Its FRR 5.x API calls should be treated as historical integration syntax, while the EIGRP/BFD state relationship described here remains the design contract.

## 2.3 Configuration model

BFD is retained on the named address-family interface object.

The portable interface configuration needs an explicit BFD configuration state just as it already retains interface-specific EIGRP settings such as Hello interval, hold time, split horizon, authentication, and shutdown state.

The operator-facing commands are:

```text
af-interface <interface>
 bfd

af-interface <interface>
 no bfd
```

and:

```text
af-interface default
 bfd
```

The existing `af-interface default` inheritance model determines the effective setting on runtime interfaces.

Conceptually:

```text
                    protocol default
                           |
                           v
                 af-interface default
                           |
                           v
                af-interface Ethernet0
                           |
                           v
                effective runtime state
                           |
                           v
                       BFD enabled
```

A specific interface may therefore inherit BFD from the default template or override that effective setting through its own named interface configuration, following the same configuration-resolution rules as other inheritable named-mode interface attributes.

The public configuration target belongs in the BFD namespace, for example as explicit set/reset operations. The northbound layer resolves host/YANG values and calls that EIGRP-owned target.

Changing the effective BFD state causes the runtime interface to reconcile the BFD requests for its current EIGRP neighbors.

## 2.4 Neighbor-to-BFD registration

BFD monitoring is associated with an actual EIGRP peer on a BFD-enabled interface.

The lifecycle begins with the normal EIGRP Hello exchange. When the EIGRP neighbor becomes established and the effective interface configuration enables BFD, OpenEIGRP requests a BFD session from the host:

```text
EIGRP neighbor becomes UP
          |
          v
is BFD enabled on interface?
          |
         yes
          |
          v
build normalized BFD request
          |
          +-- address family
          +-- local address
          +-- remote neighbor address
          +-- interface identity
          +-- routing/VRF context
          +-- hop model
          |
          v
host BFD session request
```

For the initial directly connected EIGRP implementation, the request represents a single-hop BFD relationship to the EIGRP neighbor.

The host BFD subsystem may satisfy that request by creating a new BFD session or by attaching OpenEIGRP to an existing session already used by another routing protocol. That distinction is deliberately invisible to portable EIGRP.

The portable side retains only enough EIGRP-owned binding state to know that the neighbor has an active BFD request and to correlate host notifications with the current EIGRP neighbor.

## 2.5 Safe callback identity

BFD notifications are asynchronous with respect to EIGRP neighbor lifecycle.

A neighbor can disappear because of an expired hold timer, an interface-down event, a Goodbye, an administrative clear, an RTP retry failure, or another cause at approximately the same time that the BFD service reports a state change.

The host callback therefore must not depend on a stale host-held pointer to a freed portable neighbor object.

The normalized callback should identify the EIGRP relationship using stable data such as the EIGRP instance/address-family context, interface identity, and peer address. Portable code then resolves the currently published neighbor before taking action.

Conceptually:

```text
host callback

    { instance, interface, peer, old-state, new-state }
                          |
                          v
                 portable peer lookup
                          |
              +-----------+-----------+
              |                       |
        current neighbor          no neighbor
              |                       |
              v                       v
        process event              ignore
```

This matches the current OpenEIGRP neighbor publication model, where receive-side lookup is protected from teardown and deleted neighbors are unpublished before their memory is released.

The same rule also makes delayed BFD callbacks harmless after BFD has been disabled or the neighbor has already been removed.

## 2.6 BFD state interpretation

BFD is advisory to EIGRP. The state transition matters, not merely the current value.

The actionable failure transition for the base feature is:

```text
BFD UP  --->  BFD DOWN
```

That transition means a path previously proven operational by BFD has failed.

The corresponding EIGRP flow is:

```text
BFD UP -> DOWN
       |
       v
resolve current EIGRP neighbor
       |
       v
record neighbor state change
reason = BFD
       |
       v
delete EIGRP neighbor
       |
       v
existing topology_neighbor_down()
       |
       v
normal DUAL convergence
```

This should use the existing EIGRP neighbor-down machinery rather than a BFD-specific route withdrawal path.

The current OpenEIGRP implementation already centralizes neighbor deletion in `eigrp_nbr_delete()`. That function unpublishes the neighbor, transitions it down, calls `eigrp_topology_neighbor_down()`, cancels neighbor-owned timers, releases retransmission state, and frees the neighbor. BFD should enter this path in the same manner as hold expiration, Goodbye, interface failure, RTP retry exhaustion, administrative clear, or SIA neighbor loss.

A BFD-specific neighbor event-log reason should be added so the exact location of the state transition records why the neighbor was removed.

## 2.7 Initial and administrative BFD states

A BFD session can exist in Down before it has ever become operational. That state does not prove that an established EIGRP forwarding path has just failed.

For this reason, initial BFD Down does not tear down an EIGRP adjacency.

The state model is:

```text
new EIGRP adjacency
       |
       v
BFD request created
       |
       +---- BFD DOWN / UNKNOWN / INIT
       |          |
       |          +---- EIGRP remains established
       |
       v
BFD UP
       |
       | path now monitored as operational
       v
BFD DOWN
       |
       +---- EIGRP neighbor failure
```

This behavior is important in partially deployed networks. If one router enables EIGRP BFD before the other endpoint has an operational BFD session, the EIGRP adjacency remains usable and its normal Hello/hold mechanism continues to provide liveness detection.

BFD `AdminDown` has a different meaning from forwarding failure. RFC 5882 defines AdminDown as an administrative BFD condition rather than evidence that the data path has failed. An established EIGRP adjacency therefore remains up when the BFD service reports AdminDown.

The same behavior is visible in current FRR's routing-protocol integrations: OSPF explicitly keeps its adjacency when an established BFD session changes to AdminDown.

The resulting decision model is:

```text
BFD notification
       |
       +-- initial DOWN ----------> maintain EIGRP
       |
       +-- ADMIN_DOWN ------------> maintain EIGRP
       |
       +-- UP --------------------> record monitored path as up
       |
       +-- UP -> DOWN ------------> fail EIGRP neighbor
```

## 2.8 Neighbor rediscovery after BFD failure

After BFD removes an EIGRP neighbor, BFD does not bring the EIGRP neighbor back.

The BFD request associated with the deleted neighbor is released. EIGRP continues its normal Hello behavior on the interface. When valid Hellos are received again, normal neighbor creation and adjacency establishment take place.

When the new neighbor reaches the appropriate established state, OpenEIGRP requests BFD monitoring again.

This deliberately creates a clean lifecycle boundary:

```text
old neighbor
    |
BFD failure
    |
neighbor deleted
    |
BFD request released
    |
-------------------------
    |
new Hello
    |
new neighbor
    |
new EIGRP adjacency
    |
new BFD request
```

Cisco documents the same EIGRP/BFD interaction and notes that BFD does not independently restore the EIGRP peer; EIGRP Hello processing performs rediscovery and registration.

## 2.9 Interface and multiaccess behavior

BFD enablement is configured on an EIGRP address-family interface, but monitoring is per peer.

On a multiaccess network:

```text
                         LAN

              +-----------+-----------+
              |           |           |
             R1          R2          R3
```

R1 may have EIGRP neighbors R2 and R3. Enabling BFD on the interface causes R1 to request monitoring for both neighbor paths.

Each BFD failure maps back to the corresponding EIGRP neighbor:

```text
BFD R1<->R2 DOWN
        |
        +---- remove R2 EIGRP neighbor

R3 BFD remains UP
        |
        +---- R3 EIGRP neighbor remains UP
```

An individual BFD session failure therefore does not imply that the EIGRP interface itself is down.

If the actual host interface goes down, the existing EIGRP interface-down path remains authoritative and removes the affected neighbors. Any BFD callbacks arriving during that teardown are resolved against the current neighbor table and become harmless once the neighbor has been unpublished.

## 2.10 BFD service restart and replay

The BFD service itself may restart while EIGRP remains operational.

FRR's current BFD integration is designed to reconnect and replay client session requests after BFD service availability changes. BIRD similarly owns BFD session lifecycle independently of the routing protocols that request it.

OpenEIGRP should therefore model a BFD client request as desired host state rather than as proof that `bfdd` or another BFD engine is continuously connected.

A BFD service restart should result in:

```text
EIGRP adjacency remains UP
          |
          v
host BFD service restarts
          |
          v
session request replay / reattachment
          |
          v
BFD re-establishes
```

The BFD state transitions involved in establishing or re-establishing the monitoring session do not represent a proven forwarding failure and must not flap the EIGRP adjacency merely because the BFD process restarted.

A later real `UP -> DOWN` transition remains actionable.

## 2.11 BFD timing and profiles

EIGRP enables or disables use of BFD. The BFD subsystem determines how the BFD session itself operates.

On Cisco, baseline BFD session parameters are configured on the interface and EIGRP registers its neighbors with those sessions.

FRR exposes BFD profiles and session parameters through its BFD service. BIRD exposes BFD configuration and interface patterns through its BFD protocol.

OpenEIGRP should preserve that separation.

The EIGRP configuration:

```text
af-interface Ethernet0
 bfd
```

means:

```text
"Use the host BFD service for EIGRP neighbors on Ethernet0."
```

It does not create a second OpenEIGRP-owned timer configuration language.

This allows operators and platforms to select software BFD, hardware-offloaded BFD, echo mode, authentication, timer profiles, or platform-specific tuning without changing EIGRP protocol logic.

## 2.12 Observability

A BFD-triggered neighbor loss should be visible through the same operational surfaces used for other EIGRP neighbor changes.

The neighbor state event must identify BFD as the reason at the point where the neighbor transitions down:

```text
neighbor X:
    UP -> DOWN
    reason: BFD
```

The current event log already distinguishes hold expiration, K-value mismatch, Goodbye, peer restart, RTP retry limit, administrative clear, interface down, peer termination, and SIA. BFD becomes another precise reason in that family.

Interface detail should indicate whether BFD is effectively enabled for EIGRP on the interface.

Detailed BFD packet/session diagnostics remain the responsibility of the host BFD service. FRR operators can inspect the FRR BFD state; BIRD operators can inspect BIRD's BFD protocol state. OpenEIGRP does not need to duplicate the host's full BFD diagnostic database.

## 2.13 IPv4 and IPv6

The BFD relationship is naturally address-family aware.

The first implementation follows the project development order and completes named-mode IPv4 before IPv6. The portable host contract should nevertheless carry an EIGRP-owned address-family and address representation from the beginning so that IPv6 does not require a second BFD architecture.

The IPv6 implementation follows the same lifecycle:

```text
IPv6 EIGRP neighbor UP
        |
        v
IPv6 BFD session request
        |
        v
BFD UP -> DOWN
        |
        v
IPv6 EIGRP neighbor DOWN
        |
        v
normal IPv6 DUAL behavior
```

Cisco named mode exposes the same `bfd` command in IPv6 address-family interface mode.

---

# 3. Host Requirements, Limitations, and Exclusions

## 3.1 Required host service

A host capable of OpenEIGRP BFD provides a client-facing BFD service.

OpenEIGRP needs to be able to request monitoring for a path identified by the local and remote endpoints, interface/routing context, and address family; remove that request; and receive normalized state notifications.

The host owns the mechanics required to satisfy that service.

This permits the same portable EIGRP behavior to sit over very different BFD implementations:

```text
                    Portable OpenEIGRP
                            |
                            v
                      BFD host API
                      /           \
                     /             \
                    v               v

               FRR / libbfd      BIRD BFD
                    |               |
                    v               v
                  bfdd        BIRD BFD engine
```

Both implementations expose the same meaning to EIGRP: an established monitored path has either remained operational or has failed.

## 3.2 FRR/Linux

FRR/Linux is the first production target.

Modern FRR already supplies the necessary BFD client infrastructure in `lib/bfd.h`. A routing daemon can allocate a BFD session request with a callback, set IPv4 or IPv6 endpoints, interface, VRF, hop count and optional profile information, install or uninstall the request, and receive the current and previous state in the callback.

OpenEIGRP's FRR adapter should use that API rather than add BFD packet processing to `eigrpd`.

The expected integration is:

```text
OpenEIGRP neighbor UP
       |
       v
FRR adapter creates/updates BFD request
       |
       v
libbfd
       |
       v
bfdd / FRR BFD implementation
       |
       v
state callback
       |
       v
FRR adapter normalizes event
       |
       v
portable OpenEIGRP BFD target
```

FRR's existing BFD protocol integration initialization should be used once for the EIGRP daemon's BFD client connection.

The current FRR API should be treated as authoritative for implementation. The historical Freivald code was written against an older FRR generation and is useful for architecture and behavior, not for copying obsolete BFD API calls.

A normal implementation should not require an FRR-wide patch because BFD is already a supported FRR daemon service. If repository inspection during implementation identifies an actual missing FRR hook, that gap must be documented before considering a managed patch under `frr/patch/`.

## 3.3 BIRD/Linux and BIRD/BSD

BIRD already includes a native BFD protocol and a client API in `nest/bfd.h`.

Routing protocols call `bfd_request_session()` with the remote address, local address, interface/VRF context, callback and BFD options. BIRD returns a request object attached to the underlying BFD session and invokes the client when the state changes.

The BIRD/OpenEIGRP shim should use that service directly.

Conceptually:

```text
OpenEIGRP neighbor UP
       |
       v
BIRD adapter requests BFD session
       |
       v
BIRD BFD protocol
       |
       v
state notification
       |
       v
BIRD adapter normalizes event
       |
       v
portable OpenEIGRP BFD target
```

The same adapter architecture is appropriate on BIRD/Linux and BIRD/BSD. BIRD's own BFD implementation remains responsible for host-specific socket, timer, and platform behavior.

This makes BFD materially simpler to port than a dataplane feature such as OTP: both primary routing-stack targets already provide a BFD service designed for protocol clients.

## 3.4 Runtime capability

The current OpenEIGRP tree already reserves `EIGRP_FEATURE_BFD` as an image capability and reports BFD in the feature/status contract. The common image currently disables the feature until a real `eigrp_bfd_supported()` implementation exists.

That capability should become true when the image contains a functional host BFD adapter for the target integration.

BFD configuration remains retained in the named configuration model even when the host image does not provide the runtime capability. Runtime activation on such a host returns the structured unsupported result required by the project's capability-boundary model.

Temporary loss or restart of the BFD daemon is operational service state, not loss of the compiled image capability.

## 3.5 Session sharing

The host BFD service may have other clients using the same monitored path.

OpenEIGRP therefore owns its **request**, not the underlying BFD session.

Removing an EIGRP neighbor or disabling EIGRP BFD releases OpenEIGRP's interest. The host BFD implementation decides whether the underlying session can be destroyed or remains necessary for another protocol.

This is required for correct coexistence with BGP, OSPF, IS-IS, static-route tracking, or another BFD client on the same host.

## 3.6 Initial feature boundary

The first production implementation covers EIGRP named-mode IPv4 neighbors on normal EIGRP interfaces.

It provides interface/default `bfd` configuration, host BFD registration for established neighbors, BFD failure notification, precise EIGRP neighbor teardown, normal DUAL reconvergence, event logging, lifecycle cleanup, and FRR/Linux interoperability with Cisco BFD-EIGRP.

IPv6 follows after IPv4 according to the project development order.

Multihop BFD for future routed remote-neighbor/OTP use can use the same normalized host contract because both current FRR and BIRD have multihop BFD concepts. It should be enabled as an explicit extension after the base directly connected BFD behavior is complete.

BFD echo mode, authentication algorithms, timer negotiation, discriminator allocation, packet encoding, and hardware offload remain services of the host BFD implementation.

---

# 4. Test Strategy and Test Cases

## 4.1 Testing objective

A successful BFD integration must prove more than that a BFD session reaches Up.

The feature is correct only when EIGRP configuration creates the right host request, a genuine BFD path failure causes exactly one EIGRP neighbor loss, the normal topology and DUAL machinery handles that loss, recovery occurs through EIGRP Hello rediscovery, and BFD service lifecycle events do not create artificial EIGRP failures.

The tests therefore follow the same lifecycle a production router experiences.

## 4.2 Named-mode configuration

The first tests exercise:

```text
router eigrp savage
 address-family ipv4 autonomous-system 4453
  af-interface default
   bfd
```

and a specific interface:

```text
af-interface Ethernet0
 bfd
```

The configuration must survive writeback and reload.

`no bfd` restores the inherited/default behavior according to the existing named-mode interface configuration model.

The tests verify both retained configuration and effective runtime state. Enabling BFD on the default interface template must apply to participating runtime interfaces, while a specific interface object must follow the same override rules used by the rest of named-mode interface configuration.

## 4.3 Session registration lifecycle

Two OpenEIGRP routers form a normal IPv4 adjacency on a BFD-enabled interface.

The host adapter should not use BFD to invent the neighbor. The sequence under test is:

```text
EIGRP Hello
    |
neighbor creation
    |
adjacency UP
    |
BFD request installed
```

The test verifies that the request contains the correct local address, remote neighbor address, interface and routing context.

Deleting the neighbor must release the OpenEIGRP BFD request.

Disabling BFD on the interface while the EIGRP neighbor remains established must release the BFD request while leaving the EIGRP adjacency operational under its normal Hello/hold-time mechanism.

Re-enabling BFD must register the current eligible neighbor again without requiring the EIGRP adjacency to be rebuilt.

## 4.4 Initial BFD Down

A critical negative test starts with an established EIGRP adjacency while the remote endpoint does not yet establish BFD.

The local host BFD request may remain Down or Unknown.

The EIGRP adjacency must remain Up.

This protects staged deployment and verifies that OpenEIGRP reacts to a demonstrated `UP -> DOWN` forwarding failure rather than treating "BFD is not currently Up" as equivalent to "the EIGRP neighbor has failed."

When BFD later reaches Up, no EIGRP neighbor transition occurs.

## 4.5 BFD Up-to-Down failure

With both EIGRP and BFD established:

```text
EIGRP neighbor = UP
BFD session    = UP
```

break the forwarding path in a way detected by BFD before the EIGRP hold timer expires.

The host callback should report:

```text
previous = UP
current  = DOWN
```

OpenEIGRP must record the BFD neighbor-down reason at the actual neighbor state transition and enter the existing neighbor deletion path.

The topology must remove all paths owned by that neighbor and DUAL must respond normally.

The test should confirm that the EIGRP hold timer had not expired when the neighbor was removed, demonstrating that BFD actually accelerated detection.

## 4.6 Feasible-successor convergence

Construct a topology where the BFD-protected neighbor is the current successor and a valid feasible successor already exists.

After the BFD `UP -> DOWN` transition, the successor should be removed and the feasible successor should be installed through normal DUAL processing.

The prefix should remain Passive and no Query should be necessary.

This validates the complete value proposition:

```text
fast BFD detection
       |
       v
normal neighbor loss
       |
       v
precomputed DUAL backup
       |
       v
fast route replacement
```

## 4.7 Active transition without a feasible successor

Repeat the failure with no feasible successor.

The BFD event still enters the same neighbor-down path, but DUAL should now place the affected destination into the appropriate Active state and send Queries.

The test verifies that BFD changes only when failure is detected, not how DUAL computes the replacement route.

## 4.8 AdminDown

Bring an established BFD session to AdminDown while leaving the EIGRP path usable.

The EIGRP neighbor must remain Up.

Normal Hellos and hold-time refresh continue.

This test enforces the RFC 5882 semantic that AdminDown represents administrative BFD state rather than proof of connectivity failure.

## 4.9 BFD service restart

With EIGRP and BFD established, restart the host BFD service while leaving the data path intact.

The EIGRP adjacency must remain established while the host BFD integration reconnects and replays or reconstructs its requested session state.

Once BFD returns to Up, monitoring continues.

This test prevents daemon restart or BFD client reconnection from being mistaken for a forwarding failure.

## 4.10 Neighbor rediscovery

After a real BFD failure removes the EIGRP neighbor, restore the path.

EIGRP Hellos must rediscover the peer and create a new adjacency.

Once that new adjacency reaches Up, OpenEIGRP registers the new neighbor with BFD.

The test verifies that BFD does not directly resurrect the previous EIGRP neighbor object and that no stale BFD binding survives the old neighbor lifecycle.

## 4.11 Multiaccess isolation

Use three EIGRP routers on one broadcast interface.

R1 maintains BFD sessions to R2 and R3.

Cause only the R1-R2 BFD path to fail.

R1 must remove R2 while preserving R3 and keeping the EIGRP interface operational.

This validates the per-neighbor relationship between BFD state and EIGRP liveness.

## 4.12 Failure races and stale callbacks

Exercise races between BFD failure and the other existing EIGRP neighbor-removal paths.

Useful cases include BFD Down concurrent with interface Down, BFD Down concurrent with hold expiration, BFD Down after administrative clear, and a delayed BFD callback after the neighbor has already been deleted.

Each case must result in one effective neighbor removal, one topology neighbor-down operation, no duplicate free, no stale pointer access, and no state-change event claiming a transition that did not actually occur.

This test is especially important because BFD notifications are asynchronous to EIGRP timer and interface events.

## 4.13 Shared host BFD session

Where the host supports session sharing, configure another routing protocol to use BFD to the same endpoint.

Enable EIGRP BFD and confirm that both clients receive the appropriate host BFD state.

Then remove or disable EIGRP BFD.

The underlying BFD session must remain available to the other client when the host determines it is still required.

This validates the requirement that OpenEIGRP owns a client request rather than the shared BFD protocol session.

## 4.14 VRF and routing-context isolation

Create identical neighbor addresses in separate routing contexts.

BFD events must resolve to the EIGRP neighbor in the correct VRF/context and must not affect the same address in another context.

This proves that neighbor identity is not keyed by remote address alone.

## 4.15 Cisco interoperability

The primary interoperability topology is:

```text
            OpenEIGRP / FRR -------- Cisco EIGRP
                     BFD session
```

Configure normal EIGRP named mode and BFD on the shared routing interface.

Verify that the BFD session reaches Up and that both routers maintain the EIGRP adjacency.

Introduce a forwarding failure and verify that BFD detects it before the EIGRP hold timer, OpenEIGRP tears down the neighbor with a BFD reason, and the Cisco endpoint exhibits the corresponding BFD/EIGRP behavior.

Restore connectivity and verify EIGRP Hello rediscovery followed by renewed BFD registration.

Repeat with BFD enabled first on only one endpoint to verify that an unestablished BFD session does not prevent EIGRP adjacency operation.

## 4.16 FRR-native integration

FRR UUT testing should inspect both sides of the boundary.

The FRR BFD operational view should show the session requested by OpenEIGRP with the expected endpoint, interface and VRF.

The OpenEIGRP operational view should show BFD enabled on the EIGRP interface while neighbor-change logging identifies BFD when a real failure occurs.

Restarting `bfdd` independently of `eigrpd` must exercise request replay/recovery without an artificial EIGRP flap.

## 4.17 BIRD-native integration

Once the BIRD adapter exists, the same behavior is tested using BIRD's BFD protocol service.

The test should verify that the OpenEIGRP/BIRD adapter requests the correct BFD session, receives the callback, and feeds the identical portable EIGRP state transition used by FRR.

Interoperability between OpenEIGRP/FRR and OpenEIGRP/BIRD demonstrates the intended design property: the host BFD engines differ, while the EIGRP BFD feature behaves the same.

## 4.18 IPv6

After named-mode IPv4 is complete, repeat the configuration, session lifecycle, failure, AdminDown, rediscovery, race, and interoperability tests for IPv6.

The IPv6 work should reuse the same portable BFD state model and host contract rather than introducing an IPv6-specific BFD architecture.

## 4.19 Completion criteria

The initial production feature is complete when an OpenEIGRP/FRR/Linux router can enable BFD through named `af-interface` configuration, register every eligible established EIGRP neighbor with FRR's BFD service, preserve the EIGRP adjacency through initial BFD Down and AdminDown states, tear the correct neighbor down immediately on a genuine BFD `UP -> DOWN` failure, drive ordinary DUAL convergence through the existing neighbor-down path, rediscover the peer through EIGRP Hellos, safely survive BFD daemon restart and lifecycle races, and interoperate with Cisco BFD-EIGRP.

---

# 5. Implementation Plan

Section 5 is the engineering handoff. Each task should leave the tree buildable and establish a complete, testable layer needed by the next task.

## TASK1 — Add the portable BFD configuration target

Add BFD to the named address-family interface configuration model.

The work should extend the existing `eigrp_intf_config_t` / named `af-interface` inheritance machinery so `bfd` behaves like an ordinary EIGRP interface feature. Add explicit BFD set/reset targets following the project naming convention and connect them to the northbound layer.

The default interface template and specific interface objects must produce the correct effective runtime state, and configuration must remain retained even when the current host image does not support BFD at runtime.

The existing `EIGRP_FEATURE_BFD` capability remains disabled until a real runtime adapter exists.

**Completion:** `af-interface default` and a specific named interface can retain, inherit, override, write, and reset `bfd` configuration through a real EIGRP target, with portable configuration tests.

## TASK2 — Define the portable BFD host contract

Add the minimum OpenEIGRP-owned service required to request and release BFD monitoring and receive state changes.

The request should carry normalized EIGRP information: address family, local and remote addresses, interface identity, routing context, and the hop model needed by the host.

The callback/state contract should expose the BFD states needed by EIGRP, including at least Unknown, AdminDown, Down, and Up, plus enough transition information to distinguish initial Down from `Up -> Down`.

Host-native session objects must remain inside the FRR or BIRD adapter. The portable side should correlate callbacks through stable EIGRP-owned identity rather than a host-retained raw pointer to a neighbor that may already have been freed.

**Completion:** a mock host can accept requests, report state changes, release requests, and prove the full lifecycle without FRR or BIRD types in the portable API.

## TASK3 — Implement portable EIGRP/BFD neighbor lifecycle

Create the portable BFD module that binds effective interface BFD configuration to EIGRP neighbor state.

When an eligible neighbor becomes Up on a BFD-enabled interface, request host monitoring. When the neighbor is deleted or BFD becomes disabled, release the request.

Implement the state interpretation defined by this specification. Initial Down/Unknown and AdminDown preserve the adjacency. A genuine `UP -> DOWN` transition resolves the current neighbor and enters the existing neighbor-down path.

Add `BFD` as an event-log neighbor-down reason and record it at the exact location where the EIGRP state is changed.

Use `eigrp_nbr_delete()` for teardown so topology removal, timers, retransmission state, and DUAL behavior remain common.

**Completion:** portable tests can drive simulated BFD state transitions and prove that exactly one current EIGRP neighbor is deleted only for the actionable failure transition.

## TASK4 — Implement the FRR BFD adapter

Add the FRR-specific BFD client integration under `frr/`.

Use the current `lib/bfd.h` API. Initialize FRR's BFD protocol integration for `eigrpd`, create/update a BFD session request for each OpenEIGRP request, set the appropriate endpoint/interface/VRF information, install or uninstall the request, and normalize FRR's callback into the portable BFD state contract.

The adapter must treat the FRR session object as host-private state.

Review the historical Freivald integration if a complete source copy becomes available, but use current FRR APIs and current lifecycle practices as the implementation authority.

**Completion:** an FRR/OpenEIGRP neighbor causes a real `bfdd` session request to appear, and FRR BFD state callbacks reach the portable BFD module.

## TASK5 — Complete FRR named-mode CLI/YANG integration

Expose `bfd` and `no bfd` in named `af-interface` mode and add the corresponding YANG/northbound leaf beneath the existing named address-family interface object.

The command must work under both `af-interface default` and a specific interface and terminate at the real portable BFD set/reset functions.

Writeback should reflect explicit configuration rather than emitting host BFD internals.

Update capability/status output so the FRR image reports BFD supported when the real adapter is linked.

**Completion:** FRR named-mode configuration round-trips `bfd`, drives the portable target, and activates/deactivates live BFD requests for existing and future neighbors.

## TASK6 — Add portable failure and race coverage

Populate `eigrp/test/bfd/` with the portable contract tests already reserved by the repository.

Cover initial Down, Up, `Up -> Down`, AdminDown, disable/re-enable, neighbor deletion, interface deletion, stale callback, hold-timer race, interface-down race, administrative-clear race, and BFD failure while routes have both feasible-successor and no-feasible-successor outcomes.

The tests should verify event ordering as well as final state so a future refactor cannot log a BFD transition at a different location from the actual neighbor state change.

**Completion:** the portable suite proves the complete EIGRP/BFD state contract without requiring a running host BFD daemon.

## TASK7 — Add FRR native/UUT tests

Add FRR tests that run OpenEIGRP against the real FRR BFD service.

Verify request parameters, session establishment, peer failure, request removal, `bfdd` restart/replay, VRF identity, multiaccess neighbor isolation, and interaction with another FRR BFD client where practical.

The UUT should confirm that loss detection occurs before the configured EIGRP hold timer and that `eigrpd` reports the neighbor-down reason as BFD.

**Completion:** FRR native tests prove that the host service and portable EIGRP lifecycle remain synchronized through normal operation and restart.

## TASK8 — Cisco IPv4 interoperability

Connect OpenEIGRP/FRR/Linux to a Cisco router using named-mode EIGRP BFD.

Verify initial adjacency with BFD not yet Up, BFD establishment, real BFD failure, EIGRP convergence, Hello-based rediscovery, and renewed BFD registration.

Test asymmetric initial BFD enablement and confirm the EIGRP adjacency remains available until BFD has actually established and subsequently reports failure.

Where Cisco exposes registered clients in `show bfd neighbors details`, capture the operational state as interoperability evidence.

**Completion:** OpenEIGRP and Cisco use BFD to accelerate failure of the same EIGRP adjacency and recover through ordinary EIGRP neighbor discovery.

## TASK9 — Implement BIRD BFD adapter

Implement the BIRD host adapter using BIRD's native `bfd_request_session()` service.

Map the same normalized OpenEIGRP request into BIRD endpoint/interface/VRF information, retain the BIRD request only inside the adapter, translate BIRD state notifications into the portable contract, and release the request with the owning BIRD resource lifecycle.

No BIRD BFD object or configuration object may enter portable `eigrp/` APIs.

**Completion:** BIRD/OpenEIGRP exercises the same portable state transitions as FRR while using BIRD's own BFD engine.

## TASK10 — BIRD/Linux and BIRD/BSD interoperability

Run the BIRD adapter first on Linux and then on the BSD production target.

Verify direct OpenEIGRP FRR-to-BIRD operation and Cisco-to-BIRD operation where the lab permits.

The purpose is to prove that BFD engine differences are entirely below the shim: a real BFD failure produces the same EIGRP neighbor transition, event reason, and DUAL behavior on every supported host.

**Completion:** BIRD/Linux and BIRD/BSD provide the same OpenEIGRP BFD feature semantics as FRR/Linux.

## TASK11 — IPv6 BFD

Extend the completed design to named-mode IPv6.

Reuse the same BFD interface configuration target, portable session model, state-transition rules, neighbor lifecycle, and host adapters with IPv6 addresses.

Add Cisco interoperability and host-native tests for IPv6 BFD/EIGRP.

**Completion:** IPv6 BFD provides the same failure-detection and EIGRP convergence semantics established for IPv4.

## TASK12 — Remote/multihop BFD extension

After the base feature and OTP remote-peer behavior are both stable, evaluate BFD monitoring for routed remote EIGRP peers.

The existing host contract should already carry enough identity to express single-hop versus multihop requests. This task defines the explicit OpenEIGRP policy for associating multihop BFD with a remote EIGRP relationship and validates it against the host BFD implementations.

It should reuse the same core rule: EIGRP establishes the adjacency, BFD monitors the established forwarding path, and an actionable BFD failure enters the normal EIGRP neighbor-down path.

**Completion:** supported remote peers can use host multihop BFD without changing the base directly connected BFD behavior.

---

# References

Cisco, *Configuring BFD-EIGRP Support*:
https://www.cisco.com/c/en/us/td/docs/routers/ios/config/17-x/ip-routing/b-ip-routing/m_irb-bi-fwd-det-0-1.html

Cisco, *BFD Support for EIGRP IPv6*:
https://www.cisco.com/c/en/us/td/docs/routers/ios/config/17-x/ip-routing/b-ip-routing/m_ire-bfd-ipv6.html

Cisco, *Configure EIGRP Named Mode*:
https://www.cisco.com/c/en/us/support/docs/ip/enhanced-interior-gateway-routing-protocol-eigrp/200156-Configure-EIGRP-Named-Mode.html

RFC 5880, *Bidirectional Forwarding Detection (BFD)*:
https://www.rfc-editor.org/rfc/rfc5880.html

RFC 5881, *BFD for IPv4 and IPv6 (Single Hop)*:
https://www.rfc-editor.org/rfc/rfc5881.html

RFC 5882, *Generic Application of Bidirectional Forwarding Detection (BFD)*:
https://www.rfc-editor.org/rfc/rfc5882.html

RFC 5883, *BFD for Multihop Paths*:
https://www.rfc-editor.org/rfc/rfc5883.html

FRRouting developer discussion, June 2020, *EIGRP*:
https://lists.frrouting.org/archives/list/dev@lists.frrouting.org/2020/6/

Repository implementation references:
- Current FRR `lib/bfd.h`
- Current FRR routing-protocol BFD integrations, especially `ospfd/ospf_bfd.c`
- Current BIRD `nest/bfd.h` and `proto/bfd/`
- OpenEIGRP `eigrp_features.[ch]`, `eigrp_neighbor.c`, `eigrp_eventlog.[ch]`,
  `eigrp_interface.[ch]`, and `eigrp/test/bfd/`
