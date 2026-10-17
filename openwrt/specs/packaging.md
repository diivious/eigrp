# OpenEIGRP OpenWrt packaging and build specification

Copyright (C) 2026 Donnie V. Savage

## 1. Scope

This document describes how the OpenEIGRP OpenWrt shim should be packaged and
introduced into an OpenWrt buildroot.

The preferred approach is an **external OpenEIGRP-owned feed** that OpenWrt can
consume through `src-link` or an equivalent feed reference.

## 2. Packaging overview

```text
+-----------------------------------+        +---------------------------+
| OpenEIGRP checkout                |        | OpenWrt buildroot         |
|                                   |        |                           |
| openwrt/feed/openeigrp/           | <----> | feeds.conf / feeds.conf.d |
|   openeigrp/Makefile              |        | scripts/feeds             |
|   openeigrp/files/                |        | package index             |
|   staging/source rules            |        |                           |
+----------------+------------------+        +-------------+-------------+
                 |                                               |
                 v                                               v
          package source staging                         toolchain / target build
                 |                                               |
                 +----------------------------+------------------+
                                              |
                                              v
                                   package/openeigrp/compile
```

## 3. Layout

Planned OpenEIGRP-owned layout:

```text
openwrt/
├── README.md
├── TASKS.md
├── specs/
├── code/                     (future native runtime shim)
├── feed/
│   └── openeigrp/
│       └── Makefile           (OW-02 portable static-core package)
│       # procd/UCI files added when their runtime targets exist
└── patch/                    (future, only if unavoidable)
```

## 4. Feed model

### 4.1 Preferred integration

The OpenWrt checkout should consume the OpenEIGRP package as an external local
feed.

```text
feeds.conf:
    src-link openeigrp /absolute/path/to/eigrp/openwrt/feed
```

Then:

```sh
./scripts/feeds update openeigrp
./scripts/feeds install -p openeigrp openeigrp
```

### 4.2 Why a feed is preferred

- keeps the OpenEIGRP repository authoritative for the shim;
- avoids copying package recipes into `openwrt/package/` by hand;
- keeps OpenWrt and OpenEIGRP histories independent;
- makes local iteration simpler and more repeatable.

## 5. Package boundaries

The OW-02 package **actually builds and installs `libeigrp-core.a`**,
using OpenWrt's selected `TARGET_CC` and `TARGET_AR`. Its developer staging
exports `eigrp.h`, `eigrp_cli.h`, `eigrp_mgnt.h`, `eigrp_rib.h`, and
`eigrp_sys.h`. It does not install an executable daemon.

When the real OpenWrt shim and daemon are implemented, the package can expand
to include:

- executable daemon binary;
- init/procd service definition;
- default UCI config file;
- optional ubus or helper scripts;
- supporting runtime data owned by OpenEIGRP.

The package must **not** replace unrelated OpenWrt base files or modify
OpenWrt-wide internals as part of normal install.

## 6. Build flow

```text
tools/openwrt.sh --install
    -> verify checkout
    -> register local OpenEIGRP feed and package symlink

tools/openwrt.sh --config
    -> select/preserve OpenWrt target and CONFIG_PACKAGE_openeigrp=y
    -> run defconfig and prerequisite checks

tools/openwrt.sh --build [--jobs N]
    -> OpenWrt tools/install (host utilities)
    -> OpenWrt toolchain/install (selected architecture)
    -> OpenWrt package/openeigrp/compile (portable core)
    -> build the openeigrp package containing libeigrp-core.a
```

## 7. Versioning and source selection

The package recipe should record:

- OpenEIGRP revision;
- package version/release;
- OpenWrt buildroot revision used for build;
- any local staging source path or tarball source metadata.

A developer should always be able to identify exactly which OpenEIGRP source
revision produced a package artifact.

## 8. Patch ownership

If OpenWrt-wide patches become unavoidable, they must obey strict rules.

```text
openwrt/patch/
    0001-...
    0002-...
```

Rules:

- patch application must be explicit via a future `tools/openwrt.sh --patch`;
- package install or build must not silently apply them;
- application must be idempotent and exact;
- reverse-check an already applied patch;
- reject conflicts and any patch that requires fuzz/force.

## 9. Service payload concept

```text
OW-02 package install
    -> /usr/lib/libeigrp-core.a       static core archive

OW-04 through OW-09 (not shipped yet)
    -> /usr/sbin/openeigrpd          daemon
    -> /etc/init.d/openeigrp         procd service script
    -> /etc/config/openeigrp         retained UCI config
```

The exact binary and file names remain to be finalized, but the ownership model
should look like the diagram above.

## 10. Development host considerations

### Debian/Linux

This is the primary supported package build environment.

### macOS

macOS should support as much setup and package compilation as OpenWrt's tool
requirements permit, especially for local development on a MacBook Pro, but it
remains an experimental build environment for the full OpenWrt package flow.

## 11. Future image build relationship

Once the package and runtime are mature, the same package output becomes the
basis for:

- QEMU or VM test images;
- device-installable package artifacts;
- later `openeigrp-owrt` container or image automation.

```text
OpenEIGRP feed/package
        |
        +--> OpenWrt package build
                 |
                 +--> QEMU/device UUT
                 |
                 +--> future openeigrp-owrt image pipeline
```
