# OpenEIGRP OpenWrt shim

Copyright (C) 2026 Donnie V. Savage

## Purpose and current status

**OW-10: real QEMU/unit-under-test and CI acceptance has been added.**
The package/daemon cross-build was reported passing on Aurora. The new
QEMU image-build and two-guest live adjacency/route gates still require
Aurora/CI execution; native test success is not evidence of QEMU convergence.
See [specs/uut.md](specs/uut.md) for topology, requirements and CI scope.

`openwrt/` is the **native OpenWrt host shim** for the portable OpenEIGRP
implementation in `../eigrp/code/`. It is the OpenWrt-specific translation
boundary between OpenWrt host mechanics and the portable RFC 7868 EIGRP core.

OpenWrt is the operating system, packaging, and service-management environment.
It is **not** another EIGRP implementation. DUAL, RTP, topology, packet
semantics, metric handling, and neighbor behavior remain in `../eigrp/code/`.

**Status: OW-09 daemon and procd service integration implemented.** The package now installs a real target executable and read-only operational client. Live OpenWrt device behavior and adjacency/RIB acceptance remain to be validated.

- `../tools/openwrt.sh --setup` prepares a Debian/Ubuntu or macOS developer
  host and an OpenWrt buildroot checkout.
- `../tools/openwrt.sh --config` owns OpenWrt target selection, `defconfig`
  and prerequisite validation; `--verify` checks host and stored target values
  read-only, but does not re-run Kconfig.
- `../tools/openwrt.sh --install` registers the local OpenEIGRP feed without
  installing unrelated packages or modifying OpenWrt-wide source files.
- `--config` selects `CONFIG_PACKAGE_openeigrp=y` when the package is registered.
- The package builds the portable core (`libeigrp-core.a`) and the native
  OpenWrt scheduler (`libeigrp-owrt-runtime.a`) using the selected target
  compiler. Public host-interface headers and the private runtime binding
  headers are staged for later adapters.
- `openwrt/code/eigrp_owrt_runtime.c` implements timers, event callbacks,
  FD readiness, work queues, monotonic/wall-clock services, and shutdown.
- `openwrt/code/eigrp_owrt_interface.c` and
  `eigrp_owrt_interface_state.c` implement Linux rtnetlink link/address
  inventory, event subscriptions, IPv4/IPv6 interface walks, and normalized
  EIGRP-owned interface lifecycle notifications (default VRF).
- `openwrt/code/eigrp_owrt_packet.c` provides raw IPv4/IPv6 protocol 88
  send/receive, source/interface packet info, multicast group membership,
  and packet framing metadata for portable EIGRP.
- `eigrp_owrt_rib.c` and `eigrp_owrt_rib_netlink.c` implement kernel route
  installation/withdrawal/replay and source route observation via Linux
  rtnetlink, within the default VRF and main routing table.
- `eigrp_owrt_config.c`, `eigrp_owrt_uci.c` and `eigrp_owrt_policy.c`
  normalize UCI data, validate and apply retained named configuration through
  `eigrp_cli.h`, and provide prefix-policy/keychain services. The package
  now installs `/etc/config/openeigrp` as a retained configuration file.
- `tools/openwrt.sh --clean`, `--test`, `--uut` and `--kernel` orchestrate
  package cleanup, independent component tests and isolated kernel UUTs.
- `openwrt/code/eigrp_owrt_daemon.c` runs a foreground protocol daemon, consumes
  UCI configuration, starts the packet/RIB/interface services and handles
  SIGHUP reload and SIGTERM/SIGINT cleanup.
- `/etc/init.d/openeigrp` provides procd supervision, startup checks, reload
  triggers and logging through OpenWrt logread.
- `openeigrpctl` exposes **read-only** protocols, interfaces, neighbors,
  topology, traffic counters and event logs through a protected local socket.
- **Not yet validated on an actual router:** two-node protocol adjacency,
  kernel route convergence, repeated live reload and procd crash recovery.
  There is no custom ubus RPC object yet; procd service status is available
  using OpenWrt's standard service/ubus facilities.

See [TASKS.md](TASKS.md), [specs/configuration.md](specs/configuration.md) and
[specs/rib.md](specs/rib.md) for staged implementation and host boundaries.

## Authoritative source repositories

- OpenEIGRP: <https://github.com/diivious/OpenEIGRP>, branch `openwrt-shim`
- OpenWrt buildroot: <https://github.com/diivious/openwrt>, branch `main`

