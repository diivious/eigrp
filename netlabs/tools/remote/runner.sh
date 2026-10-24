#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# Server-side session owner for interactive labs and automated UUT runs.

set -Eeuo pipefail

# SSH commands do not reliably load the netlab user's login profile.
export PATH="/home/netlab/netlab-venv/bin:/home/netlab/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:$PATH"

BASE="${REMOTE_LAB_DATA:-/home/netlab-data}"
RUNS="$BASE/runs"
RESULTS="$BASE/results"
STATE_DIR="${REMOTE_LAB_STATE:-/home/netlab/.remote-lab}"
SESSION="$STATE_DIR/session"
LOCK="$STATE_DIR/lock"

mkdir -p "$RUNS" "$RESULTS" "$STATE_DIR"
chmod 700 "$STATE_DIR"

# Hold the same lock through deployment, execution, reporting, and teardown.
exec 9>"$LOCK"
flock -n 9 || {
    echo 'BUSY: another lab/UUT operation is in progress' >&2
    exit 3
}

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

state_get() {
    [[ -f "$SESSION" ]] && sed -n "s/^$1=//p" "$SESSION" | head -n 1 || :
}

state_write() {
    local file
    file="$(mktemp "$STATE_DIR/session.XXXXXX")"
    printf 'mode=%s\nname=%s\nworkspace=%s\nphase=%s\n' \
        "$1" "$2" "$3" "$4" >"$file"
    chmod 600 "$file"
    mv -f "$file" "$SESSION"
}

state_clear() {
    rm -f "$SESSION"
}

