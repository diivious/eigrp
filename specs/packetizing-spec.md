# EIGRP Packetizing Specification

Copyright (C) 2026 Donnie V. Savage

This file is a new project design document. New files created for this
EIGRP work use Donnie V. Savage as the copyright owner unless stated
otherwise.

Existing source files must preserve all prior copyright notices, SPDX
identifiers, and author history. Refactoring an existing file is not
permission to remove earlier authorship.

## 1. Purpose

This document defines the first-pass packetizing model for the `eigrpd` project.

It owns the handoff between DUAL/topology work and packet transmission:

```text
DUAL/FSM/topology event
  -> packetizer work queue
  -> packetizer
  -> per-interface packet queue
  -> interface transmit pacing
  -> reliable transport / ACK handling
```

This document extends `specs/design-spec.md`.

## 2. Authority

Packetizing authority follows the project authority order defined in `design-spec.md`:

1. Donnie V. Savage.
2. RFC 7868 for protocol behavior unless intentionally clarified or superseded by Donnie V. Savage.
3. FRR only for build compatibility, daemon integration, library/API compatibility, and event-loop integration.
4. Existing `eigrpd` code only where it does not conflict with this specification.

The Cisco internal term `threading` is not used as a new code module name because it conflicts with OS threads and FRR `struct event *thread` naming.

## 3. RFC Conflict Review

No protocol conflict is intended by this specification.

This document describes implementation machinery that RFC 7868 does not specify directly:

- the DUAL work queue
- NDB/RDB work beads
- packetizer work queue scheduling
- southbound host work queue adaptation
- packet object ownership/refcounting
- internal queue names

These are implementation details as long as the emitted packets and reliable transport behavior follow RFC 7868.

RFC-sensitive rules captured here:

- UPDATE, QUERY, REPLY, SIA-QUERY, and SIA-REPLY carry per-destination routing information and are processed individually by DUAL.
- Multiple destinations may be packed into one or more packets.
- Reliable packets may be multicast on first send, but retransmission is unicast to neighbors that have not acknowledged.
- Reliable transport ACK state is per neighbor.
- Interface pacing limits EIGRP bandwidth use per interface.
- Split horizon, poison reverse, pending-neighbor exclusion, and conditional receive behavior are interface/neighbor sensitive.
- Classic TLV and multiprotocol/wide TLV support may require TLV1, TLV2, or both on the same interface depending on peer capability.

If any future implementation chooses behavior different from these rules, the difference must be documented as an intentional protocol clarification or reviewed as a possible design mistake.

## 4. Terminology

### 4.1 Packetizer Work Item

A packetizer work item is an internal marker that tells the packetizer an EIGRP topology object needs to be advertised, queried, replied, withdrawn, poisoned, or otherwise emitted.

A work item is not a packet.

A work item must not contain prebuilt wire TLVs.

Preferred code name:

```text
eigrp_packetizer_work
```

### 4.2 NDB Work

An NDB work item represents destination-level work.

The interface is not known at enqueue time.

When the packetizer processes NDB work, it walks eligible EIGRP interfaces and builds interface-specific packets.

### 4.3 RDB Work

An RDB work item represents route-descriptor-level work.

The interface or neighbor context is known from the route descriptor.

When the packetizer processes RDB work, it builds only for the relevant interface/neighbor context unless the packetizing rule explicitly requires broader emission.

### 4.4 Packet

An `eigrp_packet` is a built wire packet buffer.

Once built, the packet is immutable and locked to one interface.

A built packet must not be modified after it is queued for transmission.

### 4.5 Interface Packet Queue

An interface packet queue is the per-interface transmit queue.

It holds built packet references waiting for interface pacing and send.

Preferred code name:

```text
eigrp_packet_queue
```

There is one `eigrp_packet_queue` per EIGRP interface.

## 5. Queue Model

The design uses two real queues.

```text
eigrp_packetizer_queue
  global DUAL work queue
  holds NDB/RDB work items
  no wire packet yet
  no TLV1/TLV2 decision yet
  no interface pacing yet

eigrp_packet_queue
  one per EIGRP interface
  holds built packet refs
  packet is already locked to that interface
  enforces transmit pacing
```

