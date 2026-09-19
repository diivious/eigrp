# Post Command Audit Item 6 Closure

Copyright (C) 2026 Donnie V. Savage

## Scope

Post Command Audit item 6 finishes the IPv4 named-mode EXEC state backend used by
these operational views:

```text
show eigrp address-family ipv4 ... interfaces [detail]
show eigrp address-family ipv4 ... neighbors [detail]
show eigrp address-family ipv4 ... accounting
show eigrp address-family ipv4 ... timers
show eigrp address-family ipv4 ... traffic
```

Topology and event-history views already terminate at their feature-owned state
targets and remain on those paths.  This pass does not expand classic EXEC
commands; `cli-spec.md` keeps classic operational commands preserved rather than
expanded.

The closure rule is the same as the rest of the command audit: display runtime
state that the implementation actually owns, do not manufacture Cisco-only
values, and keep an unavailable runtime feature at its real EIGRP target or mark
the individual field unavailable rather than replacing the entire command with a
generic stub.

## IPv4 traffic counters

Packet traffic accounting is now owned by the common packet I/O path instead of
being scattered across opcode handlers and packetizer call sites.

On successful transmit, and only after a real host send succeeds, the interface
records:

- Hello and standalone ACK packets;
- Update, Query, Reply, SIA-Query, and SIA-Reply packets;
- reliable/unreliable unicast and multicast transmissions;
- conditional-receive packets;
- retransmissions; and
- multicast-exception retransmissions.

On receive, opcode counters are recorded only after the common EIGRP header and
authentication checks have accepted the packet.  A HELLO opcode with sequence 0
and a nonzero ACK field is counted as an ACK, matching the RFC packet encoding,
rather than being double-counted as a Hello.

`show ... traffic` therefore reports real sent/received values for all seven
packet categories instead of `n/a` placeholders.

## Interface detail

The IPv4 interface state target now exports the live hello timer expiration and
the transport counters maintained by packet I/O.  `interfaces detail` reports:

- configured/runtime hello and hold values;
- split-horizon state;
- next scheduled Hello expiration;
- unreliable/reliable multicast counts;
- unreliable/reliable unicast counts;
- multicast exceptions;
- conditional-receive packets; and
- retransmissions sent.

Mean SRTT, packet pacing/flow timers, ACK suppression, and out-of-sequence receive
counters remain `n/a`.  The current reliable-transport implementation does not
maintain those algorithms/counters, so the EXEC layer must not synthesize them.
Interface bandwidth-percent pacing remains an item-5 `NOT_IMPLEMENTED` runtime
boundary.

## Neighbor detail

The neighbor state target now reports runtime values rather than configured or
constant placeholders where runtime data exists:

- hold time is the remaining live hold timer;
- uptime is elapsed monotonic time since the adjacency entered UP;
- RTO reflects the currently implemented fixed retransmission interval in
  milliseconds;
- Q count remains the neighbor reliable queue depth;
- retransmissions are cumulative successful retransmission sends;
- retries are the retry count of the current reliable packet; and
- prefix count is derived from topology route descriptors advertised by that
  neighbor.

SRTT remains `n/a` because the current transport has no SRTT estimator or adaptive
RTO.  Graceful-restart elapsed state is not invented from adjacency uptime.

## Timers and accounting

`show ... timers` now walks actual scheduled IPv4 runtime timers:

- interface Hello timers; and
- neighbor hold timers.

SIA/ACTIVE expiration remains unavailable because ACTIVE-time runtime enforcement
is an explicit item-5 `NOT_IMPLEMENTED` boundary.

Accounting continues to report the real topology prefix total and real
per-neighbor advertised-prefix count.  Restart count and restart/reset countdown
remain `n/a` until neighbor maximum-prefix enforcement exists; item 5 explicitly
leaves that runtime behavior unimplemented.

## `multicast` grammar decision

The optional `multicast` selector **stays in the named EXEC grammar**.

It means the EIGRP Multicast Address Family (MAF), identified by RFC 7868 VRID
`0x0001`.  It does not mean ordinary EIGRP packet multicast transport.  The
project currently has no MAF configuration/runtime model, so every parsed
`multicast` operational request is carried in `eigrp_state_request_t` to the
portable instance state target and terminates with
`EIGRP_RESULT_NOT_IMPLEMENTED`.

This preserves the documented command surface without falsely showing unicast
state under a multicast-family selector and without making FRR CLI parsing the
feature boundary.

## Regression guards

`test/portable/cli/eigrp_ipv4_exec_backend/` guards:

- centralized validated packet counter ownership and ACK classification;
- transport/interface detail counter wiring;
- live neighbor hold/uptime/retry/prefix state;
- real timer-expiration walking;
- accounting and traffic state ownership; and
- retained `multicast` grammar routed to the portable MAF `NOT_IMPLEMENTED`
  target.