state_validate() {
    [[ -f "$SESSION" ]] || fail 'No active lab or UUT session'

    local work
    work="$(state_get workspace)"
    [[ "$work" == "$RUNS"/* && "$work" != *'..'* ]] || \
        fail "Refusing invalid session workspace: $work"
}

assert_free() {
    if [[ -f "$SESSION" ]]; then
        echo "BUSY: $(state_get mode) '$(state_get name)' ($(state_get phase)); use --status/--stop first" >&2
        exit 3
    fi

    # A missing state file must not hide orphaned Containerlab resources.
    if docker ps -a --format '{{.Names}}' | grep -Eq '^clab-(eigrp-neighbor|oeig-lab)-'; then
        fail 'Orphaned OpenEIGRP containerlab nodes detected; investigate before starting a new session'
    fi
}

validate_stage() {
    [[ "$1" =~ ^${RUNS}/upload\.[A-Za-z0-9]+$ && -d "$1" ]] || \
        fail 'Invalid or missing staged workspace'
}

clab_nodes_exist() {
    docker ps -a --format '{{.Names}}' | grep -Eq '^clab-eigrp-neighbor-'
}

netlab_destroy() {
    local topology="$1"
    if ! command -v netlab >/dev/null 2>&1; then
        echo 'ERROR: netlab not installed' >&2
        return 1
    fi

    if (cd "${topology%/*}" && netlab down); then
        return 0
    fi

    # netlab up may have failed before it created any containers.
    ! clab_nodes_exist
}

run_lab() {
    local stage="$1" id="$2" work
    assert_free
    validate_stage "$stage"
    [[ "$id" =~ ^[0-9]{3}$ ]] || fail 'Invalid lab number'

    work="$stage/netlabs/labs/$id"
    [[ -f "$work/topology.yml" ]] || fail 'Missing lab topology.yml'
    state_write lab "$id" "$work" deploying

    if (cd "$work" && netlab up); then
        state_write lab "$id" "$work" running
        echo "ACTIVE LAB $id (workspace: $work)"
    else
        echo 'Lab deployment failed; attempting teardown' >&2
        if (cd "$work" && netlab down); then
            state_clear
            rm -rf -- "$stage"
        else
            echo 'Cleanup failed; session retained for recovery' >&2
        fi
        return 1
    fi
}

run_uut() {
    local stage="$1" name="$2" work report rc clean_rc
    local -a steps
    assert_free
    validate_stage "$stage"
    [[ "$name" =~ ^[0-9]+R-[a-z][a-z0-9-]*$ ]] || fail 'Invalid scenario name'

    work="$stage/netlabs/uut/$name"
    [[ -f "$work/scenario.yaml" && -f "$work/topology.yml" &&
       -f "$stage/netlabs/uut/$name.py" ]] || fail 'Missing UUT scenario or topology'
    command -v docker >/dev/null || fail 'Docker unavailable'
    command -v netlab >/dev/null || fail 'netlab unavailable'

    report="$(mktemp -d "$RESULTS/uut-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXXXX")"
    state_write uut "$name" "$stage" running
    mkdir -p "$stage/artifacts"

    # Stream Python output while keeping an identical transcript in runner.log.
    # PIPESTATUS[0] is the test result; tee must never mask a UUT failure.
    set +e
    PYTHONUNBUFFERED=1 timeout --signal=TERM --kill-after=20s 1500s \
        "${NETLAB_PYTHON:-/home/netlab/netlab-venv/bin/python}" \
        "$stage/netlabs/uut/$name.py" \
        --scenario "$work/scenario.yaml" \
        --topology "$work/topology.yml" \
        --artifacts "$stage/artifacts" 2>&1 | tee "$stage/artifacts/runner.log"
    steps=("${PIPESTATUS[@]}")
    set -e

    rc="${steps[0]}"
    if (( steps[1] != 0 )); then
        echo 'ERROR: Unable to save UUT output to runner.log' >&2
        rc=1
    fi

    echo 'Cleaning up remote topology...'
    clean_rc=0
    if ! netlab_destroy "$work/topology.yml" >>"$stage/artifacts/cleanup.log" 2>&1; then
        clean_rc=1
        echo 'FAIL: netlab cleanup failed; session retained' >&2
    fi

    cp -a "$stage/artifacts/." "$report/"
    printf 'scenario=%s\nstatus=%s\ncleanup=%s\n' \
        "$name" "$rc" "$clean_rc" >"$report/run-status.txt"

    if [[ "$clean_rc" -eq 0 ]]; then
        state_clear
        rm -rf -- "$stage"
    else
        rc=1
    fi

    echo "REPORT_ID=${report##*/}"
    if [[ "$rc" -eq 0 ]]; then
        echo "PASS: $name"
    else
        echo "FAIL: $name"
    fi
    return "$rc"
}

show_status() {
    if [[ ! -f "$SESSION" ]]; then
        echo 'No active lab or UUT session'
        return 0
    fi

    state_validate
    local mode work
    mode="$(state_get mode)"
    work="$(state_get workspace)"
    echo "Active: $mode $(state_get name) ($(state_get phase))"
    echo "Workspace: $work"

    if [[ "$mode" == lab && -d "$work" ]]; then
        (cd "$work" && netlab status) || true
    else
        docker ps --filter name=clab-eigrp-neighbor
    fi
}

connect_lab() {
    state_validate
    [[ "$(state_get mode)" == lab && "$(state_get phase)" == running ]] || \
        fail 'Only running instructional labs allow connections'

    local work node choice nodes=()
    work="$(state_get workspace)"
    mapfile -t nodes < <(
        "${NETLAB_PYTHON:-/home/netlab/netlab-venv/bin/python}" - "$work/topology.yml" <<'PY'
import sys
import yaml

with open(sys.argv[1], encoding='utf8') as file:
    topology = yaml.safe_load(file)
for name in (topology.get('nodes') or {}):
    print(name)
PY
    )

    ((${#nodes[@]})) || fail 'No nodes in topology'
    echo 'Routers:'
    local i
    for i in "${!nodes[@]}"; do
        printf '%d) %s\n' "$((i + 1))" "${nodes[i]}"
    done

    read -r -p 'Select router number: ' choice
    [[ "$choice" =~ ^[0-9]+$ && "$choice" -ge 1 &&
       "$choice" -le "${#nodes[@]}" ]] || fail 'Invalid selection'
    node="${nodes[choice-1]}"

    cd "$work"
    flock -u 9  # A connected operator must not block a later --stop operation.
    exec netlab connect "$node"
}

stop_session() {
    state_validate
    local mode work
    mode="$(state_get mode)"
    work="$(state_get workspace)"

    if [[ "$mode" == lab ]]; then
        if (cd "$work" && netlab down); then
            state_clear
            echo 'Lab stopped'
        else
            fail 'Lab teardown failed; session retained'
        fi
    elif [[ "$mode" == uut ]]; then
        # A running UUT holds the lock; this can only recover an abandoned one.
        local topology="$work/netlabs/uut/$(state_get name)/topology.yml"
        if netlab_destroy "$topology"; then
            state_clear
            echo 'Stale UUT session cleared'
        else
            fail 'UUT recovery teardown failed; session retained'
        fi
    else
        fail "Unknown session mode: $mode"
    fi
}

[[ $# -ge 1 ]] || fail 'Missing runner action'
action="$1"
shift
case "$action" in
    lab-start)
        [[ $# == 2 ]] || exit 2
        run_lab "$1" "$2"
        ;;
    uut-start)
        [[ $# == 2 ]] || exit 2
        run_uut "$1" "$2"
        ;;
    status)
        [[ $# == 0 ]] || exit 2
        show_status
        ;;
    connect)
        [[ $# == 0 ]] || exit 2
        connect_lab
        ;;
    stop)
        [[ $# == 0 ]] || exit 2
        stop_session
        ;;
    *)
        fail "Unknown action: $action"
        ;;
esac
