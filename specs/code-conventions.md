# OpenEIGRP Code Conventions

Copyright (C) 2026 Donnie V. Savage

## 1. Scope

This document defines naming and source-organization conventions shared across
the OpenEIGRP repository.

It is intentionally about code navigation and public vocabulary. Protocol
algorithms belong in `eigrp/specs/`; FRR- or BIRD-specific coding rules belong
with those integrations.

The project name is **OpenEIGRP**. Existing protocol/API identifiers continue
to use `eigrp_` and `EIGRP_`; the rebrand does not trigger source-wide symbol
renames.

## 2. Navigation-first naming

Names should tell a developer where to look.

Portable modules normally use:

```text
eigrp_<module>.c
eigrp_<module>.h
eigrp_<module>_<object>_<action>()
```

Examples:

```text
eigrp_query.c              -> eigrp_query_*
eigrp_interface.c          -> eigrp_interface_*
eigrp_instance.c           -> eigrp_instance_*
eigrp_metric.c             -> eigrp_metric_*
```

Put the narrow object or property after the module and the action last:

```c
eigrp_metric_variance_set()
eigrp_metric_variance_reset()
eigrp_interface_passive_set()
eigrp_interface_passive_reset()
```

A developer who knows the protocol area should be able to predict the source
file and symbol prefix without searching the entire tree.

## 3. Module ownership beats CLI hierarchy

Portable names come from protocol ownership, not parser nesting.

A command entered under `topology base` that modifies variance still belongs to
the metric module. A command entered under an FRR YANG subtree does not acquire
a YANG-shaped portable name.

Do not derive portable names from:

- Cisco CLI hierarchy;
- FRR YANG paths;
- BIRD configuration grammar;
- host process names;
- transient parser implementation details.

## 4. Grouped modules and deliberate exceptions

Small, tightly related feature families may share a module when that improves
navigation.

The established model is `eigrp_filter.c`, which owns both
`eigrp_distribute_*` and `eigrp_offset_*` operations. Do not rename those
families merely to force every public symbol to match the filename.

Exceptions should be deliberate and uncommon. A module that becomes a bucket
for unrelated features should be split.

## 5. Action vocabulary

Use a small, predictable set of action verbs.

### 5.1 Object lifetime

Use:

```text
create      establish a logical/config/runtime object
delete      destroy that logical object
alloc       allocate raw/internal storage when allocation itself is the action
free        release raw/internal storage
init        initialize an existing object or subsystem
finish      shut down/finalize a subsystem
dup         duplicate an object
```

Prefer `create/delete` for public logical objects. Do not use `new`, `destroy`,
or `clone` when the project vocabulary already expresses the operation.

### 5.2 Configuration mutation

For a narrow public property, prefer distinct action functions:

```text
set         configure/enable/assign a non-default value
reset       restore the configuration default
add         add an entry to a collection
remove      remove an entry from a collection
update      apply an operation when the API intentionally accepts SET/RESET
```

Examples:

```c
eigrp_metric_variance_set(...);
eigrp_metric_variance_reset(...);
eigrp_offset_add(...);
eigrp_offset_remove(...);
```

A private shared helper may accept an operation enum. Do not expose a generic
public dispatcher merely because multiple commands can share internal code.

Existing public `*_update()` APIs may remain where they are established; new
APIs should prefer explicit actions when that makes call sites clearer.

### 5.3 Lookup and selection

Use:

```text
read        retrieve a property/state snapshot through an API
lookup      locate one object by identity/key
find        search algorithmically for a matching candidate
select      choose the preferred candidate from a set
best        qualify a selection helper only when "best" is a protocol concept
iterate     invoke a callback over a collection
walk        host/system traversal where the host provides the enumeration
```

Do not use `get` as an all-purpose verb when `read`, `lookup`, or `find` states
the contract more precisely.

### 5.4 Comparison and copying

Use:

```text
match       boolean semantic match
cmp         ordered comparison
cpy         copy into caller-owned storage
dup         allocate/produce a duplicate object
```

Do not use `equal` as an alias for `match` without a strong reason.

### 5.5 Validation

Use:

```text
valid       boolean predicate
validate    perform validation and return a result/status
```

Validation helpers should describe the value being validated rather than the
CLI command that supplied it.

### 5.6 Encoding and protocol representation

Use verbs that identify the direction of transformation:

```text
encode      native state -> protocol/wire representation
decode      protocol/wire representation -> native state
parse       interpret externally formatted input
format      native state -> presentation text
packetize   semantic protocol work -> packet work
send        submit/transmit through the owned boundary
receive     accept input from the owned boundary
```

Do not call packet construction `serialize` in one module and `encode` in
another without a meaningful distinction.

## 6. Reset versus clear

Use `reset` for configuration `no` operations that restore defaults.

Use `clear` primarily for operational actions that discard or restart runtime
state, for example:

```text
clear neighbor
clear topology
clear event log
```

Do not name a configuration default-restoration function `clear` merely because
the CLI keyword is `no`.

## 7. Predicates

Boolean functions should read naturally in conditions.

Preferred forms include:

```text
*_valid()
*_enabled()
*_supported()
*_active()
*_present()
*_empty()
*_match()
*_allows()
```

