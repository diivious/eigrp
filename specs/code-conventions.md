# EIGRP Code Conventions

Copyright (C) 2026 Donnie V. Savage

## 1. Purpose

The primary purpose of the EIGRP naming convention is human code navigation. A developer who knows the protocol area should be able to predict the source file to inspect and the function prefix to search without already knowing the implementation.

This document refines the naming and module rules in `design-spec.md`. Exceptions are allowed when they improve maintainability, but they should remain uncommon and intentional.

## 2. Navigation-First Naming

The normal source layout is:

```text
eigrp_<module>.c
eigrp_<module>.h
```

Functions owned by that module should normally begin with:

```text
eigrp_<module>_
```

and then narrow to the object or operation, with the action at the end:

```text
eigrp_<module>_<object>_<action>()
```

Examples:

```c
/* eigrp_query.c */
eigrp_query_receive();
eigrp_query_send();
eigrp_query_process();

/* eigrp_interface.c */
eigrp_interface_create();
eigrp_interface_delete();
eigrp_interface_update();

/* eigrp_instance.c */
eigrp_instance_create();
eigrp_instance_start();
eigrp_instance_stop();
eigrp_instance_lookup();

/* eigrp_metric.c */
eigrp_metric_calculate();
eigrp_metric_weights_set();
eigrp_metric_weights_reset();
eigrp_metric_variance_set();
eigrp_metric_variance_reset();
eigrp_metric_default_set();
eigrp_metric_default_reset();
```

The point is that a developer debugging metrics can search for `eigrp_metric_` and find the metric implementation family. A developer debugging query processing can go to `eigrp_query.c` and search for `eigrp_query_`.

Do not derive portable function names from the CLI/YANG hierarchy merely because a command appears under a particular CLI mode. For example, a command entered under `topology base` that configures metrics belongs to the metric navigation namespace, not automatically to `eigrp_topology_*`.

## 3. Module Grouping and Exceptions

File count and navigation consistency must be balanced. Closely related small feature families may share a source file when splitting them would create many trivial one- or two-function files.

Existing example:

```text
eigrp_filter.c
    eigrp_distribute_*
    eigrp_offset_*
```

This is an intentional exception: the functions keep clear searchable feature prefixes while the closely related filtering implementations share a file.

Do not force every function in a grouped file to inherit an artificial filename prefix. Conversely, do not create exceptions merely to shorten names. The normal expectation remains that filename and function namespace align.

When a grouped feature grows enough to become a meaningful subsystem, it may be promoted into its own `eigrp_<module>.c/.h` pair as a pre-production refactor.

## 4. Action Placement and Verbs

Actions normally appear last:

```c
eigrp_metric_variance_set();
eigrp_metric_variance_reset();
eigrp_neighbor_create();
eigrp_neighbor_delete();
eigrp_query_receive();
```

Prefer separate public target functions for distinct actions rather than an operation enum used as a public dispatcher:

```c
eigrp_metric_variance_set(...);
eigrp_metric_variance_reset(...);
```

instead of:

```c
eigrp_metric_variance(EIGRP_SET, ...);
eigrp_metric_variance(EIGRP_RESET, ...);
```

A private helper may use an internal operation enum when that genuinely removes duplicated implementation:

```c
static eigrp_result_t
eigrp_metric_variance_update(..., enum eigrp_config_operation operation, ...);
```

The public API remains explicit and searchable.

Use `reset` for a configuration `no` operation that restores a default. Reserve `clear` primarily for operational behavior such as clearing neighbors, counters, or protocol state.

Use `add/remove` for membership in keyed collections where that better expresses the data relationship, and `create/delete` for object lifecycle where the implementation owns allocation/lifetime.

## 5. Feature Target Rule

Every implemented CLI or management feature must terminate at its own real EIGRP target namespace. Do not route unrelated commands through a generic CLI stub, generic `not configured`, generic `not implemented`, or feature dispatcher.

An incomplete target is still a real target:

```c
eigrp_metric_variance_set(...)
{
    return EIGRP_RESULT_NOT_IMPLEMENTED;
}
```

The function can later be implemented in place without redesigning the CLI/northbound call path.

Shared helpers below those targets are allowed.

## 6. Host/Platform Naming

Portable EIGRP modules describe EIGRP protocol concepts. Host adapters describe the host integration point.

Examples:

```text
eigrp_southbound_*   portable core-to-host runtime contract
eigrp_zebra_*        FRR Zebra/RIB integration
eigrp_vty_*          FRR VTY presentation
eigrp_cli_*          FRR CLI/configuration front end
```

Do not use a generic protocol term such as `route` when the intended object is actually a Zebra RIB entry, kernel route, DUAL topology descriptor, or another distinct object. Name the owning domain when ambiguity exists.

## 7. Topology Descriptor Terminology

EIGRP's DUAL topology database contains two related descriptor object classes:

```text
prefix_descriptor    destination/prefix-level topology object
route_descriptor     one path/neighbor-derived route for that prefix
```

Cisco historically used DNDB/NDB and DRDB/RDB terminology. Those names are concise, familiar in EIGRP debugging, easy to search, and avoid collision with a host platform's broader use of the word `route`.

The final source/API terminology for these topology objects is intentionally deferred to the pre-production refactor review in `refactor-work.md`. Until then:

- do not perform rename-only churn in this area;
- do not introduce new ambiguous uses of bare `route` where a topology descriptor, RIB route, or prefix is meant;
- preserve current descriptor names unless a functional change requires otherwise;
- comments/debug output may note the Cisco DNDB/DRDB terminology where it improves understanding.

## 8. Rename Discipline

Naming cleanup applies when code is new, touched, materially refactored, or moved into a clearer module boundary. Avoid giant rename-only commits during active feature development.

When a function is renamed, update callers directly. Do not leave alias wrappers merely to preserve an obsolete internal name.

Exceptions to the normal naming pattern should be explainable in terms of navigation, implementation grouping, or an external host/API boundary. The goal is not 100 percent mechanical naming; the goal is that a human can reasonably predict where code lives and what to search for.
