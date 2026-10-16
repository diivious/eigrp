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
through EIGRP requires an explicit `redistribute: [static]` on the originating
router; source routes are never automatically treated as connected networks.

Scenario operations in v1 are `start`, `stop`, `link-up`, `link-down`, `links-up`,
`links-down`, `wait`, `packet-mark`, `validate-neighbor`, `validate-route`,
`validate-packet`, `validate-convergence`, `source-route-add`,
`source-route-remove`, `redistribute-add`, and `redistribute-remove`.
`links-up` / `links-down` issue a
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

Packet assertions must specify `event` explicitly: `sent` proves the router
emitted the packet, `delivered` proves the broker passed it to a recipient,
`dropped` proves a test fault blocked the delivery, and `any` is permitted only
for zero-packet checks (`min: 0, max: 0`). In particular, a dropped packet
cannot satisfy a positive `event: delivered` assertion. For QUERY suppression,
`event: any` includes attempts that faults subsequently drop, unlike checking
only delivered packets. Scenario field names and packet phase marks are
validated strictly; a misspelled field, undefined mark, or empty convergence
list is a failing test rather than a weakened assertion.

The read-only virtual-wire decoder can filter the destination prefix for IPv4
and IPv6 classic and MP route TLVs. Its IPv6 extractor is verified against
actual portable C encoder outputs, including malformed-packet negative vectors.
The IPv6 three-router gate uses routable documentation prefixes as passive
connected destinations; link-local addresses are only used for adjacencies and
interface-scoped next hops. The gate validates IPv6 prefix-specific UPDATE,
QUERY, and REPLY delivery, route withdrawal and recovery, Passive DUAL
successors, and rejection of learned link-local destinations. IPv6 global
connected origins require an explicitly configured passive interface;
this cannot create neighbor relationships using global source addresses.

## Medium-severity qualification guards

`validate-convergence` can specify `stable_for` (seconds) to require **every**
neighbor and route expectation to remain true continuously over the specified
observation interval. Any failed poll resets the interval. This is different
from a one-time successful snapshot and is applied to the basic IPv4, diamond,
mesh, and two-router recovery gates. `timeout` includes the stable interval.

`checkpoint: <phase-name>` captures each running UUT's raw `STATE` and `EVENTS`
responses and the incremental broker packet journal **at that phase**. In `--report` mode, every existing `packet-mark` also creates an automatic
phase checkpoint, even for scenarios that do not declare explicit checkpoints.
Reports include all phase checkpoints before final diagnostics, preserving earlier
evidence even when a subsequent full (500-entry) event ring wraps. Checkpoints
never resize the production ring and do not replace positive route, neighbor,
and packet assertions. Use checkpoints after convergence and at failure/recovery
boundaries in tests with many DUAL transitions.

The two-router framework smoke now has real qualification assertions: each
router must learn the other's remote connected prefix, both destinations must
be withdrawn when both transit interfaces go down, and both routes, Passive
DUAL state, Up adjacencies, and empty neighbor RTP queues must recover for a
bounded stable interval. It is still a basic smoke, not a DUAL multipath gate.

The portable Python suite includes tests in several distinct categories:
**source guards** inspect C structure and API boundaries; **codec-vector-C**
tests compile and execute production C codec functions; **RTP-C** runs the
production neighbor timing code with a deterministic fake monotonic clock;
**UUT-multiprocess** tests exchange actual EIGRP packets between isolated
routers through the virtual wire. FRR/BIRD host integration is **not** covered
by `make uut` and must be tested in their respective platform trees. IPv4
TLV1/TLV2 tests now include C encoder/decoder round trips, malformed/truncated
input rejections, and wire-decoder cross-validation in addition to the
previous independently written Python layout fixtures.

The runner automatically fails on unexpected virtual-wire `send-error` events
reported by the broker, even if every explicit YAML route and neighbor check
passes. At a scenario's **terminal** `stop`, the runner closes the virtual-wire
broker and checks its journal **before** stopping the router processes. This
prevents orderly sequential teardown from creating an artificial socket-write
failure against an already stopped UUT. Genuine send errors generated while
the broker was live, including any discovered while it closes, still fail.
The same broker-before-routers ordering applies to implicit cleanup after a
test failure. Nonterminal stop/restart steps leave the broker running. A regression injects such an event into the journal and requires a
failed result. Colliding attached/transit interface names also fail during
YAML schema validation, before routers are started.

