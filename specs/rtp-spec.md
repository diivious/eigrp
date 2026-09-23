# EIGRP Packetization and Reliable Transport Specification

Copyright (C) 2026 Donnie V. Savage

Existing source files must preserve prior copyright notices, SPDX identifiers,
and author history.

## 1. Purpose

This document defines EIGRP packetization and Reliable Transport Protocol (RTP) behavior, from DUAL/topology work through interface pacing, ACK tracking, and retransmission.

```text
DUAL/FSM/topology event
  -> packetizer work queue
  -> packetizer
  -> interface-specific built packet
  -> per-interface output queue/pacing
  -> reliable transport / ACK / retransmission
```

It extends `design-spec.md` and `dual.md` and does not redefine RFC 7868 wire behavior.

This is a core support document, same class as `dual.md` and `rfc7868.md`.
Platform integrators do not need packetizer or RTP internals to write a shim.
Use `integration-spec.md` and `EIGRP-Config-Guide.md` for that job.

## 2. Authority and protocol invariants

Packetization and RTP follow the project authority in `design-spec.md`. Internal queues,
work objects, and ownership mechanics are implementation details; emitted
packets and reliable transport must follow RFC 7868.

Required protocol behavior includes:

- UPDATE, QUERY, REPLY, SIA-QUERY, and SIA-REPLY carry destination routing
  information consumed by DUAL;
- multiple destinations may be packed into one packet and split across packets
  at packet boundaries;
- reliable multicast may be used for first transmission when appropriate;
- retransmission of a reliable multicast packet is unicast only to neighbors
  that still owe an ACK;
- ACK/retransmission state is per neighbor;
- transmit pacing is interface-specific;
- split horizon, poison reverse, pending-neighbor exclusion, and conditional
  receive behavior are evaluated with interface/neighbor context;
- one interface may need TLV1, TLV2, or both depending on established neighbor
  capabilities.

Any implementation that intentionally differs from these protocol rules requires
an explicit protocol-design decision; host-framework convenience is not enough.

## 3. Terminology

### 3.1 Packetizer work item

An `eigrp_packetizer_work_t` is a request to inspect current EIGRP topology state
and emit route information for an opcode/context.

It is not a packet and does not contain prebuilt route TLVs.

A work item may identify:

- an opcode;
- a destination/prefix descriptor;
- a route/path descriptor;
- an exception interface;
- a target neighbor;
- ownership/lifetime flags required by the queued operation.

### 3.2 Destination-level work (NDB/DNDB concept)

Destination-level work represents one topology destination whose final outbound
interface set is determined when the packetizer runs.

The work item does not freeze TLV family, output interface, MTU, pacing state, or
neighbor capability at enqueue time.

### 3.3 Route-descriptor work (RDB/DRDB concept)

Route-descriptor work represents a path/neighbor-specific operation. The
interface or neighbor context may be known when queued, but the packetizer
revalidates that context before use.

### 3.4 Built packet

An `eigrp_packet_t` is a wire-image packet buffer plus transmission metadata.
After final encoding/checksum/authentication, its wire image is immutable.

A built packet is interface-specific. Do not share one mutable packet object
across interfaces with different MTU, authentication, pacing, or target sets.

### 3.5 Interface packet queue

Each EIGRP interface owns a built-packet output queue. The queue contains packet
objects/references, not topology state.

## 4. Queue model

The architecture has two scheduling layers:

```text
eigrp_instance_t::packetizer_queue
  protocol work queue
  holds native EIGRP work items
  no final wire TLV/interface packet exists yet

interface eigrp_packet_queue_t
  built-packet transmit queue
  packet is bound to one interface
  subject to interface pacing/send scheduling
```

Reliable transport maintains per-neighbor outstanding-packet/sequence state.
That state is not a third generic packetizer queue.

## 5. Packetizer work ownership

DUAL, topology, and route-management code request route-packet work by queueing
native EIGRP objects/context. They do not prebuild route TLVs for topology-change
traffic.

The ownership rule is:

```text
DUAL/topology selects semantic work
  -> packetizer selects target interface/neighbor context
  -> selected codec encodes the route TLV
  -> packet layer queues/sends the immutable packet
```

### 5.1 Work represents current state

Queued work is a request to inspect the owning object at packetization time; it
is not a frozen serialized snapshot of an earlier topology state.

If the same object changes while work is queued, the implementation may coalesce
or reprioritize an unconsumed item. If the earlier item is already being
processed, queue a new item for the newer state when another transmission is
required.

Object lifetime must remain valid until queued work either consumes the object
or explicitly owns a safe snapshot/deferred-free reference.

