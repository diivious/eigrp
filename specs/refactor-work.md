# EIGRP Pre-Production Refactor Work

Copyright (C) 2026 Donnie V. Savage

## Purpose

This file holds three kinds of work.

Sections 1-3 are naming and architecture items parked until a coordinated
review. Do not use those as an excuse to rename half the tree in a feature
PR.

Section 4 is incomplete feature work. The public target exists, config is
retained, and the runtime path still returns `NOT_IMPLEMENTED`. A contributor
can pick one of those items, implement it, and send a PR.

Section 5 is packet-path and parser hardening. P0-P2 are the items worth
doing before trusting an unauthenticated LAN image. P3 and below are
low-value cleanup that can wait.

Completed work comes out of this file. Historical audit notes do not belong
here.

## 1. DUAL topology descriptor naming

The topology database has a destination-level descriptor with one or more
neighbor/path descriptors:

```text
prefix_descriptor
    route_descriptor
    route_descriptor
    ...
```

Cisco historical terminology maps these concepts to DNDB/NDB and DRDB/RDB.

Before production, choose one final source/API navigation convention after
reviewing real usage across:

```text
eigrp_topology
DUAL/FSM processing
query/update/reply/SIA processing
packetizer and TLV codecs
show/debug/dump output
southbound RIB installation
tests
```

Candidate naming families include:

```text
eigrp_topology_prefix_* / eigrp_topology_route_*
eigrp_topology_prefix_descriptor_* / eigrp_topology_route_descriptor_*
eigrp_topology_dndb_* / eigrp_topology_drdb_*
```

The decision must optimize human navigation and make it immediately clear
whether an operation acts on a DUAL destination, a DUAL path, or a host/RIB
route.

Until the review:

- preserve `prefix_descriptor` / `route_descriptor` names;
- do not perform rename-only churn;
- do not introduce new ambiguous bare `route` APIs;
- DNDB/DRDB terminology may appear in comments/debug output where it improves
  EIGRP understanding.

## 2. Runtime lifecycle namespace consolidation

Named configuration ownership is already under `eigrp_instance_*`, while some
low-level runtime allocation/destruction entry points remain under legacy names
in `eigrpd.c`, including `eigrp_get()`, `eigrp_lookup()`, and `eigrp_finish()`.

Before production, decide whether the remaining runtime lifecycle should be
consolidated into a predictable `eigrp_instance.c/.h` ownership boundary and
`eigrp_instance_*` namespace.

The review must preserve:

- named parent/address-family ownership;
- classic compatibility behavior;
- AF/VRF/AS runtime identity;
- southbound lifecycle isolation;
- teardown ordering;
- no alias-wrapper compatibility layer after an approved rename.

Do not perform a rename-only migration before that coordinated review.

## 3. Naming consistency pass

Before production, perform one bounded navigation/naming review against
`design-spec.md`:

- module/file and public symbol prefixes normally align;
- object/detail follows the module name;
- action appears last;
- configuration uses the intended `set/reset`, `add/remove`, or
  `create/delete` semantics;
- operational actions use `clear` only where appropriate;
- generic CLI/not-implemented dispatchers do not exist;
- grouped-module exceptions remain intentional;
- no obsolete alias wrappers remain after approved renames.

This review should produce a finite rename set and be committed separately from
protocol feature changes.

## 4. Incomplete feature targets

These targets keep retained configuration and return
`EIGRP_RESULT_NOT_IMPLEMENTED` when the live path is missing. That is
intentional. Do not add a generic stub dispatcher. Finish the real target.

IPv6 named config uses the same targets. IPv6 now owns a live runtime and host
packet-I/O foundation; adjacency completion remains separate protocol work.

