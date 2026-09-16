## Application Process and Worker Model

Named EIGRP is modeled as a parent process with one or more address-family protocol contexts/workers.

`router eigrp <name>` creates the parent named process container. Removing that command destroys the parent process and all child address-family state.

Each configured `address-family <afi> [vrf <vrf>] autonomous-system <as>` creates an EIGRP address-family protocol context. When the runtime for that address family is implemented and enabled, that context owns its packet-processing worker/thread, topology table, neighbor table, packetizer queue, interface packet queues, timers, and reliable-transport state.

The address-family configuration object owns the binding to that runtime context. Creating an IPv4 named address family establishes the runtime binding through the EIGRP southbound lifecycle API after the host VRF has been resolved. Child configuration such as network, interface, topology, filter, and redistribution commands consumes this binding; those commands must not independently look up or create an EIGRP process. Removing the address family tears down the bound runtime before its retained configuration is freed. Removing the named parent first tears down every bound child runtime.

Named-mode configuration supports both IPv4 and IPv6 address-family contexts even when one address-family data path is not yet implemented. IPv6 configuration must therefore be accepted and retained now rather than waiting for the IPv6 packet/runtime implementation.

Configuration and route objects passed into common EIGRP code must identify their address family explicitly and contain normalized address/prefix data for that family. IPv4 and IPv6 should share common target functions wherever their protocol behavior is otherwise identical; the AF/type carried by the EIGRP object selects the correct data representation and later runtime behavior.

Until the IPv6 data path is implemented, IPv6 runtime target functions may return the EIGRP structured `not implemented` result while preserving the configured IPv6 state.

The same retention rule applies when an IPv4 runtime exists but a particular runtime action is not implemented yet: configuration is committed first, and the target may then report structured `not implemented` for the missing runtime behavior. Merely having a live runtime must not turn a previously retainable named command into a failed configuration transaction.

Management and runtime ownership are separate from the address-family worker model. `eigrp_cli.[c|h]` and `eigrp_vty.[c|h]` are FRR-facing user/management front ends. Retained configuration is committed through FRR management/YANG and applied to EIGRP through `eigrp_northbound.c`, which normalizes host values and invokes the real EIGRP target function. The CLI must not submit a management change and then directly duplicate that runtime mutation.

Once inside portable EIGRP code, address-family contexts and workers use EIGRP-owned data and APIs. Host runtime services are reached through `eigrp_southbound.[c|h]`, while FRR Zebra/RIB integration remains in `eigrp_zebra.[c|h]`. FRR/YANG/VTY/Zebra native objects do not become fields or parameters of the portable address-family worker contract.

Inbound packets are received by the parent EIGRP receive path and demultiplexed to the correct enabled address-family worker using:

- receiving VRF / socket context
- packet address family, IPv4 or IPv6
- receiving interface
- EIGRP header AS number

The AS number is mandatory for protocol acceptance. A packet whose AS does not match a configured enabled address-family context is discarded.

The local named process name is not carried on the wire. Therefore packet demux must not depend on the configured EIGRP name except as local ownership of the matching address-family context.

If multiple local named processes would create an ambiguous `{vrf, afi, as, interface}` receive context, configuration must reject it or the receive path must treat it as invalid.

### Runtime instance navigation

The protocol runtime/process context is represented by `eigrp_instance_t`. As process/thread lifecycle code is refactored, its human-navigation namespace should converge on `eigrp_instance_*`, with a corresponding `eigrp_instance.c/.h` module where that produces a clearer ownership boundary. Existing lifecycle code should not be renamed merely for aesthetics while the named address-family runtime binding is still being established.

The named router parent is configuration ownership, not an on-wire or worker identity. A configured `{name, AF, VRF, AS}` address-family must bind to an unambiguous runtime EIGRP instance/worker context. The pre-production lifecycle consolidation is tracked in `refactor-work.md`.

The current IPv4 binding deliberately reuses the existing FRR-backed runtime allocation/destruction machinery behind `eigrp_southbound.[c|h]`; it does not rename or broadly restructure the legacy low-level lifecycle functions. IPv6 remains configuration-only with no runtime binding until its data path is implemented.
