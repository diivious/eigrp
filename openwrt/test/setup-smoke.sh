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
"$driver" --help | grep -q -- '--install'
"$driver" --help | grep -q -- '--config'
"$driver" --help | grep -q -- '--build'

if "$driver" --doctor > /dev/null 2>&1; then
	echo 'error: removed --doctor unexpectedly accepted' >&2
	exit 1
fi
if "$driver" --install --target armsr/armv8 > /dev/null 2>&1; then
	echo 'error: --install accepted --target' >&2
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
if "$driver" --config --target armsr/armv8 --menuconfig > /dev/null 2>&1; then
	echo 'error: --target and --menuconfig unexpectedly combined' >&2
	exit 1
fi
if "$driver" --setup --minimal-feeds > /dev/null 2>&1; then
	echo 'error: --minimal-feeds unexpectedly accepted outside --config' >&2
	exit 1
fi
if "$driver" --config --jobs 2 > /dev/null 2>&1; then
	echo 'error: --jobs unexpectedly accepted outside --build' >&2
	exit 1
fi
for invalid in 0 -1 nope 257; do
	if "$driver" --build --jobs "$invalid" > /dev/null 2>&1; then
		echo "error: invalid --jobs $invalid was accepted" >&2
		exit 1
	fi
done

# CI's macOS runner may have a case-insensitive filesystem. Functional
# checkout tests require Linux and are run in the Ubuntu matrix leg.
if [[ "$(uname -s)" != Linux ]]; then
	echo 'OpenWrt script syntax/help/action guards: PASS (macOS)'
	exit 0
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/openwrt-smoke.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/fake-bin" "$tmp/openwrt/scripts" \
	"$tmp/openwrt/target/linux/armsr/armv8" \
	"$tmp/openwrt/target/linux/x86/64"
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
if [ "${MOCK_FAIL_STAGE:-}" = "$2" ] && [ -n "${MOCK_FAIL_STAGE:-}" ]; then
  exit 42
fi
if [ "$2" = tools/install ] && [ "${MOCK_MISSING_HOST_TOOLS:-0}" != 1 ]; then
  mkdir -p staging_dir/host/bin
  : > staging_dir/host/bin/openssl
  : > staging_dir/host/bin/libdeflate-gzip
  chmod +x staging_dir/host/bin/openssl staging_dir/host/bin/libdeflate-gzip
fi
if [ "$1" = defconfig ] && [ "${MOCK_KCONFIG_ERROR:-0}" = 1 ]; then
  printf '%s\n' 'feeds/video/qt5/Config.in:18:error: recursive dependency detected!'
  printf '%s\n' '# No change to .config'
  exit 0
fi
MOCK
chmod +x "$tmp/fake-bin/make"
# Keep CI runner platform unchanged but simulate CPU architectures so the
# auto-default logic can be exercised on either Linux runner architecture.
cat > "$tmp/fake-bin/uname" <<'MOCK'
#!/bin/sh
if [ "$1" = '-m' ] && [ -n "${MOCK_HOST_ARCH:-}" ]; then
  printf '%s\n' "$MOCK_HOST_ARCH"
else
  /usr/bin/uname "$@"
fi
MOCK
chmod +x "$tmp/fake-bin/uname"
export PATH="$tmp/fake-bin:$PATH"
export MAKE_CALL_LOG="$tmp/make.log"
export FEEDS_CALL_LOG="$tmp/feeds.log"

printf 'world:\n\t@true\n' > "$tmp/openwrt/Makefile"
cat > "$tmp/openwrt/scripts/feeds" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" >> "$FEEDS_CALL_LOG"
if [ "$1" = install ] && [ "$2" = -p ] && [ "$3" = openeigrp ] && [ "$4" = openeigrp ]; then
  mkdir -p package/feeds/openeigrp
  ln -sfn "$EIGRP_FEED_FOR_SMOKE/openeigrp" package/feeds/openeigrp/openeigrp
fi
if [ "$1" = uninstall ] && [ "$2" = -a ]; then
  rm -rf package/feeds