Reliable ACK state is per neighbor, but it does not need to be a third general queue.

Neighbor ACK state may hold packet references until the neighbor ACKs, resets, times out, or otherwise releases the packet.

## 6. Packetizer Queue Rules

### 6.1 Ownership

DUAL, topology, and route-management code enqueue work into `eigrp_packetizer_queue`.

Packet send functions should not independently build route TLVs for topology changes.

Preferred model:

```text
DUAL/topology marks work.
Packetizer chooses interface/neighbor destination. The selected encoder vector owns the route TLV wire format.
```

### 6.2 No Early Interface Decision for NDB

NDB work must not decide TLV1/TLV2/both at enqueue time.

The selected encoder is interface-sensitive and is maintained on the interface by neighbor bind/unbind events. The packetizer must use the selected vector instead of recalculating TLV format from the neighbor list.

### 6.3 RDB Interface Context

RDB work may carry known interface or neighbor context.

The packetizer must still validate the context is current before building a packet.

If the interface or neighbor no longer exists, the work item is discarded safely.

### 6.4 Coalescing and Rethreading

A topology object may change while an existing work item for that object is still queued.

If the queued work item has not yet been pulled by the packetizer:

```text
update/coalesce the existing work item
move it to the desired queue position if needed
```

If the packetizer has already pulled the work item:

```text
create a new work item for the newer object state
enqueue it at the head/high-priority side when required
```

The object state at packetization time wins.

Queued work should be treated as a request to inspect current object state, not as a frozen copy of old state.

### 6.5 Southbound Work Queue Scheduling

Adding work to the packetizer queue schedules the packetizer through an EIGRP-owned work queue abstraction.

Core packetizer code must not call FRR `work_queue` APIs directly. It calls:

```c
eigrp_work_queue_enqueue(eigrp_work_queue_t *queue, void *data);
```

The FRR build implements that API in `eigrp_southbound.[c|h]` using FRR `work_queue`. A future BSD build replaces the southbound implementation without changing packetizer, DUAL, topology, or TLV code. `eigrp_southbound` is the runtime adapter; CLI/VTY/northbound management code must not reach around this boundary to call FRR work-queue/event APIs on behalf of portable packetizer logic.

FRR Zebra/RIB integration remains in `eigrp_zebra.[c|h]` as a specialized southbound adapter. Packetizer and other portable core modules operate on EIGRP-owned route/interface/neighbor representations and do not accept Zebra-native objects.

There is no public packetizer `wakeup` API. Scheduling is an internal side effect of enqueueing work.

## 7. Packetizer Processing Rules

### 7.1 Runtime Encoder/Decoder Rule

Packetizer and receive-path code must not branch on TLV version.

The selected function vector is the branch.

```c
ei->encoder(eigrp, ei, NULL, packet, route);
nbr->encoder(eigrp, ei, nbr, packet, route);
nbr->decoder(eigrp, nbr, packet, packet_len);
```

Runtime paths:

```text
interface-wide / multicast route packet
  -> ei->encoder(...)

neighbor-targeted first-build unicast route packet
  -> nbr->encoder(...)

receive path
  -> nbr->decoder(...)
```

A multicast packet that is later retransmitted as unicast must not be re-encoded with `nbr->encoder`. Retransmission resends the already-built immutable packet only to the neighbor(s) that have not acknowledged it.

### 7.2 Native Data Boundary

Packetizer work items carry native EIGRP data, not prebuilt TLVs.

The encoder receives native topology objects such as prefix/route descriptors and writes the correct wire TLV representation into the packet buffer.

The decoder receives packet bytes and emits native EIGRP data, such as prefix/route descriptors.

TLV1 and TLV2 wire structures must not leak into packetizer, topology, DUAL, CLI, or reliable transport code.

### 7.3 NDB Processing

For NDB work:

