# OpenEIGRP

[![OpenEIGRP Core CI](https://github.com/diivious/eigrp/actions/workflows/core.yml/badge.svg)](https://github.com/diivious/eigrp/actions/workflows/core.yml)

OpenEIGRP is a portable implementation of the Enhanced Interior Gateway Routing Protocol defined by RFC 7868. OpenEIGRP is the project name; EIGRP remains the protocol name used by the wire protocol, CLI, data structures, and source APIs.

RFC 7868 is the primary protocol reference for this implementation. Project design decisions are made to remain consistent with that behavior, while FRR, Unix, BIRD, and other routing stacks are treated as host and integration environments rather than sources of protocol semantics.

## Project state

The repository contains a portable EIGRP protocol core, an FRR integration, and a standalone Unix host used for development and protocol qualification. The `bird/` tree defines the BIRD/BSD integration area; BIRD host integration is not implemented in the current tree.

The portable core owns protocol state and decisions for:

- EIGRP instances, address families, interfaces, and neighbors;
- DUAL state transitions, feasibility-condition evaluation, feasible-successor selection, successor selection, and ACTIVE processing;
- classic and wide metrics, variance, maximum paths, metric weights, offset lists, and summary metrics;
- topology descriptors and route selection;
- UPDATE, QUERY, REPLY, SIA-QUERY, and SIA-REPLY processing;
- reliable transport, sequencing, acknowledgements, retransmission, conditional receive, and packetization;
- IPv4 and IPv6 protocol addressing through EIGRP-owned address/prefix types;
- route filtering, redistribution, default information, summaries, static neighbors, and prefix limits;
- MD5 and HMAC-SHA-256 authentication;
- graceful restart / NSF protocol processing;
- event logging and operational state exposed through portable management APIs.

Classic IPv4 configuration and named-mode configuration terminate at the same portable feature targets. Named mode supports IPv4 and IPv6 address-family configuration, `af-interface`, `topology base`, multiple autonomous-system contexts under one named parent, and case-sensitive named parents.

EIGRP Stub runtime behavior is outside project scope.

## Start here by role

| Role | Read first | Then |
|---|---|---|
| Contributor | `CONTRIBUTING.md` | `specs/architecture.md`, `specs/code-conventions.md` |
| Operator / CLI user | `specs/EIGRP-Config-Guide.md` | `tools/README.md` |
| Platform integrator | `specs/platform-integration.md`, `specs/public-api.md` | public headers under `eigrp/code/` and the platform README |
| Portable protocol developer | `specs/architecture.md` | `eigrp/specs/dual.md`, `eigrp/specs/route-selection-spec.md`, `eigrp/specs/rtp.md`, `specs/rfc7868.md` |
| FRR adapter developer | `frr/README.md`, `frr/patch/README.md` | `specs/platform-integration.md`, `specs/EIGRP-Config-Guide.md` |
| Unix host developer | `unix/README.md`, `unix/specs/integration.md` | `specs/public-api.md` |

## Repository layout

```text
eigrp/
  eigrp/
    code/          Portable EIGRP implementation
    specs/         Portable protocol/module specifications
    test/          Portable protocol and UUT tests
  frr/
    code/          FRR adapters and integration
    patch/         Managed FRR-wide patches
    specs/         FRR-specific integration specifications
    test/          FRR-native integration/UUT material
  bird/
    code/          BIRD/BSD adapter area
    specs/         BIRD/BSD integration specifications
    test/          BIRD/BSD-native test area
  unix/
    code/          Standalone Unix host implementation
    specs/         Unix-host integration specification
    test/          Unix-host tests
  specs/           Repository-wide architecture, public API, and operator contracts
  tools/           Build, staging, patch, UUT, and packaging helpers
  CONTRIBUTING.md Contribution, style, AI, and test rules
```

The repository tree is the source of truth. Platform staging projects portable code and platform adapters into the host build tree; staged copies are not development sources.

## Architecture

OpenEIGRP has three logical layers:

```text
+-------------------------------------------------------------+
| Host routing stack                                          |
| CLI/config, RIB, interfaces, sockets, timers, policy, logs  |
+------------------------------+------------------------------+
                               |
                               v
+-------------------------------------------------------------+
| Host adapter                                                 |
| FRR, Unix, BIRD/BSD, or another platform                    |
| normalizes host state and implements EIGRP host services    |
+------------------------------+------------------------------+
                               |
                               v
+-------------------------------------------------------------+
| Portable EIGRP core                                          |
| instances, neighbors, DUAL, metrics, topology, RTP, TLVs    |
+-------------------------------------------------------------+
```

The governing rule is:

> If equivalent normalized inputs must produce the same EIGRP decision on every host, that decision belongs in the portable core.

Host adapters provide mechanisms. Portable code owns protocol decisions.

### Public integration boundary

The primary integration contracts are:

```text
eigrp/code/eigrp.h
eigrp/code/eigrp_cli.h
eigrp/code/eigrp_mgnt.h
eigrp/code/eigrp_rib.h
eigrp/code/eigrp_sys.h
```

Configuration and administrative requests enter through EIGRP-owned semantic targets. Portable code requests host services through EIGRP-owned southbound contracts. FRR, BIRD, Zebra, VTY, YANG, routing-table, event-loop, timer, socket, and other host-native objects do not cross into portable protocol APIs.

## Protocol behavior and feature ownership

Every configuration or operational feature terminates at the module that owns the behavior. CLI and YANG layers parse and normalize values; they do not implement protocol semantics.

Typical ownership is:

```text
neighbor lifecycle/authentication  -> neighbor/authentication modules
DUAL state and ACTIVE processing   -> FSM/DUAL modules
metric/variance/weights            -> metric module
route selection                    -> topology/route-selection modules
packetization/RTP                  -> packetizer/packet modules
summaries                          -> summary module
filtering/offset lists             -> filter module
redistribution                     -> redistribution module
event history                      -> event-log module
host RIB operations                -> eigrp_rib.h boundary
```

Portable production code uses structured EIGRP result codes and contains no `EIGRP_RESULT_NOT_IMPLEMENTED` return path. Unsupported runtime capabilities return the appropriate structured result without substituting placeholder behavior.

Human-navigation naming follows:

```text
eigrp_<module>.c
eigrp_<module>_<object>_<action>()
```

See `specs/code-conventions.md` before adding or renaming public symbols.

## Configuration models

OpenEIGRP exposes two configuration models through the host adapter.

### Classic IPv4

Classic configuration uses `router eigrp <asn>` and includes process, network, neighbor, metric, filtering, redistribution, interface timer/authentication, and summary controls.

### Named mode

Named mode uses `router eigrp <name>` with address-family, `af-interface`, and `topology base` scopes. The named parent is local configuration identity; autonomous-system identity belongs to the address-family context.

Named mode supports:

- IPv4 and IPv6 address-family configuration;
- multiple autonomous-system contexts under one named parent;
- distinct case-sensitive named parents;
- network participation and static neighbors;
- router ID and neighbor controls;
- interface bandwidth/delay, timers, authentication, passive mode, next-hop-self, split horizon, and summaries;
- variance, maximum paths, active timer, default information, default metric, administrative distance, prefix limits, metric/traffic controls, event-log sizing, distribute lists, offset lists, redistribution, and summary metrics.

The authoritative command and writeback contract is `specs/EIGRP-Config-Guide.md`.

## Authentication

Portable authentication supports:

- no authentication;
- MD5;
- HMAC-SHA-256.

Authentication policy, TLV validation, digest validation, and replay handling are owned by portable EIGRP code. Host key-chain/configuration objects are normalized at the platform boundary.

Encoded type-7 configuration text is retained as configuration where exposed by the host but is not treated as HMAC key material. A runtime that cannot obtain plaintext key material reports the capability as unsupported rather than authenticating with encoded text.

## Diagnostic event log

Each EIGRP runtime owns a bounded diagnostic event ring. Event entries contain a timestamp, one address/prefix, and four scalar arguments; they do not retain pointers to mutable protocol objects.

Events are emitted at the operation that performs the decision or state mutation. DUAL state changes are logged at the exact assignment site so the event history records what the implementation actually committed.

The event log covers the protocol path needed for field diagnosis, including:

- DUAL state transitions;
- metric commits, feasible-successor search, FC satisfied/not-satisfied decisions, FD/RD-related selection context, ACTIVE peer counts, and committed successor changes;
- UPDATE, QUERY, REPLY, SIA-QUERY, and SIA-REPLY activity;
- route packetization and poison-reverse decisions;
- packet receive/transmit, packet rejection reasons, transmit failures, and route-encoding failures;
- RTP acknowledgements, unexpected acknowledgements, retransmissions, and retry-limit failures;
- neighbor state changes and explicit down reasons;
- peer capability/codec selection changes;
- interface protocol-state changes and multicast participation failures;
- graceful-restart/NSF lifecycle events;
- summary and redistribution decisions;
- filtering, offset-list metric changes, and prefix-limit rejections;
- route updates received from the host RIB;
- route install/withdraw requests and southbound results;
- topology route deletion.

The ring tracks stored entries, total writes, and overwritten history so operators can determine whether an incident exceeded the configured history depth. Event-log size is configurable and the log can be cleared operationally.

The event log is diagnostic evidence, not a trace of every successful routine operation. High-frequency events that do not change protocol behavior remain in normal debugging/statistics paths so large convergences do not immediately erase the useful failure history.

## Specifications

Repository-wide contracts live under `specs/`; portable protocol internals live under `eigrp/specs/`.

- `specs/architecture.md` — ownership and portability boundaries.
- `specs/code-conventions.md` — naming and source conventions.
- `specs/platform-integration.md` — host integration contract.
- `specs/public-api.md` — host-neutral callable API and data structures.
- `specs/EIGRP-Config-Guide.md` — configuration and operational command contract.
- `specs/rfc7868.md` — RFC authority and project interpretation rules.
- `eigrp/specs/dual.md` — DUAL state-machine design.
- `eigrp/specs/route-selection-spec.md` — topology path ordering and successor selection.
- `eigrp/specs/rtp.md` — packetization and reliable transport design.

## Build and test

### Portable/Unix build

From the repository root:

```sh
make
```

The root build compiles the portable EIGRP core and Unix host support without requiring an FRR checkout, BIRD checkout, VM, root privileges, or host VLAN configuration.

### Portable qualification

```sh
make test
```

The root test gate includes portable compile/build validation, portable Python/UUT tests, Unix-host tests, IPv4 and IPv6 core scenarios, RTP qualification, and topology scenarios.

Useful targets include:

```sh
make portable-build
make portable-test
make uut
make ipv4-basic-core
make ipv6-basic-core
make rtp-core
make core-topology
make mesh-topology
```

Run:

```sh
make help
```

for the complete target list.

GitHub Actions runs the root portable gate on Linux and macOS.

## FRR integration

Keep the OpenEIGRP and FRR repositories as sibling checkouts when practical:

```text
~/devel/eigrp/
~/devel/frr/
```

Stage the current source into FRR with:

```sh
tools/frr.sh --install --frr-root ../frr
```

The projection is:

```text
eigrp/code/ + frr/code/  -> ../frr/eigrpd/
frr/test/                 -> ../frr/tests/eigrpd/
```

FRR-wide changes are managed patches under `frr/patch/`. Apply them explicitly:

```sh
tools/frr.sh --patch --frr-root ../frr
```

Patch application is idempotent and stops on source drift rather than fuzzing or forcing a patch.

Build an already configured FRR tree with:

```sh
tools/frr.sh --build --frr-root ../frr
```

Run the complete configure/build/check gate with:

```sh
tools/frr.sh --all --frr-root ../frr
```

Run the live FRR UUT with:

```sh
tools/frr.sh --uut --frr-root ../frr
```

`tools/frr-uut.sh` provides aggregate and remote UUT execution. See `frr/README.md` and `tools/README.md` for the integration workflow.

## Unix host

The `unix/` tree is the native macOS/Linux host for portable development and protocol qualification. It implements the same public integration contract used by routing-stack adapters without importing FRR behavior into the portable core.

The Unix host provides the event/timer, interface, packet-wire, RIB, and host-service mechanisms required by the standalone build and UUT scenarios. See `unix/README.md` and `unix/specs/integration.md`.

## BIRD/BSD integration

The `bird/` tree is the BIRD/BSD integration boundary. BIRD-specific configuration, protocol, channel, table, interface, event-loop, timer, socket, and routing objects remain confined to that tree and must implement the same EIGRP-owned public contracts used by other hosts.

The current repository does not contain a BIRD runtime implementation. Host-independent protocol behavior and tests remain under `eigrp/`.

## Debugging

For FRR daemon debugging, stop any service-managed or manually started EIGRP daemon before launching another instance. From the FRR checkout:

```sh
sudo gdb eigrpd/.libs/eigrpd
```

Useful operational commands include:

```sh
sudo vtysh -d eigrpd -c 'show running-config'
sudo vtysh -d eigrpd -c 'show ip eigrp topology'
sudo vtysh -d eigrpd -c 'show ip eigrp neighbors'
```

Use the event-log operational commands defined in `specs/EIGRP-Config-Guide.md` when diagnosing protocol decisions, convergence, adjacency loss, packet rejection, RTP failure, or RIB discrepancies.

## Contribution rules

Preserve existing copyright, SPDX, and author history. New files use the copyright of the person who wrote them.

Do not add alias wrappers, duplicate old/new paths, compatibility layers, or host-specific protocol logic without an approved architectural reason.

See `CONTRIBUTING.md` for contribution workflow, AI use, style, test requirements, and diff hygiene.

## Security

Security reports may be sent to:

```text
diivious [at] hotmail.com
```