fi
MOCK
printf 'ARCH:=aarch64\n' > "$tmp/openwrt/target/linux/armsr/armv8/target.mk"
printf 'ARCH:=x86_64\n' > "$tmp/openwrt/target/linux/x86/64/target.mk"
chmod +x "$tmp/openwrt/scripts/feeds"

git -C "$tmp/openwrt" init -q
git -C "$tmp/openwrt" remote add origin https://github.com/diivious/openwrt.git
git -C "$tmp/openwrt" -c user.name=CI -c user.email=ci@example.invalid \
	commit --allow-empty -qm 'fixture'

original="$(git -C "$tmp/openwrt" rev-parse HEAD)"
"$driver" --setup --skip-packages --skip-clone --skip-feeds \
	--openwrt-root "$tmp/openwrt" > "$tmp/setup.log"
[[ "$original" == "$(git -C "$tmp/openwrt" rev-parse HEAD)" ]]
# Setup should refresh feed indexes, but must not install every package.
"$driver" --setup --skip-packages --skip-clone \
	--openwrt-root "$tmp/openwrt" > "$tmp/setup-with-feeds.log"
grep -qx 'update -a' "$tmp/feeds.log"
! grep -q '^install -a$' "$tmp/feeds.log"
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
grep -q 'armsr/armv8 (selected;' "$tmp/verify.log"
[[ "$original" == "$(git -C "$tmp/openwrt" rev-parse HEAD)" ]]

# Plain --config must preserve an existing target, even if the host CPU
# would suggest a different generic default. It must not make a new backup.
backup_count_before="$(find "$tmp/openwrt" -maxdepth 1 -name '.config.before-openeigrp.*' | wc -l)"
export MOCK_HOST_ARCH=x86_64
"$driver" --config --openwrt-root "$tmp/openwrt" > "$tmp/preserve.log"
grep -q 'preserving existing target: armsr/armv8' "$tmp/preserve.log"
grep -qx 'CONFIG_TARGET_armsr_armv8=y' "$tmp/openwrt/.config"
[[ "$(find "$tmp/openwrt" -maxdepth 1 -name '.config.before-openeigrp.*' | wc -l)" == "$backup_count_before" ]]

# On an unconfigured ARM64 host, choose the generic ARMv8 target automatically.
rm -f "$tmp/openwrt/.config"
export MOCK_HOST_ARCH=aarch64
"$driver" --config --openwrt-root "$tmp/openwrt" > "$tmp/auto-arm.log"
grep -q 'auto-selected generic target armsr/armv8' "$tmp/auto-arm.log"
grep -qx 'CONFIG_TARGET_armsr_armv8=y' "$tmp/openwrt/.config"

# On an unconfigured x86-64 host, choose the generic x86/64 target.
rm -f "$tmp/openwrt/.config"
export MOCK_HOST_ARCH=x86_64
"$driver" --config --openwrt-root "$tmp/openwrt" > "$tmp/auto-x86.log"
grep -q 'auto-selected generic target x86/64' "$tmp/auto-x86.log"
grep -qx 'CONFIG_TARGET_x86_64=y' "$tmp/openwrt/.config"
grep -qx 'CONFIG_TARGET_x86_64_DEVICE_generic=y' "$tmp/openwrt/.config"

# Unsupported CPUs must require an explicit override rather than silently
# selecting unrelated router hardware.
rm -f "$tmp/openwrt/.config"
export MOCK_HOST_ARCH=unknowncpu
if "$driver" --config --openwrt-root "$tmp/openwrt" > /dev/null 2>&1; then
	echo 'error: unknown host CPU got a default target' >&2
	exit 1
fi
"$driver" --config --target armsr/armv8 --openwrt-root "$tmp/openwrt" > /dev/null
unset MOCK_HOST_ARCH

# Kconfig may report fatal dependency errors while make itself returns 0.
# The driver must fail, skip prereq and never claim configuration completed.
: > "$tmp/make.log"
export MOCK_KCONFIG_ERROR=1
if "$driver" --config --openwrt-root "$tmp/openwrt" > "$tmp/kconfig-failure.log" 2>&1; then
	echo 'error: --config ignored a recursive Kconfig dependency' >&2
	exit 1