```text
for each EIGRP interface:
  validate interface is active/eligible
  apply split horizon / poison reverse / pending-neighbor rules
  call ei->encoder(...)
  queue packet only if encoder produced packet content
```

The packetizer does not decide `tlv1`, `tlv2`, `both`, or `none` with local `if/else` logic. That decision is represented by the interface encoder vector.

One NDB work item may produce zero, one, or many interface-specific packets.

### 7.4 RDB Processing

For RDB work:

```text
validate route descriptor still exists
validate interface/neighbor context still exists
apply split horizon / poison reverse / pending-neighbor rules
if neighbor-targeted:
  call nbr->encoder(...)
else:
  call ei->encoder(...)
queue packet only if encoder produced packet content
```

An RDB work item normally produces packet output for one interface or one neighbor.

### 7.5 Encoder/Decoder Ownership

Packet code owns safe and aggregate packet-level vectors:

```c
eigrp_packet_encoder_safe()
eigrp_packet_decoder_safe()
eigrp_packet_encoder_both()
```

`eigrp_packet_encoder_safe()` writes no route TLVs and returns safely.

`eigrp_packet_decoder_safe()` emits no native route data and advances the packet cursor to a safe stop point so receive loops cannot spin on undecodable data.

`eigrp_packet_encoder_both()` is used by mixed interfaces. It calls the instance TLV codec vectors for TLV1 and TLV2. It must not require TLV1/TLV2 private wire types to leak outside their modules.

The EIGRP instance owns the local/self TLV codecs used for bind operations and mixed-interface encoding:

```c
typedef struct eigrp_tlv_codec {
	eigrp_packet_encoder_t encoder;
	eigrp_packet_decoder_t decoder;
} eigrp_tlv_codec_t;

struct eigrp {
	eigrp_tlv_codec_t tlv1_codec;
	eigrp_tlv_codec_t tlv2_codec;
};
```

Instance initialization installs the codec vectors once:

```c
eigrp_tlv1_init(&eigrp->tlv1_codec);
eigrp_tlv2_init(&eigrp->tlv2_codec);
```

`eigrp->tlv1_codec` and `eigrp->tlv2_codec` are not neighbor state. They are the local codec vectors used to bind neighbors/interfaces and to encode both TLV families for mixed interfaces.

TLV1 code owns the real TLV1 wire implementation:

```text
eigrp_tlv1.c / eigrp_tlv1.h
  private TLV1 encoder
  private TLV1 decoder
  public TLV1 init/bind APIs
```

TLV2 code owns the real TLV2 wire implementation:

```text
eigrp_tlv2.c / eigrp_tlv2.h
  private TLV2 encoder
  private TLV2 decoder
  public TLV2 init/bind APIs
```

The public TLV init APIs install private TLV encode/decode functions into `eigrp_tlv_codec_t`. The public bind APIs copy the selected codec vectors into neighbor/interface dispatch slots. Callers do not call private TLV encode/decode functions directly.

### 7.6 Neighbor Codec State

Code and CLI should use `neighbor`, not `peer`, to match the existing codebase.

A neighbor has exactly one negotiated TLV version. A neighbor is never mixed.

Required neighbor state:

```c
uint8_t tlv_version;
eigrp_packet_encoder_t encoder;
eigrp_packet_decoder_t decoder;
```

Neighbor lifecycle:

```text
neighbor create
  tlv_version = EIGRP_TLV_VERSION_NONE
  encoder = eigrp_packet_encoder_safe
  decoder = eigrp_packet_decoder_safe

neighbor up / TLV version detected
  tlv_version = EIGRP_TLV_VERSION_1 or EIGRP_TLV_VERSION_2
  bind neighbor encoder/decoder through eigrp_tlv1_* or eigrp_tlv2_* public API

neighbor down/free
  unbind the interface using nbr->tlv_version before clearing/freeing the neighbor
  free/reset neighbor state
```

There is no separate `bound_interface_codec` field. The interface bind contributed by a neighbor always matches `nbr->tlv_version`. Teardown order must preserve `nbr->tlv_version` until after interface unbind completes.