Diagnostic triage: `Packet transmit failure` for an unattached synthetic
interface's multicast HELLO is a known fixture limitation, **not** a blanket
allowance for transmit failures on connected test links. Tests must check
unexpected adjacency-reset reasons (for example, peer restart or retry-limit)
and any send-error events seen on live broker segments. Deliberately injected
packet drops/rejections must be tied to the named scenario fault and counted
rather than accepted as arbitrary packet-loss warnings. The phase checkpoints
expose these records for inspection; checkpointing alone does not constitute
a global unexpected-event failure policy.

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

## Three-router redistribution qualification

`make uut` automatically discovers the scenarios under `eigrp/test/redistribution/`.
They run three independent EIGRP routers on the R1--R2--R3 line topology,
using the actual portable protocol implementation and Unix RIB notifications.
`source-route-add` / `source-route-remove` modify a static route in the UUT
host RIB; `redistribute-add` / `redistribute-remove` invoke the real portable
redistribution target. They are separate operations: disabling redistribution
must not delete the static route from the source RIB.

Both scenarios verify that the route propagates to the far-end R3 and is
withdrawn from R2 and R3 before the test can pass. These end-to-end tests
complement, but do not replace, fast direct portable FSM unit tests.

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

## MD5 mixed-authentication LAN regression

Run the three-router authentication isolation test with:

```
make -C eigrp/test/uut md5-mixed-lan
```

R1 and R2 share a LAN and authenticate their EIGRP packets with an MD5
keychain containing the **test-only** secret `eigrprocks`. R3 is on the same
segment and runs EIGRP without authentication. The scenario requires R1/R2
to form a neighbor relationship, exchange their distinct attached prefixes,
and converge Passive, while R3's Hello packets are delivered but never
accepted as an adjacency and no R3 prefixes enter R1/R2's RIBs.

The Unix shim owns the in-memory keychain provider in
`unix/code/eigrp_unix_keychain.c` and implements the existing
`eigrp_sys_auth_key_lookup()` southbound contract. The test node only registers
its configured key with `eigrp_unix_keychain_add()`; neither the key nor an
authentication override is built into the UUT executable. Unregistered
keychains fail closed. The portable authentication implementation is unchanged.

The topology schema accepts optional per-interface `authentication` mappings,
e.g. `authentication: {mode: md5, keychain: md5-lan, key: eigrprocks}`.
Negative neighbor expectations may specify `state: down, observe: N` to assert
that no adjacency forms for the *whole* N-second interval, rather than only
at one instant. These cases are also discovered by `make uut`.

## RTP Core Gate

Run the standalone reliable-transport qualification from the repository root:

```
make rtp-core
```

The gate runs the versioned RTP fault scenarios under `eigrp/test/rtp/`,
including SRTT/RTO sampling, lost reliable-packet retransmission, lost ACK/retry
handling, and reliable-multicast peer retry behavior. The same unprivileged Unix
wire/UUT framework is used; no host networking setup or platform VM is required.

Root `make test` runs this RTP gate as part of the discovered UUT scenarios, without rerunning separate basic-core or RTP targets.

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

## Pre-commit test runner reliability

`make uut` runs portable pytest and each discovered YAML UUT scenario once.
`make test` invokes the same gate (it does not repeat pytest or rerun the
IPv4/IPv6/RTP scenarios). Individual scenario targets remain available.

`eigrp/test/uut/run_all.py` enforces a **per-scenario wall-clock watchdog**
(default: 180 seconds). A stalled scenario is terminated with its UUT process
group, recorded as `TIMEOUT` with exit status 124, and its captured runner
output is written to `eigrp/test/uut/logs/<scenario>.log`. Subsequent scenarios
still run; the suite exits nonzero if any scenario failed or timed out. The
watchdog is a safety net, not permission to relax YAML convergence timeouts.

Override the timeout in seconds with `EIGRP_UUT_SCENARIO_TIMEOUT=240 make uut`
or invoke `./tools/python eigrp/test/uut/run_all.py --scenario-timeout 240`.
Values must be finite and strictly positive. A router's command output is
consumed by a dedicated reader thread; both the initial response and every
subsequent line must arrive before the command's single deadline. Incomplete
`STATE`/`EVENTS` snapshots now fail with a timeout rather than hanging forever.
