# EIGRP Code Conventions

Copyright (C) 2026 Donnie V. Savage

## 1. Purpose

EIGRP naming is optimized for human navigation. A developer who knows
the protocol area should be able to predict the symbol to search without
already knowing the implementation.

Names describe EIGRP semantics, not CLI/YANG nesting, source-file
placement, or host-framework implementation details. Prefer short,
conventional C/networking terms where they remain unambiguous.

This document refines the naming rules in `design-spec.md`.

## 2. Navigation-first naming

The normal conceptual pattern is:

``` text
eigrp_<module>_<object>_<action>()
```

The module qualifier may be omitted when it adds no useful semantic
distinction. A symbol does not have to match the `.c` filename
containing it. Small portable helpers may be static inline and have no
corresponding `.c` implementation.

Examples:

``` c
eigrp_query_receive();
eigrp_query_send();

eigrp_nbr_lookup(addr);
eigrp_nbr_down(nbr);

eigrp_metric_update(EIGRP_SET, ...);
eigrp_metric_update(EIGRP_RESET, ...);

eigrp_af_instance_create(...);
eigrp_af_instance_delete(...);
```

Avoid action-first names:

``` c
eigrp_create_neighbor();    /* avoid */
eigrp_decode_packet();      /* avoid */
```

Do not encode object ownership hierarchy merely because an object is
nested inside another object. Name the semantic object being operated
on.

A Cisco/YANG command location is not a portable module namespace. A
command entered under `topology base` that changes metric behavior
belongs in the metric namespace, not automatically the topology
namespace.

If a multi-word object name has no obvious short form, choose clarity
and review the name rather than inventing an abbreviation.

## 3. Module grouping and deliberate exceptions

Avoid needless file proliferation. Closely related small feature
families may share a source file while retaining distinct searchable
prefixes.

The model exception is:

``` text
eigrp_filter.c
    eigrp_distribute_*
    eigrp_offset_*
```

Do not mechanically rename those functions to `eigrp_filter_*` merely
because they share a file.

## 4. Core action vocabulary

Use one preferred verb for one semantic operation. Do not add wrapper
functions merely to satisfy an abstract API pattern.

### 4.1 Object lifetime and memory

``` text
create / delete   semantic object lifetime
alloc / free      raw/simple storage lifetime
init              explicit initialization independent of create
```

`create()` is the constructor and includes normal initialization.
`delete()` is the semantic destructor and performs any required teardown
before storage is freed.

A separate `free()` helper is permitted when there is a real
implementation reason to separate raw storage release from semantic
teardown. Do not introduce one merely because the appropriate semantic
name is already being used for a different operation.

Use `init()` only when initialization genuinely occurs independently of
object construction, such as caller-owned, static, embedded, or
host-owned storage.

Do not use `_new()` for EIGRP APIs. Use `_create()`.

### 4.2 Configuration/state mutation

`update` is the action for changing retained configuration or object
state.

`SET` and `RESET` are opcodes supplied to `_update()`; they are not
public action names:

``` c
eigrp_auth_update(EIGRP_SET, ...);
eigrp_auth_update(EIGRP_RESET, ...);
```

Likewise `ENABLE` and `DISABLE` may be opcodes when enable/disable is
simply one form of object/configuration mutation:

``` c
eigrp_auth_update(EIGRP_ENABLE, ...);
eigrp_auth_update(EIGRP_DISABLE, ...);
```

A dedicated action function is still appropriate when the action is a
first-class operation on an existing object and materially improves the
API:

``` c
eigrp_intf_enable(intf);
eigrp_intf_disable(intf);
eigrp_timer_start(timer);
eigrp_timer_stop(timer);
```

Do not expand the public API merely to create symmetric wrappers around
`update()`.

`apply` and `refresh` are not standard EIGRP action synonyms; use
`update` when that is the operation being performed.

### 4.3 Search and selection