### 7.7 Interface Aggregate Encoder State

An interface owns the aggregate outbound route encoder for all fully up neighbors on that interface.

Required interface state:

```c
uint16_t tlv1_peer_count;
uint16_t tlv2_peer_count;
eigrp_packet_encoder_t encoder;
```

The peer counts count only neighbors that are fully up and eligible for route packet encoding. Pending/discovery neighbors do not count.

Interface lifecycle:

```text
interface up/create
  tlv1_peer_count = 0
  tlv2_peer_count = 0
  encoder = eigrp_packet_encoder_safe

neighbor up on interface
  eigrp_interface_encoder_bind(ei, nbr->tlv_version)

neighbor down on interface
  eigrp_interface_encoder_unbind(ei, nbr->tlv_version)

interface down/free
  clear interface encoder state unconditionally
```

Interface bind/unbind primitives own count changes and aggregate encoder selection.

Conceptual bind result:

```text
no counted neighbors
  encoder = eigrp_packet_encoder_safe

only TLV1 counted neighbors
  encoder = TLV1 interface encoder

only TLV2 counted neighbors
  encoder = TLV2 interface encoder

both TLV1 and TLV2 counted neighbors
  encoder = eigrp_packet_encoder_both
```

The runtime packetizer path remains a single vector call:

```c
ei->encoder(eigrp, ei, NULL, packet, route);
```

Do not replace this with encoder arrays, linked encoder lists, or packetizer-side TLV branching.

### 7.8 CLI Debug State Rule

CLI show commands read the owning structure. They must not recompute codec state on the fly.

```text
show interface
  reads interface-owned TLV peer counts and aggregate encoder state

show neighbor
  reads neighbor-owned tlv_version, encoder, and decoder state
```

This makes CLI output useful for catching bind/unbind count drift instead of hiding it by recomputing from neighbor walks.

### 7.9 Packet Immutability

After the packetizer builds an `eigrp_packet`, the packet is immutable.

The packet is locked to one interface.

Do not queue the same built packet object to multiple interfaces unless the implementation explicitly proves the wire image, auth, pacing, MTU, and ACK target set are identical.

Default rule:

```text
one interface packet = one built packet object
```

## 8. Interface Packet Queue Rules

### 8.1 Per-Interface Queue

Each EIGRP interface owns one `eigrp_packet_queue` for outbound packets.

The queue holds packet references.

The queue does not own topology state.

### 8.2 Pacing

Transmit pacing is an interface packet queue responsibility.

The packetizer work queue must not perform global bandwidth gating.

Reason:

- RFC pacing is interface-sensitive.
- NDB work may build packets for multiple interfaces with different bandwidth and pacing budgets.
- RDB work may target a specific interface that has independent pacing state.

### 8.3 Pacing Result

If an interface is over its allowed transmit rate, the packet remains queued for that interface.

Other interfaces may still transmit their own packets if their pacing budget allows.

A blocked interface queue must reschedule transmission when budget is expected to be available.

## 9. Reliable Transport Rules

### 9.1 First Send

A reliable packet may be sent multicast on first transmission when appropriate for the interface and packet type.

The sender records the neighbor ACK set for all peers that must acknowledge the packet.

### 9.2 ACK Tracking

ACK tracking is per neighbor.

When a neighbor ACKs the packet sequence number, that neighbor is removed from the packet's pending ACK set.

When all required neighbors have ACKed or been removed due to reset/drop policy, the reliable hold on the packet is released.

### 9.3 Retransmission

A reliable multicast packet must not be multicast again for retransmission.

Retransmission is unicast only to neighbors that have not ACKed.

Reason:

- peers that already processed the reliable packet must not be forced to process it again
- duplicate QUERY/UPDATE delivery can cause unnecessary DUAL churn
- RFC reliable transport describes multicast first send and unicast retransmission to missing peers

### 9.4 Packet References

A packet may be held by:

```text
interface packet queue
send-in-progress state
neighbor ACK state
retransmit timer/state
```

