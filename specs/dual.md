# EIGRP DUAL Design

Copyright (C) 2026 Donnie V. Savage

## 1. Purpose

This document describes how this EIGRP implementation uses the Diffusing Update
Algorithm (DUAL). It is an implementation design document, not a replacement for
RFC 7868 or the original DUAL research.

The goal is to let a contributor follow a destination through topology input,
feasibility testing, Passive/Active state transitions, QUERY/REPLY processing,
SIA handling, and return to a stable successor set without first reverse
engineering `eigrp_fsm.c`.

Protocol behavior follows RFC 7868 and the DUAL design decisions used by this
project. Source ownership and naming rules are defined by `design-spec.md`.

## 2. Topology objects

DUAL maintains destination state separately from per-neighbor path state.
Current source terminology is:

```text
prefix_descriptor
  route_descriptor
  route_descriptor
  ...
```

The prefix descriptor is the destination-level DUAL object. A route descriptor
is one path to that destination through one advertising neighbor.

Historical EIGRP terminology maps these concepts to DNDB/NDB and DRDB/RDB.
Final source/API naming is deliberately deferred in `refactor-work.md`.

A host RIB route is not a DUAL route descriptor. Route installation is a later
output of DUAL/topology selection.

## 3. Distance values

For a destination/path pair DUAL distinguishes:

- **Reported Distance (RD)**: the distance a neighbor advertises to the destination;
- **Computed Distance (CD)**: this router's distance to the destination through that neighbor;
- **Feasible Distance (FD)**: the destination-level loop-free anchor used by the Feasibility Condition;
- **current destination distance**: the selected destination metric when Passive.

The Feasibility Condition is:

```text
neighbor RD < destination FD
```

A path satisfying the condition is a feasible successor candidate. A successor
is a feasible path selected as a least-cost path for the destination.

The condition is sufficient for loop freedom. A path can be loop free without
meeting it, but DUAL will not adopt that path as a successor without the
required diffusing computation.

## 4. Passive and Active

A destination is **Passive** when DUAL has a usable least-cost loop-free path and
no coordinated recomputation is in progress.

A destination becomes **Active** when the least-cost candidate cannot be proven
loop free using the current FD. DUAL then coordinates with neighbors using
QUERY and REPLY.

Active is destination state, not neighbor state. One destination can be Active
while unrelated destinations remain Passive and continue normal processing.

## 5. The Active-state freeze invariant

This is the most important implementation invariant.

When a destination enters Active, the destination-level successor, FD, reported
distance, current distance, and advertised destination metric remain frozen
until the destination returns to Passive.

While Active, the implementation may update:

- per-neighbor/path RD and CD observations;
- route/path metrics;
- reply-status bookkeeping;
- query-origin bookkeeping;
- SIA bookkeeping;
- work needed to send QUERY, REPLY, SIA-QUERY, or SIA-REPLY.

It must not use those observations to rewrite the frozen destination values
before the Passive transition.

This is why `eigrp_topology_update_distance()` may update path/CD state before
the FSM event is selected, while the Active event handlers avoid adopting a new
destination successor or FD until the computation completes.

## 6. FSM states used by the implementation

The source retains the traditional four Active substates plus Passive:

```text
PASSIVE
ACTIVE_0
ACTIVE_1
ACTIVE_2
ACTIVE_3
```

The Active substates encode DUAL query-origin/history needed to respond
correctly when a query from the current successor or a successor distance
increase occurs during a diffusing computation. They are not separate routing
modes exposed to the operator.

The state/event table lives in `eigrp_fsm.c`. The implementation recognizes
these event classes:

```text
NQ_FCN   non-query event, Feasibility Condition not satisfied
LR       last outstanding reply
Q_FCN    query event, Feasibility Condition not satisfied
LR_FCS   last reply, Feasibility Condition satisfied
DINC     successor distance increase while Active
QACT     query from successor while Active
LR_FCN   last reply, Feasibility Condition still not satisfied
KEEP     no state transition
```

## 7. Entering Active

In Passive state the path observations are first updated and the best path is
examined against the current FD.

If the best path satisfies the Feasibility Condition, the destination remains
Passive and normal successor/topology/RIB processing can continue.

If it does not satisfy the Feasibility Condition:

- a non-query trigger enters `ACTIVE_1`;
- a query from the current successor enters `ACTIVE_3`.

Entering Active queues QUERY work when there are eligible neighbors. If there
are no neighbors left to query, the computation can complete locally.

The transition into Active does not rewrite the frozen destination state.

## 8. Reply-status set

A diffusing computation must know which neighbors still owe a REPLY. The prefix
reply-status set (`rij` in current source) records those outstanding neighbors.

Receiving a REPLY removes that neighbor from the set. A REPLY that is not the
last outstanding reply updates path information but normally leaves the FSM in
its current Active state.

The last reply is special because it gives DUAL enough information to decide
whether the destination can return to Passive or another diffusing computation
is required.

## 9. Completing a diffusing computation

