# OpenWrt shim implementation tasks

Copyright (C) 2026 Donnie V. Savage

Baseline: `diivious/eigrp@master` and `diivious/openwrt@main` (GitHub).
Reviewed revisions (2026-10-17):
- EIGRP `951d7549beb447b8f9c42b4f253638b392729491`
- OpenWrt `cd2c55e43b4e648c6e74101284ebff70fb050403`
All changes are delivered for local review; the repositories are not modified
remotely by this work. Preserve the five EIGRP-owned public API boundaries.
Do not implement EIGRP Stub behavior.


## Supporting design specs

- `openwrt/specs/integration.md`: runtime boundaries and call flows
- `openwrt/specs/configuration.md`: UCI, reload, writeback and management
- `openwrt/specs/packaging.md`: feed, package and patch ownership

## Task overview

| ID | Deliverable | Acceptance gate |
|---|---|---|
| OW-01 | Debian/macOS host setup and target configuration | Idempotent setup, `--verify`, `--config`, CI smoke |
| OW-02 | OpenWrt package/feed skeleton and staged install | Real OpenWrt package identified by menuconfig; repeatable source staging |
| OW-03 | OpenWrt build driver and target selection | Target build via toolchain; no silent patching; deterministic artifacts |
| OW-04 | Host runtime, event loop, process lifecycle | Portable system API link test, timers and teardown with no callback leaks |
| OW-05 | Linux interface inventory and address events | netifd / netlink changes reach public EIGRP interface APIs |
| OW-06 | IPv4/IPv6 packet I/O | IP protocol 88 Hello exchange on two OpenWrt nodes |
| OW-07 | Linux kernel RIB, redistribution and reconnect | Routes installed, withdrawn and replayed without leaked host types |
| OW-08 | Configuration + policy/keychain integration | UCI retention/reload/no forms and structured semantic results |
| OW-09 | procd/ubus management and operational visibility | Lifecycle, observability, logs and restart on live nodes |
| OW-10 | UUT and GitHub Actions acceptance | Linux cross-build; QEMU/device adjacency/route tests; Mac smoke |

### OW-01: host setup (first deliverable)

Files: `tools/openwrt.sh`, `openwrt/dev-env.sh`, `openwrt/README.md`,
`openwrt/test/setup-smoke.sh`, `.github/workflows/openwrt.yml`.

- Install Debian/Ubuntu prerequisites with apt or macOS prerequisites with
  Homebrew; allow a no-install path for already provisioned hosts.
- Clone only `github.com/diivious/openwrt`, default `main`, into a sibling
  OpenWrt checkout. Do not reset/pull/overwrite any existing checkout.
- Validate existing repo origin and buildroot markers before feed operations.
- Warn/fail before clone on a case-insensitive macOS filesystem.
- Initialize feeds idempotently unless explicitly skipped.
- Preserve `make build` / `make test` as the native macOS/Unix protocol gate.
- Provide `--verify` as a read-only check of the host and checkout state.
- Provide `--config` as the only target-selection/configuration entry point,
  running `menuconfig` (or a selected ARMv8 preset), `defconfig`, and `prereq`.
- Test shell grammar, help, argument handling, safe existing checkout use,
  and configuration command invocation.

**Exit criterion:** Shell smoke passes on Ubuntu/macOS Actions; host setup can
be run by a developer, and leaves source and installed packages untouched when
`--verify` is selected. A live run of the package-install path is a developer
acceptance gate, not something claimed by CI smoke.

### OW-02: feed and package layout

- Add `openwrt/feed/openeigrp/Makefile`, package install data and a linkable
  executable source plan under `openwrt/code/`. Decide explicit build version,
  license metadata, runtime dependency set and package conflicts.
- Support OpenWrt `src-link` from `feeds.conf`, without editing the EIGRP core.
- Make future `tools/openwrt.sh --install` stage or register **only** the
  OpenEIGRP package/feed. It must not invoke `--patch` implicitly.
- Verify `scripts/feeds install -p openeigrp openeigrp`, package visibility in
  menuconfig, and correct dependency resolution.

### OW-03: compile and patch ownership

- Add `--build` and `--patch` only when there are real targets for them.
- `--patch` is limited to tracked `openwrt/patch/` and must use clean, exact
  `git apply --check` / reverse-check idempotence. Reject conflicts, no fuzz.
- Compile on Debian with a pinned OpenWrt target using `make
  package/openeigrp/compile V=s`; record buildroot and EIGRP revisions.
- On macOS, source `openwrt/dev-env.sh`, attempt the same build, and record
  platform-specific deficiencies separately. Do not suppress build failures.

### OW-04: core/host runtime

- Implement OpenWrt-owned `eigrp_sys.h` host operations: scheduling, timers,
  FD readiness, work queues, monotonic time, socket lifecycle and shutdown.
- Retain OpenWrt-private objects under `openwrt/code/`, following
  `specs/public-api.md`. Avoid importing host-native objects into portable APIs.
- Verify initialization, ordering/cancellation, restart, and no pending
  callbacks after shutdown. Use core API smoke before packet integration.

### OW-05: interfaces

- Add link/address snapshot discovery and notifications via netlink/netifd
  adapters, including IPv4/IPv6, ifindex, MTU, bandwidth and multicast state.
- Test start-up discovery, interface up/down, address add/remove and flaps.

### OW-06: packet path

- Implement raw IPv4 and IPv6 IP protocol 88 socket transmit/receive,
  appropriate multicast memberships, interface and VRF metadata, and checksum
  and length validation per portable envelope contract.
- Exercise neighbor establishment/teardown, reliable updates, malformed
  packets, and interface/multicast membership transitions.

### OW-07: kernel RIB

- Use the public `eigrp_rib.h` route API to translate selected routes to/from
  Linux routes. Support add/delete/replace, multipath as applicable, source
  route subscriptions, and reconnect/replay.
- Keep route-selection and redistribution protocol decisions in `eigrp/code/`.

### OW-08: configuration

- Implement OpenWrt-native UCI configuration mapping to semantic `eigrp_cli.h`
  targets, reliable writeback, reload, and documented `no`/reset operations.
- Follow named IPv4 AS 4453, IPv6 AS 4453, multiple AS, case-sensitive parent
  ordering. Retain IPv6 config even where runtime is incomplete.
- Implement policy/key lookup across the public boundary, without exposing UCI
  objects to portable functions. Missing features must return real structured
  EIGRP results, never fake success.

### OW-09: daemon and management

- Install procd service with explicit graceful stop/reload and crash lifecycle.
- Expose live management using `eigrp_mgnt.h` via CLI/ubus as appropriate,
  including interfaces, neighbors, topology, traffic and event logs.
- Validate permissions, log retention and restart cleanup on real OpenWrt.

### OW-10: real UUT and CI

- Create OpenWrt-specific tests under `openwrt/test/`. One or more QEMU images
  with a reproducible topology should verify adjacencies and RIB exchange.
- Add build-matrix Actions jobs on Linux for target architecture(s). Cache
  toolchain carefully and fail on missing/dirty/stale package source.
- Retain the existing portable macOS/Linux `make test` gate. Add macOS shim
  syntax/host-neutral compile and configuration tests, without pretending to
  run Linux raw-socket or OpenWrt procd tests on Darwin.
- Do not publish artifacts or write to GitHub as part of this delivery.

## Development sequence

Start with OW-01 and validate both hosts. Then OW-02/03 for a real cross-build;
OW-04 through OW-09 for native functionality; OW-10 grows incrementally with
each completed phase. Full firmware and protocol UUT cannot be the first CI
gate because neither the package nor OpenWrt daemon exists yet.
