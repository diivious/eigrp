# OpenWrt shim implementation tasks

Copyright (C) 2026 Donnie V. Savage

Baseline: `diivious/OpenEIGRP@openwrt-shim` and `diivious/openwrt@main` (GitHub).
Reviewed revisions (2026-10-18):
- EIGRP source tree `edb28da4daffa70b1a8a8ef5f8bd3f34941eb68d`
- OpenWrt `cd2c55e43b4e648c6e74101284ebff70fb050403`
All changes are delivered for local review; the repositories are not modified
remotely by this work. Preserve the five EIGRP-owned public API boundaries.
Do not implement EIGRP Stub behavior.


## Supporting design specs

- `openwrt/specs/integration.md`: runtime boundaries and call flows
- `openwrt/specs/runtime.md`: OW-04 scheduler, locks, cancellation, shutdown
- `openwrt/specs/interfaces.md`: OW-05 rtnetlink interface/address lifecycle
- `openwrt/specs/packets.md`: OW-06 IPv4/IPv6 raw socket transport
- `openwrt/specs/rib.md`: OW-07 kernel routes, source subscriptions and replay
- `openwrt/specs/configuration.md`: UCI, reload, writeback and management
- `openwrt/specs/packaging.md`: feed, package and patch ownership
- `openwrt/specs/daemon.md`: procd lifecycle and operational console
- `openwrt/specs/uut.md`: OW-10 two-node QEMU UUT and cross-build CI acceptance

## Task overview

| ID | Deliverable | Acceptance gate |
|---|---|---|
| OW-01 | Debian/macOS host setup and target configuration | Idempotent setup, `--verify`, `--config`, CI test |
| OW-02 | OpenWrt package/feed skeleton and staged install | Real OpenWrt package identified by menuconfig; repeatable source staging |
| OW-03 | OpenWrt build driver and target selection | Target build via toolchain; no silent patching; deterministic artifacts |
| OW-04 | Host runtime, event loop, process lifecycle | Portable system API link test, timers and teardown with no callback leaks |
| OW-05 | Linux interface inventory and address events | netifd / netlink changes reach public EIGRP interface APIs |
| OW-06 | IPv4/IPv6 packet I/O | IP protocol 88 Hello exchange on two OpenWrt nodes |
| OW-07 | Linux kernel RIB, redistribution and reconnect | Routes installed, withdrawn and replayed without leaked host types |
| OW-08 | Configuration + policy/keychain integration | UCI retention/reload/no forms and structured semantic results |
| OW-09 | procd/ubus management and operational visibility | Lifecycle, observability, logs and restart on live nodes |
| OW-10 | UUT and GitHub Actions acceptance | Linux cross-build; QEMU/device adjacency/route tests; Mac test |

### OW-01: host setup (first deliverable)

Files: `tools/openwrt.sh`, `openwrt/dev-env.sh`, `openwrt/README.md`,
`openwrt/test/setup-test.sh`, `.github/workflows/openwrt.yml`.

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

**Exit criterion:** Shell test passes on Ubuntu/macOS Actions; host setup can
be run by a developer, and leaves source and installed packages untouched when
`--verify` is selected. A live run of the package-install path is a developer
acceptance gate, not something claimed by CI test.

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
- Unit/test coverage exercises package recipe, duplicate registration and
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

### OW-04: core/host runtime (implemented; Aurora ARM64 build passed)

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
- Add `runtime-test.sh` / `runtime-harness.c` as native Linux/macOS tests,
  plus package staging/build recipe checks and CI execution.
- OW-06 will supply the actual packet descriptor producer. OW-04 does not ship
  sockets or a daemon entry point.

**Acceptance:** Native test harness passes event, timer, cancel, FD binding,
work-queue, shutdown and restart tests. OpenWrt ARM64 package cross-build must
be confirmed on Aurora before declaring the target build gate passed.

