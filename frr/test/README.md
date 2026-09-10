# FRR EIGRP Tests

Files in this directory are the FRR-native EIGRP tests for this project.

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
`tools/frr-named-uut.sh`.  That gate drives the real daemon with
`sudo vtysh -d eigrpd -c ...` and validates named-mode configuration
acceptance, running-config writeback, mutation, documented removal paths,
IPv4/IPv6 AS 4453/6473 address-family lifecycle, and case-sensitive process
names (`savage` versus `SAVAGE`).
