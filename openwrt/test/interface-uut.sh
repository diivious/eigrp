#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# Linux read-only netlink test; no CAP_NET_ADMIN or active daemon required.
set -euo pipefail
[[ "$(uname -s)" == Linux ]] || { echo 'SKIP: OW-05 netlink UUT requires Linux'; exit 0; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/owrt-interface.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L \
    -Wall -Wextra -Werror -pthread \
    -I"$root/eigrp/code" -I"$root/openwrt/code" \
    "$root/openwrt/code/eigrp_owrt_interface_state.c" \
    "$root/openwrt/code/eigrp_owrt_interface.c" \
    "$root/openwrt/test/interface-harness.c" -o "$tmp/interface-test"
"$tmp/interface-test"