### 5.2 Context validation

Before building a packet, validate that referenced destination/path,
interface, and neighbor objects are still valid for the requested operation.
Stale work is discarded safely; it must not dereference detached runtime state.

## 6. Work queue scheduling boundary

Portable packetizer code schedules through the EIGRP-owned work-queue contract:

```c
eigrp_work_queue_t *eigrp_work_queue_new(...);
void eigrp_work_queue_free(...);
void eigrp_work_queue_reset(...);
void eigrp_work_queue_enqueue(...);
eigrp_instance_t *eigrp_work_queue_eigrp(...);
```

The FRR implementation lives in `frr/eigrp_southbound.c` and may use FRR
`work_queue`/event facilities privately. The BIRD adapter provides the
corresponding host implementation without changing packetizer, DUAL, topology,
or TLV code.

There is no portable packetizer API that exposes a host event/wakeup object.
Queue insertion owns scheduling.

## 7. Native data and codec dispatch

Packetizer work carries native EIGRP topology/address/metric data. TLV1/TLV2
private wire structures do not leak into packetizer, topology, DUAL, CLI, or
reliable-transport state.

### 7.1 Selected codec is the runtime branch

Runtime route encoding/decoding uses bound function vectors rather than repeated
packetizer-side TLV-version branching.

Conceptually:

```c
ei->encoder(eigrp, ei, NULL, stream, route);
nbr->encoder(eigrp, ei, nbr, stream, route);
nbr->decoder(eigrp, nbr, stream, packet_len);
```

Use:

```text
interface-wide / first-send multicast route packet -> interface encoder
neighbor-targeted first-build unicast packet        -> neighbor encoder
receive path                                         -> neighbor decoder
```

A packet already built for reliable multicast is not re-encoded for
retransmission. The existing immutable wire image is sent unicast to each
neighbor that still owes the sequence ACK.

### 7.2 Codec ownership

`eigrp_tlv1.[c|h]` owns classic route TLV encoding/decoding.
`eigrp_tlv2.[c|h]` owns multiprotocol/wide route TLV encoding/decoding.

The EIGRP instance owns local codec vectors used to bind neighbor/interface
dispatch state:

```c
typedef struct eigrp_tlv_codec {
    eigrp_packet_encoder_t encoder;
    eigrp_packet_decoder_t decoder;
} eigrp_tlv_codec_t;
```

TLV init functions install private codec functions into these vectors.
`eigrp_neighbor_codec_bind()` copies the selected neighbor codec and records its
TLV version. Interface aggregate selection uses the interface encoder state.
Callers do not need TLV1/TLV2-specific public bind entry points and do not call
private TLV codec internals directly.

### 7.3 Safe codec state

Before capability negotiation, a neighbor/interface uses safe codec functions.
The safe encoder produces no route TLV. The safe decoder fails/stops decoding in
a way that guarantees cursor progress or termination; it must not create an
infinite receive loop on undecodable input.

## 8. Neighbor codec state

Use `neighbor` in code and CLI terminology. A single neighbor negotiates one
route TLV family and is never itself mixed.

Neighbor state contains the negotiated TLV version plus bound encoder/decoder:

```c
uint8_t tlv_version;
eigrp_packet_encoder_t encoder;
eigrp_packet_decoder_t decoder;
```

Lifecycle:

```text
neighbor create
  -> TLV version NONE
  -> safe encoder/decoder

neighbor becomes fully up and capability is known
  -> bind the selected TLV version and codec with eigrp_neighbor_codec_bind()
  -> contribute that version to interface aggregate encoder state

neighbor teardown
  -> remove interface aggregate contribution while tlv_version is still valid
  -> clear/free neighbor state
```

Teardown ordering must prevent interface TLV counters from drifting.

## 9. Interface aggregate encoder state

An interface owns the aggregate route encoder used for interface-wide route
packets. It reflects only fully established neighbors eligible to receive route
information.

The interface tracks TLV1/TLV2 established-neighbor counts and one selected
encoder:

```c
uint16_t tlv1_peer_count;
uint16_t tlv2_peer_count;
eigrp_packet_encoder_t encoder;
```

The field names may retain historical `peer_count` spelling; semantically these
are established EIGRP neighbor counts.

Aggregate selection is:

```text
no eligible neighbors       -> safe encoder
TLV1 neighbors only         -> TLV1 encoder
TLV2 neighbors only         -> TLV2 encoder
both TLV1 and TLV2 present  -> eigrp_packet_encoder_both
```

`eigrp_interface_encoder_bind()` and
`eigrp_interface_encoder_unbind()` own counter changes and aggregate selection.
Interface teardown clears the aggregate state unconditionally.

