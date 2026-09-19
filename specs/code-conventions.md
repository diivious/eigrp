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
eigrp_metric_weights_update();
eigrp_metric_weights_delete();
eigrp_metric_variance_update();
eigrp_metric_variance_delete();
eigrp_metric_default_update();
eigrp_metric_default_delete();
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
eigrp_metric_variance_update();
eigrp_metric_variance_delete();
eigrp_neighbor_create();
eigrp_neighbor_delete();
eigrp_query_receive();
```

Use modern CRUD terminology as the default for configuration and object lifecycle APIs when it accurately describes the operation:

```text
create  introduce a configured/keyed object or relationship
read    retrieve configuration or state without side effects
update  change an existing object or configurable value
delete  remove explicit configuration or an owned object/relationship
```

Do not invent a complete CRUD quartet where the feature does not need it. A scalar configuration commonly needs only `update()` and `delete()`; a keyed relationship may need only `create()` and `delete()`.

For example:

```c
eigrp_metric_variance_update(...);
eigrp_metric_variance_delete(...);
```

A CLI `no` command frequently maps to `delete()` because it removes explicit configuration so default or inherited behavior applies.

Use `set/reset`, `add/remove`, and similar verbs only after considering whether CRUD terminology is genuinely inappropriate. They are not the default for new or materially refactored configuration APIs.

Do not force CRUD terminology onto protocol/domain actions where a semantic verb is clearer. Operations such as `advertise`, `withdraw`, `install`, `uninstall`, `enable`, `disable`, `attach`, `detach`, and operational `clear` remain appropriate when they describe what EIGRP is actually doing.

Prefer separate public target functions for distinct actions rather than a public operation-enum dispatcher. Private helpers and enums are acceptable when they remove duplicated implementation without obscuring the public target.

## 5. Feature Target Rule

Every implemented CLI or management feature must terminate at its own real EIGRP target namespace. Do not route unrelated commands through a generic CLI stub, generic `not configured`, generic `not implemented`, or feature dispatcher.

An incomplete target is still a real target:

```c
eigrp_metric_variance_update(...)
{
    return EIGRP_RESULT_NOT_IMPLEMENTED;
}
```

The function can later be implemented in place without redesigning the CLI/northbound call path.

Shared helpers below those targets are allowed.

## 6. Classic and Named Surface Convergence

When named mode adds a command that already has a classic implementation, both
configuration surfaces should converge on the same EIGRP-owned behavior whenever
that can be done without importing host objects into portable code.  The normal
shape is:

```text
classic CLI -> host/classic adapter --\
                                  -> EIGRP-owned target/processor
named CLI   -> host/named adapter ---/
```

Do not call a classic FRR callback from named mode and do not move FRR VTY, YANG,
`struct interface`, Zebra, or other host objects into a portable target merely to
reuse an existing callback.  Reuse the EIGRP behavior below the host boundary.

If an existing classic callback cannot be changed because it is host/upstream-owned,
leave that callback intact.  A portable compatibility wrapper or shared EIGRP
processor may be introduced below it so classic and named execution converge as
early as possible without modifying host-owned code.  Document that exception;
do not pretend the two front ends call the same public target when they do not.

When no classic implementation exists, the named command still terminates at its
own real EIGRP target as required by the feature target rule.

### Authentication exception

Authentication is currently an explicit non-converged exception under the
no-FRR-change rule.  The existing classic FRR northbound callbacks directly
mutate the runtime interface fields:

```text
classic authentication -> FRR classic callback -> ei->params.auth_type/auth_keychain
named authentication   -> FRR named callback   -> eigrp_auth_* target -> runtime/config
```

The named path must use the EIGRP-owned `eigrp_auth_*` targets and those targets
must update the active EIGRP interface when one exists.  Do not route named mode
through the classic callback merely to claim reuse.  Unless the classic FRR
callback is intentionally changed in a later approved increment, authentication
must not be described or tested as true classic/named endpoint convergence.

MD5 plus key-chain has a usable runtime path.  The named direct-password
HMAC-SHA-256 form remains retained configuration when an active runtime is
present and returns `EIGRP_RESULT_NOT_IMPLEMENTED` from the EIGRP authentication
target until the SHA-256 runtime key-material and receive-validation path is
implemented.  Do not silently substitute a configured key-chain for the named
HMAC password.

### Redistribution and distribute-list exception

Redistribution and distribute-list are also intentionally non-converged at the
public classic/named endpoint while the existing FRR classic callbacks remain
unchanged.  The named path owns portable retained state and crosses the host
boundary only through EIGRP southbound targets:

```text
classic redistribute  -> FRR classic NB -> eigrp_redistribute_set/unset()
named redistribute    -> FRR named NB   -> eigrp_redistribute_update/delete()
                                      -> portable redistribution state
                                      -> eigrp_southbound_redistribute_*()
                                      -> FRR Zebra subscription

classic distribute-list -> FRR distribute framework -> FRR policy adapter
                                         -> eigrp_filter_runtime_replace()
named distribute-list   -> FRR named NB -> eigrp_distribute_list_*()
                                         -> portable retained/runtime filter state
filter decision          -> eigrp_filter_prefix_apply()
                         -> eigrp_southbound_filter_evaluate()
                         -> FRR policy adapter -> portable decision
```

Do not route the named path through `eigrp_redistribute_set/unset()` or the FRR
`group_distribute_list_*` callbacks merely to reuse the classic implementation.
Those endpoints consume FRR-owned state.  Classic distribute-list callbacks now
normalize FRR state into the portable filter runtime representation, while the
public classic/named configuration endpoints remain intentionally separate.
Redistribution remains non-converged at the public endpoint until its classic
FRR callback is intentionally refactored.

Redistribution configuration, including route-map policy, is retained by the
portable named state.  The current FRR external-route receive callback does not
yet populate EIGRP topology state or apply route-map policy, so the named
southbound update reports `EIGRP_RESULT_NOT_IMPLEMENTED` after establishing the
Zebra redistribution subscription.  This preserves truthful feature status while
still exercising the correct host boundary.

## 7. Host/Platform Naming

Portable EIGRP modules describe EIGRP protocol concepts. Host adapters describe the host integration point.

Examples:

```text
eigrp_southbound_*   portable core-to-host runtime contract
eigrp_zebra_*        FRR Zebra/RIB integration
eigrp_vty_*          FRR VTY presentation
eigrp_cli_*          FRR CLI/configuration front end
```

Do not use a generic protocol term such as `route` when the intended object is actually a Zebra RIB entry, kernel route, DUAL topology descriptor, or another distinct object. Name the owning domain when ambiguity exists.

## 8. Topology Descriptor Terminology

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

## 9. Rename Discipline

Naming cleanup applies when code is new, touched, materially refactored, or moved into a clearer module boundary. Avoid giant rename-only commits during active feature development.

When a function is renamed, update callers directly. Do not leave alias wrappers merely to preserve an obsolete internal name.

Exceptions to the normal naming pattern should be explainable in terms of navigation, implementation grouping, or an external host/API boundary. The goal is not 100 percent mechanical naming; the goal is that a human can reasonably predict where code lives and what to search for.
