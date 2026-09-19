# Post Command Audit Item 7 — Named IPv6 command/runtime parity

## Scope

After the single-AS named IPv4/4453 matrix is complete, run the same applicable
command, writeback, mutation, and documented `no`-form matrix under named IPv6
AS 4453.  `network` and classful `auto-summary` are intrinsically IPv4 and are
not part of the IPv6 matrix.  EIGRP Stub remains out of scope.

Named IPv6 configuration existing before the IPv6 packet data path is ready is
intentional.  Configuration retention is not conditional on packet support.

## Runtime model

A named address family binds a control runtime identified by `{AF, VRF, AS}`.
IPv4 creates that runtime with `data_path_ready == true`; IPv6 currently creates
it with `data_path_ready == false`.

The IPv6 control runtime owns common protocol/configuration state needed by real
EIGRP target functions, including router ID, metric coefficients, policy/filter
state, topology configuration, event-log configuration, and other retained
control values.  It does **not** create or enable the IPv6 packet data path.

While `data_path_ready` is false, runtime creation must not:

- open the EIGRP protocol socket or schedule receive processing;
- create the local/self neighbor used by packet processing;
- initialize packetizer or reliable-transport packet state;
- send Hellos or route packets;
- join EIGRP multicast groups;
- form adjacencies; or
- install/redistribute routes through the host RIB.

A packet-, adjacency-, interface-I/O-, or RIB-dependent IPv6 target terminates at
its real target and returns structured `EIGRP_RESULT_NOT_IMPLEMENTED`.  The
northbound configuration transaction remains committed for commands whose
configuration is valid and retainable.

Legacy runtime lookups used by the existing IPv4 packet/Zebra path remain
explicitly IPv4-scoped.  Named control-runtime lookup is AF-aware so an IPv6
control instance cannot accidentally satisfy an IPv4 datapath lookup.

## IPv6 summaries

Portable summary targets already use `eigrp_prefix_t` and are address-family
neutral.  FRR management storage therefore uses an `inet:ip-prefix` key for
both `summary-address` and `summary-metric`.

The CLI preserves IOS-style IPv4 writeback (`address mask`) while accepting the
native IPv6 prefix forms:

```text
summary-address 2001:db8:4453::/48 [distance [leak-map NAME]]
summary-metric 2001:db8:4453::/48 ...
```

Both forms terminate at the same portable EIGRP summary target functions.

## UUT order

`tools/frr-named-uut.sh` must keep the staged order:

1. complete IPv4 AS 4453;
2. complete the applicable IPv6 AS 4453 matrix;
3. only then exercise multiple AS values under one named parent;
4. only after that exercise case-distinct named parents.

A Stage-2 failure is a command/parser/configuration/runtime-target failure until
shown otherwise; it must not be attributed to the intentionally absent IPv6
packet datapath merely because that datapath is not yet implemented.
