# Portable EIGRP YAML UUT framework

This directory owns orchestration tests for portable `eigrpd` behavior.  Unix is
only the execution host: each router is an independent process linked with the
portable EIGRP implementation and the Unix implementation of the public host
contract.

Two versioned declarative schemas are accepted:

* `eigrp-uut-topology/v1` describes routers, interfaces, IPv4/IPv6 addresses,
  shared segments, attachments, and attached/source networks.
* `eigrp-uut-test/v1` describes ordered operations and validations.

Run every portable YAML UUT scenario through automatic discovery with:

```
make uut
```

Every `eigrp-uut-test/v1` document names its topology with the `topology` key. The aggregate runner recursively discovers these scenario documents under `eigrp/test/`; an invalid or missing topology is a gate failure rather than silently dead coverage.

The aggregate runner writes one durable report per scenario under `eigrp/test/uut/logs/<test-case-name>.log`. Each report records the scenario name/path, exact runner command, a repository-root rerun command, exit status, verbose scenario operations, `show eigrp tech-support` state, `show eigrp events` state, complete UUT debug/stderr output, self-describing management records, traffic counters, the complete packet journal since the most recent scenario packet mark, explicit fault-match records, and the final 20 packet events for trailing context. The aggregate run continues after a scenario failure so every scenario receives a report; its final exit status is nonzero when any scenario failed. `make clean` removes generated UUT logs.

Run a scenario from the repository root:

```
python3 eigrp/test/uut/run.py \
  eigrp/test/uut/examples/two-router.yaml \
  eigrp/test/uut/examples/two-router-neighbor-route.yaml
```

The runner builds `eigrp-uut-node` on demand.  No root privileges, network
namespaces, kernel routes, FRR, or BIRD are required.  A test-side broker uses
the existing Unix wire envelope to connect arbitrary shared segments and keeps
the scenario packet journal for validation/diagnostics.

Topology YAML is data only.  Interface names and router names are arbitrary;
segments may have any number of attachments.  `attached_networks` create
synthetic host interfaces on a router and participate in EIGRP like normal
connected networks.  `source_networks` create host-originated static-route fixtures in the Unix RIB.
They are observable with `validate-route: {table: source, ...}`; advertising them
through EIGRP still requires explicit redistribution configuration when that
scenario operation is added, rather than silently treating them as connected.

Scenario operations in v1 are `start`, `stop`, `link-up`, `link-down`, `links-up`,
`links-down`, `wait`, `packet-mark`, `validate-neighbor`, `validate-route`,
`validate-packet`, and `validate-convergence`. `links-up` / `links-down` issue a
batched physical-event transition to multiple UUT interfaces, which is useful for
full-mesh and shared-failure qualification. Packet marks let later phases require packets emitted after a
specific failure/recovery boundary.  `validate-convergence` contains declarative neighbor
and/or route expectations and polls until all are true or the timeout expires.

Topology interfaces may specify `delay` to drive the real EIGRP-owned interface-delay
configuration target after the address family and networks exist. `validate-route` also
supports `table: path` with `next_hop`, `interface`, `successor`,
`feasible_successor`, `distance`, and `rd` fields for per-neighbor topology paths.
`present: false` applies to the fully filtered row, so it can reject a specific stale
next hop while other paths for the prefix remain. `count` requires an exact number of
matching rows and is used to reject unexpected extra paths/successors.

`validate-packet` supports `min` and `max`. It also supports `group_by` plus
`max_per_group` (and `min_groups` / `max_groups`) for bounded fanout checks such as
"at most one QUERY per source/destination pair." Any upper-bound assertion observes
the complete requested timeout window instead of returning as soon as its minimum is met.
A negative assertion such as `max: 0`
observes the complete requested `timeout` window and fails if a matching packet appears
at any point. For route-bearing IPv4 packets, `prefix` restricts the match to packets
whose classic or multiprotocol internal-route TLV carries that destination. This allows
DUAL tests to prove, for example, that no QUERY for one prefix was emitted after a
packet mark without forbidding unrelated QUERY traffic.

## DUAL Diamond Gate

Run the reusable four-router DUAL route-selection qualification with:

```
make -C eigrp/test/uut dual-diamond
```

The first scenario qualifies feasible-successor immediate failover: R2 is R1's
preferred successor to N1, R3 satisfies `RD < FD` as a higher-cost feasible successor,
and loss of R1-R2 must switch forwarding to R3 while remaining Passive and emitting no
QUERY for N1. The scenario restores R1-R2 and verifies deterministic recovery.

Failures print the failing UUT and expectation plus a diagnostic snapshot:
interfaces, neighbors, topology, Unix RIB, traffic counters, the packet journal since the latest mark, explicit fault matches, and trailing packet context. Diagnostic state records use `key=value` fields so every value is self-describing.

## IPv4 Basic Core Gate

Run the authoritative three-router portable IPv4 core qualification from the
repository root:

```
make -C eigrp/test/uut ipv4-basic-core
```

The gate covers HELLO/adjacency startup, reliable UPDATE/ACK delivery, end-to-end
route propagation and metrics, Unix RIB state, DUAL Passive state, R2-R3 link
failure with QUERY/REPLY convergence, and adjacency/route recovery.

## RTP Core Gate

Run the standalone reliable-transport qualification from the repository root:

```
make rtp-core
```

The gate runs the versioned RTP fault scenarios under `eigrp/test/rtp/`,
including SRTT/RTO sampling, lost reliable-packet retransmission, lost ACK/retry
handling, and reliable-multicast peer retry behavior. The same unprivileged Unix
wire/UUT framework is used; no host networking setup or platform VM is required.

Root `make test` runs this gate after the IPv4 and IPv6 Basic Core Gates.

## Current authority-code observation

The IPv4 Basic Core Gate is the first scenario intended to qualify the complete
portable three-router core rather than only orchestration. Failures are reported
with per-UUT interface, neighbor, topology, RIB, and recent packet diagnostics;
the gate must not weaken expectations to accommodate a protocol defect.

In the `eigrp-alpha-45` authority tree, the gate currently reaches recovery but
fails the final route-relearning/Passive assertions after the R2-R3 link is
restored.  The gate intentionally reports that protocol defect as FAIL with the
remaining Active/stale topology state rather than accepting adjacency-only
recovery as convergence.

## IPv6 Basic Core Gate

The IPv6 gate is the address-family equivalent of the IPv4 three-router core
qualification and uses the same runner, topology schema, shared-segment broker,
scenario operations, packet journal, and state validators:

```
make -C eigrp/test/uut ipv6-basic-core
```

The IPv6 topology uses link-local addresses for EIGRP neighbor identity and
next-hop validation.  Its synthetic N1/N2 endpoint prefixes are distinct /128
link-local fixtures so the current connected-interface model can exercise the
same route propagation, DUAL, failure, QUERY/REPLY, withdrawal, and recovery
path without adding an IPv6-only test mechanism.

Run both address-family gates, independently in sequence through the same UUT
framework, with:

```
make -C eigrp/test/uut basic-core
```

The combined target attempts both gates even when the first reports a protocol
failure, then returns failure if either gate failed.  As with the IPv4 gate,
expectations are not weakened to hide an authority-code defect.

## ACTIVE/SIA qualification

`make active-topology` runs the advanced SIA scenarios under `active-topology/`.
Topology routers may set `active_time` in seconds; the UUT applies that value
through the portable `eigrp_timer_active_time_update()` configuration API.
Fault rules may also match `prefix`, limiting a packet action to route-bearing
packets that carry that decoded destination.  These are test-side controls only;
they do not alter protocol state or synthesize EIGRP behavior.
