#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Smoke tests for the OpenWrt setup/config/verify driver. No apt/brew/network.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
driver="$root/tools/openwrt.sh"
envscript="$root/openwrt/dev-env.sh"

bash -n "$driver" "$envscript"
"$driver" --help | grep -q -- '--setup'
"$driver" --help | grep -q -- '--verify'
"$driver" --help | grep -q -- '--config'

if "$driver" --doctor > /dev/null 2>&1; then
	echo 'error: removed --doctor unexpectedly accepted' >&2
	exit 1
fi
if "$driver" --patch > /dev/null 2>&1; then
	echo 'error: --patch unexpectedly succeeded before implementation' >&2
	exit 1
fi
if "$driver" --setup --verify > /dev/null 2>&1; then
	echo 'error: multiple actions were accepted' >&2
	exit 1
fi
if "$driver" --setup --target armsr/armv8 > /dev/null 2>&1; then
	echo 'error: --target unexpectedly accepted outside --config' >&2
	exit 1
fi

# CI's macOS runner may have a case-insensitive filesystem. Functional
# checkout tests require Linux and are run in the Ubuntu matrix leg.
if [[ "$(uname -s)" != Linux ]]; then
	echo 'OpenWrt script syntax/help/action guards: PASS (macOS)'
	exit 0
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/openwrt-smoke.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/fake-bin" "$tmp/openwrt/scripts" \
	"$tmp/openwrt/target/linux/armsr/armv8"
# Minimal CI images may not preinstall these OpenWrt build-only tools.
for tool in gawk flex bison; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		ln -s /usr/bin/true "$tmp/fake-bin/$tool"
	fi
done
cat > "$tmp/fake-bin/make" <<'MOCK'
#!/bin/sh
if [ "$1" = '--version' ]; then
  echo 'GNU Make 4.4'
  exit 0
fi
printf '%s\n' "$*" >> "$MAKE_CALL_LOG"
MOCK
chmod +x "$tmp/fake-bin/make"
export PATH="$tmp/fake-bin:$PATH"
export MAKE_CALL_LOG="$tmp/make.log"

printf 'world:\n\t@true\n' > "$tmp/openwrt/Makefile"
printf '#!/bin/sh\nexit 0\n' > "$tmp/openwrt/scripts/feeds"
printf 'ARCH:=aarch64\n' > "$tmp/openwrt/target/linux/armsr/armv8/target.mk"
chmod +x "$tmp/openwrt/scripts/feeds"

git -C "$tmp/openwrt" init -q
git -C "$tmp/openwrt" remote add origin https://github.com/diivious/openwrt.git
git -C "$tmp/openwrt" -c user.name=CI -c user.email=ci@example.invalid \
	commit --allow-empty -qm 'fixture'

original="$(git -C "$tmp/openwrt" rev-parse HEAD)"
"$driver" --setup --skip-packages --skip-clone --skip-feeds \
	--openwrt-root "$tmp/openwrt" > "$tmp/setup.log"
[[ "$original" == "$(git -C "$tmp/openwrt" rev-parse HEAD)" ]]
"$driver" --verify --openwrt-root "$tmp/openwrt" > "$tmp/verify.log"
grep -q 'not configured' "$tmp/verify.log"

# --config owns both Kconfig reconciliation and prerequisite validation.
printf '%s\n' 'CONFIG_PACKAGE_demo=y' 'CONFIG_TARGET_x86=y' \
	'CONFIG_TARGET_x86_64=y' > "$tmp/openwrt/.config"
"$driver" --config --target armsr/armv8 --openwrt-root "$tmp/openwrt" \
	> "$tmp/config.log"
grep -qx 'CONFIG_PACKAGE_demo=y' "$tmp/openwrt/.config"
grep -qx 'CONFIG_TARGET_armsr=y' "$tmp/openwrt/.config"
grep -qx 'CONFIG_TARGET_armsr_armv8=y' "$tmp/openwrt/.config"
grep -qx 'CONFIG_TARGET_armsr_armv8_DEVICE_generic=y' "$tmp/openwrt/.config"
! grep -q '^CONFIG_TARGET_x86' "$tmp/openwrt/.config"
grep -qx 'defconfig' "$tmp/make.log"
grep -qx 'prereq' "$tmp/make.log"
ls "$tmp/openwrt/".config.before-openeigrp.* >/dev/null
"$driver" --verify --openwrt-root "$tmp/openwrt" > "$tmp/verify.log"
grep -q 'armsr/armv8 (configured)' "$tmp/verify.log"
[[ "$original" == "$(git -C "$tmp/openwrt" rev-parse HEAD)" ]]

# Calling --config more than once must not require deleting a backup.
"$driver" --config --target armsr/armv8 --openwrt-root "$tmp/openwrt" \
	> /dev/null

# Never continue against a different source repository.
git -C "$tmp/openwrt" remote set-url origin https://github.com/elsewhere/openwrt.git
if "$driver" --setup --skip-packages --skip-clone --skip-feeds \
	--openwrt-root "$tmp/openwrt" > /dev/null 2>&1; then
	echo 'error: foreign checkout was accepted' >&2
	exit 1
fi
if "$driver" --config --target armsr/armv8 --openwrt-root "$tmp/openwrt" \
	> /dev/null 2>&1; then
	echo 'error: foreign checkout was configured' >&2
	exit 1
fi

echo 'OpenWrt setup/config/verify functional smoke: PASS (Linux)'