``` text
read     retrieve existing object/value/state without changing it
lookup   search for and return an object using identifying information
select   evaluate candidates and choose the appropriate/best/latest one
```

`read` is the normal semantic replacement for accessor-style `get`. Before
renaming an existing `get`, inspect what it actually does: use `lookup` when it
searches by identity/key, use `select` when it evaluates candidates, and retain
`get` when it is an established low-level primitive such as stream/buffer
access. Do not create read/get wrappers for directly visible structure members.

`lookup` is the standard search verb regardless of whether the
implementation walks a list, hashes a key, searches a tree, or uses
another mechanism. A combined lookup-or-create operation should say so rather
than being hidden behind `get`.

Do not use `find` as a synonym for `lookup`.

`select` is distinct: it expresses an actual choice among candidates, not a
general replacement for `get` or `read`.

`put` has no general EIGRP mutation convention; use `update` for
mutation.

### 4.4 Containers

Container operations describe membership, not object lifetime.

``` text
add       add an object to the end of a list/collection
insert    place an object at a particular/appropriate position
remove    remove an object from a list/collection
enqueue   add an object to a queue
dequeue   remove an object from a queue
```

The same membership model applies whether the container is EIGRP-owned
or host-owned. For example, an EIGRP topology and a host RIB are both
containers from the perspective of add/remove semantics.

`remove` never implies destruction. Object lifetime remains
`create/delete`.

The container-owned membership/linkage object is an `item`:

``` c
eigrp_list_item_t;
eigrp_queue_item_t;
eigrp_list_item_create();
eigrp_queue_item_create();
```

Do not use `node`, `element`, or `entry` as synonyms for a list/queue
item.

### 4.5 Traversal

Normal traversal uses `first`/`next` primitives or a well-defined
iteration macro:

``` c
EIGRP_LIST_ITERATE(list, item) {
    ...
}

EIGRP_TOPO_ITERATE(topo, item) {
    ...
}
```

`iterate` is the traversal concept and is primarily appropriate for
macros or helpers. `iterator` is reserved for an actual iterator state
object when one is genuinely needed.

Do not create `_walk()` or `_iterate()` API families when ordinary
traversal primitives are sufficient.

### 4.6 Comparison and copying

``` text
match   boolean comparison/matching predicate
cmp     ordered comparison, normally < 0 / == 0 / > 0
cpy     copy into existing destination/storage
dup     create an independent duplicate
```

Do not introduce `equal` as a synonym for `match`. Do not use `clone`.

Examples:

``` c
if (eigrp_addr_match(a1, a2))
    ...

rc = eigrp_addr_cmp(a1, a2);
eigrp_addr_cpy(&dst, &src);
copy = eigrp_route_dup(route);
```

### 4.7 Validation

``` text
valid       boolean predicate asking whether something is valid
validate    perform validation and return a validation result/status
```

Examples:

``` c
if (eigrp_packet_valid(packet))
    ...

result = eigrp_packet_validate(packet);
```

Do not use `verify` as a synonym for `validate`.

Do not use `check` as a general validation/predicate verb. `check` is
reserved for a specific host/system synchronization or watched-state API
when that is the host's terminology.

### 4.8 Protocol and representation actions

``` text
encode / decode    internal <-> wire representation
send / receive     protocol transport direction
process            perform the substantive processing of an input/object
format             produce a formatted representation
dump               detailed/raw debug output, especially packet contents
```

`process` is valid both as an architectural noun (`eigrp_process`) and
as a verb when a function genuinely performs the principal multi-step
processing of an input. Do not use it when a narrower action such as
`validate`, `decode`, `update`, or `send` accurately describes the
function.

Do not use generic `build` when `create`, `encode`, `packetize`, or
another specific operation describes the work.

`parse` is normally host/shim text/configuration interpretation.
Protocol wire processing uses `decode`.

`show` is CLI/user-facing presentation and belongs in the host/shim
layer.

