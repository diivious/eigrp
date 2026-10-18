#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# OW-06 native Linux protocol-88 packet I/O UUT, no active daemon or CAP_NET_ADMIN.
set -euo pipefail
if [[ "$(uname -s)" != Linux ]]; then
  printf '%s\n' 'SKIP: OW-06 requires Linux raw IP sockets'
  exit 0
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/owrt-packet.XXXXXXXX")"
trap 'rm -rf "$tmp"' EXIT
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -pthread \
  -I"$root/eigrp/code" -I"$root/openwrt/code" \
  "$root/openwrt/code/eigrp_owrt_packet.c" \
  "$root/openwrt/test/packet-harness.c" -o "$tmp/packet-test"
"$tmp/packet-test"