`tools/openwrt.sh` does **not** pull, reset, rewrite history, or replace an
existing checkout. It treats `diivious/openwrt` as the baseline buildroot and
uses OpenWrt feeds through `scripts/feeds`.

## OpenWrt shim overview

The OpenWrt shim is best understood as an adapter layer between OpenWrt-native
subsystems and the portable EIGRP public APIs.

```text
+---------------------------------------------------------------+
| OpenWrt host                                                  |
|                                                               |
|  UCI config   procd   ubus/CLI   netifd   netlink   kernel RIB|
|     |          |         |         |         |          |     |
+-----+----------+---------+---------+---------+----------+-----+
      |                    OpenWrt shim                         |
      v                                                         v
+-----------------------------------------------------------------------+
| openwrt/code/                                                         |
|                                                                       |
|  config      lifecycle      interfaces      sockets      RIB          |
|  policy/key  management     packet I/O      timers       redistribution|
+-------------------------+----------------------------+-----------------+
                          | public OpenEIGRP boundary |
                          v                            
+-----------------------------------------------------------------------+
| eigrp/code/                                                           |
|                                                                       |
|  RFC 7868 core: DUAL, RTP, topology, metrics, TLVs, packet semantics |
+-----------------------------------------------------------------------+
```

### Core design rule

OpenWrt-native objects must stop at the shim boundary. `eigrp/code/` receives
only EIGRP-owned public values and opaque identities defined by:

- `eigrp/code/eigrp.h`
- `eigrp/code/eigrp_cli.h`
- `eigrp/code/eigrp_mgnt.h`
- `eigrp/code/eigrp_rib.h`
- `eigrp/code/eigrp_sys.h`

That means:

- no UCI section objects in the portable core;
- no procd service structs in the portable core;
- no ubus, netifd, netlink, or Linux route objects in the portable core;
- no Linux socket wrappers or OpenWrt event-loop types in the portable core.

## Specification set

The OpenWrt shim is documented by the following files:

- [specs/integration.md](specs/integration.md)  
  Overall runtime architecture, boundaries, startup/shutdown, interface,
  packet, and RIB call flows.
- [specs/configuration.md](specs/configuration.md)  
  Implemented UCI schema, retained config replay, best-effort reload rollback,
  native writeback, semantic results, host policy and keychain lookup.
- [specs/runtime.md](specs/runtime.md)  
  Implemented OW-04 event scheduler, lock ordering, FD binding and lifecycle.
- [specs/interfaces.md](specs/interfaces.md)  
  OW-05 Linux rtnetlink snapshots, event-driven reconciliation, notification
  paths, and daemon startup/shutdown sequence diagrams.
- [specs/packets.md](specs/packets.md)  
  OW-06 protocol 88 raw sockets, IPv4/IPv6 metadata, transmit/receive flow,
  multicast groups and acceptance boundaries.
- [specs/daemon.md](specs/daemon.md)  
  OW-09 service lifecycle, management console, procd and validation gates.
- [specs/packaging.md](specs/packaging.md)  
  Feed layout, package/build model, install payload, and how the shim should be
  brought into an OpenWrt checkout.

Start with `../specs/platform-integration.md` for the host-neutral contract,
then read the OpenWrt-specific specs listed above.

## Implementation boundaries

The OpenWrt shim will own host mechanics only.

### OpenWrt-owned responsibilities

- process initialization and shutdown;
- timers, events, readiness notifications, and work queues;
- raw IPv4/IPv6 EIGRP packet I/O using IP protocol 88;
- multicast membership on participating interfaces;
- interface discovery and link/address lifecycle events;
- Linux kernel RIB add/delete/replay and redistribution subscriptions;
- UCI configuration parsing, retention, reload, and writeback;
- policy and authentication key lookup at the host boundary;
- procd service control and ubus/CLI management presentation.

### Portable-core responsibilities

- DUAL and topology ownership;
- successor and feasible-successor logic;
- EIGRP metric semantics;
- RTP sequencing, acknowledgment, retransmission, and packetization;
- neighbor finite-state behavior;
- TLV parsing/encoding and packet semantics.

## Development host setup

### Debian / Ubuntu

From your existing OpenEIGRP checkout:

```sh
./tools/openwrt.sh --setup
./tools/openwrt.sh --verify
```