`print`, `load`, `save`, `open`, `close`, `register`, `unregister`,
`bind`, and `unbind` are host/system vocabulary when required by the
host API. They are not portable common EIGRP action vocabulary.

### 4.9 Directional transformations

When converting between representations/types and the direction matters,
use:

``` text
<source>2<destination>
```

Examples:

``` c
eigrp_metric_classic2wide();
eigrp_metric_wide2classic();
eigrp_ipv4_addr2prefix();
eigrp_ipv4_prefix2addr();
```

Do not use `X2Y` merely to describe movement between containers. Use the
actual operation (`add`, `remove`, `send`, etc.).

### 4.10 Clear and reset

`clear` is a narrow readability action for returning reusable
storage/buffers to a clean/empty initialized state. It may reset
metadata and/or memory; it does not merely mean zero bytes.

`zero` is not an EIGRP action convention.

`RESET` is a configuration opcode supplied to `_update()` and must not
be overloaded as a buffer/storage action.

### 4.11 Runtime actions and events

`start/stop` are actions on an already-created object that naturally has
running/stopped behavior; timers are the canonical example.

`enable/disable` are allowed as first-class object actions when
meaningful and when they do not unnecessarily expand the public API.
Otherwise use `update(EIGRP_ENABLE/EIGRP_DISABLE, ...)`.

Common-code protocol stimuli name the meaningful English state/event at
the point the common entry point is called:

``` c
eigrp_nbr_up(nbr);
eigrp_nbr_down(nbr);
eigrp_timer_expired(timer);
eigrp_config_changed(config);
```

Do not mechanically manufacture tense (`downed`, etc.). Use natural
English.

In portable common code, `event` primarily denotes DUAL event/event-log
semantics. Host event-loop/timer terminology remains in the shim.

`notify` and `signal` are not standard common EIGRP action vocabulary.

## 5. Predicates

Boolean predicates use the shortest meaningful condition name. Do not
add `is_`, `has_`, `can_`, `check_`, or `test_` merely to announce that
the function returns a boolean.

Prefer:

``` c
if (eigrp_nbr_up(nbr))
    ...

if (eigrp_route_external(route))
    ...
```

The condition name must still be meaningful English and unambiguous.

## 6. Address-family and common object naming

`afi` denotes an address-family identifier/value.

The configured/runtime address-family object is `af_instance`:

``` c
eigrp_af_instance_t;
eigrp_af_instance_create();
eigrp_af_instance_delete();
eigrp_af_instance_lookup();
```

Do not use `address_family` or bare `af` for the AF instance object.

Do not duplicate public IPv4/IPv6 APIs when one AF-aware EIGRP object
and target correctly represents both families. AF-specific behavior
belongs in AF-specific implementation/vector logic when protocol
semantics differ.

A synchronous function-pointer table is a `vector`/`vectors`. The target
functions themselves do not acquire `_cb`, `_vec`, or `_vector` suffixes
merely because they are reached through a function pointer.

## 7. Callback and host terminology

`_cb` is host/shim terminology. Use it only when the host/system API
actually defines the registered function as a callback.

Do not use `_cb` merely because a function is invoked through a function
pointer. `_cb` should normally not appear in portable common EIGRP code.

Host adapters may use the host's native vocabulary for callbacks,
events, binding, registration, file/socket operations, and similar
framework concepts. Do not leak those terms, datatypes, headers,
ownership rules, or lifetimes into portable common APIs.

The shim speaks the host's vocabulary; common code speaks EIGRP's
vocabulary.

## 8. Common portable primitives

Small, portable, broadly reusable primitives with no meaningful module
ownership belong in `eigrp_inlines.h` when inline implementation is
appropriate.

Examples include list/queue traversal and pointer manipulation
primitives.

Do not create module/file proliferation merely to house a few tiny
generic operations, and do not bloat the public umbrella header with
private utility implementation.

