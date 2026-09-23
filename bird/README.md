# BIRD host adapter

This directory is reserved for a BIRD shim.

There is no BIRD implementation in this tree yet. Do not treat an empty
directory as an integration contract.

If you are adding EIGRP to BIRD, start with `../specs/integration-spec.md`
and the five public headers in `../eigrpd/`. Use `../frr/README.md` only as a
job-split example. Do not import FRR types or FRR test objects.

Host-independent tests stay under `../test/portable/` and `../test/common/`.
BIRD-native tests belong under `test/` in this directory when they exist.
