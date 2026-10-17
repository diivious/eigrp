# OpenEIGRP OpenWrt shim

Copyright (C) 2026 Donnie V. Savage

## Purpose and current status

`openwrt/` is the **native OpenWrt host shim** for the portable OpenEIGRP
implementation in `../eigrp/code/`. It is the OpenWrt-specific translation
boundary between OpenWrt host mechanics and the portable RFC 7868 EIGRP core.

OpenWrt is the operating system, packaging, and service-management environment.
It is **not** another EIGRP implementation. DUAL, RTP, topology, packet
semantics, metric handling, and neighbor behavior remain in `../eigrp/code/`.

**Status: Phase 1, host setup, target configuration and design specification.**

- `../tools/openwrt.sh --setup` prepares a Debian/Ubuntu or macOS developer
  host and an OpenWrt buildroot checkout.
- `../tools/openwrt.sh --config` owns OpenWrt target selection, `defconfig`
  and prerequisite validation; `--verify` checks the environment read-only.
- `openwrt/specs/*.md` define how the OpenWrt shim is intended to work.
- No deployable OpenEIGRP OpenWrt package, daemon runtime, UCI integration,
  or live UUT is claimed at this stage.

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
./scripts/feeds install -a
```

No firmware, toolchain, or OpenEIGRP package is built during initial setup.
OpenWrt builds should be run as your normal user, not root.

**All OpenWrt target configuration is driven through `--config`.** For the
recommended generic ARM64 QEMU test target, use:

```sh
./tools/openwrt.sh --config --target armsr/armv8
./tools/openwrt.sh --verify
```

This chooses `armsr` / `armv8` / generic EFI noninteractively, then runs
OpenWrt's `make defconfig` and `make prereq`. To select or change other
supported target platforms interactively, run:

```sh
./tools/openwrt.sh --config
```

That action opens OpenWrt's `menuconfig`, then runs `defconfig` and `prereq`.
A preexisting `.config` is preserved when using interactive selection; the
scripted ARM64 selection writes a separate backup before changing it.
**The Debian VM's architecture does not dictate the target architecture.**
MediaTek platforms should be selected only when deploying to compatible
MediaTek hardware, not because the build host runs on Apple Silicon.

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

## Bringing the finished shim into an OpenWrt checkout

The preferred model is an **external local OpenEIGRP feed** referenced by the
OpenWrt checkout.

```text
OpenEIGRP checkout                          OpenWrt checkout
------------------                          -----------------
eigrp/openwrt/feed/  <---- src-link ----    feeds.conf
        |
        +-- openeigrp/Makefile
        +-- openeigrp/files/
        +-- openeigrp source staging rules
```

When the feed exists, add a local entry to the OpenWrt checkout's `feeds.conf`
(start by copying `feeds.conf.default` if necessary):

```text
src-link openeigrp /absolute/path/to/eigrp/openwrt/feed
```

Then, from the OpenWrt checkout:

```sh
./scripts/feeds update openeigrp
./scripts/feeds install -p openeigrp openeigrp
# Return to OpenEIGRP checkout to configure the OpenWrt target/package:
/path/to/eigrp/tools/openwrt.sh --config --openwrt-root /path/to/openwrt
# Future --build will own package compilation.
# (Package build is not implemented yet.)
```

The package name and exact package targets will be finalized when the package
recipe is implemented. The feed should stage only the OpenEIGRP package and its
service/configuration payload. It must not replace OpenWrt internals.

If OpenWrt-wide patches are ever required, keep them under `openwrt/patch/`,
apply them only with an explicit future `--patch`, and reject any patch that
does not apply cleanly.

## Planned driver actions

| Action | Status | Intended semantics |
|---|---|---|
| `--setup` | Implemented | Host prerequisites, fork checkout, feeds |
| `--verify` | Implemented | Read-only host, checkout and target checks |
| `--config` | Implemented | Menuconfig or ARMv8 preset, defconfig, prereq |
| `--patch` | Future | Idempotent, explicit, managed OpenWrt-wide patches only |
| `--install` | Future | Stage OpenEIGRP feed/package into buildroot without patches |
| `--build` | Future | Compile a selected OpenWrt package/target, no silent patching |
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