The packetizer uses the selected vector; it does not walk neighbors simply to
recompute TLV-family choice for every destination.

Operational/debug output reads this owned state rather than recomputing it, so
count/bind drift remains visible.

## 10. Destination-level packetization

For destination-level work:

```text
for each eligible EIGRP interface:
  skip excluded/down/nonparticipating interface
  evaluate pending-neighbor eligibility
  evaluate split horizon / poison reverse
  select interface encoder
  create interface-specific packet content
  enqueue only when route content was produced
```

One destination work item may produce zero, one, or multiple interface packets.

The packetizer does not decide `tlv1`, `tlv2`, or `both` with local capability
branches; `ei->encoder` represents that decision.

## 11. Route/neighbor-specific packetization

For route-descriptor or neighbor-targeted work:

```text
validate path descriptor
validate interface/neighbor binding
apply pending-neighbor and split-horizon/poison rules
use neighbor encoder for neighbor-targeted first-build unicast
otherwise use interface encoder
queue only when route content was produced
```

REPLY and SIA-REPLY are normally neighbor-specific because the inbound request
identifies the required neighbor/interface response context.

SIA-QUERY is also neighbor/destination-specific. Standard QUERY normally starts
as destination/interface-scoped work and may use reliable multicast first send.

## 12. Packet immutability and lifetime

After final route TLV encoding, authentication, checksum, and header completion,
the packet wire image is immutable.

Default ownership rule:

```text
one interface wire image -> one packet object or immutable backing buffer
```

The implementation may duplicate packet objects for independent neighbor
reliable queues or may share an immutable backing buffer with explicit
reference ownership. Either representation must guarantee:

- no post-queue wire mutation;
- no use-after-free across output/retransmit timers;
- release only after the last queue/send/retransmit holder is done;
- retransmission uses the original sequence/wire image;
- interface/neighbor teardown releases outstanding ownership safely.

This is a lifetime contract, not a requirement for one specific refcount
implementation.

## 13. Interface output queue and pacing

Each EIGRP interface owns one built-packet output queue.

Transmit pacing is an interface responsibility. The packetizer work queue does
not impose one global bandwidth gate because different interfaces have
different bandwidth, MTU, and pacing budgets.

When an interface is temporarily pacing-blocked:

- its packet remains queued;
- other interfaces may transmit independently;
- host scheduling resubmits the interface write when its budget permits.

## 14. Reliable transport

### 14.1 First send

A reliable route packet may be sent multicast on its first transmission when
protocol/interface conditions permit. The sender records every established
neighbor required to acknowledge the packet sequence.

Neighbor-targeted reliable messages are sent unicast from the first send.

### 14.2 ACK tracking

ACK state is per neighbor. An ACK for the expected sequence releases that
neighbor's outstanding ownership/state for the packet.

A packet is completely released when every required neighbor has acknowledged
or has been removed by defined neighbor-reset/drop handling.

### 14.3 Retransmission

A reliable packet first sent multicast is never retransmitted multicast.
Retransmission is unicast only to the neighbor(s) that still owe the ACK.

Do not re-encode the packet for retransmission merely because the retransmit is
unicast.

Each neighbor maintains its own SRTT, RTT variation, and RTO. RTT samples are
taken only from successfully ACKed reliable packets that have not been
retransmitted. The sample is measured from the packet's first successful wire
send to receipt of the matching ACK; HELLO traffic does not create RTT samples.

The first valid sample initializes `SRTT = R` and `RTTVAR = R/2`. Later samples
use Jacobson-style smoothing with `alpha = 1/8` and `beta = 1/4`, updating
RTTVAR from the old SRTT before updating SRTT. The EIGRP operational RTO is
`6 * SRTT`, clamped to 200 through 5000 milliseconds. Before the first valid
sample, the implementation uses a 2000 millisecond initial RTO. A timeout backs
the current RTO off, never beyond 5000 milliseconds; a later unambiguous RTT
sample replaces the backed-off value with a newly calculated RTO. The peer is
allowed 16 retransmissions of the same reliable packet; if the sixteenth retry
is also not acknowledged, the neighbor is reset with the `retry limit exceeded`
reason.

### 14.4 Conditional receive

Conditional Receive/Sequence TLV behavior is part of reliable transport, not a
packetizer shortcut. Neighbor exception sets and multicast receive conditions
must remain synchronized with the sequence being transmitted.

## 15. Packet sizing

Packet construction obeys the transmitting interface MTU/EIGRP packet limit.

Rules:

