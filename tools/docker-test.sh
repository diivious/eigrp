#!/usr/bin/env bash
# Copyright (C) 2026 Donnie V. Savage
# Docker container integration test: daemons, CLI, AS 4453, persistence, restart.
set -euo pipefail

[[ $# -eq 1 ]] || { echo 'usage: tools/docker-test.sh IMAGE:TAG' >&2; exit 2; }
image="$1"
command -v docker >/dev/null 2>&1 || { echo 'error: Docker CLI is required' >&2; exit 2; }
docker image inspect "$image" >/dev/null 2>&1 || {
    echo "error: image not found locally: $image (build it or pull it before --test)" >&2
    exit 2
}
id="${RANDOM:-0}-$$"
container="openeigrp-test-$id"
volume="openeigrp-test-vol-$id"

fail() { echo "FAIL: $*" >&2; exit 1; }
cleanup() {
    status=$?
    if [[ "$status" -ne 0 ]]; then
        echo 'FRR diagnostics:' >&2
        docker inspect -f 'State={{.State.Status}} ExitCode={{.State.ExitCode}} Error={{.State.Error}}' "$container" >&2 || true
        # Docker emits daemon errors on stderr; never discard docker logs stderr.
        docker logs "$container" >&2 || true
        if [[ "$(docker inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" == true ]]; then
            docker exec "$container" ps -ef >&2 || true
            docker exec "$container" cat /etc/frr/daemons >&2 || true
        fi
    fi
    docker rm -f "$container" >/dev/null 2>&1 || true
    docker volume rm -f "$volume" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker volume create "$volume" >/dev/null
docker run -d --name "$container" \
    --cap-add NET_ADMIN --cap-add NET_RAW --cap-add SYS_ADMIN \
    --mount "type=volume,src=$volume,dst=/etc/frr" \
    "$image" >/dev/null

ready() {
    [[ "$(docker inspect -f '{{.State.Running}}' "$container")" == true ]] || return 1
    # Missing vtysh.conf causes vtysh to log a misleading frr.conf error
    # (CMD_ERR_NO_FILE / status 11), even when the rest of FRR is healthy.
    docker exec "$container" test -r /etc/frr/vtysh.conf >/dev/null 2>&1 || return 1
    for daemon in watchfrr mgmtd zebra staticd eigrpd; do
        docker exec "$container" pidof "$daemon" >/dev/null 2>&1 || return 1
    done
    docker exec "$container" vtysh -c 'show version' >/dev/null 2>&1 || return 1
    docker exec "$container" vtysh -d eigrpd -c 'show eigrp tech-support' >/dev/null 2>&1 || return 1
}
wait_ready() {
    local i
    for ((i = 0; i < 60; i++)); do
        if [[ "$(docker inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" != true ]]; then
            fail 'FRR container exited before startup completed'
        fi
        if ready; then return 0; fi
        sleep 2
    done
    fail 'FRR/ eigrpd not ready after startup'
}

wait_ready
version="$(docker image inspect -f '{{index .Config.Labels "org.opencontainers.image.version"}}' "$image")"
[[ -n "$version" && "$version" != '<no value>' ]] || fail 'missing image version label'
tech="$(docker exec "$container" vtysh -d eigrpd -c 'show eigrp tech-support')"
grep -Fq "$version" <<< "$tech" || fail "tech-support does not report build version $version"

# Apply a minimal named-mode IPv4/4453 configuration. Parse errors and failed
# commands are fatal; having vtysh connect is not sufficient.
configured="$(docker exec "$container" vtysh -d eigrpd \
    -c 'configure terminal' \
    -c 'router eigrp savage' \
    -c 'address-family ipv4 unicast autonomous-system 4453' \
    -c 'eigrp router-id 10.44.53.1' \
    -c 'network 10.44.0.0 0.0.255.255' \
    -c 'end' 2>&1)" || fail 'named-mode configuration rejected'
if grep -Eqi '(unknown command|ambiguous command|command incomplete|failed to connect)' <<< "$configured"; then
    echo "$configured" >&2
    fail 'named-mode configuration was not accepted'
fi
running="$(docker exec "$container" vtysh -d eigrpd -c 'show running-config')"
grep -Fq 'router eigrp savage' <<< "$running" || fail 'named-mode config not retained'
grep -Fq 'address-family ipv4 unicast autonomous-system 4453' <<< "$running" || fail 'AS 4453 not retained'
grep -Fq 'eigrp router-id 10.44.53.1' <<< "$running" || fail 'router ID not retained'

# Force a saved configuration and prove restart reads it back from the volume.
saved="$(docker exec "$container" vtysh -c 'write memory' 2>&1)" || {
    echo "$saved" >&2
    fail 'write memory failed'
}
if grep -Eqi '(unknown command|failed|error)' <<< "$saved"; then
    echo "$saved" >&2
    fail 'FRR could not save configuration'
fi
docker exec "$container" grep -Fq 'router eigrp savage' /etc/frr/frr.conf || fail 'saved configuration missing on volume'

docker stop --time 15 "$container" >/dev/null
docker start "$container" >/dev/null
wait_ready
running="$(docker exec "$container" vtysh -d eigrpd -c 'show running-config')"
grep -Fq 'router eigrp savage' <<< "$running" || fail 'saved config lost on restart'
grep -Fq 'address-family ipv4 unicast autonomous-system 4453' <<< "$running" || fail 'AS 4453 lost on restart'
grep -Fq 'eigrp router-id 10.44.53.1' <<< "$running" || fail 'router ID lost on restart'

# Runtime remains operational beyond the initial startup notification.
sleep 5
ready || fail 'FRR daemon exited after successful startup'
echo "PASS: $image — FRR, OpenEIGRP, AS 4453, version $version, save and restart"