The script installs OpenWrt build prerequisites with `sudo apt-get`, clones
`diivious/openwrt` into a sibling `../openwrt/` if missing, and runs:

```sh
cd ../openwrt
./scripts/feeds update -a
# setup does not install every optional feed package
```

No firmware, toolchain, or OpenEIGRP package is built during initial setup.
OpenWrt builds should be run as your normal user, not root.

**All OpenWrt target configuration is driven through `--config`.** Normally,
no target argument is needed:

```sh
./tools/openwrt.sh --config
./tools/openwrt.sh --verify
```

`--config` preserves an existing valid OpenWrt `.config` target. If none is
configured, it selects a generic development target using the host CPU:

| Host architecture | New default OpenWrt target |
|---|---|
| Debian ARM64 / Apple Silicon macOS | `armsr/armv8` Generic EFI/QEMU |
| Debian or macOS x86-64 | `x86/64` Generic x86-64 |

These are **development defaults**, not detected router hardware. Use an
explicit target override when building for a different architecture:

```sh
./tools/openwrt.sh --config --target armsr/armv8
./tools/openwrt.sh --config --target x86/64
```

For a device-specific OpenWrt target, use the interactive menu through the
**same** driver action:

```sh
./tools/openwrt.sh --config --menuconfig
```

All three forms run OpenWrt's `make defconfig` and `make prereq`. An existing
valid target is not overwritten by plain `--config`; a changed target receives
a separate `.config` backup. A host architecture alone cannot identify a
MediaTek, Rockchip, or other particular router.

An existing checkout is verified for origin and expected buildroot markers and
is **never automatically updated**. For a local buildroot elsewhere:

```sh
./tools/openwrt.sh --setup --openwrt-root /path/to/openwrt
```

Useful controlled variants:

```sh
./tools/openwrt.sh --setup --skip-packages
./tools/openwrt.sh --setup --skip-clone --skip-feeds
./tools/openwrt.sh --verify --openwrt-root /path/to/openwrt
```

### macOS, native-first development

The OpenWrt buildroot permits a macOS or BSD host **provided its filesystem is
case-sensitive**, but the primary supported build path is still GNU/Linux.
Native OpenWrt cross-builds on macOS are therefore **experimental**.

1. Install Xcode Command Line Tools with `xcode-select --install`.
2. Install Homebrew.
3. Put the **OpenWrt checkout** on a case-sensitive APFS volume.
4. Run:

   ```sh
   ./tools/openwrt.sh --setup --openwrt-root /Volumes/OpenWrtDev/openwrt
   ```

5. Before manually invoking OpenWrt's `make`, load the GNU-tool environment:

   ```sh
   source /path/to/eigrp/openwrt/dev-env.sh
   cd /path/to/eigrp
   ./tools/openwrt.sh --config --openwrt-root /Volumes/OpenWrtDev/openwrt
   ```

Portable code builds and tests already run natively on macOS:

```sh
cd /path/to/eigrp
make build
make test
```

This remains the fastest edit/compile/test loop on the MacBook Pro. The future
OpenWrt shim should compile as much host-neutral code as possible on macOS,
while Linux-specific packet sockets, kernel RIB operations, and procd behavior
continue to require Linux or OpenWrt test environments.

## OW-02: Install and build the portable core package

The local external feed is versioned with OpenEIGRP, rather than copied into
OpenWrt's repository. The initial package compiles actual RFC 7868 portable
sources using the selected OpenWrt target compiler.

```text
OpenEIGRP checkout                          OpenWrt checkout
------------------                          -----------------
eigrp/code/*.c, *.h --------------------+       feeds.conf
openwrt/code/eigrp_owrt_runtime.c ------+           |
                                        |           |
openwrt/feed/openeigrp/Makefile <-------+--src-link-+
                   |
                   v
          OpenWrt Build/Prepare
                   |
                   v
              target toolchain
                   |
          +--------+----------------+
          |                         |
          v                         v
   libeigrp-core.a       libeigrp-owrt-runtime.a
          |                         |
          +-----------+-------------+
                      v
                openeigrp package

     [OW-09 links and packages the running daemon]
```

Run on Aurora, from `~/projects/eigrp`:

```sh
./tools/openwrt.sh --install
./tools/openwrt.sh --config
./tools/openwrt.sh --verify
```

