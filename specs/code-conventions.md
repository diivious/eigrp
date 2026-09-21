# EIGRP Code Conventions

Copyright (C) 2026 Donnie V. Savage

## 1. Purpose

EIGRP naming is optimized for human navigation. A developer who knows the
protocol area should be able to predict the source file and symbol prefix to
search without already knowing the implementation.

This document refines the naming rules in `design-spec.md`.

## 2. Navigation-first naming

The normal source/module pattern is:

```text
eigrp_<module>.c
eigrp_<module>.h
eigrp_<module>_<object>_<action>()
```

The module/function prefix normally aligns. The object/detail follows the module
name and the action normally appears last.

Examples:

```c
/* eigrp_query.c */
eigrp_query_receive();
eigrp_query_send();

/* eigrp_interface.c */
eigrp_interface_create();
eigrp_interface_delete();
eigrp_interface_shutdown_set();
eigrp_interface_shutdown_reset();

/* eigrp_instance.c */
eigrp_instance_parent_create();
eigrp_instance_parent_delete();
eigrp_instance_address_family_create();
eigrp_instance_address_family_delete();

/* eigrp_metric.c */
eigrp_metric_calculate();
eigrp_metric_weights_set();
eigrp_metric_weights_reset();
eigrp_metric_variance_set();
eigrp_metric_variance_reset();
```

Avoid action-first or CLI-derived names when the module owner is clear:

```c
eigrp_metric_set_variance();     /* avoid */
eigrp_create_neighbor();         /* avoid */
eigrp_decode_packet();           /* avoid */
```

A Cisco/YANG command location is not a portable module namespace. A command
entered under `topology base` that changes metric behavior belongs in
`eigrp_metric_*`, not automatically `eigrp_topology_*`.

## 3. Module grouping and deliberate exceptions

Avoid needless file proliferation. Closely related small feature families may
share a source file while retaining distinct searchable public prefixes.

The model exception is:

```text
eigrp_filter.c
    eigrp_distribute_*
    eigrp_offset_*
```

Do not mechanically rename those functions to `eigrp_filter_*` merely because
they share a file.

A grouped family may move to its own module when it becomes a substantial
subsystem and the move materially improves navigation. Do not split files for
symmetry alone.

## 4. Action placement and verb selection

Actions normally appear last.

Use the verb that describes ownership and semantics:

```text
create / delete   object lifecycle
add / remove      keyed collection membership or protocol relationship
set / reset       retained configuration and restore-to-default/no-form
init / finish     module/subsystem initialization lifecycle
start / stop      runtime operation lifecycle
enable / disable  protocol capability or runtime behavior where literal
attach / detach   ownership/binding relationship
install / remove  route or host-state installation
send / receive    protocol messages
encode / decode   wire conversion
parse / build     representation construction
validate          semantic/wire validation
calculate         metric/derived-value computation
find / lookup     object retrieval
walk              enumeration
clear             operational state/counters/neighbors only
```

For configuration, prefer explicit `set/reset` pairs when a positive command
sets a value and the `no` form restores the default:

```c
eigrp_metric_variance_set(...);
eigrp_metric_variance_reset(...);
```

Use `add/remove` when configuration represents collection membership:

```c
eigrp_redistribute_add(...);
eigrp_redistribute_remove(...);
```

Use `create/delete` when the command owns an object with a distinct lifecycle:

```c
eigrp_instance_address_family_create(...);
eigrp_instance_address_family_delete(...);
```

Reserve `clear` for operational actions:

```c
eigrp_neighbor_clear(...);
eigrp_eventlog_clear(...);
```

Do not use a public `SET|RESET` or generic operation enum dispatcher where two
clear public action functions make the target easier to find. Private helpers
and internal operation enums are acceptable when they remove duplicated code.

Avoid vague public verbs such as `process`, `handle`, `do`, `run`, or `manage`
when a protocol-specific action is available.

### 4.1 Public API granularity

A public symbol represents a semantic action, not every possible value of an
attribute used by that action. If several public functions have the same
contract and differ only by a level, category, codec family, time unit, or
similar selector, prefer one public API with an EIGRP-owned typed selector or a
single canonical unit.

Examples:

```c
eigrp_log(EIGRP_LOG_WARNING, ...);
eigrp_debug_set(EIGRP_DEBUG_TARGET_NEIGHBOR, flags, scope);
eigrp_neighbor_codec_bind(nbr, tlv_version);
eigrp_southbound_timer_add(event, callback, arg, delay_msec);
```

Do not apply this mechanically. Keep separate public functions when the
operations have different ownership, lifecycle, side effects, argument
contracts, or protocol meaning. Width-specific packet-buffer helpers may also
remain separate when the typed operation makes wire code materially easier to
read and audit.

This rule does not replace the action rule above. `set/reset`, `add/remove`, and
`create/delete` remain separate public actions when they represent distinct
semantics. Do not collapse those actions into a public operation enum merely to
reduce the symbol count.

## 5. Feature target rule

Every CLI or management feature terminates at its own real EIGRP target
namespace.

Do not route unrelated commands through:

```text
generic CLI stubs
generic "not configured" handlers
generic "not implemented" dispatchers
unrelated command-family functions
```

An incomplete feature still owns a real target:

```c
eigrp_metric_variance_set(...)
{
    return EIGRP_RESULT_NOT_IMPLEMENTED;
}
```

The implementation can be completed in place without redesigning the
CLI/northbound path.

## 6. Classic and named surface convergence

Classic and named configuration surfaces converge on the same EIGRP-owned
semantic behavior whenever the protocol operation is the same.

Normal shape:

```text
classic CLI -> FRR classic adapter --\
                                  -> EIGRP-owned target/processor
named CLI   -> FRR named adapter ---/
```

Do not call a classic FRR CLI/northbound callback from named mode merely to
claim reuse. Reuse the EIGRP behavior below the host boundary.

If an existing host-owned classic callback cannot immediately be changed, keep
that host callback intact and converge below it at an EIGRP-owned processor or
state representation where possible. Do not leak FRR objects into portable APIs
to force endpoint convergence.

When no working classic implementation exists, the named command still owns
its real EIGRP target and reports its runtime capability truthfully.

## 7. Address-family naming

Do not duplicate public IPv4/IPv6 APIs when one AF-aware EIGRP object and target
correctly represents both families.

Prefer:

```c
eigrp_summary_create(context, prefix);
```

over parallel family-specific public functions unless the protocol semantics
actually differ.

AF-specific behavior belongs in the AF implementation/vector when appropriate,
for example IPv4 classful auto-summary derivation or IPv6 packet-address rules.

## 8. Host/platform naming

Portable modules name EIGRP protocol concepts. Host adapters name the host
integration point.

Examples:

```text
eigrp_southbound_*   portable core-to-host contract
eigrp_zebra_*        FRR-private Zebra integration
eigrp_policy_*       FRR-private policy integration
eigrp_vty_*          FRR VTY presentation
eigrp_cli_*          FRR CLI front end
```

Do not use a generic protocol word such as `route` when the object is actually
a Zebra RIB route, kernel route, DUAL route descriptor, southbound route
snapshot, or another distinct object. Name the owning domain when ambiguity
exists.

## 9. Topology descriptor terminology

The DUAL topology database owns two related object classes:

```text
prefix_descriptor    destination/prefix-level topology object
route_descriptor     one neighbor/path descriptor beneath that destination
```

Cisco historically used DNDB/NDB and DRDB/RDB terminology. That terminology is
useful for EIGRP debugging/navigation and avoids collision with host/RIB uses of
`route`.

The final source/API terminology is deliberately deferred to
`refactor-work.md`. Until that decision:

- do not perform rename-only churn in this area;
- preserve existing descriptor names unless a functional change requires
  touching them;
- avoid new ambiguous bare `route` APIs;
- comments/debug output may include DNDB/DRDB terminology when useful.

## 10. Rename discipline

Apply naming cleanup to new APIs and to existing APIs when they are materially
touched, moved, or refactored. Do not create broad rename-only churn during
feature work.

When a function is intentionally renamed, update callers directly. Do not leave
alias wrappers or parallel obsolete names without an approved migration reason.

Existing names that do not yet match the final convention are refactor debt,
not precedent for new APIs. Bounded cleanup that requires coordinated rename
work belongs in `refactor-work.md`.