| Target | What works today | What is still missing |
|---|---|---|
| `eigrp_auth_mode_set` HMAC-SHA-256 with a named direct password | config retained; MD5 runtime works; classic HMAC via key-chain works | named direct-password key material and receive validation |
| `eigrp_offset_add` / `eigrp_offset_remove` | config retained | apply offset into metric processing |
| `eigrp_instance_parent_shutdown_set` / `_reset` | config retained | parent-wide runtime shutdown semantics |
| `eigrp_instance_distance_set` / `_reset` | config retained | RIB administrative-distance application |
| `eigrp_interface_bandwidth_percent_set` / `_reset` | config retained | live pacing application |
| `eigrp_interface_next_hop_self_set` / `_reset` | config retained | live packet-path application |
| `eigrp_metric_traffic_share_balanced_set` / `_reset` | config retained | forwarding/runtime application |
| `eigrp_summary_create` / `_delete` | config retained | advertise/withdraw manual summaries |
| `eigrp_summary_auto_set` / `_reset` | config retained | auto-summary runtime |
| `eigrp_summary_metric_set` / `_reset` | config retained | use the override when originating a summary |
| `eigrp_neighbor_maximum_prefix_set` / `_reset` / `_all_*` | config retained | enforce the prefix limit on a neighbor |
| `eigrp_topology_create` / `_delete` | config/identity retained | non-base topology runtime |
| `eigrp_topology_default_information_set` / `_reset` | config retained | originate/accept default by that policy |
| `eigrp_topology_maximum_prefix_set` / `_reset` | config retained | enforce the topology prefix limit |
| `eigrp_rib_redistribute_add` / FRR `eigrp_zebra_redistribute_update` | Zebra subscribe happens | install source routes into topology and apply route-map |
| IPv6 adjacency | live IPv6 runtime, proto-88 socket, multicast, packet send/receive | AF-correct HELLO/adjacency bring-up and end-to-end neighbor validation |
| `eigrp_init` / `eigrp_terminate` | FRR main calls them from `eigrpd.h` | move process init into the public `eigrp.h` contract |

Rules for Section 4 work:

- Keep the existing target. Do not invent a parallel API.
- Keep valid retained config even while you fill in runtime.
- Add or extend a test that fails if the path goes back to `NOT_IMPLEMENTED`.
- IPv6 adjacency must continue through the shared runtime/packet model; do not invent a second integration model for it.
- Stub routing stays out of scope. Do not add it here.

Show/state walkers return `NOT_IMPLEMENTED` when `data_path_ready` is false.
That remains a generic capability gate, not an IPv6-specific show API.

## 5. Packet-path and parser hardening

This is an on-link protocol. Anyone on the LAN can send proto 88. Auth-off
is the common case today. Work P0-P2 first.

A PR should add a crafted-packet test for the hole it closes. Do not "harden"
by rewriting the codec.

### P0. Memory safety on the wire

#### 5.1 Hello TLV body read without a type-specific min length

`eigrp_hello_receive()` only checks `length >= 4` and `length <= remaining`,
then casts to `TLV_Parameter_Type`, `TLV_Software_Type`, or
`TLV_Peer_Termination_type`.

Real sizes:

```text
EIGRP_TLV_PARAMETER_LEN          12
EIGRP_TLV_SW_VERSION_LEN          8
EIGRP_TLV_PEER_TERMINATION_LEN    9
```

A Hello with Parameter type and length 4 on a 4-byte remaining packet reads
K-values and `hold_time` past the packet. Same pattern for a short Software
or Peer-term TLV.

Fix: require the real min length before those decoders touch fields. Drop or
skip otherwise. Test: crafted Parameter TLV with length 4.

#### 5.2 `eigrp_packet_read()` trusts `meta.eigrp_length` more than the stream

After skipping `network_header_length`, receive uses `meta.eigrp_length` for
checksum, auth walk, and Hello `size`. It does not clamp to `endp - getp`.

FRR southbound currently sets those equal. A new shim, or a later socket
change, that lies about `eigrp_length` over-reads `ibuf`.

Fix: after the IP skip, set length to `min(meta.eigrp_length, remaining)`
and drop if that is `< EIGRP_HEADER_LEN`. Do not parse a declared length
bigger than bytes actually in the stream.

#### 5.3 Hello creates the neighbor before the TLV walk succeeds

`eigrp_nbr_create()` runs, then a short or malformed TLV returns. On an
unauthenticated interface that fills the neighbor table and pairs with 5.1.

Fix: parse TLVs first. Create or update the neighbor only after a Parameter
TLV is present and well-sized. If you keep the current order, tear down a
half-created neighbor on parse fail.

### P1. Auth, spoof, and parser fail-open

#### 5.4 SHA-256 receive is not implemented. SHA-256 send hashes with `strlen(packet)`

Receive rejects SHA-256. Keep it that way until send is fixed.

