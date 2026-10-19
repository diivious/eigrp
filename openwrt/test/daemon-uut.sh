#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# OW-09: real portable core + host adapter link and daemon signal/IPC lifecycle.
# Uses a test-only UCI reader for empty configuration; live libuci is gated by
# the OpenWrt cross-build and on-device UUT, never claimed by this test.
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo 'SKIP: OW-09 daemon needs Linux signalfd/netlink'
    exit 0
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/openeigrp-ow09-uut.XXXXXXXX")"
pid=''
cleanup() {
    if [[ -n "$pid" ]]; then
        kill -TERM "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT
mkdir -p "$tmp/core" "$tmp/host"
cp "$root/eigrp/code/"*.{c,h} "$root/eigrp/code/Makefile" "$tmp/core/"
cp "$root/openwrt/code/"*.{c,h} "$root/openwrt/code/Makefile" "$tmp/host/"
cat > "$tmp/uci-mock.c" <<'C'
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "eigrp_owrt_config.h"
eigrp_result_t eigrp_owrt_uci_load(const char *directory,
    eigrp_owrt_config_t **config, eigrp_owrt_config_diagnostic_t *diag)
{
    char file[512];
    if (directory && strcmp(directory, "empty") != 0) {
        (void)snprintf(file, sizeof(file), "%s/invalid", directory);
        if (access(file, F_OK) == 0) {
            if (diag) {
                diag->result = EIGRP_RESULT_INVALID_ARGUMENT;
                (void)snprintf(diag->message, sizeof(diag->message), "test invalid UCI");
            }
            return EIGRP_RESULT_INVALID_ARGUMENT;
        }
    }
    *config = eigrp_owrt_config_create();
    return *config ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_INTERNAL_FAILURE;
}
C
make -s -C "$tmp/core" CC="${CC:-cc}" libeigrp-core.a
make -s -C "$tmp/host" CC="${CC:-cc}" OWRT_LINUX=1 OWRT_UCI=0 \
    EIGRP_CORE_INCLUDE="$tmp/core" libeigrp-owrt-runtime.a
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$tmp/core" -I"$tmp/host" \
    -c "$tmp/uci-mock.c" -o "$tmp/uci-mock.o"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -I"$tmp/core" -I"$tmp/host" \
    -c "$tmp/host/eigrp_owrt_daemon.c" -o "$tmp/daemon.o"
"${CC:-cc}" -pthread -o "$tmp/openeigrpd" "$tmp/daemon.o" "$tmp/uci-mock.o" \
    -Wl,--start-group "$tmp/host/libeigrp-owrt-runtime.a" "$tmp/core/libeigrp-core.a" -Wl,--end-group
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "$tmp/host/eigrp_owrt_ctl.c" -o "$tmp/openeigrpctl"
mkdir "$tmp/uci"
"$tmp/openeigrpd" --check --config-dir "$tmp/uci" > "$tmp/check.out"
grep -q 'configuration valid' "$tmp/check.out"
touch "$tmp/uci/invalid"
if "$tmp/openeigrpd" --check --config-dir "$tmp/uci" > /dev/null 2>&1; then
    echo 'FAIL: invalid UCI accepted'; exit 1
fi
rm "$tmp/uci/invalid"
"$tmp/openeigrpd" --foreground --config-dir "$tmp/uci" --socket "$tmp/control.sock" \
    > "$tmp/daemon.out" 2> "$tmp/daemon.err" &
pid=$!
for _ in $(seq 1 100); do
    [[ -S "$tmp/control.sock" ]] && break
    kill -0 "$pid" 2>/dev/null || { cat "$tmp/daemon.err"; exit 1; }
    sleep 0.05
done
[[ -S "$tmp/control.sock" ]] || { cat "$tmp/daemon.err"; exit 1; }
[[ "$(stat -c %a "$tmp/control.sock")" == 600 ]]
# A second instance must fail without unlinking the active socket.
if "$tmp/openeigrpd" --foreground --config-dir "$tmp/uci" \
    --socket "$tmp/control.sock" > "$tmp/second.out" 2> "$tmp/second.err"; then
    echo 'FAIL: duplicate daemon took over management socket'; exit 1
fi
[[ -S "$tmp/control.sock" ]]
"$tmp/openeigrpctl" --socket "$tmp/control.sock" status > "$tmp/status"
grep -q 'no configured instances' "$tmp/status"
"$tmp/openeigrpctl" --socket "$tmp/control.sock" neighbors > /dev/null
if "$tmp/openeigrpctl" --socket "$tmp/control.sock" invalid > /dev/null 2>&1; then
    echo 'FAIL: invalid operational command accepted'; exit 1
fi
touch "$tmp/uci/invalid"
kill -HUP "$pid"
for _ in $(seq 1 100); do
    grep -q 'UCI reload.*failed' "$tmp/daemon.err" && break
    sleep 0.05
done
grep -q 'UCI reload.*failed' "$tmp/daemon.err"
"$tmp/openeigrpctl" --socket "$tmp/control.sock" status > /dev/null
rm "$tmp/uci/invalid"
kill -HUP "$pid"
for _ in $(seq 1 100); do
    grep -q 'UCI reload successful' "$tmp/daemon.err" && break
    sleep 0.05
done
grep -q 'UCI reload successful' "$tmp/daemon.err"
kill -TERM "$pid"
wait "$pid"
pid=''
[[ ! -e "$tmp/control.sock" ]]
grep -q 'stopped' "$tmp/daemon.err"
echo 'OW-09 daemon lifecycle and operational console UUT: PASS'
