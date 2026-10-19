# Pre-merge EIGRP integration gate

Copyright (C) 2026 Donnie V. Savage

## Entrypoints

The root `Makefile` intentionally keeps `make all` as the existing fast
portable/Unix build; it does not silently grow into an OpenWrt firmware and FRR
installation job.

- `make merge-check-host`: run all native checks that do not require a prepared
  FRR installation, an OpenWrt buildroot, privileged namespaces, or QEMU. This
  reports **HOST REGRESSION: PASS** but never claims full merge qualification.
- `make merge-check`: strict integration gate, including FRR and OpenWrt builds,
  live FRR UUT, newly built OpenWrt QEMU images, a two-node EIGRP routing UUT,
  and an isolated kernel RIB UUT. Missing mandatory capabilities are `BLOCKED`
  and return nonzero, not a fabricated green result.
- `make merge-check-test`: exercise orchestration with fake checkout/tool
  executables. No root, QEMU, or live FRR instance is needed.

For a configured Linux development host, the complete gate is:

```sh
make merge-check MERGE_CHECK_ARGS="--privileged --jobs 4"
```

A gate with `--privileged` requires **noninteractive sudo** (`sudo -n -v`).
It does not run the top-level script as root, alter sudo policy, or attempt to
provision dependencies. The real FRR UUT may control EIGRP services in the FRR
host installation, so run it on the designated UUT host, not on a production
router. The OpenWrt kernel UUT uses the existing disposable namespace script.

If FRR or OpenWrt is not in the default sibling location:

```sh
make merge-check MERGE_CHECK_ARGS="--privileged --jobs 4 --frr-root /path/to/frr --openwrt-root /path/to/openwrt"
```

`MERGE_FRR_ROOT`, `MERGE_OPENWRT_ROOT` and `MERGE_LOG_DIR` are equivalent
script environment overrides. `--log-dir` selects a persistent per-stage log
directory; otherwise a unique system temp directory is printed in the summary.

## Stages

| Integration | Host gate | Full gate |
|---|---|---|
| Gate runner | Driver self-tests | Driver self-tests |
| Portable/Unix | `make build`, `make uut`, `make scale` | Same |
| FRR adapter | Standalone compile and relocatable link guard | Same, followed by `tools/frr.sh --all` and `--uut` |
| OpenWrt | `tools/openwrt.sh --uut` | Same, then `--build`, `--image-build`, `--qemu`, `--kernel` |
| BIRD/BSD | When `bsd/` exists, invoke `tools/bsd.sh --build` and `--uut` | Same |

The `bsd/` integration has not been added to this repository baseline. Its
absence is reported as `SKIP (not present)`. Once that directory exists,
missing build/UUT driver support is treated as `BLOCKED`, not ignored. Platform-
incompatible host checks must be identified as skips, rather than pretending
that Linux-specific runtime operations have passed on macOS.

The driver aggregates all independent failures, prints PASS/FAIL/BLOCKED/SKIP
for each stage, and exits nonzero if any required stage fails or is blocked.
Full logs are retained separately. A failed FRR build blocks its live UUT, and
a failed OpenWrt package or image build blocks downstream QEMU tests to prevent
accidentally testing stale binaries. No automatic `--setup`, `--install`,
`--config`, or Git modification occurs during this gate.

The OpenWrt `--qemu` UUT must demonstrate an actual two-node EIGRP adjacency
and installed kernel routes. Neither a link-only test nor a boot-only test is
sufficient for full acceptance.

## Ownership and future shims

Host-level build and UUT logic remains in each adapter's driver. The merge
script **orchestrates** those entrypoints and owns only overall policy,
ordering, prerequisites, results and logs. Individual tests remain directly
runnable, for example `bash openwrt/test/rib-uut.sh`.

Future host integrations must register build, unit and live acceptance stages
in the merge gate before being considered merge-qualified. A new adapter is not
allowed to satisfy the gate with a placeholder that always returns success.

Volume/performance tests such as `make scale-all` remain a separate extended
qualification. Their high time/memory requirements make them inappropriate
as an unconditional part of every pre-merge run; protocol changes should run
them where relevant, or through a separately scheduled CI gate.