### OW-05: interfaces (implemented; Aurora ARM64 build passed)

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
- Extend package archive and native unit/CI test to compile and test Linux
  netlink integration with deterministic synthetic link/address transitions.
- Document default-VRF-only scope and defer privileged live topology flaps to
  the later QEMU/device UUT stage.

**Acceptance:** Linux interface synthetic/dump UUT, runtime UUT and recipe
test pass. ARM64 OpenWrt cross-build and privileged live interface-flap tests
remain to be verified on Aurora/OpenWrt UUT.

### OW-06: packet path (implemented; Aurora ARM64 build passed; adjacency gate outstanding)

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

### OW-07: kernel RIB (implemented; target/device gates outstanding)

- Add `eigrp_owrt_rib.c`, `eigrp_owrt_rib_netlink.c`, and the private
  `eigrp_owrt_rib.h`. No changes to the portable `eigrp_rib.h` contract.
- Use Linux route-netlink acknowledgments for kernel add/replace/del,
  protect first installs with exclusive creation, record installed prefixes
  per instance, support IPv4/IPv6 next hops and Linux multipath weights.
- Maintain protocol/route-instance keyed subscriptions to imported Linux main
  table routes (connected, static, RIP, OSPF, ISIS, BGP). Observe changes by
  full dump reconciliation; never reimport OpenEIGRP's RTPROT_EIGRP routes.
- Re-evaluate portable selected successors after an externally deleted kernel
  EIGRP route. Invalidate the cached installed flag so an unchanged selected
  route really causes an exclusive kernel reinstall.
- Constrain OW-07 to the default VRF, main table and route-instance 0;
  unsupported source variants return structured EIGRP results.
- Add Linux `rib-uut.sh` with read-only real netlink, deterministic simulated
  route changes and an optional isolated CAP_NET_ADMIN namespace test.
- Rename existing shell tests to `*-test.sh`/`*-uut.sh` by semantics and
  update CI, documentation and test references. No legacy alias scripts.
- Preserve EIGRP-authoritative route selection and redistribution decisions.

**Acceptance:** Local native RIB tests, runtime, interface, packet and recipe
checks pass. Aurora ARM64 cross-build, optional isolated kernel write test,
and an actual OpenWrt router route exchange are separate outstanding gates.

### OW-08: configuration and centralized tests (implemented; target/device gates remain)

- Add `eigrp_owrt_config.c/.h`, `eigrp_owrt_uci.c`, and
  `eigrp_owrt_policy.c`. Load real `libuci` configuration into normalized
  host-owned sections and validate before applying any semantic operation.
- Support case-sensitive named parents, IPv4/IPv6 AF, multiple AS, IPv4
  networks, interface timers/options, authentication settings and keychains.
  The portable core remains the semantic authority. IPv6 retained configuration
  is not discarded because runtime behavior is incomplete.
- Support native UCI writeback and deletion/reset operations with prospective
  schema validation; provide a retained snapshot and best-effort semantic
  replacement/reload rollback. OW-09 will attach these calls to procd.
- Implement named prefix-list rule evaluation and keychain lookup behind
  EIGRP public host policy functions. Return structured `NOT_IMPLEMENTED` for
  unsupported access-list/route-map capabilities rather than fake permit.
- Package protected `/etc/config/openeigrp`, stage real UCI source, and depend
  on `libuci`. No dummy executable or premature procd service.
- Implement `tools/openwrt.sh --clean`, `--test`, `--uut`, `--kernel` as the
  project-wide OpenWrt task orchestration pattern. Keep every underlying
  `openwrt/test/*-test.sh` and `*-uut.sh` directly runnable for debugging/CI.
  Future OW-* work adds tests to this central sequence instead of introducing
  orphan manual commands.
- Local Linux tests cover syntax, isolated driver build safety, config
  semantics, retained identity/case sensitivity, key policy, interface, packet
  and RIB UUTs. The actual ARM64 `libuci` cross-build and live procd reload
  require Aurora/device acceptance; root namespace writes are optional.

