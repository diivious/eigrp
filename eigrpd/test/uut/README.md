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

Every `eigrp-uut-test/v1` document names its topology with the `topology` key. The aggregate runner recursively discovers these scenario documents under `eigrpd/test/`; an invalid or missing topology is a gate failure rather than silently dead coverage.

The aggregate runner writes one durable report per scenario under `eigrpd/test/uut/logs/<test-case-name>.log`. Each report records the scenario name/path, exact runner command, a repository-root rerun command, exit status, verbose scenario operations, `show eigrp tech-support` state, `show eigrp events` state, complete UUT debug/stderr output, self-describing management records, traffic counters, the complete packet journal since the most recent scenario packet mark, explicit fault-match records, and the final 20 packet events for trailing context. The aggregate run continues after a scenario failure so every scenario receives a report; its final exit status is nonzero when any scenario failed. `make clean` removes generated UUT logs.

Run a scenario from the repository root:

```
python3 eigrpd/test/uut/run.py \
  eigrpd/test/uut/examples/two-router.yaml \
  eigrpd/test/uut/examples/two-router-neighbor-route.yaml
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

Scenario operations in v1 are `start`, `stop`, `link-up`, `link-down`, `wait`,
`packet-mark`, `validate-neighbor`, `validate-route`, `validate-packet`, and
`validate-convergence`. Packet marks let later phases require packets emitted after a
specific failure/recovery boundary.  `validate-convergence` contains declarative neighbor
and/or route expectations and polls until all are true or the timeout expires.

Failures print the failing UUT and expectation plus a diagnostic snapshot:
interfaces, neighbors, topology, Unix RIB, traffic counters, the packet journal since the latest mark, explicit fault matches, and trailing packet context. Diagnostic state records use `key=value` fields so every value is self-describing.

## IPv4 Basic Core Gate

Run the authoritative three-router portable IPv4 core qualification from the
repository root:

```
make -C eigrpd/test/uut ipv4-basic-core
```

The gate covers HELLO/adjacency startup, reliable UPDATE/ACK delivery, end-to-end
route propagation and metrics, Unix RIB state, DUAL Passive state, R2-R3 link
failure with QUERY/REPLY convergence, and adjacency/route recovery.

## RTP Core Gate

Run the standalone reliable-transport qualification from the repository root:

```
make rtp-core
```

The gate runs the versioned RTP fault scenarios under `eigrpd/test/rtp/`,
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
make -C eigrpd/test/uut ipv6-basic-core
```

The IPv6 topology uses link-local addresses for EIGRP neighbor identity and
next-hop validation.  Its synthetic N1/N2 endpoint prefixes are distinct /128
link-local fixtures so the current connected-interface model can exercise the
same route propagation, DUAL, failure, QUERY/REPLY, withdrawal, and recovery
path without adding an IPv6-only test mechanism.

Run both address-family gates, independently in sequence through the same UUT
framework, with:

```
make -C eigrpd/test/uut basic-core
```

The combined target attempts both gates even when the first reports a protocol
failure, then returns failure if either gate failed.  As with the IPv4 gate,
expectations are not weakened to hide an authority-code defect.
