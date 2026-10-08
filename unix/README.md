# Standalone Unix host adapter

`unix/` is the native macOS/Linux host for developing and qualifying the
portable OpenEIGRP engine without embedding it in FRR or BIRD. It implements
host services and test-oriented facilities, **not an alternative EIGRP
protocol implementation**. Protocol decisions remain in `../eigrp/code/`.

## Runtime and host services

The Unix adapter provides process initialization and teardown, monotonic time,
immediate events and timers, cancellation, file-descriptor readiness, basic
work queues, default-VRF resolution, and standalone logging. Its scheduler
uses POSIX threads, `poll(2)`, and a private wake pipe; Unix descriptors stay
inside the adapter.

It also maintains a host-side interface inventory with IPv4/IPv6 addresses
and lifecycle notifications. An **unprivileged in-memory RIB** holds
connected/source routes and learned EIGRP routes; it does not change the
macOS or Linux kernel routing table. Interface, route, and management walkers
make the resulting state observable to tests and host front ends.

| Area | Implementation / entry point |
|:--|:--|
| Scheduling and host services | `code/eigrp_unix_sys.c`, `code/eigrp_unix.h` |
| Interfaces and lifecycle | `code/eigrp_unix_interface.c`, `code/eigrp_unix_interface.h` |
| In-memory routing table | `code/eigrp_unix_rib.c`, `code/eigrp_unix_rib.h` |
| Standalone configuration | `code/eigrp_unix_config.c` |
| Logical segments and packet transport | `code/eigrp_unix_segment.c`, `code/eigrp_unix_wire.c` |
| Wire broker | `code/eigrp_unix_wire_broker.c`, `code/eigrp-wire` |

## Logical segments and packet transport

A shared segment model attaches multiple units under test (UUTs) to a logical
control-plane network. **It is not an Ethernet simulator:** it does not model
VLANs, switching, MAC learning, ARP, or IPv6 neighbor discovery.

The `eigrp-wire` broker uses local Unix-domain `SOCK_STREAM` connections, one
per UUT. A versioned, length-delimited envelope transports segment,
interface, address-family, and endpoint metadata together with the unchanged
EIGRP packet. Multicast fans out to members of the same segment; unicast is
delivered only to the registered destination interface. The forwarding
boundary is intended to support controlled packet faults. Full UUT
orchestration and policy integration remain separate work.

## Build and test

From the repository root:

```sh
make build       # Portable engine and Unix host support
make test        # Portable and Unix tests
make uut         # UUT pytest and scenario suites
make help        # Available targets and descriptions
```

To run the broker directly, use:

```sh
unix/code/eigrp-wire [socket-path]
```

Set `EIGRP_UNIX_WIRE_SOCKET` for Unix EIGRP processes when using a non-default
broker socket path. Unix-specific tests live in [`test/`](test/); portable
tests live in `../eigrp/test/`.

## Architecture references

The authoritative Unix integration details and call flows are in
[`specs/integration.md`](specs/integration.md). The host-neutral API is in
[`../specs/public-api.md`](../specs/public-api.md); the cross-platform
integration contract is in
[`../specs/platform-integration.md`](../specs/platform-integration.md).

Unix-specific configuration, timer, socket, interface, and routing-table
objects must not appear in the portable EIGRP API. The FRR and BIRD adapters
have independent host implementations.