fi
grep -q 'recursive dependency detected' "$tmp/kconfig-failure.log"
grep -q 'configuration was NOT validated' "$tmp/kconfig-failure.log"
! grep -q 'target configuration complete' "$tmp/kconfig-failure.log"
grep -qx defconfig "$tmp/make.log"
! grep -q '^prereq$' "$tmp/make.log"
unset MOCK_KCONFIG_ERROR

# Explicit repair removes optional feed package registration only; it must
# preserve feed sources, restore healthy config flow and be guarded against
# deleting developer-owned files under package/feeds.
mkdir -p "$tmp/openwrt/package/feeds/video" "$tmp/openwrt/feeds/video"
ln -s ../../../../feeds/video "$tmp/openwrt/package/feeds/video/qt5"
printf 'feed source\n' > "$tmp/openwrt/feeds/video/README"
"$driver" --config --minimal-feeds --openwrt-root "$tmp/openwrt" > "$tmp/minimal-feeds.log"
grep -qx 'uninstall -a' "$tmp/feeds.log"
ls "$tmp/openwrt/".config.before-openeigrp-feed-reset.* >/dev/null
[[ ! -e "$tmp/openwrt/package/feeds/video/qt5" ]]
grep -qx 'feed source' "$tmp/openwrt/feeds/video/README"

mkdir -p "$tmp/openwrt/package/feeds/custom"
printf 'owned\n' > "$tmp/openwrt/package/feeds/custom/private.txt"
if "$driver" --config --minimal-feeds --openwrt-root "$tmp/openwrt" > /dev/null 2>&1; then
	echo 'error: --minimal-feeds removed unowned files' >&2
	exit 1
fi
[[ -f "$tmp/openwrt/package/feeds/custom/private.txt" ]]
rm -rf "$tmp/openwrt/package/feeds"

# OW-02: --install registers exactly our local feed, not optional packages.
printf '%s\n' 'src-git packages https://example.invalid/packages.git' > "$tmp/openwrt/feeds.conf.default"
export EIGRP_FEED_FOR_SMOKE="$root/openwrt/feed"
"$driver" --install --openwrt-root "$tmp/openwrt" > "$tmp/install.log"
grep -qx "src-link openeigrp $EIGRP_FEED_FOR_SMOKE" "$tmp/openwrt/feeds.conf"
grep -qx 'update openeigrp' "$tmp/feeds.log"
grep -qx 'install -p openeigrp openeigrp' "$tmp/feeds.log"
[[ -L "$tmp/openwrt/package/feeds/openeigrp/openeigrp" ]]
# Idempotency: a second installation must not duplicate the feed declaration.
"$driver" --install --openwrt-root "$tmp/openwrt" > "$tmp/install-again.log"
[[ "$(grep -c '^src-link openeigrp ' "$tmp/openwrt/feeds.conf")" == 1 ]]
# Package selection belongs to --config, not --install.
! grep -qx 'CONFIG_PACKAGE_openeigrp=y' "$tmp/openwrt/.config"
"$driver" --config --openwrt-root "$tmp/openwrt" > "$tmp/config-pkg.log"
grep -qx 'CONFIG_PACKAGE_openeigrp=y' "$tmp/openwrt/.config"
"$driver" --verify --openwrt-root "$tmp/openwrt" > "$tmp/verify-pkg.log"
grep -q 'OpenEIGRP feed registered' "$tmp/verify-pkg.log"
# OW-03: --build must refuse unconfigured or unselected packages and never
# begin a build without its feed registration.
: > "$tmp/make.log"
cp "$tmp/openwrt/.config" "$tmp/healthy-config"
sed -i '/^CONFIG_PACKAGE_openeigrp=y$/d' "$tmp/openwrt/.config"
if "$driver" --build --openwrt-root "$tmp/openwrt" > "$tmp/build-unselected.log" 2>&1; then
	echo 'error: --build accepted an unselected OpenEIGRP package' >&2
	exit 1
fi
[[ ! -s "$tmp/make.log" ]]
cp "$tmp/healthy-config" "$tmp/openwrt/.config"

