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