The `--install` action creates `feeds.conf` from `feeds.conf.default` only
when necessary, appends the single local `src-link openeigrp` entry, then runs
`scripts/feeds update openeigrp` and
`scripts/feeds install -p openeigrp openeigrp`. Repeating `--install` does not
append duplicate entries. When the checkout is renamed from `eigrp/` to
`openeigrp/`, `--install` also repairs a **broken** src-link and package
symlinks that point to the now-absent sibling checkout. It backs up the prior
`feeds.conf` before changing its sole OpenEIGRP declaration. A registered
source that still exists, an unrelated feed, or a live/regular-file package
path is refused rather than overwritten. Neither `--build` nor `--clean`
repairs registration implicitly: run `--install` once after relocation.
The action does **not** modify `.config`, download unrelated feeds, or
apply any patches.

**Configuration remains behind `--config`.** Once the recipe is available,
`--config` selects `CONFIG_PACKAGE_openeigrp=y`, preserves the target
(e.g. `armsr/armv8`), and runs `defconfig` plus `prereq` validation.
In particular, repairing a relocated feed with `--install` does not recreate
OpenWrt's `.config`. If `--verify` says `target: not configured`, run
`./tools/openwrt.sh --config --target armsr/armv8` on Aurora before
`--clean`, `--build`, or `--image-build`. The noninteractive `--config` action
prevents OpenWrt make from falling back to terminal-sized `menuconfig`.
`--clean` refuses an absent/invalid target or unselected package without
modifying `.config` or starting an interactive configurator. Once the feed is
registered, `--verify` also reports a missing configuration as a failure.

For the **first ARM64 build**, from the OpenEIGRP checkout on Aurora:

```sh
./tools/openwrt.sh --build
# Optional: override the default two parallel jobs
./tools/openwrt.sh --build --jobs 4
```

The driver validates the checkout, registered feed and selected package,
then executes these OpenWrt build targets **in order**:

```text
OpenWrt .config (created by --config)
          |
          v
  tools/install            OpenWrt host openssl, libdeflate-gzip, etc.
          |
          v
  toolchain/install        Target C compiler and libraries
          |
          v
  package/openeigrp/compile
          |
          v
  OpenEIGRP target core and runtime archives / package
```

Each stage runs as the normal developer account using `make -jN ... V=s`.
A failed stage stops the build immediately. OpenWrt controls incremental
rebuilds through its own stamps; the driver does not reset/patch repositories,
change `.config`, or build a complete firmware image. Host executables
`staging_dir/host/bin/openssl` and `libdeflate-gzip` are checked after
`tools/install` because a missing stage caused the first Aurora failure.

After a successful build, inspect artifacts:

```sh
cd ~/projects/openwrt
find bin/packages -type f \( -name 'openeigrp*.apk' -o -name 'openeigrp*.ipk' \) -print
```

OpenWrt's exact package artifact paths depend on its target, packaging format
and build profile. No actual cross-build was run as part of preparing this ZIP;
Aurora is the first real OpenWrt ARM64 acceptance environment.

This package is intentionally a **static development artifact**, not an
operational router daemon. It installs `/usr/lib/libeigrp-core.a` and
`/usr/lib/libeigrp-owrt-runtime.a`, and stages the public headers and the
OpenWrt-private runtime binding header under
`staging_dir/target-*/usr/include/openeigrp/`. This is meaningful because
OpenWrt must compile the existing protocol engine for the selected target,
without any substituted or host-architecture objects.

The runtime and interface adapter are tested independently of an OpenWrt buildroot:

```sh
bash openwrt/test/runtime-test.sh
bash openwrt/test/interface-uut.sh  # Linux only, no root required
bash openwrt/test/recipe-test.sh
```

The Linux interface adapter opens its own route-netlink sockets and
subscribes to link/address notifications. It must start after `eigrp_init()`
and stop before `eigrp_terminate()`; the daemon integration will own this
lifecycle in a later task. See [specs/interfaces.md](specs/interfaces.md).

The scheduler callbacks run in one host thread. The private
`eigrp_owrt_runtime_enter/leave` functions serialize host-side changes; the
OW-06 packet adapter binds descriptors using `eigrp_owrt_runtime_fd_set/clear`.
See [specs/runtime.md](specs/runtime.md) for the locking and teardown rules.

When a portable or shim source edit needs a fresh stage, use:

```sh
./tools/openwrt.sh --clean
./tools/openwrt.sh --build
```

