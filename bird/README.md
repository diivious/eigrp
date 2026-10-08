# BIRD/BSD host integration

OpenEIGRP is designed to run within different routing stacks without changing
its EIGRP protocol behavior. `bird/` is the reserved integration area for a
BIRD/BSD adapter. **Alpha 144 does not include an operational BIRD adapter.**
This directory is not evidence of BIRD interoperability or production readiness.

## Integration boundary

The portable engine lives in [`../eigrp/code/`](../eigrp/code/). A BIRD
integration must supply its own native bindings for configuration, interface
and address discovery, event scheduling, timers, sockets, routing-table
interaction, logging, and operational visibility. BIRD data structures and
lifecycle objects must remain inside this host adapter; the portable core uses
only EIGRP-owned APIs and result types.

| Interface | Role for a BIRD adapter |
|:--|:--|
| `eigrp.h` | Common EIGRP types and protocol identities |
| `eigrp_sys.h` | Host-provided runtime, packet, and system services |
| `eigrp_rib.h` | Routing-table and route-notification boundary |
| `eigrp_cli.h` | Configuration operations into the portable core |
| `eigrp_mgnt.h` | Operational state and management access |

These public headers are in `../eigrp/code/`. They are not BIRD-specific APIs.

## Implementation and qualification

BIRD integration should use BIRD/BSD-native build, runtime, and test mechanisms.
Do not import FRR objects, CLI types, event-loop assumptions, or test fixtures
into the adapter or the portable engine. The FRR implementation can illustrate
where responsibilities are divided, but it does not define the BIRD contract.

Adapter and integration tests belong under [`test/`](test/README.md). Common
protocol and UUT tests remain under `../eigrp/test/`. Before claiming host
readiness, validate interface lifecycle, route installation/withdrawal, packet
I/O, neighbor recovery, IPv4/IPv6 behavior, configuration handling, and
interoperability in the intended BIRD environment.

## Reference documents

- [Platform integration](../specs/platform-integration.md) — host/core boundary.
- [Public API](../specs/public-api.md) — EIGRP-owned callable contracts.
- [Architecture](../specs/architecture.md) — portable engine and adapters.
- [FRR adapter](../frr/README.md) and [Unix adapter](../unix/README.md) —
  host-specific examples, not implementation dependencies.
