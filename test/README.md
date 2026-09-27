# EIGRP Tests

Tests are organized by ownership boundary first, then protocol family and module.
The portable tree is intentionally runnable on a bare Unix development/CI host
without FRR or BIRD installed.

```text
test/
  build/               lightweight compile/syntax/prototype smoke harness
  common/              portable EIGRP behavior
    configuration/     behavior shared by address families
    process/
    metric/
    packet/
    topology/
    rib/
    ipv4/              IPv4-specific portable behavior
    ipv6/              IPv6-specific portable behavior
    bfd/               optional portable BFD feature tests
    manet/             optional portable MANET feature tests
  platform/            host-adapter and integration-boundary tests
    frr/
      common/           FRR behavior shared by address families
      ipv4/
      ipv6/
```

A future standalone Unix adapter belongs under `test/platform/unix/`; end-to-end
standalone smoke scenarios belong under `test/smoke/` when that framework is
implemented.  Do not make portable EIGRP depend on NETCONF, YANG, FRR, BIRD, or
any future smoke harness.  A configuration transport may drive the EIGRP-owned
northbound/configuration API from outside the portable core.

`test/platform/` is for tests that inspect or exercise a host adapter.  Actual
host-native/UUT tests remain with the integration: `frr/test/` for FRR and
`bird/test/` for BIRD.

Run the bare-host portable suite with:

```sh
make portable-test
```

Run repository adapter-contract tests with:

```sh
make platform-test
```

The normal source gate runs compile/link smoke plus both suites:

```sh
make test
```