# The build performs the actual OpenWrt host tools, toolchain, and package
# targets in strict order. It neither rewrites .config nor modifies Git.
"$driver" --build --jobs 3 --openwrt-root "$tmp/openwrt" > "$tmp/build.log"
grep -qx -- '-j3 tools/install V=s' <(sed -n '1p' "$tmp/make.log")
grep -qx -- '-j3 toolchain/install V=s' <(sed -n '2p' "$tmp/make.log")
grep -qx -- '-j3 package/openeigrp/compile V=s' <(sed -n '3p' "$tmp/make.log")
cmp -s "$tmp/healthy-config" "$tmp/openwrt/.config"
[[ "$original" == "$(git -C "$tmp/openwrt" rev-parse HEAD)" ]]

# Repeated build must run the same incremental OpenWrt targets, relying on
# OpenWrt's own stamps rather than a driver-maintained completion marker.
: > "$tmp/make.log"
"$driver" --build --openwrt-root "$tmp/openwrt" > "$tmp/build-again.log"
grep -qx -- '-j2 tools/install V=s' <(sed -n '1p' "$tmp/make.log")
grep -qx -- '-j2 toolchain/install V=s' <(sed -n '2p' "$tmp/make.log")
grep -qx -- '-j2 package/openeigrp/compile V=s' <(sed -n '3p' "$tmp/make.log")

# A tools-stage failure must stop before the toolchain and package.
: > "$tmp/make.log"
export MOCK_FAIL_STAGE=tools/install
if "$driver" --build --openwrt-root "$tmp/openwrt" > "$tmp/tools-fail.log" 2>&1; then
	echo 'error: --build ignored tools/install failure' >&2
	exit 1
fi
grep -q 'tools/install failed' "$tmp/tools-fail.log"
[[ "$(wc -l < "$tmp/make.log")" == 1 ]]
unset MOCK_FAIL_STAGE

# An OpenWrt tools target claiming success without installing required host
# executables must also stop early with an actionable error.
rm -rf "$tmp/openwrt/staging_dir/host/bin"
export MOCK_MISSING_HOST_TOOLS=1
: > "$tmp/make.log"
if "$driver" --build --openwrt-root "$tmp/openwrt" > "$tmp/tools-missing.log" 2>&1; then
	echo 'error: --build ignored missing host tool executable' >&2
	exit 1
fi
grep -q 'staging_dir/host/bin/openssl is missing' "$tmp/tools-missing.log"
[[ "$(wc -l < "$tmp/make.log")" == 1 ]]
unset MOCK_MISSING_HOST_TOOLS

# A toolchain failure must stop before trying package compilation.
: > "$tmp/make.log"
export MOCK_FAIL_STAGE=toolchain/install
if "$driver" --build --openwrt-root "$tmp/openwrt" > "$tmp/toolchain-fail.log" 2>&1; then
	echo 'error: --build ignored toolchain/install failure' >&2
	exit 1
fi
grep -q 'toolchain/install failed' "$tmp/toolchain-fail.log"
[[ "$(wc -l < "$tmp/make.log")" == 2 ]]
unset MOCK_FAIL_STAGE

# Package failure must remain failure, never a reported successful base build.
: > "$tmp/make.log"
export MOCK_FAIL_STAGE=package/openeigrp/compile
if "$driver" --build --openwrt-root "$tmp/openwrt" > "$tmp/package-fail.log" 2>&1; then
	echo 'error: --build ignored package compilation failure' >&2
	exit 1
fi
grep -q 'package/openeigrp/compile failed' "$tmp/package-fail.log"
! grep -q 'base package build complete' "$tmp/package-fail.log"
[[ "$(wc -l < "$tmp/make.log")" == 3 ]]
unset MOCK_FAIL_STAGE

# Do not overwrite an existing feed with the same name from elsewhere.
sed -i "s|src-link openeigrp .*|src-link openeigrp /tmp/foreign-feed|" "$tmp/openwrt/feeds.conf"
if "$driver" --install --openwrt-root "$tmp/openwrt" > /dev/null 2>&1; then
	echo 'error: conflicting OpenEIGRP feed entry was overwritten' >&2
	exit 1
fi

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
