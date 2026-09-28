# FRR EIGRP Tests

FRR-owned tests are separated by the FRR destination they target and by whether
they are repository-layout source-inspection guards.

```text
frr/test/build/              FRR-backed compile/link smoke infrastructure
frr/test/eigrpd/             staged to <FRR>/tests/eigrpd/
frr/test/topotests/          staged to <FRR>/tests/topotests/
frr/test/uut/  project-tree pytest guards; not staged into FRR
```

The source-inspection tests are retained as-is in this reorganization. They are
not converted into FRR topotests here; framework cleanup can be handled as a
separate test-design task.

`tools/frr.sh` owns FRR staging/build operations and `tools/frr-uut.sh` owns the
FRR UUT workflow.
