# OpenEIGRP Portable Route Selection and Topology Path Storage

Copyright (C) 2026 Donnie V. Savage

## 1. Scope

This document defines how portable OpenEIGRP topology code stores candidate
paths and selects successors/feasible successors after DUAL permits selection.

It does not define DUAL Active/Passive transition rules, packetization, host RIB
APIs, redistribution front ends, or CLI syntax.

DUAL invariants are defined in `dual.md`. Host route installation is defined by
the repository-wide integration contract.

## 2. Destination and path storage

Each destination is represented by a prefix descriptor. Individual learned or
locally generated paths are represented by route descriptors.

```text
prefix_descriptor
  internal route descriptors
  external route descriptors
```

Internal/external grouping is a storage and selection aid. It does not create a
second DUAL state machine and it does not alter the Feasibility Condition.

## 3. Internal before external

When both internal and external candidates exist for the same destination,
internal EIGRP paths are preferred before external EIGRP paths.

Selection therefore evaluates usable internal paths first. External paths are
considered when no eligible internal path wins under the applicable selection
rules.

This ordering must be explicit in portable topology/selection logic rather than
emerging accidentally from insertion order.

## 4. Candidate eligibility

Before metric ordering, a path must be eligible for the operation being
performed.

Eligibility may exclude a path because it is:

- withdrawn or unreachable;
- associated with an unusable neighbor/interface;
- filtered from the relevant use;
- outside the current DUAL/selection context;
- otherwise invalid according to portable protocol state.

A host RIB's opinion about the path does not determine portable candidate
eligibility.

## 5. Best-path search

For the set being considered:

1. discard ineligible paths;
2. compare computed distance/metric according to EIGRP metric semantics;
3. identify the least-cost candidate set;
4. apply DUAL feasibility requirements when selecting feasible successors;
5. choose successor paths subject to configured maximum-paths and variance
   behavior;
6. commit flags/state only when DUAL allows destination selection.

Tie handling must be deterministic and must not depend on host pointer order or
container accidents.

## 6. DUAL ownership

Route-selection helpers do not decide whether a destination may leave Active.

While a destination is Active, helpers may inspect updated path observations to
support DUAL evaluation, but they must not commit a new destination successor,
FD, or selected distance in violation of `dual.md`.

The DUAL FSM owns the legal point at which a new selection becomes committed
Passive state.

## 7. Successor selection

A successor is a selected least-cost usable path permitted by DUAL.

OpenEIGRP may install multiple equal-cost successors up to the configured
maximum paths.

Successor marking is derived from the current committed selection. Stale flags
must be cleared before a new set is applied.

## 8. Feasible successors

A feasible-successor candidate satisfies:

```text
neighbor RD < destination FD
```

Feasible-successor marking reflects the current committed DUAL/topology state.
It is not a substitute for running the DUAL FSM when FC is not satisfied.

Paths that are loop free but fail FC are not promoted merely because their
metric appears attractive.

## 9. Variance

Variance permits unequal-cost traffic sharing only among paths that remain
eligible under EIGRP feasibility rules.

A larger variance value does not make an infeasible path feasible.

Selection code must therefore apply feasibility before using the configured
variance multiplier to extend the installed successor set.

## 10. Maximum paths

`maximum-paths` limits the number of successor paths committed/installed for a
destination.

The limit does not delete additional known topology paths. Those paths remain
available for feasibility evaluation, DUAL recomputation, management output,
and future reselection.

## 11. Operations that require all paths

Several operations must inspect more than the current successor set, including:

- DUAL feasibility evaluation;
- `all-links` topology management output;
- neighbor/interface teardown;
- route withdrawal;
- metric or policy changes;
- recomputation after successor loss;
- cleanup of external versus internal path state.

Do not optimize the topology representation in a way that makes non-successor
paths inaccessible to these operations.

## 12. External route metadata

External EIGRP paths may carry additional origin/provenance data needed for
loop prevention, metric presentation, or re-advertisement.

That metadata belongs to portable route/path state. Host-native redistributed
route objects must be normalized before they become topology paths.

Internal/external classification must remain available after normalization.

## 13. RIB consequences

After DUAL permits a committed selection, route-selection/topology code creates
the normalized public RIB representation for the selected successor set.

The host adapter then installs or removes that route through `eigrp_rib.h`.

Host RIB callbacks must not reach back into private route descriptors to change
selection decisions.

## 14. Packet consequences

A committed topology change may queue UPDATE, QUERY, or REPLY work according to
DUAL/protocol state.

Route-selection code does not encode route TLVs and does not choose packet
transport. Those responsibilities belong to `rtp.md` and the packet/TLV
modules.

## 15. Topology descriptor terminology

The current source names `prefix_descriptor` and `route_descriptor` remain in
place pending the planned pre-production naming review.

Do not introduce new ambiguous bare `route` APIs or perform rename-only churn as
part of route-selection work.

## 16. Route-selection correctness checklist

A change is complete only when:

- internal/external preference is explicit and deterministic;
- candidate eligibility is portable and host-neutral;
- FC is never bypassed by variance or metric ordering;
- Active-state freeze remains intact;
- stale successor/feasible-successor flags cannot survive reselection;
- maximum-paths limits installed successors, not topology visibility;
- all known paths remain available to operations that require them;
- RIB output is a normalized consequence of portable selection.
