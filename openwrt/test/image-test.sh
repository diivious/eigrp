#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# OW-10: image-build driver regression for actual OpenWrt scripts/config/
# directory layout, Kconfig options, idempotence and failure propagation.
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo 'OW-10 image-build driver test: SKIP (Linux-only profile)'
    exit 0
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/eigrp-ow10-image-test.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/openwrt/scripts/config" "$tmp/openwrt/package/feeds/openeigrp" "$tmp/bin"
# In the actual OpenWrt buildroot scripts/config is a directory, not a tool.
cat > "$tmp/openwrt/Makefile" <<'MAKE'
all:
	@true
MAKE
printf '#!/bin/sh\nexit 0\n' > "$tmp/openwrt/scripts/feeds"
cat > "$tmp/bin/make" <<'SCRIPT'
#!/bin/sh
set -eu
if [ "$1" = '--version' ]; then
    exec /usr/bin/make --version
fi
printf '%s\n' "$*" >> ../build-events.log
case "$*" in
    *defconfig*)
        if [ "${OW10_REJECT_KMOD:-}" = 1 ]; then
            sed -i '/^CONFIG_PACKAGE_kmod-dummy=y$/d' .config
        fi
        ;;
    *)
        [ "${OW10_FAIL_BUILD:-}" != 1 ] || exit 44
        if grep -qx 'CONFIG_TARGET_armsr_armv8=y' .config; then
            mkdir -p bin/targets/armsr/armv8
            echo TEST-IMAGE > bin/targets/armsr/armv8/openwrt-armsr-armv8-generic-ext4-combined-efi.img.gz
        else
            mkdir -p bin/targets/x86/64
            echo TEST-IMAGE > bin/targets/x86/64/openwrt-x86-64-generic-ext4-combined.img.gz
        fi
        ;;
esac
SCRIPT
chmod +x "$tmp/openwrt/scripts/feeds" "$tmp/bin/make"
for tool in bash git gcc g++ python3 perl rsync gawk flex bison unzip wget; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$tool"
        chmod +x "$tmp/bin/$tool"
    fi
done
(
    cd "$tmp/openwrt"
    git init -q
    git -c user.name=OW10 -c user.email=ow10@example.invalid commit -q --allow-empty -m baseline
    git remote add origin https://github.com/diivious/openwrt.git
)
ln -s "$root/openwrt/feed/openeigrp" "$tmp/openwrt/package/feeds/openeigrp/openeigrp"
cat > "$tmp/openwrt/.config" <<'CONFIG'
CONFIG_TARGET_x86_64=y
CONFIG_PACKAGE_openeigrp=y
CONFIG_UNRELATED_TEST="preserve"
# CONFIG_TARGET_ROOTFS_EXT4FS is not set
CONFIG_TARGET_IMAGES_GZIP=n
CONFIG_PACKAGE_kmod-dummy=m
# CONFIG_PACKAGE_ip-full is not set
CONFIG_PACKAGE_ip-full-extra=y
# CONFIG_GRUB_IMAGES is not set
CONFIG
cp "$tmp/openwrt/.config" "$tmp/initial-config"

PATH="$tmp/bin:$PATH" "$root/tools/openwrt.sh" --image-build \
    --openwrt-root "$tmp/openwrt" --jobs 2

grep -qx 'defconfig' "$tmp/build-events.log"
grep -qx -- '-j2 V=s' "$tmp/build-events.log"
for symbol in TARGET_ROOTFS_EXT4FS TARGET_IMAGES_GZIP PACKAGE_kmod-dummy PACKAGE_ip-full GRUB_IMAGES; do
    grep -qx "CONFIG_${symbol}=y" "$tmp/openwrt/.config"
    test "$(grep -xc "CONFIG_${symbol}=y" "$tmp/openwrt/.config")" -eq 1
done
grep -qx 'CONFIG_UNRELATED_TEST="preserve"' "$tmp/openwrt/.config"
grep -qx 'CONFIG_PACKAGE_ip-full-extra=y' "$tmp/openwrt/.config"
test -s "$tmp/openwrt/bin/targets/x86/64/openwrt-x86-64-generic-ext4-combined.img.gz"
backups=("$tmp"/openwrt/.config.before-openeigrp-image.*)
test "${#backups[@]}" -eq 1
cmp "$tmp/initial-config" "${backups[0]}"
cp "$tmp/openwrt/.config" "$tmp/first-config"

# Re-run: no extra config rewrite/backup and no duplicate settings.
PATH="$tmp/bin:$PATH" "$root/tools/openwrt.sh" --image-build \
    --openwrt-root "$tmp/openwrt" --jobs 2
cmp "$tmp/first-config" "$tmp/openwrt/.config"
backups=("$tmp"/openwrt/.config.before-openeigrp-image.*)
test "${#backups[@]}" -eq 1

# A refused dependency must stop before the firmware build.
rm -f "$tmp/openwrt/bin/targets/x86/64/openwrt-x86-64-generic-ext4-combined.img.gz"
if PATH="$tmp/bin:$PATH" OW10_REJECT_KMOD=1 \
    "$root/tools/openwrt.sh" --image-build --openwrt-root "$tmp/openwrt" \
    > "$tmp/reject.log" 2>&1; then
    echo 'FAIL: rejected Kconfig setting did not fail image-build' >&2
    exit 1
fi
grep -q 'rejected required image option CONFIG_PACKAGE_kmod-dummy=y' "$tmp/reject.log"
test ! -e "$tmp/openwrt/bin/targets/x86/64/openwrt-x86-64-generic-ext4-combined.img.gz"

# A failed OpenWrt make must not produce a positive result.
if PATH="$tmp/bin:$PATH" OW10_FAIL_BUILD=1 \
    "$root/tools/openwrt.sh" --image-build --openwrt-root "$tmp/openwrt" \
    > "$tmp/failure.log" 2>&1; then
    echo 'FAIL: image-build hid a failed make stage' >&2
    exit 1
fi
grep -q 'OpenWrt firmware image build failed' "$tmp/failure.log"

# ARM64 profile must not enable x86-only GRUB_IMAGES.
printf '%s\n' CONFIG_TARGET_armsr_armv8=y CONFIG_PACKAGE_openeigrp=y \
    'CONFIG_UNRELATED_TEST="preserve"' > "$tmp/openwrt/.config"
PATH="$tmp/bin:$PATH" "$root/tools/openwrt.sh" --image-build \
    --openwrt-root "$tmp/openwrt" --jobs 2
for symbol in TARGET_ROOTFS_EXT4FS TARGET_IMAGES_GZIP PACKAGE_kmod-dummy PACKAGE_ip-full; do
    grep -qx "CONFIG_${symbol}=y" "$tmp/openwrt/.config"
done
! grep -qx 'CONFIG_GRUB_IMAGES=y' "$tmp/openwrt/.config"
test -s "$tmp/openwrt/bin/targets/armsr/armv8/openwrt-armsr-armv8-generic-ext4-combined-efi.img.gz"

echo 'OW-10 image-build driver test: PASS'
