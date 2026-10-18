#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# OW-07 Linux RIB UUT: read-only real netlink and synthetic changes by default.
# --kernel exercises actual FIB writes only in a disposable network namespace.
set -euo pipefail
if [[ "$(uname -s)" != Linux ]]; then
  echo 'SKIP: Linux netlink RIB UUT requires Linux'
  exit 0
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/owrt-rib.XXXXXXXX")"
namespace=""
cleanup() {
  if [[ -n "$namespace" ]]; then
    ip netns del "$namespace" >/dev/null 2>&1 || true
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT
kernel=0
if (( $# > 0 )); then
  [[ "$#" == 1 && "$1" == --kernel ]] || {
    echo 'Usage: rib-uut.sh [--kernel]' >&2
    exit 2
  }
  kernel=1
  [[ "$(id -u)" == 0 ]] && command -v ip >/dev/null || {
    echo '--kernel requires root and iproute2' >&2
    exit 2
  }
fi
"${CC:-cc}" -std=c11 -D_POSIX_C_SOURCE=200809L \
  -Wall -Wextra -Werror -pthread \
  -I"$root/eigrp/code" -I"$root/openwrt/code" \
  "$root/openwrt/code/eigrp_owrt_rib.c" \
  "$root/openwrt/code/eigrp_owrt_rib_netlink.c" \
  "$root/openwrt/test/rib-harness.c" -o "$tmp/rib-test"
if (( kernel )); then
  namespace="owrib$$"
  ip netns add "$namespace"
  ip -n "$namespace" link add rib0 type dummy
  ip -n "$namespace" link set rib0 up
  ip netns exec "$namespace" "$tmp/rib-test" --kernel
else
  "$tmp/rib-test"
fi