The packet is freed only after the last holder releases it.

A future implementation may use refcounting or an equivalent explicit ownership model.

## 10. Packet Type Behavior

### 10.1 Hello and ACK

Hello and ACK packet generation are not DUAL work items.

They may continue to use packet send paths directly as long as packet safety, checksum/auth, and queue ownership rules are honored.

### 10.2 Update

UPDATE packet generation for topology changes should flow through the packetizer queue.

Startup full-table UPDATEs may use the same packetizer machinery or a dedicated startup walker, but the final packets must still be interface-specific and pacing-aware.

### 10.3 Query

QUERY packet generation should flow through the packetizer queue.

DUAL marks the destination as needing QUERY work.

The packetizer decides which interfaces and neighbors receive the QUERY and applies split horizon and pending-neighbor rules.

### 10.4 Reply and SIA-Reply

REPLY and SIA-REPLY use the same packetizer path. The work item carries the desired opcode.

The inbound QUERY/SIA-QUERY identifies the neighbor/interface context, so these are neighbor-specific route work items. The packetizer owns final TLV encoding and packet construction.

### 10.5 Query and SIA-Query

QUERY and SIA-QUERY use the same packetizer path where the packet body is concerned. The work item carries the desired opcode.

Standard QUERY is normally destination/interface scoped and may multicast on first send. SIA-QUERY is neighbor/destination specific. The packetizer uses the opcode and neighbor/interface context to choose the final transmission behavior.

## 11. Split Horizon, Poison Reverse, and Pending Neighbors

Packetizer owns outbound suppression/poison decisions because they are interface/neighbor sensitive.

Rules:

- do not send QUERY/UPDATE to pending neighbors that are not eligible for convergence
- apply split horizon based on the inbound/successor interface context
- use poison reverse where required instead of suppression
- re-evaluate these rules at packetization time

## 12. Packet Sizing

Packetizer owns packet size management.

Rules:

- do not exceed interface MTU
- split multiple destinations across multiple packets when needed
- do not split one TLV across packets
- preserve reliable transport sequencing rules
- packet fragments are EIGRP packets, not IP fragments

## 13. Proposed Modules

### 13.1 `eigrp_packetizer.[c|h]`

Owns packetizer work items and work-to-packet orchestration.

Public API:

```c
void eigrp_packetizer_init(eigrp_instance_t *eigrp);
void eigrp_packetizer_finish(eigrp_instance_t *eigrp);
eigrp_packetizer_work_t *eigrp_packetizer_work_new(uint8_t opcode);
void eigrp_packetizer_work_free(eigrp_packetizer_work_t *work);
void eigrp_packetizer_enqueue(eigrp_instance_t *eigrp,
                              eigrp_packetizer_work_t *work);
```

The packetizer work item is intentionally opcode-driven instead of subtype-function-driven:

```c
typedef struct eigrp_packetizer_work {
    uint8_t opcode;
    eigrp_prefix_descriptor_t *prefix;
    eigrp_route_descriptor_t *route;
    eigrp_interface_t *exception;
    eigrp_neighbor_t *nbr;
    void *owner;
    uint32_t flags;
} eigrp_packetizer_work_t;
```

`QUERY` and `SIA-QUERY` share packetizing behavior and differ by opcode/context. `REPLY` and `SIA-REPLY` share packetizing behavior and differ by opcode/context.

### 13.2 `eigrp_southbound.[c|h]`

Owns host runtime adaptation.

Initial FRR-backed work queue API:

```c
eigrp_work_queue_t *eigrp_work_queue_new(eigrp_instance_t *eigrp,
                                         const char *name,
                                         eigrp_work_queue_func_t workfunc,
                                         eigrp_work_queue_delete_func_t deletefunc);
void eigrp_work_queue_free(eigrp_work_queue_t *queue);
void eigrp_work_queue_reset(eigrp_work_queue_t *queue);
void eigrp_work_queue_enqueue(eigrp_work_queue_t *queue, void *data);
eigrp_instance_t *eigrp_work_queue_eigrp(eigrp_work_queue_t *queue);
```