`eigrp_make_sha256_digest()` does `eigrp_hmac_sha256_update(..., ibuf, strlen(ibuf))`.
`ibuf` is raw packet bytes. First `0x00` stops the hash. That is not HMAC
over the packet.

Fix: hash `s->endp` bytes and match the digest layout used on the wire.
Do not enable SHA-256 receive until that is done.

#### 5.5 MD5 replay window is weak

`eigrp_check_md5_digest()` drops only when `key_sequence < stored`. Equal
sequence is accepted. Last packet can be replayed.

Fix: require strictly greater sequence. Define wrap behavior.

#### 5.6 No auth means full adjacency control from the LAN

Expected for classic EIGRP. Items 5.1, 5.3, 5.10, and 5.11 are reachable
by anyone on the link when auth is off. Treat auth-off as hostile-LAN.

#### 5.7 Stream getters fail open

`eigrp_stream_getc` / `getw` / `getl` return 0 on short read and do not tell
the caller. TLV1/TLV2 mostly check remaining first. Hello and some dumps
do not use that API. One missed check becomes a valid-looking zero field.

Fix: getters should fail visibly, or every decode path uses a `stream_has()`
check the way IPv4 prefix decode does.

#### 5.8 `pktlen` is ignored in TLV1 and TLV2

`(void)pktlen`. Decode bound is whatever `endp` is. That is only safe if
`packet_read` clamps the stream to the EIGRP payload. It does not today.
See 5.2.

#### 5.9 Hello pointer walk and hold time 0

Hello advances with `tlv_header += length`. Keep the `length <= remaining`
check. The missing piece is the min-size check in 5.1.

Hello copies `hold_time` with no floor. `v_holddown * 1000` becomes a 0 ms
timer. Neighbor either never dies or flaps immediately.

Fix: reject 0 or clamp to the protocol minimum.

### P2. Resource and loop abuse

#### 5.10 No cap on neighbors or topology entries

Each accepted Hello can `calloc` a neighbor. Each route TLV can `calloc` a
descriptor. Prefix-limit config still returns `NOT_IMPLEMENTED`. On-link
flood is memory and CPU.

Fix: enforce max-neighbors and max-prefix on the receive path, not just
retained config. That is the same work as Section 4 max-prefix items.

#### 5.11 Unknown Hello TLVs are skipped after the neighbor exists

Combined with 5.3, junk Hellos still allocate.

#### 5.12 Update/Query/Reply decoder loop

The loop keeps calling the decoder while `endp > getp`. TLV1/TLV2 abort to
`endp` on unknown type, so they advance. A future codec that returns NULL
without moving `getp` hangs the read callback.

Fix: if decoder returns NULL and `getp` did not move, drop the packet.

#### 5.13 `eigrp_print_addr()` is `inet_ntoa` of IPv4 only

Static buffer, IPv6-wrong. Two prints in one log can alias. Not an
overflow. Logging lie.

### P3. Low value. Already in decent shape. Pick up later if you want.

These are not holes to burn a release on. They can still be cleaned up.

- IPv4/IPv6 prefix decode already caps bitlen and checks remaining before
  `stream_get`. Keep that pattern. Add tests so it cannot regress.
- FRR `recvmsg` already requires `ip_v == 4`, sane `ip_hl`, and
  `ip_len == recv size`. Keep a portable contract test so a new shim cannot
  drop those checks.
- Auth TLV framing walk already rejects `tlv_length < 4` or
  `tlv_length > remaining`. Keep it. Do not rewrite it.
- Debug dump prefix printer already checks length before copy.
- `eigrp_stream_get()` already caps the copy to available bytes. The
  problem is the untyped `getc`/`getw`/`getl` wrappers in 5.7, not `get()`.
- TLV1/TLV2 `tlv_start + length` is safe today because `length <= remaining`
  and `size_t` is wide. Do not churn it. If you copy that walk, keep the
  check.
- `eigrp_header.tlv[0]` is typed as `char *` and used as bytes. Ugly, not
  an overflow by itself. Cleanup only.

### P4. Later hygiene. Not security-critical.

- Replace `inet_ntoa` in `eigrp_print_addr()` with a caller-supplied buffer
  that handles IPv4 and IPv6.
- Stop ignoring `pktlen` once 5.2 clamps the stream. Pass one bound and use
  it.
- Document that SHA-256 config may be retained while receive stays
  `NOT_IMPLEMENTED`. That is already a Section 4 auth item.
