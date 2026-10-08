# OpenEIGRP

[![OpenEIGRP Core CI](https://github.com/diivious/eigrp/actions/workflows/core.yml/badge.svg)](https://github.com/diivious/eigrp/actions/workflows/core.yml)

OpenEIGRP is a portable implementation of the Enhanced Interior Gateway Routing Protocol defined by RFC 7868. OpenEIGRP is the project name; EIGRP remains the protocol name used by the wire protocol, CLI, data structures, and source APIs.

RFC 7868 is the primary protocol reference for this implementation. Project design decisions are made to remain consistent with that behavior, while FRR, Unix, BIRD, and other routing stacks are treated as host and integration environments rather than sources of protocol semantics.

## Architecture and code overview

```text
+-----------------------------------------------------------+
| Host routing stack / operating environment                |
| Configuration, RIB, interfaces, sockets, timers, logging  |
+----------------------------+------------------------------+
                             |
+----------------------------v------------------------------+
| Host adapter (FRR / Unix / BIRD / other)                  |
| Normalization, management, and host-service implementation|
+----------------------------+------------------------------+
                             |
+----------------------------v------------------------------+
| Portable EIGRP protocol core                              |
| DUAL, topology, metrics, neighbors, RTP, packetizer, TLVs  |
+-----------------------------------------------------------+
```

**Architectural rule:** equivalent normalized inputs must yield the same EIGRP protocol decisions regardless of host. The portable core owns protocol behavior; adapters implement operating-system and routing-stack services through EIGRP-owned interfaces.
Portable APIs use EIGRP-owned types and structured results. FRR, BIRD, and Unix-specific objects remain in their respective adapters. The public integration contracts include `eigrp.h`, `eigrp_cli.h`, `eigrp_mgnt.h`, `eigrp_rib.h`, and `eigrp_sys.h` under `eigrp/code/`. See [architecture](specs/architecture.md), [platform integration](specs/platform-integration.md), and [public API](specs/public-api.md).

## Start here by role

| Role | Read first | Then |
|---|---|---|
| Contributor | `CONTRIBUTING.md` | `specs/architecture.md`, `specs/code-conventions.md` |
| Operator / CLI user | `specs/EIGRP-Config-Guide.md` | `tools/README.md` |
| Platform integrator | `specs/platform-integration.md`, `specs/public-api.md` | public headers under `eigrp/code/` and the platform README |
| Portable protocol developer | `specs/architecture.md` | `eigrp/specs/dual.md`, `eigrp/specs/route-selection-spec.md`, `eigrp/specs/rtp.md`, `specs/rfc7868.md` |
| FRR adapter developer | `frr/README.md`, `frr/patch/README.md` | `specs/platform-integration.md`, `specs/EIGRP-Config-Guide.md` |
| Unix host developer | `unix/README.md`, `unix/specs/integration.md` | `specs/public-api.md` |
| BIRD/BSD adapter developer | `bird/README.md` | `specs/platform-integration.md`, `specs/public-api.md` |

## Project State

### Cisco EIGRP / OpenEIGRP capability matrix

The following comparison presents the supplied Alpha 144 feature assessment. **Yes** indicates supported capability, **Partial** indicates incomplete coverage, and **No** indicates unavailable functionality. These are feature-status declarations, not a claim of completed production certification or identical host support. Platform-specific deployment readiness must be assessed separately.

| Feature | Cisco EIGRP | OpenEIGRP | Notes |
|:--|:--:|:--:|:--|
| **Address Families** | | | |
| IPv4 | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| IPv6 | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| IPv4/IPv6 named mode | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Multiple autonomous systems | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Multiple named instances | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| VRF / VRF-Lite | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> |  |
| **Core Protocol** | | | |
| DUAL FSM | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Feasibility Condition | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Feasible Successors | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Active/Passive states | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Query / Reply | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| SIA-Query / SIA-Reply | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Reliable Transport Protocol | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Reliable multicast | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Reliable unicast | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Conditional Receive (CR) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Neighbor discovery / adjacency | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Neighbor resynchronization | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Graceful Restart (GR) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| NSF awareness | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| NSF restarting router | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> |  |
| End-of-Table (EOT) signaling | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| **Metric and Path Selection** | | | |
| Classic composite metrics | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Wide metrics | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Configurable K-values | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Bandwidth / Delay metrics | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Reliability / Load metrics | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| MTU advertisement | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Unequal-cost load balancing | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Variance | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Maximum paths | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Maximum hops | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Administrative distance | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Loop-Free Alternate (LFA FRR) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | Planned |
| Add-Path | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | Under consideration |
| **Route Management** | | | |
| Internal routes | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| External routes | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Route redistribution | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Redistribute connected | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Redistribute static | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Redistribute OSPF | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Redistribute BGP | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Redistribute RIP / IS-IS | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Redistribution route maps | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Default metric | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| External route tagging | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Route tag filtering | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | Route-tag filtering (match tag) and modification (set tag) need to be implemented |
| Default route advertisement | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| **Summarization** | | | |
| Manual summarization | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Automatic summarization (IPv4) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Summary metric configuration | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Summary leak maps | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Summary discard / Null0 route | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | The summary implementation does not install a  local discard route,  |
| Summary administrative distance | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | Needs the Summary discard route support |
| **Policy and Filtering** | | | |
| Distribute lists | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Prefix lists | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Access lists | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Route maps | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Offset lists | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Per-interface filtering | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Inbound / outbound filtering | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| **Interface and Neighbor Controls** | | | |
| Passive interfaces | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Static neighbors | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Hello interval | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Hold interval | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Split horizon | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Next-hop-self | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Bandwidth percentage | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Interface delay configuration | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Neighbor descriptions | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Neighbor prefix limits | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | EIGRP MPLS VPN PE-CE features |
| Topology prefix limits | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | EIGRP MPLS VPN PE-CE features |
| Active timer | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Path Graph Feasibility | <span style="color: #c62828; font-weight: 700;">No</span> | <span style="color: #c62828; font-weight: 700;">No</span> | Planned |
| Peer Groups | <span style="color: #c62828; font-weight: 700;">No</span> | <span style="color: #c62828; font-weight: 700;">No</span> | Planned |
| **Security** | | | |
| MD5 authentication | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| HMAC-SHA-256 authentication | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | The digest primitives and packet integration already exist,  tbd is type-7 support, and interoperability tests |
| Key chains | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Key rollover | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | Runtime selection not fully established |
| **Fast Convergence** | | | |
| BFD integration (IPv4) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | [BFD Design](specs/feature-bfd.md) |
| BFD integration (IPv6) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | [BFD Design](specs/feature-bfd.md) |
| SIA detection | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Graceful neighbor recovery | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Nonstop Forwarding (NSF) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | Requires host support |
| **Advanced / Specialized** | | | |
| EIGRP Stub | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | No plans |
| EIGRP Over the Top (OTP) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | [OTP Design](specs/feature-otp.md) |
| EIGRP MANET | Legacy/specialized | <span style="color: #c62828; font-weight: 700;">No</span> | No plans |
| Multi-Topology Routing (MTR) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | No plans |
| DMVPN integration | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | OpenEIGRP can support DMVPN, but the host must provide the underlying tunnel and NHRP functionality |
| EIGRP MPLS VPN PE-CE | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | No plans |
| EIGRP 6PE / 6VPE | Platform-dependent | <span style="color: #c62828; font-weight: 700;">No</span> | No plans |
| EIGRP Site of Origin (SoO) | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #c62828; font-weight: 700;">No</span> | EIGRP MPLS VPN PE-CE features |
| **Configuration and Operations** | | | |
| Classic IPv4 configuration | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Named-mode configuration | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| IPv6 named-mode configuration | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Configuration writeback | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Show interfaces | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Show neighbors | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Show topology | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Show protocol statistics | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Show traffic counters | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Debug packet / neighbor / DUAL | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> | minimal logs |
| Event logging | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| DUAL state transition logging | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Clear neighbors | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Clear topology | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| Clear event logs | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #16803c; font-weight: 700;">Yes</span> |  |
| SNMP EIGRP MIB | <span style="color: #16803c; font-weight: 700;">Yes</span> | <span style="color: #1769c2; font-weight: 700;">Partial</span> | Conditional on host SNMP |

### Implementation and integration status

The portable core implements EIGRP protocol state and decisions, including DUAL, topology and metric selection, neighbor management, reliable transport, packet encoding, and IPv4/IPv6 addressing. Classic IPv4 and named-mode configuration use portable feature targets. EIGRP Stub runtime behavior is intentionally outside project scope.

FRR integration and the standalone Unix host are present. The `bird/` tree currently defines an integration boundary rather than a working BIRD runtime. The feature matrix must not be interpreted as proof that every capability is operational on every host.

### Qualification and deployment readiness

The capability matrix describes implementation scope, not a platform-wide production qualification result. The repository provides automated portable and host-specific test paths, but deployment evaluation must distinguish protocol feature coverage, regression test results, interoperability with other EIGRP implementations, and qualification of each host adapter on the target appliance. The build and test section below identifies available test entry points; no blanket production certification is asserted.

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

### Source areas

| Source area | Responsibility |
|:--|:--|
| [`eigrp/code/`](eigrp/code/) | Portable protocol implementation and public EIGRP APIs |
| [`eigrp/specs/`](eigrp/specs/) | DUAL, route selection, RTP, and other protocol-module designs |
| [`frr/`](frr/README.md) | FRR routing-stack adapter and integration |
| [`unix/`](unix/README.md) | Standalone Unix host and portable qualification |
| [`bird/`](bird/README.md) | BIRD/BSD integration area |
| [`specs/`](specs/) | Cross-platform architecture, integration, and configuration contracts |
| [`tools/`](tools/README.md) | Build, staging, and test orchestration |

The repository tree is the source of truth. Platform staging projects portable code and platform adapters into the host build tree; staged copies are not development sources.


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

Portable code uses structured EIGRP result codes. Where a feature remains incomplete, its real feature target retains configuration and reports `EIGRP_RESULT_NOT_IMPLEMENTED` rather than simulating support in a CLI or adapter.

Human-navigation naming follows:

```text
eigrp_<module>.c
eigrp_<module>_<object>_<action>()
```

See `specs/code-conventions.md` before adding or renaming public symbols.

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

## Contribution rules

Preserve existing copyright, SPDX, and author history. New files use the copyright of the person who wrote them.

Do not add alias wrappers, duplicate old/new paths, compatibility layers, or host-specific protocol logic without an approved architectural reason.

See `CONTRIBUTING.md` for contribution workflow, AI use, style, test requirements, and diff hygiene.

## Security

Security reports may be sent to:

```text
diivious [at] hotmail.com
```
