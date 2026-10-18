#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/owrt-runtime.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
cc="${CC:-cc}"
"$cc" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
    -pthread -I"$root/eigrp/code" -I"$root/openwrt/code" \
    "$root/openwrt/code/eigrp_owrt_runtime.c" \
    "$root/openwrt/test/runtime-harness.c" -o "$tmp/runtime-test"
"$tmp/runtime-test"
