# FRR EIGRP Tests

Files in this directory are the FRR-native EIGRP tests for this project. Family-specific payloads are grouped below `ipv4/` or `ipv6/` when applicable; shared FRR integration tests may remain in a common integration subtree.

`tools/frr-install.sh` stages this directory to:

```text
frr/tests/eigrpd/
```

The normal UUT workflow is:

```sh
tools/frr-uut.sh --frr-root /path/to/frr
```

`frr-uut.sh` stages the current EIGRP source and FRR test payload, builds the
UUT, and runs tests only after the build succeeds. Use `--frr` when only the
FRR-native tests are required; the UUT is still built first.

The layout intentionally mirrors a single FRR protocol test directory such as
`frr/tests/ospfd`, not the entire FRR `tests/` tree.

## Live named-mode configuration gate

Before FRR-native pytest payloads, `tools/frr-uut.sh --all` and `--frr` run
`tools/frr-cli-navigation-uut.sh` followed by `tools/frr-named-uut.sh`.  The
navigation gate verifies top-level `router eigrp` re-entry, sibling named-mode
transitions, parent-context enforcement, and multiple classic AS instances in
one VRF.  The named-mode gate then drives the real daemon with
`sudo vtysh -d eigrpd -c ...` and validates named-mode configuration
acceptance, running-config writeback, mutation, documented removal paths,
IPv4/IPv6 AS 4453/6473 address-family lifecycle, and case-sensitive process
names (`savage` versus `SAVAGE`).

## IPv6 Hello packet-capture gate

Task 6 adds an opt-in live capture gate for a dedicated IPv6 UUT interface:

```sh
EIGRP_IPV6_HELLO_IF=eth1 tools/frr-ipv6-hello-capture-uut.sh
```

The gate configures only an IPv6 named AF (AS 4453 by default), captures a
protocol-88 packet to `ff02::a`, and verifies that the packet source is the
interface link-local address. It deliberately does not require adjacency or
route exchange. Override the AS, router ID, process name, or capture path with
`EIGRP_IPV6_HELLO_AS`, `EIGRP_IPV6_HELLO_ROUTER_ID`,
`EIGRP_IPV6_HELLO_PROCESS`, and `EIGRP_IPV6_HELLO_CAPTURE`.