When the last reply arrives, DUAL evaluates the best available path using the
FD that remained frozen during the Active interval.

If the Feasibility Condition is satisfied, DUAL can return to Passive, update
the selected destination values, refresh successor flags, update the host RIB,
and advertise the resulting topology state.

If the Feasibility Condition is still not satisfied, the destination remains
Active and begins another query round. The destination-level FD/RD/distance
values remain frozen across that continued Active computation.

Where DUAL semantics require a reply to the upstream querying successor, that
reply is sent only when the local diffusing computation has reached the point
where the answer is valid.

## 10. QUERY processing

QUERY is route-bearing DUAL input.

A query for a Passive destination may be answered immediately when local DUAL
state can provide the required response. If the query causes the destination to
lose a feasible least-cost path, it triggers the Active path described above.

A QUERY from the current successor while already Active changes the query-origin
substate. It does not unfreeze or rewrite destination FD/successor state.

QUERY packet construction is not performed by the FSM. DUAL marks required
work and packetization later selects interfaces/neighbors and builds packets as
defined by `rtp-spec.md`.

## 11. REPLY processing

REPLY updates the path information associated with the replying neighbor and
clears that neighbor from the reply-status set.

Intermediate replies do not complete the computation. The last reply causes the
FSM to evaluate whether FC is now satisfied and either return to Passive or
start another query round.

A successor distance increase observed during Active changes the DUAL Active
substate/origin bookkeeping. It does not rewrite the frozen destination values.

## 12. UPDATE processing

UPDATE may change RD/CD for one or more paths. In Passive state this can lead to
normal successor reselection and UPDATE generation when FC permits.

In Active state UPDATE information is recorded at path level, but destination
selection remains frozen. If the UPDATE represents a successor distance
increase, the FSM records the corresponding Active transition without violating
the freeze invariant.

## 13. SIA processing

SIA handling belongs to the same destination-level diffusing computation.
SIA-QUERY tests whether an outstanding neighbor is still participating in the
computation. SIA-REPLY confirms progress and prevents a healthy but long-running
computation from being treated as a dead neighbor solely because the original
ACTIVE timer has elapsed.

SIA packet semantics live in `eigrp_siaquery.c` and `eigrp_siareply.c`; timer
ownership remains with the DUAL/timer path. Packet delivery itself follows the
RTP rules in `rtp-spec.md`.

## 14. Successor and feasible-successor flags

Successor/feasible-successor flags are derived from current topology state when
DUAL is allowed to select paths. They are not a substitute for the destination
FSM state.

When a destination returns to Passive, topology code may:

1. update destination metric/FD/RD state;
2. recompute successor and feasible-successor flags;
3. select up to the configured maximum successor paths;
4. build a public RIB snapshot and install/remove routes;
5. queue required UPDATE work.

RIB installation is an output of DUAL selection, not part of the FSM decision
itself.

## 15. DUAL to packetizer boundary

DUAL produces semantic work, not wire TLVs.

```text
topology/FSM event
  -> mark QUERY/REPLY/UPDATE/SIA work
  -> packetizer queue
  -> interface/neighbor selection
  -> TLV codec
  -> immutable packet
  -> RTP
```

The packetizer revalidates current destination/path/interface/neighbor context
when work is consumed. This lets DUAL remain focused on causal routing state
rather than packet MTU, authentication, pacing, or neighbor codec details.

## 16. Concurrency and ordering

Inbound receive, DUAL work, packetization, and outbound RTP may make progress
independently, but the observable protocol ordering for a destination must
remain causal.

In particular:

- newer path observations may arrive while earlier packetizer work is queued;
- queued work represents a request to inspect current owned state, not a frozen TLV;
- an immutable reliable packet already handed to RTP is never rewritten to
  reflect later topology state;
- if later state requires another advertisement, another work item/packet is
  queued in causal order;
- a slow neighbor must not force unrelated neighbors or interfaces to stop
  making progress.

## 17. Debugging DUAL

Useful questions when tracing one destination are:

```text
What was the destination state before the event?
What packet/event triggered the FSM?
What changed in the advertising route descriptor?
Did the best path satisfy RD < FD?
Which neighbors are still in the reply-status set?
Did a query come from the current successor?
Did a successor distance increase while Active?
Was destination-level FD/successor state kept frozen?
What work was queued on the transition?
When did the destination return to Passive?
```

A DUAL bug should be debugged at the destination/path/FSM level before packet
format or host-RIB behavior is blamed.

## 18. Source ownership

```text
eigrp_topology.c   topology descriptors, path metrics, successor selection
eigrp_fsm.c        DUAL state/event transitions
eigrp_query.c      QUERY semantic send/receive work
eigrp_reply.c      REPLY semantic send/receive work
eigrp_siaquery.c   SIA-QUERY semantics
eigrp_siareply.c   SIA-REPLY semantics
eigrp_update.c     UPDATE semantics and adjacency synchronization
```

Packetization/RTP are deliberately outside this state-machine ownership and are
documented by `rtp-spec.md`.
