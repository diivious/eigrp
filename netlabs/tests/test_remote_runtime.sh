#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# Exercise runner lifecycle and streaming with isolated fake netlab/docker/SSH.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
runner="$root/netlabs/tools/remote/runner.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/bin" "$tmp/state/runs/upload.abc/netlabs/labs/101"
cat >"$tmp/bin/docker" <<'SH'
#!/bin/sh
[ "$1" != ps ] || exit 0
exit 0
SH
cat >"$tmp/bin/netlab" <<'SH'
#!/bin/sh
case "$1" in
    up|down|status|connect)
        printf '%s\n' "$1" >> "$LAB_MOCK_LOG"
        [ "$1" != status ] || echo 'Router R1 running'
        ;;
    *) exit 2 ;;
esac
SH
chmod +x "$tmp/bin/docker" "$tmp/bin/netlab"
printf 'nodes:\n  r1: {}\n' >"$tmp/state/runs/upload.abc/netlabs/labs/101/topology.yml"

export PATH="$tmp/bin:$PATH"
export REMOTE_LAB_DATA="$tmp/state"
export REMOTE_LAB_STATE="$tmp/state/session"
export LAB_MOCK_LOG="$tmp/actions.log"
export NETLAB_PYTHON="$(command -v python3)"

# Instructional labs must retain ownership and reject concurrent starts.
bash "$runner" status | grep -F 'No active lab'
bash "$runner" lab-start "$tmp/state/runs/upload.abc" 101 | grep -F 'ACTIVE LAB 101'
grep -Fqx up "$tmp/actions.log"
if bash "$runner" lab-start "$tmp/state/runs/upload.abc" 101 2>"$tmp/busy"; then
    echo 'FAIL: second lab was allowed' >&2
    exit 1
fi
grep -F 'BUSY:' "$tmp/busy"
bash "$runner" status | grep -F 'Active: lab 101 (running)'
printf '1\n' | bash "$runner" connect | grep -F 'Routers:'
grep -Fqx connect "$tmp/actions.log"
bash "$runner" stop | grep -F 'Lab stopped'
grep -Fqx down "$tmp/actions.log"
test ! -e "$tmp/state/session/session"

# A failed deployment must attempt teardown and release the session.
python3 - "$tmp/bin/netlab" <<'PY'
import sys

path = sys.argv[1]
with open(path, encoding='utf8') as file:
    source = file.read()
source = source.replace('up|down|status|connect)', 'up) exit 1 ;;\n    down|status|connect)')
with open(path, 'w', encoding='utf8') as file:
    file.write(source)
PY
if bash "$runner" lab-start "$tmp/state/runs/upload.abc" 101; then
    echo 'FAIL: broken netlab up was accepted' >&2
    exit 1
fi
test ! -e "$tmp/state/session/session"
echo 'PASS: runner lifecycle, mutual exclusion, status, teardown and failed deploy cleanup'

# The UUT mock supplies actual artifacts, output, exit codes, and an optional delay.
make_uut_workspace() {
    local workspace="$tmp/state/runs/upload.$1/netlabs/uut/2R-neighbor"
    mkdir -p "$workspace"
    printf 'schema: openeigrp-live-neighbor/v1\n' >"$workspace/scenario.yaml"
    printf '# test script\n' >"${workspace%/*}/2R-neighbor.py"
    printf 'name: eigrp-neighbor\n' >"$workspace/topology.yml"
}

make_uut_workspace uut
cat >"$tmp/bin/mock_python" <<'SH'
#!/bin/sh
case "$1" in
    */netlabs/uut/2R-neighbor.py) ;;
    *) echo "Incorrect scenario driver: $1" >&2; exit 72 ;;
