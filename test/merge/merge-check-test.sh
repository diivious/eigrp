#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# Host-neutral merge-driver contract tests. No buildroot, sudo, or root needed.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/repo/tools" "$tmp/repo/test/merge" "$tmp/repo/frr/test/build" \
    "$tmp/frr" "$tmp/openwrt" "$tmp/bin"
cp "$repo/tools/merge-check.sh" "$tmp/repo/tools/merge-check.sh"
printf '#!/bin/sh\nexit 0\n' > "$tmp/repo/test/merge/merge-check-test.sh"
printf '#!/bin/sh\nexit 0\n' > "$tmp/frr/bootstrap.sh"
printf 'all:\n\t@true\n' > "$tmp/openwrt/rules.mk"
printf 'CONFIG_PACKAGE_openeigrp=y\n' > "$tmp/openwrt/.config"
cat > "$tmp/bin/make" <<'SH'
#!/bin/sh
printf 'make %s\n' "$*" >> "$MERGE_TEST_TRACE"
case "$*" in
  *frr/test/build*) [ "${MERGE_TEST_BREAK:-}" != frr-adapter ] ;;
  *uut*) [ "${MERGE_TEST_BREAK:-}" != portable-uut ] ;;
  *) exit 0 ;;
esac
SH
cat > "$tmp/repo/tools/frr.sh" <<'SH'
#!/bin/sh
printf 'frr %s\n' "$*" >> "$MERGE_TEST_TRACE"
case " $* " in
  *' --all '*) [ "${MERGE_TEST_BREAK:-}" != frr-build ] ;;
  *) exit 0 ;;
esac
SH
cat > "$tmp/repo/tools/openwrt.sh" <<'SH'
#!/bin/sh
printf 'openwrt %s\n' "$*" >> "$MERGE_TEST_TRACE"
case " $* " in
  *' --image-build '*) [ "${MERGE_TEST_BREAK:-}" != image-build ] ;;
  *) exit 0 ;;
esac
SH
cat > "$tmp/bin/sudo" <<'SH'
#!/bin/sh
printf 'sudo %s\n' "$*" >> "$MERGE_TEST_TRACE"
exit 0
SH
cat > "$tmp/bin/id" <<'SH'
#!/bin/sh
if [ "${1:-}" = -u ]; then echo 1000; else /usr/bin/id "$@"; fi
SH
chmod +x "$tmp/bin/make" "$tmp/bin/sudo" "$tmp/bin/id" "$tmp/repo/tools/"*.sh
export PATH="$tmp/bin:$PATH"
export MERGE_TEST_TRACE="$tmp/trace"
runner=(bash "$tmp/repo/tools/merge-check.sh" --frr-root "$tmp/frr" --openwrt-root "$tmp/openwrt" --log-dir "$tmp/out")

: > "$MERGE_TEST_TRACE"
"${runner[@]}" --host > "$tmp/host.log" 2>&1 || { cat "$tmp/host.log"; exit 1; }
grep -q 'HOST REGRESSION: PASS' "$tmp/host.log"
! grep -q '^frr .*--all' "$MERGE_TEST_TRACE"
! grep -q '^openwrt .*--image-build' "$MERGE_TEST_TRACE"

a=0
MERGE_TEST_BREAK=portable-uut "${runner[@]}" --host > "$tmp/fail.log" 2>&1 || a=$?
[[ $a == 1 ]] || { echo 'FAIL: portable UUT failure was not propagated' >&2; exit 1; }
grep -q 'FAIL.*portable protocol UUT' "$tmp/fail.log"
grep -q 'PASS.*OpenWrt host UUT' "$tmp/fail.log"

: > "$MERGE_TEST_TRACE"
"${runner[@]}" --full --privileged > "$tmp/full.log" 2>&1 || { cat "$tmp/full.log"; exit 1; }
grep -q 'MERGE GATE: PASS' "$tmp/full.log"
grep -q '^frr .*--all' "$MERGE_TEST_TRACE"
grep -q '^frr .*--uut' "$MERGE_TEST_TRACE"
grep -q '^openwrt .*--image-build' "$MERGE_TEST_TRACE"
grep -q '^openwrt .*--qemu' "$MERGE_TEST_TRACE"
grep -q '^openwrt .*--kernel' "$MERGE_TEST_TRACE"

a=0
"${runner[@]}" --full > "$tmp/no-priv.log" 2>&1 || a=$?
[[ $a == 1 ]] || { echo 'FAIL: full gate silently skipped root tests' >&2; exit 1; }
grep -q 'BLOCKED.*requires --privileged' "$tmp/no-priv.log"

: > "$MERGE_TEST_TRACE"
a=0
MERGE_TEST_BREAK=image-build "${runner[@]}" --full --privileged > "$tmp/bad-image.log" 2>&1 || a=$?
[[ $a == 1 ]] || { echo 'FAIL: image build failure was not propagated' >&2; exit 1; }
grep -q 'refusing stale guest image' "$tmp/bad-image.log"
! grep -q '^openwrt .*--qemu' "$MERGE_TEST_TRACE"

: > "$MERGE_TEST_TRACE"
a=0
MERGE_TEST_BREAK=frr-build "${runner[@]}" --full --privileged > "$tmp/bad-frr.log" 2>&1 || a=$?
[[ $a == 1 ]] || { echo 'FAIL: FRR build failure was not propagated' >&2; exit 1; }
grep -q 'refusing stale executable' "$tmp/bad-frr.log"
! grep -q '^frr .*--uut' "$MERGE_TEST_TRACE"

mkdir -p "$tmp/repo/bsd"
a=0
"${runner[@]}" --host > "$tmp/bsd.log" 2>&1 || a=$?
[[ $a == 1 ]] || { echo 'FAIL: missing BIRD driver was treated as PASS' >&2; exit 1; }
grep -q 'BLOCKED.*bsd/ exists but tools/bsd.sh is missing' "$tmp/bsd.log"
rmdir "$tmp/repo/bsd"

rm -f "$tmp/openwrt/.config"
a=0
"${runner[@]}" --full --privileged > "$tmp/missing.log" 2>&1 || a=$?
[[ $a == 1 ]] || { echo 'FAIL: missing configured OpenWrt buildroot passed' >&2; exit 1; }
grep -q 'BLOCKED.*configured buildroot missing' "$tmp/missing.log"

printf 'merge-check-test: PASS (host/full, aggregation, stale outputs, prerequisites)\n'
