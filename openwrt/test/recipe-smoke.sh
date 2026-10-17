#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
#
# Test the package recipe with a synthetic tiny core and the native C compiler.
# This validates staging/toolchain flags/install destinations without claiming
# that an OpenWrt cross-toolchain or actual OpenEIGRP runtime was built.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
recipe="$root/openwrt/feed/openeigrp/Makefile"
command -v make >/dev/null
command -v cc >/dev/null
command -v ar >/dev/null

tmp="$(mktemp -d "${TMPDIR:-/tmp}/openeigrp-recipe.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/eigrp/openwrt/feed/openeigrp" "$tmp/eigrp/eigrp/code" \
    "$tmp/openwrt/include" "$tmp/out" "$tmp/dev"
cp "$recipe" "$tmp/eigrp/openwrt/feed/openeigrp/Makefile"
cat > "$tmp/openwrt/rules.mk" <<'MK'
BUILD_DIR:=$(TOPDIR)/build
INCLUDE_DIR:=$(TOPDIR)/include
TARGET_CC:=cc
TARGET_AR:=ar
TARGET_CFLAGS:=-O2
TARGET_CPPFLAGS:=
INSTALL_DIR:=mkdir -p
INSTALL_DATA:=install -m 0644
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
    printf '#pragma once\n' > "$tmp/eigrp/eigrp/code/$name"
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
.PHONY: smoke
smoke:
	$(Build/Prepare)
	$(Build/Compile)
	$(call Build/InstallDev,$(TEST_DEV))
	$(call Package/openeigrp/install,$(TEST_OUT))
MK
make -s -f "$tmp/test.mk" smoke \
    TEST_TOPDIR="$tmp/openwrt" \
    TEST_RECIPE="$tmp/eigrp/openwrt/feed/openeigrp/Makefile" \
    TEST_DEV="$tmp/dev" TEST_OUT="$tmp/out"
test -s "$tmp/out/usr/lib/libeigrp-core.a"
test -s "$tmp/dev/usr/lib/libeigrp-core.a"
test -f "$tmp/dev/usr/include/openeigrp/eigrp_sys.h"
if command -v nm >/dev/null 2>&1; then
    nm "$tmp/out/usr/lib/libeigrp-core.a" | grep -q 'eigrp_core_compiled_for_target'
fi
printf '%s\n' 'OpenWrt core-package recipe staging/build/install smoke: PASS'
