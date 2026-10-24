#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# Shared SSH transport for macOS and Linux remote-lab clients.

set -euo pipefail

REMOTE_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
REMOTE_CFG="${REMOTE_LAB_CONFIG:-$REMOTE_REPO_ROOT/netlabs/remote-lab.cfg}"

[[ -r "$REMOTE_CFG" ]] || {
    echo "Missing config: $REMOTE_CFG" >&2
    exit 2
}
# The config is a trusted Bash file, not a parsed key/value document.
# shellcheck disable=SC1090
source "$REMOTE_CFG"

: "${REMOTE_HOST:=StormTrooper}" "${REMOTE_USER:=netlab}" \
    "${REMOTE_PASSWORD:=netlab}" "${REMOTE_SSH_PORT:=22}"

[[ "$REMOTE_HOST" =~ ^[A-Za-z0-9_.-]+$ &&
   "$REMOTE_USER" =~ ^[A-Za-z0-9_-]+$ &&
   "$REMOTE_SSH_PORT" =~ ^[0-9]+$ ]] || {
    echo 'Invalid remote host, username, or SSH port in remote-lab.cfg' >&2
    exit 2
}

REMOTE_TARGET="${REMOTE_USER}@${REMOTE_HOST}"
REMOTE_RUNS='/home/netlab-data/runs'
REMOTE_MODE=''

remote_transport_prepare() {
    local ssh_base=(
        ssh -p "$REMOTE_SSH_PORT"
        -o ConnectTimeout=8
        -o StrictHostKeyChecking=accept-new
        -o ServerAliveInterval=30
        -o ServerAliveCountMax=3
    )

    if "${ssh_base[@]}" -o BatchMode=yes "$REMOTE_TARGET" true >/dev/null 2>&1; then
        REMOTE_MODE=key
    elif command -v sshpass >/dev/null 2>&1; then
        REMOTE_MODE=password
    else
        echo "SSH key login failed for $REMOTE_TARGET; install sshpass or configure SSH keys" >&2
        return 2
    fi
}

remote_ssh() {
    local tty=''
    if [[ "${1:-}" == '--tty' ]]; then
        tty='-tt'
        shift
    fi

    local args=(
        -p "$REMOTE_SSH_PORT"
        -o ConnectTimeout=8
        -o ServerAliveInterval=30
        -o ServerAliveCountMax=3
        -o StrictHostKeyChecking=accept-new
    )
    [[ -z "$tty" ]] || args+=("$tty")

    if [[ "$REMOTE_MODE" == key ]]; then
        ssh -o BatchMode=yes "${args[@]}" "$REMOTE_TARGET" "$@"
    else
        SSHPASS="$REMOTE_PASSWORD" sshpass -e ssh "${args[@]}" \
            -o NumberOfPasswordPrompts=1 "$REMOTE_TARGET" "$@"
    fi
}

remote_new_stage() {
    remote_ssh "mkdir -p '$REMOTE_RUNS' && mktemp -d '$REMOTE_RUNS/upload.XXXXXXXX'"
}

remote_stage() {
    local stage="$1"
    shift

    [[ "$stage" =~ ^/home/netlab-data/runs/upload\.[A-Za-z0-9]+$ ]] || {
        echo 'Invalid remote staging path' >&2
        return 2
    }

    # Suppress macOS quarantine xattrs before GNU tar extracts the stream.
    COPYFILE_DISABLE=1 tar --no-xattrs -C "$REMOTE_REPO_ROOT" -cf - "$@" |
        remote_ssh "tar -C '$stage' -xf -"
}

remote_run() {
    local stage="$1" action="$2"
    shift 2

    # Shell-quote runner arguments before crossing the SSH command boundary.
    local command='bash' part
    printf -v part '%q' "$stage/netlabs/tools/remote/runner.sh"
    command+=" $part"
    printf -v part '%q' "$action"
    command+=" $part"
    for part in "$@"; do
        printf -v part '%q' "$part"
        command+=" $part"
    done
    remote_ssh "$command"
}

remote_control() {
    local action="$1"
    shift
    local command="bash /home/netlab/.remote-lab/runner.sh '$action'"
    remote_ssh "$command" "$@"
}

remote_install_runner() {
    local stage="$1"

    # The unprivileged netlab account owns the durable session state.
    remote_ssh "mkdir -m 700 -p /home/netlab/.remote-lab && \
        cp '$stage/netlabs/tools/remote/runner.sh' /home/netlab/.remote-lab/runner.sh.new && \
        chmod 700 /home/netlab/.remote-lab/runner.sh.new && \
        mv -f /home/netlab/.remote-lab/runner.sh.new /home/netlab/.remote-lab/runner.sh"
}

remote_fetch_report() {
    local name="$1" output="$2"
    [[ "$name" =~ ^uut-[A-Za-z0-9_.-]+$ ]] || return 2

    mkdir -p "$output"
    remote_ssh "tar -C /home/netlab-data/results -cf - '$name'" |
        tar -C "$output" -xf -
}