esac
while [ $# -gt 0 ]; do
    if [ "$1" = --artifacts ]; then
        shift
        artifacts="$1"
    fi
    shift
done
printf 'Deploying and configuring test routers...\n'
if [ "${MOCK_PY_DELAY:-0}" != 0 ]; then sleep "$MOCK_PY_DELAY"; fi
printf 'Validating test neighbors...\n'
printf '{"ipv4":{"status":"PASS"},"ipv6":{"status":"PASS"}}\n' >"$artifacts/report.json"
echo 'PASS IPv4 and IPv6'
exit "${MOCK_PY_RC:-0}"
SH
chmod +x "$tmp/bin/mock_python"
export NETLAB_PYTHON="$tmp/bin/mock_python"

result="$(bash "$runner" uut-start "$tmp/state/runs/upload.uut" 2R-neighbor)"
printf '%s\n' "$result" | grep -F 'PASS: 2R-neighbor'
report="$(printf '%s\n' "$result" | sed -n 's/^REPORT_ID=//p')"
[[ -f "$tmp/state/results/$report/report.json" ]]
[[ ! -f "$tmp/state/session/session" ]]

# The first progress message must reach stdout before the UUT finishes.
make_uut_workspace stream
export MOCK_PY_DELAY=2
bash "$runner" uut-start "$tmp/state/runs/upload.stream" 2R-neighbor >"$tmp/stream.txt" 2>&1 &
pid=$!
stream_started=0
for _ in {1..50}; do
    if grep -Fq 'Deploying and configuring test routers...' "$tmp/stream.txt"; then
        stream_started=1
        break
    fi
    sleep 0.1
done
if [[ "$stream_started" -ne 1 ]] || ! kill -0 "$pid" 2>/dev/null; then
    echo 'FAIL: UUT did not stream progress while execution was running' >&2
    wait "$pid" || :
    exit 1
fi
wait "$pid"
unset MOCK_PY_DELAY
grep -F 'Validating test neighbors...' "$tmp/stream.txt"
grep -F 'PASS: 2R-neighbor' "$tmp/stream.txt"
[[ ! -f "$tmp/state/session/session" ]]
echo 'PASS: remote runner streams progress before UUT completion'

# Exercise the real client entry point with a fake transport library.
mkdir -p "$tmp/client/netlabs/tools/remote" "$tmp/client/netlabs/uut/2R-neighbor"
cp "$root/netlabs/tools/remote-uut.sh" "$tmp/client/netlabs/tools/remote-uut.sh"
printf 'name: 2R-neighbor\n' >"$tmp/client/netlabs/uut/2R-neighbor/scenario.yaml"
printf '# driver\n' >"$tmp/client/netlabs/uut/2R-neighbor.py"
cat >"$tmp/client/netlabs/tools/remote/common.sh" <<'SH'
#!/usr/bin/env bash
REMOTE_REPO_ROOT="$LAB_TEST_ROOT"
REMOTE_HOST=mock-StormTrooper
remote_transport_prepare() { :; }
remote_new_stage() { printf '/home/netlab-data/runs/upload.mock\n'; }
remote_stage() { :; }
remote_install_runner() { :; }
remote_run() {
    printf 'Deploying from remote...\n'
    sleep 2
    printf 'Validating from remote...\n'
    printf 'REPORT_ID=uut-mocktest\n'
    printf 'PASS: 2R-neighbor\n'
    if [ "${MOCK_CLIENT_RC:-0}" -ne 0 ]; then return "$MOCK_CLIENT_RC"; fi
}
remote_fetch_report() {
    mkdir -p "$2/$1"
    printf 'ok\n' >"$2/$1/run-status.txt"
}
SH
export LAB_TEST_ROOT="$tmp/client"
"$tmp/client/netlabs/tools/remote-uut.sh" --scenario 2R-neighbor >"$tmp/client-out.txt" 2>&1 &
pid=$!
client_started=0
for _ in {1..50}; do
    if grep -Fq 'Deploying from remote...' "$tmp/client-out.txt"; then
        client_started=1
        break
    fi
    sleep 0.1
done
if [[ "$client_started" -ne 1 ]] || ! kill -0 "$pid" 2>/dev/null; then
    echo 'FAIL: client buffered the remote stream until command completion' >&2
    wait "$pid" || :
    exit 1
fi
wait "$pid"
grep -F 'PASS: 2R-neighbor' "$tmp/client-out.txt"
grep -F 'Report: ' "$tmp/client-out.txt"
[[ -f "$tmp/client/netlabs/results/uut-mocktest/run-status.txt" ]]
echo 'PASS: remote client streams progress and retrieves reports'

# SSH failures must survive tee and report retrieval without becoming a PASS.
export MOCK_CLIENT_RC=42
if "$tmp/client/netlabs/tools/remote-uut.sh" --scenario 2R-neighbor >"$tmp/client-failed.txt" 2>&1; then
    echo 'FAIL: client replaced a remote UUT failure with success' >&2
    exit 1
else
    client_rc=$?
fi
[[ "$client_rc" -eq 42 ]] || {
    echo "FAIL: remote error became exit $client_rc" >&2
    exit 1
}
grep -F 'Report: ' "$tmp/client-failed.txt"
unset MOCK_CLIENT_RC
echo 'PASS: client preserves failure status after report transfer'

# Test failure must remain a failure, even if the final teardown succeeds.
make_uut_workspace fail
export MOCK_PY_RC=42
if bash "$runner" uut-start "$tmp/state/runs/upload.fail" 2R-neighbor >"$tmp/uut-failed.txt"; then
    echo 'FAIL: rejected UUT test exit was reported PASS' >&2
    exit 1
fi
grep -F 'FAIL: 2R-neighbor' "$tmp/uut-failed.txt"
[[ ! -f "$tmp/state/session/session" ]]
[[ $(grep -c '^down$' "$tmp/actions.log") -ge 3 ]]
echo 'PASS: UUT success/failure propagation, retained reports, and teardown'

# The entry points, transport and config must remain encapsulated in netlabs/.
[[ -r "$root/netlabs/remote-lab.cfg" ]]
[[ -f "$root/netlabs/tools/remote/common.sh" ]]
[[ -f "$root/netlabs/tools/remote/runner.sh" ]]
grep -Fq 'COPYFILE_DISABLE=1 tar --no-xattrs' "$root/netlabs/tools/remote/common.sh"
"$root/netlabs/tools/remote-lab.sh" --help | grep -F 'netlabs/tools/remote-lab.sh'
test "$("$root/netlabs/tools/remote-uut.sh" --list)" = '2R-neighbor'
if "$root/netlabs/tools/remote-uut.sh" --scenario neighbor-two-router >"$tmp/legacy.txt" 2>&1; then
    echo 'FAIL: legacy scenario identifier unexpectedly accepted' >&2
    exit 1
fi
echo 'PASS: netlabs scenario discovery and dispatch naming'

# A loopback appliance address is valid, but execution still uses SSH.
cat >"$tmp/localhost.cfg" <<'CFG'
REMOTE_HOST=127.0.0.1
REMOTE_USER=netlab
REMOTE_SSH_PORT=22
CFG
cat >"$tmp/bin/ssh" <<'SH'
#!/bin/sh
printf '%s\n' "$*" >>"$LAB_MOCK_SSH_LOG"
case "$*" in
    *'No active lab or UUT session'*) printf 'No active lab or UUT session\n' ;;
    *BatchMode=yes*) exit 0 ;;
esac
SH
chmod +x "$tmp/bin/ssh"
export LAB_MOCK_SSH_LOG="$tmp/ssh.log"
REMOTE_LAB_CONFIG="$tmp/localhost.cfg" "$root/netlabs/tools/remote-lab.sh" --status |
    grep -F 'No active lab or UUT session'
grep -Fq 'netlab@127.0.0.1' "$tmp/ssh.log"
echo 'PASS: localhost is accepted as an SSH execution host'
