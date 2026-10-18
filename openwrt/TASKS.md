# OpenWrt shim implementation tasks

Copyright (C) 2026 Donnie V. Savage

Baseline: `diivious/eigrp@master` and `diivious/openwrt@main` (GitHub).
Reviewed revisions (2026-10-17):
- EIGRP `c872f7fc471334f8ac057fc67550fd655a98999e`
- OpenWrt `cd2c55e43b4e648c6e74101284ebff70fb050403`
All changes are delivered for local review; the repositories are not modified
remotely by this work. Preserve the five EIGRP-owned public API boundaries.
Do not implement EIGRP Stub behavior.


## Supporting design specs

- `openwrt/specs/integration.md`: runtime boundaries and call flows
- `openwrt/specs/runtime.md`: OW-04 scheduler, locks, cancellation, shutdown
- `openwrt/specs/interfaces.md`: OW-05 rtnetlink interface/address lifecycle
- `openwrt/specs/packets.md`: OW-06 IPv4/IPv6 raw socket transport
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
- Update feed source indexes without blanket-installing optional packages.
- Detect fatal Kconfig diagnostics even if an OpenWrt make wrapper returns zero.
- Support explicit `--config --minimal-feeds` recovery for optional-feed cycles
  without deleting downloaded feed sources or touching unrelated source files.
- Preserve `make build` / `make test` as the native macOS/Unix protocol gate.
- Provide `--verify` as a read-only check of the host and checkout state.
- Provide `--config` as the only target-selection/configuration entry point,
  preserving existing targets, inferring a generic default from the host when
  unconfigured, or allowing `--target`/`--menuconfig` overrides; then run
  `defconfig` and `prereq`.
- Test shell grammar, help, argument handling, safe existing checkout use,
  and configuration command invocation.

**Exit criterion:** Shell smoke passes on Ubuntu/macOS Actions; host setup can
be run by a developer, and leaves source and installed packages untouched when
`--verify` is selected. A live run of the package-install path is a developer
acceptance gate, not something claimed by CI smoke.

### OW-02: feed and package layout

- Implemented `openwrt/feed/openeigrp/Makefile` as a real first-stage
  portable-core static library package. The real OpenWrt daemon is deferred
  until runtime adapters exist; no fake executable is packaged.
- `Build/Prepare` stages local `eigrp/code` sources and `Build/Compile` uses
  `TARGET_CC`/`TARGET_AR`; `Build/InstallDev` stages public header contracts.
- Implemented `--install`: safely register only the OpenEIGRP `src-link`,
  update its feed index and install its package symlink; reject conflicts.
- Implemented `--config` selection of `CONFIG_PACKAGE_openeigrp=y` after
  installation, without making the installer change `.config`.
- Unit/smoke coverage exercises package recipe, duplicate registration and
  unsafe conflict handling. A full OpenWrt ARM64 compilation is the required
  live acceptance gate on Aurora.

### OW-03: build orchestration and patch ownership

- `--build` now invokes `tools/install`, `toolchain/install`, then
  `package/openeigrp/compile`, with fail-fast stage handling and `--jobs N`
  (two jobs by default). The driver checks required staged host binaries after
  tools compilation and never changes configuration, feed membership, or Git.
- `--patch` remains unimplemented until real managed patches are required.
- `--patch` is limited to tracked `openwrt/patch/` and must use clean, exact
  `git apply --check` / reverse-check idempotence. Reject conflicts, no fuzz.
- Cross-build on Debian against the previously configured OpenWrt target;
  log OpenWrt/EIGRP revisions and preserve compiler errors for diagnosis.
- On macOS, source `openwrt/dev-env.sh`, attempt the same build, and record
  platform-specific deficiencies separately. Do not suppress build failures.

### OW-04: core/host runtime (implemented, pending target cross-build)

- Add the native `openwrt/code/eigrp_owrt_runtime.c` implementation of the
  `eigrp_sys.h` scheduler/time/work-queue surface. The portable public function
  names remain `eigrp_sys_*`; shim-private functions are `eigrp_owrt_*`.
- Implement immediate and timed callbacks, one-shot FD readiness, work queue
  success/requeue/blocked semantics, cancellation, wall-clock and monotonic
  time, and idempotent init/finish with restart.
- Use a private host lock for scheduler state and a recursive protocol lock for
  callback/control-plane serialization. Lock order: protocol -> scheduler.
- Bind real packet descriptors later via `eigrp_owrt_runtime_fd_set/clear`.
  Changing an instance binding cancels that instance's previous readiness
  callbacks, not those belonging to other instances sharing an FD.
- Extend the package to target-build both core and runtime archives.
- Add `runtime-smoke.sh` / `runtime-harness.c` as native Linux/macOS tests,
  plus package staging/build recipe checks and CI execution.
- OW-06 will supply the actual packet descriptor producer. OW-04 does not ship
  sockets or a daemon entry point.

**Acceptance:** Native test harness passes event, timer, cancel, FD binding,
work-queue, shutdown and restart tests. OpenWrt ARM64 package cross-build must
be confirmed on Aurora before declaring the target build gate passed.

### OW-05: interfaces (implemented; target cross-build pending)

- Implement real route-netlink `RTM_GETLINK`/`RTM_GETADDR` snapshots and
  multicast link/IPv4/IPv6 address notifications. No root privileges or
  device-specific netifd API are required for read-only inventory.
- Decode ifindex, name, carrier, multicast support, MTU, speed with documented
  fallback, address and primary/secondary flag into EIGRP-owned host values.
- Diff snapshots to emit `eigrp_sys_intf_update/down/remove/addr_update` at
  the actual change observation point, avoiding duplicate emissions.
- Provide `eigrp_sys_interface_walk()` filtered by instance address family.
- Add explicit `eigrp_owrt_interface_start/refresh/stop` lifecycle; watcher
  startup and callback teardown must precede core shutdown.
- Extend package archive and native unit/CI smoke to compile and test Linux
  netlink integration with deterministic synthetic link/address transitions.
- Document default-VRF-only scope and defer privileged live topology flaps to
  the later QEMU/device UUT stage.

**Acceptance:** Linux interface synthetic/dump UUT, runtime UUT and recipe
smoke pass. ARM64 OpenWrt cross-build and privileged live interface-flap tests
remain to be verified on Aurora/OpenWrt UUT.

### OW-06: packet path (implemented; ARM64 and adjacency gates outstanding)

- `eigrp_owrt_packet.c` now supplies per-instance Linux raw IPv4/IPv6
  protocol 88 sockets, bounded receive framing, normalized address/ifindex
  metadata, per-message packet info and multicast join/leave.
- The socket registry binds/clears OW-04 FD readiness and uses duplicated
  descriptors for safe overlap between I/O and host-side socket teardown.
- Packet framing is validated by the host; EIGRP checksum/authentication,
  RTP, TLVs and Hello neighbor FSM remain exclusively portable core work.
- Native Linux UUT covers IPv4/IPv6 parser boundaries and optionally real raw
  loopback send/receive with CAP_NET_RAW. Package recipe + CI updated.
- Default VRF only. IPv6 scoped multicast requires the interface ifindex.
- Cross-build on Aurora, real interface membership changes and **two-node
  Hello/neighbor establishment** are outstanding live integration gates;
  the latter requires the daemon/configuration services from OW-08/09.

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
