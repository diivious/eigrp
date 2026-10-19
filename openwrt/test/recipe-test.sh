#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
#
# Test the package recipe with a synthetic tiny core and the native C compiler.
# This validates staging/toolchain flags/install destinations without claiming
# that an OpenWrt cross-toolchain or actual OpenEIGRP runtime was built.
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    printf '%s\n' 'SKIP: OW-06 package recipe compiles Linux-specific netlink and protocol-88 code'
    exit 0
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
recipe="$root/openwrt/feed/openeigrp/Makefile"
command -v make >/dev/null
command -v cc >/dev/null
command -v ar >/dev/null

tmp="$(mktemp -d "${TMPDIR:-/tmp}/openeigrp-recipe.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/eigrp/openwrt/feed/openeigrp/files" "$tmp/eigrp/openwrt/code" \
    "$tmp/eigrp/eigrp/code" "$tmp/openwrt/include" "$tmp/out" "$tmp/dev"
cp "$root/openwrt/code/Makefile" "$root/openwrt/code/eigrp_owrt_runtime.c" \
    "$root/openwrt/code/eigrp_owrt_runtime.h" \
    "$root/openwrt/code/eigrp_owrt_interface.c" \
    "$root/openwrt/code/eigrp_owrt_interface.h" \
    "$root/openwrt/code/eigrp_owrt_interface_state.c" \
    "$root/openwrt/code/eigrp_owrt_packet.c" \
    "$root/openwrt/code/eigrp_owrt_packet.h" \
    "$root/openwrt/code/eigrp_owrt_rib.c" \
    "$root/openwrt/code/eigrp_owrt_rib.h" \
    "$root/openwrt/code/eigrp_owrt_rib_netlink.c" \
    "$root/openwrt/code/eigrp_owrt_config.c" \
    "$root/openwrt/code/eigrp_owrt_config.h" \
    "$root/openwrt/code/eigrp_owrt_policy.c" \
    "$root/openwrt/code/eigrp_owrt_uci.c" \
    "$root/openwrt/code/eigrp_owrt_mgnt.c" \
    "$root/openwrt/code/eigrp_owrt_mgnt.h" \
    "$root/openwrt/code/eigrp_owrt_daemon.c" \
    "$root/openwrt/code/eigrp_owrt_ctl.c" \
    "$tmp/eigrp/openwrt/code/"
cp "$recipe" "$tmp/eigrp/openwrt/feed/openeigrp/Makefile"
cp "$root/openwrt/feed/openeigrp/files/openeigrp.config" "$tmp/eigrp/openwrt/feed/openeigrp/files/openeigrp.config"
cp "$root/openwrt/feed/openeigrp/files/openeigrp.init" "$tmp/eigrp/openwrt/feed/openeigrp/files/openeigrp.init"
cat > "$tmp/openwrt/rules.mk" <<'MK'
BUILD_DIR:=$(TOPDIR)/build
INCLUDE_DIR:=$(TOPDIR)/include
TARGET_CC:=cc
TARGET_AR:=ar
TARGET_CFLAGS:=-O2
TARGET_CPPFLAGS:=
INSTALL_DIR:=mkdir -p
INSTALL_DATA:=install -m 0644
INSTALL_CONF:=install -m 0600
INSTALL_BIN:=install -m 0755
CP:=cp -fp
MK
cat > "$tmp/openwrt/include/package.mk" <<'MK'
# No-op registration; test invokes the real recipe macros directly below.
define BuildPackage
endef
MK
cat > "$tmp/eigrp/eigrp/code/core.c" <<'C'
int eigrp_core_compiled_for_target(void) { return 7868; }
C
for name in eigrp.h eigrp_cli.h eigrp_mgnt.h eigrp_rib.h eigrp_sys.h; do
    cp "$root/eigrp/code/$name" "$tmp/eigrp/eigrp/code/$name"
done
cat > "$tmp/eigrp/eigrp/code/Makefile" <<'MK'
libeigrp-core.a: core.o
	$(AR) rcs $@ core.o
core.o: core.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<
MK
cat > "$tmp/test.mk" <<'MK'
TOPDIR:=$(TEST_TOPDIR)
include $(TEST_RECIPE)
.PHONY: test
test:
	$(Build/Prepare)
	$(Build/Compile)
	printf '%s\n' 'int main(void) { return 0; }' > $(PKG_BUILD_DIR)/openwrt-runtime/recipe-client.c
	$(TARGET_CC) -o $(PKG_BUILD_DIR)/openwrt-runtime/openeigrpd $(PKG_BUILD_DIR)/openwrt-runtime/recipe-client.c
	$(TARGET_CC) -o $(PKG_BUILD_DIR)/openwrt-runtime/openeigrpctl $(PKG_BUILD_DIR)/openwrt-runtime/recipe-client.c
	$(call Build/InstallDev,$(TEST_DEV))
	$(call Package/openeigrp/install,$(TEST_OUT))
MK
make -s -f "$tmp/test.mk" test \
    TEST_TOPDIR="$tmp/openwrt" \
    TEST_RECIPE="$tmp/eigrp/openwrt/feed/openeigrp/Makefile" \
    TEST_DEV="$tmp/dev" TEST_OUT="$tmp/out" OPENEIGRP_UCI_BUILD=0
test -f "$tmp/out/etc/config/openeigrp"
test -x "$tmp/out/etc/init.d/openeigrp"
test -x "$tmp/out/usr/sbin/openeigrpd"
test -x "$tmp/out/usr/sbin/openeigrpctl"
test -s "$tmp/out/usr/lib/libeigrp-core.a"
test -s "$tmp/out/usr/lib/libeigrp-owrt-runtime.a"
test -s "$tmp/dev/usr/lib/libeigrp-owrt-runtime.a"
test -s "$tmp/dev/usr/lib/libeigrp-core.a"
test -f "$tmp/dev/usr/include/openeigrp/eigrp_sys.h"
test -f "$tmp/dev/usr/include/openeigrp/eigrp_owrt_runtime.h"
test -f "$tmp/dev/usr/include/openeigrp/eigrp_owrt_interface.h"
test -f "$tmp/dev/usr/include/openeigrp/eigrp_owrt_packet.h"
test -f "$tmp/dev/usr/include/openeigrp/eigrp_owrt_rib.h"
test -f "$tmp/dev/usr/include/openeigrp/eigrp_owrt_config.h"
if command -v nm >/dev/null 2>&1; then
    nm "$tmp/out/usr/lib/libeigrp-core.a" | grep -q 'eigrp_core_compiled_for_target'
fi
printf '%s\n' 'OpenWrt OW-09 recipe staging/archive/package file permissions test: PASS'