**Acceptance:** Host component/UUT tests pass. Cross-target UCI build and
service-runtime behavior are separately tracked and not claimed in CI.

### OW-09: daemon and management (implemented; device acceptance pending)

- The `openeigrpd` foreground binary links the portable EIGRP core and real
  OpenWrt host services; starts receive worker, UCI semantic config, kernel
  RIB and interface watchers; and closes them in reverse order.
- procd `/etc/init.d/openeigrp` validates UCI before startup/reload,
  supervises crashes, and routes logs through OpenWrt service logging.
- SIGHUP reloads UCI using OW-08's best-effort snapshot replacement;
  SIGTERM/SIGINT perform graceful teardown.
- `openeigrpctl` exposes protocol, interface, neighbor, topology, traffic and
  event snapshots from `eigrp_mgnt.h` on a Unix socket protected with 0600.
  A custom ubus RPC object is deferred; standard procd/ubus service status
  works through OpenWrt's service manager.
- `openwrt/test/daemon-uut.sh` runs an actual native core+host executable with
  a test-only UCI reader and checks startup, failure, reload and shutdown.
  It is included in `tools/openwrt.sh --uut`, and remains independently runnable.

**Acceptance still needed on Aurora/OpenWrt:** ARM64 target daemon link with
real `libuci`, on-device procd lifecycle/reload/crash behavior, actual protocol
88 Hello neighbors and kernel route installation/withdrawal under convergence.


### OW-10: real UUT and CI (implemented; live validation pending)

- `openwrt/test/qemu-uut.py` launches two actual QEMU OpenWrt VMs with
  separate temporary images and a host-local QEMU socket-backed Ethernet link.
  It provisions UCI named IPv4 AS 4453 and separate dummy-interface networks,
  starts the real procd daemon and checks both neighbor UP states and imported
  EIGRP Linux kernel routes (protocol 192). No simulated peer/route substitute.
- `openwrt/test/qemu-test.sh` checks the UUT runner's contracts offline, and
  is included in the `--test` / `--uut` central developer flow.
- `tools/openwrt.sh --image-build` explicitly enables the ext4, gzip, ip-full and
  dummy-kernel module image requirements and builds the complete OpenWrt image.
  It preserves the normal `--build` package-only behavior.
- `tools/openwrt.sh --qemu` chooses the configured target's image or accepts
  `--image`, optional ARM64 `--firmware`, and `--log-dir`. Missing image, QEMU,
  guest boot, procd/config, adjacency or FIB assertions fail the UUT.
- `.github/workflows/openwrt.yml` runs Linux/macOS host suites, native Linux
  x86/64 and ARM64 OpenWrt cross-builds, and a two-node x86 QEMU acceptance
  step after full image creation. It pins OpenWrt's source revision, caches
  only downloaded source tarballs, and rejects missing package artifacts.
- Test semantics and topology are specified in `openwrt/specs/uut.md`.

**Acceptance:** Offline UUT harness contract tests run locally. Aurora/CI
must still confirm the full image build, live QEMU neighbor/route exchange,
ARM64 target handling and GitHub Actions job success before claiming OW-10
live acceptance complete. Never report a pass without observing both Up
neighbors and installed EIGRP routes.
- Retain the existing portable macOS/Linux `make test` gate. Add macOS shim
  syntax/host-neutral compile and configuration tests, without pretending to
  run Linux raw-socket or OpenWrt procd tests on Darwin.
- Do not publish artifacts or write to GitHub as part of this delivery.

## Development sequence

Start with OW-01 and validate both hosts. Then OW-02/03 for a real cross-build;
OW-04 through OW-09 for native functionality; OW-10 grows incrementally with
each completed phase. Full firmware and protocol UUT cannot be the first CI
gate because neither the package nor OpenWrt daemon exists yet.