- never overrun the packet buffer or interface packet limit;
- pack multiple destination TLVs while they fit;
- start another EIGRP packet when the next complete TLV does not fit;
- never split one TLV across EIGRP packets;
- preserve sequence/reliable-transport semantics for every generated packet;
- EIGRP packet segmentation is not IP fragmentation.

Encoders must report/validate encoded length before finalizing the packet.

## 16. Packet-type ownership

### 16.1 HELLO and ACK

HELLO and ACK generation is not DUAL topology work. The Hello/reliable-transport
path may build these packets directly, while still obeying packet bounds,
authentication, checksum, queue, and lifetime rules.

### 16.2 Topology-change UPDATE

DUAL/topology-triggered UPDATE work flows through the packetizer queue. The
packetizer decides target interfaces and applies interface-sensitive suppression
and codec selection.

### 16.3 Initialization/EOT/resync UPDATE

Adjacency initialization, EOT, and graceful-resync table walks are owned by the
UPDATE/neighbor startup path because they are tied to one adjacency's handshake
and flags rather than a normal topology-change work bead.

That path may walk the topology table directly, but it must use the same native
EIGRP route data, neighbor codec, MTU/bounds, filtering/split-horizon,
authentication, sequence, and reliable-send rules as packetizer-produced UPDATEs.
It must not create a second TLV implementation.

### 16.4 QUERY

DUAL queues QUERY work when diffusing computation requires it. The packetizer
selects eligible interfaces/neighbors and applies split horizon, poison reverse,
and pending-neighbor rules.

### 16.5 REPLY / SIA-REPLY

These are neighbor-specific route work items. The packetizer owns final route
TLV encoding and packet construction for the response opcode/context.

### 16.6 SIA-QUERY

SIA-QUERY is neighbor/destination-specific and uses the same packet body
construction ownership as other route-bearing reliable messages while retaining
its opcode/timer semantics.

## 17. Module ownership

### 17.1 `eigrp_packetizer.[c|h]`

Owns packetizer work objects, queue insertion, topology-work lifetime handling,
interface/neighbor selection, and work-to-packet orchestration.

Public API includes the packetizer lifecycle/work operations required by other
protocol modules, including:

```c
void eigrp_packetizer_init(eigrp_instance_t *eigrp);
void eigrp_packetizer_finish(eigrp_instance_t *eigrp);
eigrp_packetizer_work_t *eigrp_packetizer_work_new(uint8_t opcode);
void eigrp_packetizer_work_free(eigrp_packetizer_work_t *work);
void eigrp_packetizer_enqueue(eigrp_instance_t *eigrp,
                              eigrp_packetizer_work_t *work);
```

Opcode/context drives shared packetization behavior; do not create redundant
public packetizer modules for every opcode when the construction path is the
same.

### 17.2 `eigrp_packet.[c|h]`

Owns:

- packet object/buffer lifecycle;
- fixed EIGRP header/checksum framing;
- built packet queue primitives;
- packet output scheduling;
- reliable send/retransmit mechanics;
- safe/aggregate packet-level codec helpers.

The existing packet queue remains part of packet ownership; a separate queue
module is not required merely for file symmetry.

### 17.3 `eigrp_sys.h`

Owns the portable host-runtime contract for work queue scheduling, events,
timers, sockets, interfaces, and RIB services. Host-native queue/event objects
remain in the adapter implementation.

### 17.4 `eigrp_tlv1.[c|h]` and `eigrp_tlv2.[c|h]`

Own private route-TLV wire encoding/decoding and public codec initialization or
bind operations. Private wire structs do not become packetizer public data.

### 17.5 `eigrp_update.c`

Owns UPDATE protocol semantics and adjacency initialization/EOT/resync walking.
Normal topology-change emission enters the packetizer rather than maintaining a
parallel route-TLV construction architecture.

## 18. Implementation constraints

Correctness and inspectability take priority over packetizer micro-optimization.
The design permits optimizations such as work coalescing, reduced wakeups, or
shared immutable backing buffers only when they preserve:

- object lifetime;
- interface-specific MTU/auth/pacing;
- exact reliable sequence behavior;
- encoder dispatch rules;
- split-horizon/poison/pending-neighbor decisions;
- debug visibility.

Do not add legacy aliases or parallel old/new packetization paths to stage an
optimization.

## 19. Deferred naming decision

`eigrp_prefix_descriptor_t` and `eigrp_route_descriptor_t` remain the topology
object names until the dedicated pre-production naming review in
`refactor-work.md`. Packetizer code may use DNDB/RDB terminology in comments or
debugging where useful, but this specification does not authorize rename-only
churn.
