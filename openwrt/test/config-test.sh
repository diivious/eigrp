#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# OW-08 host-neutral configuration semantic API, policy and keychain tests.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/owrt-config-test.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror \
  -I"$root/eigrp/code" -I"$root/openwrt/code" \
  "$root/openwrt/code/eigrp_owrt_config.c" \
  "$root/openwrt/code/eigrp_owrt_policy.c" \
  "$root/openwrt/test/config-harness.c" -o "$tmp/config-test"
"$tmp/config-test"