## 9. Abbreviations

Recognized C/networking/EIGRP abbreviations are encouraged when they
remain unambiguous and improve navigation/typing.

Established forms include:

``` text
af_instance   address-family runtime/config object
afi           address-family identifier
asn           autonomous-system number
addr          address
intf          interface
nbr           neighbor
topo          topology
nexthop       next hop
redist        redistribute/redistribution
routemap      route map
max_prefix    maximum prefix
rib           routing information base
fib           forwarding information base
tlv           type-length-value
rtp           reliable transport protocol
sia           stuck in active
mtu           maximum transmission unit
seq           sequence
ack           acknowledgment
msg           message
```

Protocol/algorithm names retain their conventional abbreviations and use
normal C-symbol lowercase:

``` text
ipv4
ipv6
md5
sha256
hmac
```

Prefer recognizable words over unnecessary abbreviations when the word
is already short. Avoid `int` as an abbreviation for `internal`; use
`internal`. Likewise use `external` rather than an ambiguous `ext` where
it names protocol semantics.

If a multi-word term has no obvious established abbreviation, review it
rather than inventing one.

## 10. Event log terminology

The DUAL event log records/formats messages describing DUAL events. Use
`msg` rather than generic `entry`:

``` c
eigrp_eventlog_msg_t;
eigrp_eventlog_msg_format(...);
```

`entry` has no standard generic EIGRP meaning. Name the actual semantic
object instead.

## 11. Feature target rule

Every CLI or management feature terminates at its own real EIGRP target
namespace.

Do not route unrelated commands through generic CLI stubs, generic
not-configured/not-implemented dispatchers, or unrelated command-family
functions.

An incomplete feature still owns its real EIGRP target and reports
structured `EIGRP_RESULT_NOT_IMPLEMENTED` until implemented.

A shared `_update(op, ...)` target remains feature-specific; do not turn
`update()` into a giant unrelated-command dispatcher.

## 12. Classic and named surface convergence

Classic and named configuration surfaces converge on the same
EIGRP-owned semantic behavior whenever the protocol operation is the
same.

``` text
classic CLI -> host adapter --\
                               -> EIGRP-owned target
named CLI   -> host adapter ---/
```

CLI/YANG parsing and host management objects stop at the northbound
boundary. The EIGRP target receives normalized EIGRP-owned values.

## 13. Host/platform naming and portability

Portable modules name EIGRP protocol concepts. Host adapters name host
integration concepts.

Host-specific terms and services remain in their adapter layers. Common
code must not directly or indirectly depend on FRR/BIRD datatypes,
headers, ownership rules, or object lifetimes. When host data must cross
the boundary, normalize it into an EIGRP-owned type.

Standard C/POSIX facilities do not require a southbound abstraction
merely because common code is portable. Introduce a common wrapper only
when it serves a real common-code purpose such as diagnostics,
allocation tracking, or consistent failure handling.

## 14. Topology descriptor terminology

The DUAL topology database owns two related descriptor classes
historically known as DNDB/NDB and DRDB/RDB. Final source/API
terminology remains intentionally deferred to the pre-production
descriptor review.

Until that decision:

-   do not perform descriptor rename-only churn;
-   preserve existing descriptor names unless required by the namespace
    refactor;
-   avoid introducing new ambiguous bare `route` APIs solely as aliases;
-   comments/debug output may retain DNDB/DRDB terminology where useful.

## 15. Rename discipline

Namespace refactoring must preserve behavior unless a task explicitly
authorizes a functional change.

A rename-only pass may change symbol names, declarations, references,
comments, and tests required by the rename, but must not change function
signatures, control flow, ownership, runtime behavior, configuration
semantics, or protocol behavior.

When consolidating duplicate action functions into a shared
`_update(op, ...)` API, perform that as a separate explicit pass so
namespace changes can be reviewed independently from function/API
consolidation.