FRR-specific details such as `struct work_queue`, `work_queue_add()`, `work_queue_new()`, and event-loop scheduling live here only.

### 13.3 `eigrp_packet_queue.[c|h]`

Owns built packet queues.

The current built packet queue remains in `eigrp_packet.[c|h]` for this pass. It can be moved into `eigrp_packet_queue.[c|h]` later when that refactor can be done without mixing it into the packetizer work queue change.

Candidate future API:

```c
eigrp_packet_queue_t *eigrp_packet_queue_create(void);
void eigrp_packet_queue_delete(eigrp_packet_queue_t *queue);
void eigrp_packet_queue_enqueue(eigrp_packet_queue_t *queue, eigrp_packet_t *packet);
eigrp_packet_t *eigrp_packet_queue_peek(eigrp_packet_queue_t *queue);
eigrp_packet_t *eigrp_packet_queue_dequeue(eigrp_packet_queue_t *queue);
bool eigrp_packet_queue_empty(eigrp_packet_queue_t *queue);
```

### 13.4 `eigrp_packet.[c|h]` Codec Vectors

Owns packet-level safe and aggregate encoder/decoder helpers:

```c
int eigrp_packet_encoder_safe(...);
int eigrp_packet_decoder_safe(...);
int eigrp_packet_encoder_both(...);
```

`eigrp_packet_encoder_both()` is the mixed-interface aggregate encoder. It calls `eigrp->tlv1_codec.encoder` and `eigrp->tlv2_codec.encoder` against native route data and the same packet buffer.

### 13.5 `eigrp_tlv1.[c|h]` and `eigrp_tlv2.[c|h]`

Own the private TLV-specific wire encode/decode functions and expose public init/bind APIs.

Required API shape is conceptual; exact signatures may be adjusted to existing type names:

```c
void eigrp_tlv1_init(eigrp_tlv_codec_t *codec);
void eigrp_tlv1_neighbor_bind(eigrp_neighbor_t *nbr, eigrp_tlv_codec_t *codec);
void eigrp_tlv1_interface_bind(eigrp_interface_t *ei, eigrp_tlv_codec_t *codec);

void eigrp_tlv2_init(eigrp_tlv_codec_t *codec);
void eigrp_tlv2_neighbor_bind(eigrp_neighbor_t *nbr, eigrp_tlv_codec_t *codec);
void eigrp_tlv2_interface_bind(eigrp_interface_t *ei, eigrp_tlv_codec_t *codec);
```

TLV init installs private encode/decode functions into the instance codec table.

Neighbor bind sets `nbr->tlv_version`, `nbr->encoder`, and `nbr->decoder` from the selected instance codec.

Interface bind installs the TLV-specific encoder vector from the selected instance codec for the TLV1-only or TLV2-only aggregate interface state.

## 14. Implementation Notes

Initial implementation should favor correctness over optimization.

Acceptable first pass:

```text
one work item processed at a time
one built packet per interface
FRR watched/work queue hidden behind `eigrp_southbound`
clear debug dumps
hard validation before queueing packet refs
```

Optimizations can come later:

```text
coalescing many NDBs into fewer packets
interface encoder selection shortcuts if profiling proves a need
shared immutable packet buffers where provably safe
reduced wakeups
packet packing by opcode/type/capability
```

Do not add legacy aliases or parallel old/new packet paths.

## 15. Open Questions

These should be answered before deep code work:

1. Current code still uses `eigrp_prefix_descriptor_t` and `eigrp_route_descriptor_t` for topology prefix/route records. Their lifecycle APIs are owned by topology and named `eigrp_topology_prefix_create/free()` and `eigrp_topology_route_create/free()`. A later typedef rename to NDB/RDB terminology can be reviewed separately.
2. Where should packet refcount ownership live: inside `eigrp_packet_t`, or in a small wrapper object owned by reliable transport?
3. Should startup full-table UPDATEs use the same packetizer queue from day one, or be migrated after topology-change packetizing is stable?
