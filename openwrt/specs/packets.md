# OW-06: Native OpenWrt EIGRP packet transport

Copyright (C) 2026 Donnie V. Savage

## Ownership and scope

`openwrt/code/eigrp_owrt_packet.c` implements the OpenWrt-owned Linux transport
for IP protocol **88**. The common engine retains authority over RFC 7868
packet encoding/decoding, checksums, authentication, RTP and neighbor state.
All OpenWrt-private symbols use the `eigrp_owrt_` prefix. The exported
`eigrp_sys_*` functions implement the already-established public host ABI.
No Linux socket, `msghdr`, packet-info, interface, or netlink type crosses
`eigrp_sys.h`.

### Component topology

```text
                          EIGRP-owned public contracts
+--------------------------------------------------------------------------+
|                        eigrp/code/ (RFC 7868)                              |
| neighbor FSM  RTP / packetizer   TLVs   checksums   packet receive thread  |
+------------------------------------+-------------------------------------+
                                     | eigrp_sys_packet_{send,receive}
                                     | eigrp_sys_socket_{open,close}
                                     | eigrp_sys_multicast_{join,leave}
                                     v
+--------------------------------------------------------------------------+
| openwrt/code/eigrp_owrt_packet.c                                           |
| per-instance IPv4/IPv6 raw fd | sendmsg pktinfo | recvmsg pktinfo/framing  |
| memberships per instance/ifindex | default-VRF normalized metadata        |
+-------------------------+-------------------------------+----------------+
                          |                               |
              private FD binding                    raw IP protocol 88
                          v                               v
            eigrp_owrt_runtime.c                 Linux IPv4 / IPv6 stack
             OW-04 poll readiness                 multicasts + unicast
                          ^                               ^
                          |                       interface index / source
                          +-------------------------------+
                            eigrp_owrt_interface.c (OW-05)
```

## Socket lifecycle

1. The host starts the OW-04 scheduler before any EIGRP instance is created.
2. `eigrp_sys_socket_open(instance)` creates a nonblocking, close-on-exec raw
   socket for the instance's address family and protocol 88. IPv4 enables
   `IP_PKTINFO`; IPv6 enables `IPV6_RECVPKTINFO`.
3. The socket adapter binds its fd to the OW-04 one-shot readiness scheduler via
   `eigrp_owrt_runtime_fd_set()`. Only after this succeeds is the socket added
   to the instance registry. The EIGRP instance creator registers its existing
   `eigrp_sys_read_add()` callback once the instance is published.
4. Multicast memberships are added/removed on the *instance socket*, keyed by
   interface index; 224.0.0.10 (IPv4) or ff02::a (IPv6). Interfaces must have
   multicast capability and suitable configuration before the caller joins.
5. `eigrp_sys_socket_close()` unregisters readiness *before* closing the fd.
   Send/receive obtains a duplicate descriptor under a private table mutex,
   avoiding use-after-close when host teardown overlaps socket operations.

The OpenWrt daemon startup/shutdown owner is delivered in OW-09; this phase
must not register a fictitious service or independently call `eigrp_init()`.
Host-side state changes must observe the OW-04 protocol lock ordering.

## Transmit flow

```text
EIGRP RTP packetizer -> AF-specific serializer -> EIGRP byte buffer
        -> eigrp_sys_packet_send(instance, interface, destination, bytes)
        -> socket registry lookup + dup(fd)
        -> per-message pktinfo with interface ifindex and selected source
        -> sendmsg() / protocol 88 / hop limit 2
        -> return actual bytes written or negative errno
```

The IPv6 sender sets the scope ID for ff02::a or link-local unicast and only
pins the source when the EIGRP interface exposes a link-local address. Source
selection stays in the Linux IP layer otherwise. IPv4 `IP_PKTINFO` specifies
both egress ifindex and, when present, interface IPv4 source address. Neither
path manufactures IP headers or recomputes EIGRP checksums.

## Receive flow

```text
OW-04 readable fd -> core eigrp_packet_read()
       -> eigrp_sys_packet_receive()
       -> recvmsg(MSG_DONTWAIT | MSG_TRUNC), IP_PKTINFO/IPV6_PKTINFO
       -> reject truncation, missing ingress index and invalid framing
       -> normalized EIGRP address, ifindex, default VRF and length metadata
       -> core AF packet parser -> RTP / neighbor processing
```

Linux IPv4 raw sockets return **IP header plus EIGRP payload**. The shim checks
version, IHL, IPv4 total length, protocol number and fragmentation before
reporting `network_header_length` and `eigrp_length`. IPv6 raw sockets return
**EIGRP payload only**: `network_header_length=0`, and packet info supplies the
local destination and ingress interface. Multicast classification comes from
that destination, not the remote source. The adapter requires at least the
20-byte fixed EIGRP header; portable EIGRP is responsible for protocol-level
checksums, TLVs, authentication and replay detection.

Only **default VRF** is supported in OW-06. Multiple instances may each have a
socket, but no nondefault VRF is accepted by this adapter until network
namespace/VRF semantics are explicitly implemented.

## Validation and remaining gates

- `bash openwrt/test/packet-uut.sh` tests IPv4/IPv6 framing, malformed/truncated
  metadata and (when `CAP_NET_RAW` is granted) real protocol 88 loopback I/O.
  Unprivileged runs test framing and explicitly skip socket creation.
- `bash openwrt/test/recipe-test.sh` stages and compiles the actual OW-06
  object using target-like make flags.
- `./tools/openwrt.sh --build` cross-compiles the archive against the selected
  OpenWrt target on Aurora.
- A two-node EIGRP Hello adjacency is an integration gate, **not** a result
  of OW-06 libraries alone. It requires the OW-07 RIB and OW-08/09
  configuration and daemon lifecycle to run the protocol on live nodes.
  Native source and protocol-88 loopback tests do not prove that gate.
