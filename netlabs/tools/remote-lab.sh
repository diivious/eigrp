#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# Manage a persistent instructional lab on the netlab execution host.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/remote/common.sh"

usage() {
    cat <<'HELP'
Usage: netlabs/tools/remote-lab.sh --lab NNN | --status | --connect | --stop
  --lab NNN   Deploy a published lab and leave it running
  --status    Inspect the active server-side session
  --connect   Select a router in the active lab (interactive SSH)
  --stop      Destroy the active lab and release its session

A lab needs netlabs/labs/NNN/topology.yml; curriculum-only labs are not deployable.
HELP
}

[[ $# == 1 || $# == 2 ]] || {
    usage >&2
    exit 2
}

case "$1" in
    --lab)
        [[ $# == 2 && "$2" =~ ^[0-9]{3}$ ]] || {
            usage >&2
            exit 2
        }
        action=start
        id="$2"
        [[ -f "$REMOTE_REPO_ROOT/netlabs/labs/$id/topology.yml" ]] || {
            echo "Lab $id is listed in the curriculum but is not implemented (missing netlabs/labs/$id/topology.yml)" >&2
            exit 2
        }
        ;;
    --status)
        [[ $# == 1 ]] || exit 2
        action=status
        ;;
    --connect)
        [[ $# == 1 ]] || exit 2
        action=connect
        ;;
    --stop)
        [[ $# == 1 ]] || exit 2
        action=stop
        ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

remote_transport_prepare

if [[ "$action" == start ]]; then
    stage="$(remote_new_stage)"
    files=(netlabs/tools/remote/runner.sh "netlabs/labs/$id" netlabs/config)
    [[ ! -d "$REMOTE_REPO_ROOT/netlabs/common/$id" ]] || files+=("netlabs/common/$id")

    remote_stage "$stage" "${files[@]}"
    remote_install_runner "$stage"
    remote_run "$stage" lab-start "$id"
else
    # The server owns active-session state, even when another client started the lab.
    case "$action" in
        connect)
            remote_ssh --tty "bash /home/netlab/.remote-lab/runner.sh connect"
            ;;
        status)
            remote_ssh "if test -x /home/netlab/.remote-lab/runner.sh; then bash /home/netlab/.remote-lab/runner.sh status; else echo 'No active lab or UUT session'; fi"
            ;;
        stop)
            remote_ssh "bash /home/netlab/.remote-lab/runner.sh stop"
            ;;
    esac
fi
