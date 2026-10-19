# OpenWrt real UUT and CI acceptance (OW-10)

Copyright (C) 2026 Donnie V. Savage

## Acceptance boundaries

The portable RFC 7868 engine remains in `eigrp/code/`. The OpenWrt adapter
owns kernel sockets, interface discovery, RIB, UCI and procd service lifecycle.
OW-10 does not add an independent protocol implementation or fake peer state.

**Three independent acceptance layers:**

1. `tools/openwrt.sh --test` / `--uut` runs existing native component and
   process tests, plus an offline QEMU harness-contract test. Tests stay
   individually runnable as `openwrt/test/*-test.sh` / `*-uut.sh`.
2. `--build` compiles the real daemon and `libuci` for the selected OpenWrt
   cross-target. `--image-build` explicitly produces a full bootable image;
   it is NOT a side effect of the regular package build.
3. `--qemu` runs **two real OpenWrt guests** using two private copies of
   the same firmware image. It configures both using guest UCI, starts the
   guest procd service, and requires bidirectional EIGRP neighbors in `Up`
   and two distinct EIGRP-installed routes in the Linux FIB. This is the
   actual protocol and kernel acceptance gate.

```text
  Host: tools/openwrt.sh --qemu
       │
       ├── QEMU Node 1 ── protocol 88 / EIGRP Hello ── QEMU Node 2
       │    10.99.0.1/24     Ethernet socket link      10.99.0.2/24
       │    br-lan          (QEMU -netdev socket)       br-lan
       │
       │    eigrptest0                                  eigrptest0
       │    10.77.1.0/24                                10.77.2.0/24
       │
       └── Assertions
            - each node sees neighbor state=Up
            - node 1: 10.77.2.0/24 via 10.99.0.2 proto 192
            - node 2: 10.77.1.0/24 via 10.99.0.1 proto 192
```

## Requirements and isolation

- Linux development host with Python 3 and QEMU system emulator for the image.
  The OpenWrt `--setup` action installs `qemu-system-arm` and `qemu-system-x86`
  on Debian/Ubuntu along with `iproute2`; `--setup --skip-packages` does not
  install them. `--verify` reports missing optional QEMU prerequisites, while
  `--qemu` refuses to run without the selected emulator and gives an actionable
  package-install command.
- An OpenWrt `ext4-combined` disk image built with `CONFIG_PACKAGE_openeigrp=y`,
  `CONFIG_PACKAGE_kmod-dummy=y`, `CONFIG_PACKAGE_ip-full=y` and serial console enabled. The driver
  `--image-build` sets the ext4, iproute2 and dummy package requirements. Other boot
  prerequisites remain target-specific and must not be silently ignored.
- x86/64 uses QEMU's PC firmware and `qemu-system-x86_64`; armsr/armv8 uses
  `qemu-system-aarch64` and the OpenWrt-built `u-boot-qemu_armv8` firmware.
- The two QEMU disk copies are temporary; the source image is read-only.
  The Ethernet link is connected through QEMU's socket backend on
  `127.0.0.1`, so it needs no TAP device, bridge, root privileges, or host
  route/interface mutations.
- The test uses the unconfigured OpenWrt guest's serial root shell and
  configures named mode (`savage`, IPv4, AS 4453). It starts the actual
  `/etc/init.d/openeigrp` service, not a mocked process.
- Each guest must support OpenWrt's `br-lan` default, `kmod-dummy`, UCI,
  procd and `iproute2`. Missing functionality is a **failure**, not a skip.
- All guest processes and sockets are terminated in a `finally` block.
  `--log-dir PATH` preserves both QEMU stderr/stdout and the separate guest
  serial boot transcript for troubleshooting. A boot failure includes the
  last messages from each. Missing shell access is not classified as an
  EIGRP protocol failure.

## Central developer commands

```sh
cd eigrp
./tools/openwrt.sh --uut
./tools/openwrt.sh --build
./tools/openwrt.sh --image-build --jobs 2
./tools/openwrt.sh --qemu --log-dir ./ow10-logs
```

If the OpenEIGRP checkout was renamed or moved from `eigrp/` to
`openeigrp/`, rerun `./tools/openwrt.sh --install` before package or image
build. It safely repairs a broken source-link for the old absent sibling
checkout; existing other checkouts and third-party feed entries remain
protected. Rebuild the image after re-registering the feed so QEMU boots
fresh binaries.

If the QEMU serial shell gate fails, check `ow10-logs/node1.serial.log`
and `ow10-logs/node1.qemu.log`, then the corresponding node 2 logs. The
boot trace must confirm U-Boot/EFI, kernel initialization and OpenWrt shell
before the UUT can evaluate neighbors or routes.

When using a prebuilt image, run `./tools/openwrt.sh --qemu --image
/path/to/openwrt-...ext4-combined.img.gz`. For ARM64, the optional
`--firmware` points at `u-boot-qemu_armv8/u-boot.bin` when it is not in
OpenWrt's usual target image directory.

## GitHub Actions contract

`.github/workflows/openwrt.yml` performs a Linux/macOS host UUT gate, an
ARM64 (`armsr/armv8`) cross-build on a Linux ARM runner, and x86/64 on a
Linux x86 runner. The x86 runner additionally builds the complete image and
runs the two-guest QEMU UUT. Every CI step invokes `tools/openwrt.sh`; the
Python UUT remains separately runnable for diagnosis.

OpenWrt source revision is pinned at
`cd2c55e43b4e648c6e74101284ebff70fb050403`; CI verifies it and fails
on modified tracked OpenWrt files. The download cache contains only
source archives under `openwrt/dl/`; it does **not** reuse an opaque old
compiler/toolchain or package output. The acceptance job rejects any
failed/skipped dependency.

## Status and limits

OW-09's ARM64 package cross-build was reported passing on Aurora. OW-10's
**offline** QEMU-contract tests may be run on a normal Linux/macOS host.
Actual two-guest QEMU boot, neighbor formation, route exchange and GitHub
Actions job execution must be validated on Aurora/CI. A successful offline
harness test is not evidence of live adjacency.

Two-node IPv4 named AS 4453 is the current mandatory protocol gate; IPv6,
additional AS instances, reconnect, convergence under flaps, and device
hardware acceptance can be expanded in subsequent UUT increments.
