# Portable EIGRP Tests

This directory owns host-independent EIGRP behavior and fixtures. Tests here
must not require FRR or BIRD runtime objects or platform lifecycle machinery.
Address-family-specific coverage remains grouped below `ipv4/` and `ipv6/`.

FRR adapter/source-inspection tests live under `frr/test/uut/`.
FRR-native daemon tests live under `frr/test/eigrpd/`, and actual FRR topotests
belong under `frr/test/topotests/`. BIRD-native tests live under `bird/test/`.

Run the portable suite from the repository root with:

```sh
make uut
```
