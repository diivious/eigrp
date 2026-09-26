# EIGRP Route Selection and Internal/External Topology Storage

## 1. Scope

This specification defines how the portable EIGRP topology database stores
internal and external path descriptors and how DUAL obtains the best usable
path. It does not change the DUAL FSM, FD ownership, Active/Passive rules, or
QUERY/REPLY behavior defined in `dual.md`.

## 2. Topology storage

A destination `prefix_descriptor` owns two path queues:

```text
prefix_descriptor
    internal_routes   CD ascending
    external_routes   CD ascending
```

Both queues contain the same `route_descriptor` type. Internal/external is a
property of the path advertisement, not a property of the destination. A
single destination may therefore contain both internal and external paths at
the same time.

Connected and EIGRP internal paths are stored in `internal_routes`. External
route TLVs and locally redistributed paths are stored in `external_routes`.

Each queue is maintained in ascending Computed Distance (CD) order. When a
path's CD changes it is removed and reinserted. If an existing neighbor path
changes between internal and external advertisement type, the existing path
descriptor moves between queues; the class change does not create a duplicate
neighbor path.

## 3. Internal before external

Internal and external queues are separate so route-class preference is not
encoded as an artificial metric comparison. DUAL considers internal paths
before external paths. External paths are considered only when the internal
search produces no usable path.

An external path with a lower CD than an internal path does not outrank the
internal path merely because its metric is lower.

The split queues are a topology indexing and selection optimization. They do
not themselves change DUAL state or cause Passive/Active transitions.

## 4. Best-path search

Within each route class, paths are examined in ascending CD order.

For each path:

```text
pick next lowest-CD path
        |
        +-- FC fails  -> continue
        |
        +-- FC passes -> apply applicable policy
                            |
                            +-- reject -> continue
                            |
                            +-- accept -> selected path; stop
```

The Feasibility Condition remains:

```text
RD < FD
```

FC is evaluated before any policy operation that may cross a host/integration
boundary. Once a path passes FC and policy, the search stops because CD sorting
guarantees that no later path in that class has a lower CD.

If the internal queue is exhausted without an accepted path, the same search
is performed on the external queue. If both searches are exhausted, no usable
path is returned.

Current Alpha-25 inbound distribute/prefix filtering is applied when an UPDATE
is received and represents rejection as unreachable path state. This design
does not add a second host-policy invocation in the selector. If policy is
later moved to a selection-time host callback, FC MUST remain before that
callback and the I-then-E/CD ordering defined here remains unchanged.

## 5. DUAL ownership

The route queues do not select FSM state. `eigrp_fsm.c` remains responsible for
DUAL transitions and for deciding when destination-level state may change.
Topology code owns storage, ordering, path lookup, and route-class-aware
selection.

While Active, the destination-level successor, FD, destination RD, current
destination distance, and advertised destination metric remain frozen as
defined by `dual.md`.

## 6. Successor and feasible-successor marking

Successor/feasible-successor flags are derived only from the route class that
contains the selected usable path. If an internal path is selected, external
paths cannot be marked successor or feasible successor for that destination.
If no internal path survives FC/policy and an external path is selected, the
external queue becomes the class used for successor/feasible-successor
marking.

Variance and maximum-path processing operate within that selected class and do
not combine internal and external paths into one multipath set.

## 7. Operations that need every path

Neighbor teardown, topology display/state export, cleanup, statistics, and
similar management operations are not best-path searches. They walk both
queues through the topology all-path iterator or explicitly visit both queues.
They must not use the best-path selector as a substitute for an all-path walk.

## 8. RIB and packet consequences

Host-RIB route type and external tag are derived from the selected path, not
from destination-level `prefix_descriptor.nt`. Destination-level `nt` may still
represent connected/legacy destination state where required, but it is not the
authority for internal versus external selection when both classes coexist.

Packetization likewise consumes the path selected by DUAL/topology state. The
queue split does not authorize packet code to make independent route-selection
decisions.