`--clean` removes only the OpenEIGRP package build outputs, not the OpenWrt
host tools or target cross-toolchain. `--build` respects OpenWrt stamps and
source dependencies. Developers can also run the individual package make
commands directly from the OpenWrt checkout.

If OpenWrt-wide patches are ever required, keep them under `openwrt/patch/`,
apply them only with an explicit future `--patch`, and reject any patch that
does not apply cleanly.

## Kconfig troubleshooting

`--config` treats Kconfig `error:` and `recursive dependency detected`
diagnostics as fatal even if OpenWrt's `make defconfig` happens to exit with
status 0. In that case, the driver **must not** print configuration complete
or continue to prerequisite checking.

An earlier version of `--setup` registered **all** optional feed packages.
If `--config` fails because of unrelated Qt5, WeeChat or Squeezelite dependency
cycles, you can explicitly narrow the registered package set:

```sh
./tools/openwrt.sh --config --minimal-feeds
```

`--minimal-feeds` backs up `.config` and invokes OpenWrt's own
`scripts/feeds uninstall -a` to remove registered feed packages. It **does
not** delete downloaded feed source trees, change the OpenWrt Git revision,
or edit the broken package definitions.
This option changes which third-party packages are visible in menuconfig, so
it is never run automatically. If you already ran `--install`,
`--minimal-feeds` also removes its package registration; rerun `--install`
and then plain `--config` to restore the OpenEIGRP selection.

**Note:** `--verify` reports the target recorded in `.config`; it cannot
establish that package dependency parsing succeeded. For that use `--config`.

## Central OpenWrt driver actions

| Action | Status | Intended semantics |
|---|---|---|
| `--setup` | Implemented | Host prerequisites, fork checkout, feeds |
| `--verify` | Implemented | Read-only host, checkout and target checks |
| `--config` | Implemented | Select target and registered OpenEIGRP package, run Kconfig/prereq; optional `--target`/`--menuconfig` |
| `--install` | Implemented | Register the single local OpenEIGRP feed/package, idempotently |
| `--patch` | Future | Idempotent, explicit, managed OpenWrt-wide patches only |
| `--build` | Implemented | Host tools → toolchain → OpenEIGRP package, `--jobs N` (default 2) |
| `--clean` | Implemented | Only the OpenEIGRP package build outputs; preserves toolchain |
| `--test` | Implemented | Setup, runtime, config and recipe component tests |
| `--uut` | Implemented | All nonprivileged component tests plus interface, packet and RIB UUTs on Linux |
| `--kernel` | Implemented | Explicit isolated kernel route test via sudo and disposable network namespace |

## Test levels

1. **macOS + Debian**: existing root `make test` gate and shim source or
   compile checks where supported.
2. **Linux builder**: OpenWrt package compile for one configured target,
   preferably followed by a second architecture before broad support claims.
3. **OpenWrt QEMU/device**: procd start/stop, UCI reload/writeback, raw IPv4
   and IPv6 EIGRP adjacencies, route install/withdrawal, redistribution,
   interface churn, daemon restart, and teardown.
4. **CI**: macOS/Linux setup-script test first, then Linux OpenWrt build and
   QEMU UUT once the real package/runtime is implemented.

See [TASKS.md](TASKS.md) for the staged acceptance criteria.

## Development, test naming and OW-08 validation

The standard development workflow has one entry point:

```sh
./tools/openwrt.sh --clean   # only when restaging is needed
./tools/openwrt.sh --build
./tools/openwrt.sh --test    # component tests without root
./tools/openwrt.sh --uut     # all applicable nonprivileged tests
./tools/openwrt.sh --kernel  # explicit sudo; isolated network namespace
```

**No test logic is duplicated in the driver.** It orchestrates the same
independent scripts developers and CI may run directly. `--uut` includes
`--test`; do not normally run both on every change unless isolating a failure.

OpenWrt-native tests are named by their purpose:

```sh
bash openwrt/test/setup-test.sh
bash openwrt/test/recipe-test.sh
bash openwrt/test/runtime-test.sh
bash openwrt/test/config-test.sh
bash openwrt/test/interface-uut.sh    # Linux
bash openwrt/test/packet-uut.sh       # Linux, optional raw-socket test
bash openwrt/test/rib-uut.sh          # Linux read-only netlink + synthetic routes
sudo bash openwrt/test/rib-uut.sh --kernel # disposable namespace
```

