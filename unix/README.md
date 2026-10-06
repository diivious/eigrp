# Unix EIGRP host shim

`unix/` is the native macOS/Linux host peer for portable EIGRP development. It
implements the same public integration contract used by FRR and BIRD; it is not
an FRR compatibility layer and does not reimplement protocol behavior.

The detailed Unix core/shim call-flow and ownership contract is defined in
`unix/specs/integration.md`. The complete host-neutral callable API and exposed
data structures are defined in `specs/public-api.md`.

The Unix runtime currently provides process runtime initialization/teardown,
monotonic time, immediate events, timers and cancellation, read/write readiness
scheduling, basic work queues, default-VRF resolution, and the portable logging
fallback used by the standalone runtime build. The scheduler uses POSIX threads,
`poll(2)`, and a private wake pipe. Unix file descriptors remain private to the
adapter and are associated with EIGRP instances through `eigrp_unix.h`.

The Unix shim also owns an in-memory host interface inventory with arbitrary
interfaces and IPv4/IPv6 addresses, normal create/delete/up/down/address lifecycle
notifications into portable EIGRP, and an unprivileged in-memory host RIB.
Interface addresses create normal connected/source routes in that RIB; learned
EIGRP routes are installed, replaced, and withdrawn through `eigrp_rib.h`. The
Unix RIB never modifies the macOS/Linux kernel routing table. Route walkers in
`eigrp_unix_rib.h`, interface walkers in `eigrp_unix_interface.h`, and the public
`eigrp_mgnt.h` interface/neighbor/topology walkers provide observable state for
Unix tests and operational front ends.

The Unix shim also owns a shared logical control-plane segment model with N
attached UUT interfaces.  Segments are deliberately not Ethernet
simulators: they carry membership only, with no VLAN, switching, MAC learning,
ARP, or IPv6 ND behavior.

Protocol packet delivery over those segments is provided by `eigrp-wire`, a
local Unix-domain `SOCK_STREAM` broker. Each UUT has an independent broker
connection. The small versioned envelope carries segment/interface/AF/address
metadata, explicit frame lengths, and the unchanged EIGRP packet bytes, so message
boundaries are preserved over the byte stream on both macOS and Linux. Multicast fans out within one
segment and unicast is delivered only to the registered destination interface.
The broker forwarding function is the boundary reserved for later drop/delay/
duplicate/disconnect fault injection. Policy integration and full UUT
orchestration remain separate work.

Build the runtime portion without FRR or BIRD with:

```text
make build
```

Adapter-specific tests live under `unix/test/`; the root `make test` runs both
portable EIGRP tests and Unix runtime tests.


Run the broker with `unix/code/eigrp-wire [socket-path]`. Unix EIGRP processes use `EIGRP_UNIX_WIRE_SOCKET` when a non-default socket path is required.
