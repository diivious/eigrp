# OpenEIGRP OpenWrt shim

Copyright (C) 2026 Donnie V. Savage

## Purpose and current status

`openwrt/` is the **native OpenWrt host shim** for the portable OpenEIGRP
implementation in `../eigrp/code/`. It is the OpenWrt-specific translation
boundary between OpenWrt host mechanics and the portable RFC 7868 EIGRP core.

OpenWrt is the operating system, packaging, and service-management environment.
It is **not** another EIGRP implementation. DUAL, RTP, topology, packet
semantics, metric handling, and neighbor behavior remain in `../eigrp/code/`.

**Status: OW-03 build orchestration plus a buildable portable-core OpenWrt package.**

- `../tools/openwrt.sh --setup` prepares a Debian/Ubuntu or macOS developer
  host and an OpenWrt buildroot checkout.
- `../tools/openwrt.sh --config` owns OpenWrt target selection, `defconfig`
  and prerequisite validation; `--verify` checks host and stored target values
  read-only, but does not re-run Kconfig.
- `../tools/openwrt.sh --install` registers the local OpenEIGRP feed without
  installing unrelated packages or modifying OpenWrt-wide source files.
- `--config` selects `CONFIG_PACKAGE_openeigrp=y` when the package is registered.
- The package builds and installs the **real portable static core archive**
  (`libeigrp-core.a`) and stages the five public host-interface headers.
- **Not yet implemented:** an executable OpenWrt daemon, live packet transport,
  kernel route integration, UCI/procd/ubus runtime, or live UUT.

See [TASKS.md](TASKS.md) for the staged implementation plan.

## Authoritative source repositories

- OpenEIGRP: <https://github.com/diivious/eigrp>, branch `master`
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
  UCI configuration model, reload/writeback, procd lifecycle, ubus/CLI
  management surface, and semantic mapping to `eigrp_cli.h`.
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
eigrp/code/*.c, *.h ----+                     feeds.conf
                        |                         |
openwrt/feed/openeigrp/Makefile <-- src-link ----+
                        |
                        +--> OpenWrt Build/Prepare
                                   |
                                   v
                              target toolchain
                                   |
                                   v
                        libeigrp-core.a / openeigrp package

       [No runtime daemon yet: OW-04 through OW-09]
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
append duplicate entries. An existing conflicting feed entry or package
symlink is refused rather than overwritten. The action does **not** modify
`.config`, download unrelated feeds, or apply any patches.

**Configuration remains behind `--config`.** Once the recipe is available,
`--config` selects `CONFIG_PACKAGE_openeigrp=y`, preserves the target
(e.g. `armsr/armv8`), and runs `defconfig` plus `prereq` validation.

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
  OpenEIGRP target static library / package
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

This first package is intentionally a **static development artifact**, not an
operational router daemon. It installs `/usr/lib/libeigrp-core.a` in the
package and stages the public headers under
`staging_dir/target-*/usr/include/openeigrp/`. This is meaningful because
OpenWrt must compile the existing protocol engine for the selected target,
without any substituted or host-architecture objects.

When you edit the portable core after a successful initial build, use
`make package/openeigrp/clean` before recompiling to force restaging of local
source changes. Automatic incremental source tracking for edited local files is not yet
implemented by the OW-02 recipe. `--build` respects OpenWrt stamps.

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

## Planned driver actions

| Action | Status | Intended semantics |
|---|---|---|
| `--setup` | Implemented | Host prerequisites, fork checkout, feeds |
| `--verify` | Implemented | Read-only host, checkout and target checks |
| `--config` | Implemented | Select target and registered OpenEIGRP package, run Kconfig/prereq; optional `--target`/`--menuconfig` |
| `--install` | Implemented | Register the single local OpenEIGRP feed/package, idempotently |
| `--patch` | Future | Idempotent, explicit, managed OpenWrt-wide patches only |
| `--build` | Implemented | Host tools → toolchain → OpenEIGRP package, `--jobs N` (default 2) |
| `--uut` | Future | Execute native/VM/OpenWrt acceptance gates against a built image |

## Test levels

1. **macOS + Debian**: existing root `make test` gate and shim source or
   compile checks where supported.
2. **Linux builder**: OpenWrt package compile for one configured target,
   preferably followed by a second architecture before broad support claims.
3. **OpenWrt QEMU/device**: procd start/stop, UCI reload/writeback, raw IPv4
   and IPv6 EIGRP adjacencies, route install/withdrawal, redistribution,
   interface churn, daemon restart, and teardown.
4. **CI**: macOS/Linux setup-script smoke first, then Linux OpenWrt build and
   QEMU UUT once the real package/runtime is implemented.

See [TASKS.md](TASKS.md) for the staged acceptance criteria.