`*-test.sh` covers component and build checks; `*-uut.sh` exercises real
host/protocol interfaces. On macOS, Linux-specific tests are clearly skipped.
The previous `*-smoke.sh` filenames were retired in OW-07 without aliases.

OW-08 introduced the retained configuration adapter. OW-09 connects it to
the procd-managed daemon. Use standard UCI commands (`uci set`, `uci delete`,
`uci commit openeigrp`) for writeback and the procd reload action to replay
configuration. See [specs/configuration.md](specs/configuration.md) for the
supported sections, reset semantics and best-effort rollback boundaries.

## OW-09: Service deployment and operations

On OpenWrt, install the generated `openeigrp` package and configure
`/etc/config/openeigrp` using UCI. To validate and activate it:

```sh
openeigrpd --check
/etc/init.d/openeigrp enable
/etc/init.d/openeigrp start
/etc/init.d/openeigrp status
openeigrpctl status
openeigrpctl neighbors
openeigrpctl interfaces
openeigrpctl topology
openeigrpctl traffic
openeigrpctl events
/etc/init.d/openeigrp reload
logread -e openeigrpd
```

The operational socket is `/var/run/openeigrpd.sock` with mode 0600, so
console access normally requires root. Its protocol is read-only; it cannot
modify configuration, interfaces or routes. The binary stays in the foreground
for procd, rather than forking its own daemon process. UCI startup validation
is also available without opening sockets using `openeigrpd --check`.

Configuration/reload failure is reported to logread and must not silently
replace the active configuration. Rollback of semantic changes currently uses
OW-08's documented best-effort behavior, not an atomic transaction.

Developers run `./tools/openwrt.sh --uut`, which includes the independent
`openwrt/test/daemon-uut.sh` lifecycle test. The daemon UUT links the real
portable core against native host code but uses a **test-only** UCI reader;
real libuci and procd on a target remain separate acceptance gates.

## OW-10 image and real QEMU unit-under-test

On Debian/Ubuntu, `--setup` installs the QEMU system emulators for both
supported UUT profiles (`qemu-system-arm` provides `qemu-system-aarch64`;
`qemu-system-x86` provides `qemu-system-x86_64`) and `iproute2` for isolated
kernel tests. Existing development hosts can install the ARM64 prerequisite
without rerunning setup: `sudo apt-get install qemu-system-arm`. `--verify`
reports QEMU availability for the selected profile but does not block ordinary
package builds when an optional emulator is absent. `--qemu` requires the
correct emulator and reports its package name before attempting to boot.
The QEMU emulator itself is Linux-only in this integration; `--setup` on macOS
does not install it.

After `--install`, `--config`, and a successful `--build`, use the same
OpenWrt driver for the full integration gate:

```sh
./tools/openwrt.sh --test               # component tests including QEMU harness contracts
./tools/openwrt.sh --uut                # all nonprivileged native host UUTs
./tools/openwrt.sh --image-build --jobs 2  # full OpenWrt firmware image (explicit)
./tools/openwrt.sh --qemu --log-dir ./ow10-logs
```

The real QEMU UUT boots **two OpenWrt nodes** on an isolated virtual Ethernet
link, applies UCI IPv4 named AS 4453, and requires neighbors in Up state plus
Linux EIGRP protocol-192 routes to the peer's dummy interface prefix. It
fails if any guest, package, adjacency or route requirement is missing.
On `armsr/armv8`, a built `u-boot-qemu_armv8` firmware binary is required;
use `--firmware` to select one explicitly. `--image` selects an existing
`ext4-combined` disk. Individual `openwrt/test/qemu-test.sh` and
`openwrt/test/qemu-uut.py` remain usable for diagnosis.

If QEMU reports that the OpenWrt serial shell did not start, this is a
**boot/console gate failure**, not proof of an EIGRP protocol failure. With
`--log-dir ./ow10-logs`, inspect both `node1.serial.log` (guest U-Boot,
GRUB and Linux console) and `node1.qemu.log` (QEMU errors); the harness
also prints the end of both logs on boot failure. Verify that U-Boot loads
EFI from the combined disk and that Linux reaches an interactive serial
shell before attempting to diagnose adjacency. The source image is never
modified by the runner.

GitHub Actions uses Linux ARM64 and x86/64 cross-build jobs; the x86 job also
builds and boots a full firmware image and runs the two-node UUT. Only the
OpenWrt download source cache is restored: old toolchain binaries are not
silently reused. See [specs/uut.md](specs/uut.md).

