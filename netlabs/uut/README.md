# Live EIGRP interoperability qualification

This directory contains automated tests of OpenEIGRP against Cisco and other real
router images, orchestrated remotely on StormTrooper by `netlabs/tools/remote-uut.sh`.
These tests are separate from the portable `make uut` harness in `eigrp/test/uut/`.

Naming: `<router-count>R-<purpose>.py` for the executable test driver, with a
matching directory for `scenario.yaml`, native netlab `topology.yml`, and
README. Shared named-mode configuration lives under `netlabs/config/`. For example, `2R-neighbor.py` uses `2R-neighbor/`. Scripts are executed
by filename; they are not imported as normal Python modules.

```sh
netlabs/tools/remote-uut.sh --list
netlabs/tools/remote-uut.sh --scenario 2R-neighbor
```

Implementations must return nonzero on failure, record per-family test results,
and leave teardown to the shared remote runner. Live tests must not silently
weaken or claim equivalence to portable UUT internal state assertions.

`2R`, `3R`, `4R`, etc. refer to the **total number of routers in the topology**,
not the number of OpenEIGRP routers, Cisco routers, or test phases.