Avoid `is_` when the suffix already reads as a predicate unless `is_` materially
improves clarity.

## 8. Address-family naming

Do not put `ipv4` or `ipv6` into a symbol merely because the current caller is
address-family-specific.

Use an AF-neutral name when the operation is semantically common and accepts an
OpenEIGRP address/prefix abstraction.

Use an AF-specific name when the protocol representation, host service, or
algorithm is genuinely AF-specific, for example IPv4 versus IPv6 packet I/O or
TLV codecs.

Do not create duplicate IPv4/IPv6 wrappers around one common implementation
solely to mirror CLI syntax.

## 9. Host and callback terminology

Portable public APIs use OpenEIGRP-owned terminology. Host integration code may
use host-native names at the external boundary, but those names must not leak
into portable APIs.

Examples of host-native terms that stay in adapters include:

- FRR `struct interface`, VTY, YANG, Zebra, event/thread objects;
- BIRD protocol/config/channel/table/event-loop objects;
- operating-system socket structures beyond the normalized system boundary.

Callback names should state the event or iteration contract, not the host that
happens to invoke them.

## 10. Public versus private names

Public headers should expose semantic types and opaque identities, not private
module structures.

A public API must not require a host to include a private DUAL, topology,
packetizer, or neighbor header.

Private helpers should still follow module prefixes so stack traces and search
results remain navigable.

## 11. Result values

Portable semantic APIs return OpenEIGRP-owned structured result codes where the
caller must distinguish outcomes.

Do not export FRR/BIRD error codes through a portable API.

Do not collapse materially different outcomes into a boolean when callers need
to distinguish invalid input, absence, unsupported capability, or an internal
failure.

`EIGRP_RESULT_NOT_IMPLEMENTED` remains a defined public result value for API
stability, but current portable production targets do not return it. Optional
capability limits use `EIGRP_RESULT_UNSUPPORTED` (or another precise result) and
must still terminate at the real owning semantic target.

## 12. Event-log terminology

Event-log names describe the protocol event being recorded, not the CLI view
that later prints it.

State-transition events must be logged at the point where the state is actually
changed. Do not infer a transition later from surrounding control flow; doing
so can produce a log that claims X -> Y when a defect actually changed X -> Z.

Event argument types should remain compact and generic enough for the protocol
values they carry. Presentation formatting belongs at the event-log management
boundary, not in the state machine.

Cisco event strings may guide operational compatibility, but OpenEIGRP source
names should remain internally coherent rather than reproducing undocumented
Cisco implementation names blindly.

## 13. Feature target rule

Every command or management feature must terminate at its own real OpenEIGRP
semantic target.

Bad pattern:

```text
parser -> generic "not implemented" callback
```

Required pattern:

```text
parser -> adapter normalization -> owning eigrp_<module>_* target
                                      -> implemented behavior
                                      -> precise structured capability/error result
```

This rule keeps configuration retention, validation, tests, and runtime
behavior attached to the correct module.

## 14. Classic and named configuration convergence

Classic and named CLI are two host-facing ways to configure EIGRP. Where they
express the same protocol property, they should converge on the same portable
semantic target after normalization.

Do not maintain separate metric, timer, neighbor, or topology implementations
because one command came from classic mode and another came from named mode.

Parser-specific differences remain in the host layer.

## 15. Topology descriptor terminology

The DUAL topology database historically uses DNDB/NDB and DRDB/RDB terminology.
The project currently uses `prefix_descriptor` and `route_descriptor` in
portable code.

Final naming is intentionally deferred to a pre-production refactor review
because `route` is overloaded by host/RIB terminology while historical
DNDB/DRDB names retain EIGRP debugging value.

Until that review:

- do not perform rename-only churn;
- do not introduce new ambiguous bare `route` APIs;
- preserve the current object meaning in comments and new code.

## 16. Abbreviations

Prefer full protocol nouns in public names when they improve navigation.
Established EIGRP abbreviations may be used where they are canonical and
unambiguous.

Common acceptable abbreviations include:

```text
af      address family
afi     address-family identifier
nbr     neighbor
intf    interface
rib     routing information base
rtp     EIGRP Reliable Transport Protocol
sia     stuck in active
fs      feasible successor (primarily algorithm/debug context)
rd      reported distance (primarily algorithm/debug context)
fd      feasible distance (primarily algorithm/debug context)
```

Do not invent new abbreviations merely to shorten a symbol.

## 17. File and diff discipline

A feature patch should touch the module that owns the behavior and the tests or
specification necessary to describe it.

Do not combine a functional change with broad rename, whitespace, include-order,
or comment reformatting.

New files should have one predictable purpose. Avoid compatibility wrappers,
legacy aliases, duplicate paths, or transitional APIs unless a migration reason
has been explicitly approved.

## 18. Rebrand rule

Use **OpenEIGRP** when referring to this software project, implementation,
repository policy, or project documentation.

Use **EIGRP** when referring to:

- the routing protocol;
- RFC 7868 terminology;
- EIGRP packet behavior;
- CLI keywords that are actually `eigrp`;
- existing source/API identifiers such as `eigrp_metric_*`;
- interoperability with other EIGRP implementations.

This distinction applies to new documentation and comments. It avoids both
under-branding the project and incorrectly renaming the protocol.
