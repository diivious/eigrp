#!/usr/bin/env bash
# Copyright (C) 2026 Donnie V. Savage
# Non-destructive image verification: FRR integration, EIGRP adjacency and route exchange.
# Uses disposable containers, configuration volumes and three Docker bridges.
set -euo pipefail

[[ $# -eq 1 ]] || { echo 'usage: tools/docker-verify.sh IMAGE:TAG' >&2; exit 2; }
image="$1"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
command -v docker >/dev/null 2>&1 || { echo 'error: Docker CLI is required' >&2; exit 2; }
docker image inspect "$image" >/dev/null 2>&1 || {
    echo "error: local Docker image not found: $image" >&2
    exit 2
}
original_image_id="$(docker image inspect -f '{{.Id}}' "$image")"

# --verify can safely run alongside a regular --deploy. Do not reuse any of
# its containers, networks or persistent configuration volumes.
suffix="$$-${RANDOM:-0}"
network="openeigrp-verify-transit-$suffix"
left_network="openeigrp-verify-r1-lan-$suffix"
right_network="openeigrp-verify-r2-lan-$suffix"
r1="openeigrp-verify-r1-$suffix"
r2="openeigrp-verify-r2-$suffix"
v1="openeigrp-verify-vol-r1-$suffix"
v2="openeigrp-verify-vol-r2-$suffix"

fail() { echo "FAIL: $*" >&2; exit 1; }

# If an operation fails, collect CLI and Docker diagnostics BEFORE deletion.
# A failed verify is never allowed to leave a live router or Docker volume.
diagnostics() {
    local node
    for node in "$r1" "$r2"; do
        if docker container inspect "$node" >/dev/null 2>&1; then
            echo "--- $node: state ---" >&2
            docker inspect -f 'State={{.State.Status}} ExitCode={{.State.ExitCode}} Error={{.State.Error}}' "$node" >&2 || true
            echo "--- $node: processes ---" >&2
            docker exec "$node" ps -ef >&2 || true
            echo "--- $node: IPv4 interface addresses ---" >&2
            docker exec "$node" ip -o -4 address show >&2 || true
            echo "--- $node: EIGRP topology ---" >&2
            docker exec "$node" vtysh -d eigrpd -c 'show eigrp address-family ipv4 topology 4453' >&2 || true
            echo "--- $node: EIGRP interfaces ---" >&2
            docker exec "$node" vtysh -d eigrpd -c 'show eigrp address-family ipv4 4453 interfaces' >&2 || true
            echo "--- $node: EIGRP traffic ---" >&2
            docker exec "$node" vtysh -d eigrpd -c 'show eigrp address-family ipv4 4453 traffic' >&2 || true
            echo "--- $node: neighbors ---" >&2
            docker exec "$node" vtysh -d eigrpd -c 'show eigrp address-family ipv4 4453 neighbors' >&2 || true
            # Portable receive-path rejections (bounds, interface, passive,
            # authentication, checksum, source validation) may be recorded
            # only in the EIGRP event ring and not in the daemon log.
            echo "--- $node: EIGRP receive/rejection events ---" >&2
            docker exec "$node" vtysh -d eigrpd -c 'show eigrp address-family ipv4 4453 events' >&2 || true
            echo "--- $node: eth0 multicast-capable interface ---" >&2
            docker exec "$node" ip -d link show dev eth0 >&2 || true
            echo "--- $node: eth0 multicast addresses / IGMP memberships ---" >&2
            docker exec "$node" ip maddr show dev eth0 >&2 || true
            docker exec "$node" cat /proc/net/igmp >&2 || true
            echo "--- $node: IPv4 multicast route lookup ---" >&2
            docker exec "$node" ip route get 224.0.0.10 >&2 || true
            # Distinguish a queued-but-unread eigrpd raw socket from packets
            # consumed and discarded by the FRR/EIGRP receive path.  The
            # packet diagnostic opens a SEPARATE raw socket, so successful
            # capture does not establish that eigrpd itself is receiving.
            echo "--- $node: eigrpd raw socket state BEFORE capture ---" >&2
            docker exec "$node" sh -c '
                pid=$(pidof eigrpd 2>/dev/null | awk "{print \$1}")
                if [ -n "$pid" ]; then
                    echo "eigrpd PID=$pid, socket FDs:"
                    ls -l "/proc/$pid/fd" 2>/dev/null | grep "socket:\[" || true
                fi
                echo "Linux raw sockets (local :0058 = IP protocol 88; rx_queue is hex):"
                cat /proc/net/raw
            ' >&2 || true
            echo "--- $node: eth0 protocol 88 Ethernet capture (12 seconds) ---" >&2
            docker exec -i "$node" python3 - eth0 12 \
                < "$root/tools/docker-packet-diagnostic.py" >&2 || true
            echo "--- $node: eigrpd raw socket state AFTER capture ---" >&2
            docker exec "$node" sh -c '
                pid=$(pidof eigrpd 2>/dev/null | awk "{print \$1}")
                if [ -n "$pid" ]; then
                    echo "eigrpd PID=$pid, socket FDs:"
                    ls -l "/proc/$pid/fd" 2>/dev/null | grep "socket:\[" || true
                fi
                echo "Linux raw sockets (local :0058 = IP protocol 88; rx_queue is hex):"
                cat /proc/net/raw
            ' >&2 || true
            echo "--- $node: EIGRP events AFTER protocol 88 capture ---" >&2
            docker exec "$node" vtysh -d eigrpd -c 'show eigrp address-family ipv4 4453 events' >&2 || true
            echo "--- $node: recent logs ---" >&2
            docker logs --tail 100 "$node" >&2 || true
        fi
    done
}
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if (( status != 0 )); then
        echo 'Docker verification failed; collecting diagnostics before cleanup.' >&2
        diagnostics
    fi
    # Names are unique to this invocation; never touch 'openeigrp',
    # 'openeigrp-vol' or an existing user's manually created lab.
    docker rm -f "$r1" "$r2" >/dev/null 2>&1 || true
    docker volume rm -f "$v1" "$v2" >/dev/null 2>&1 || true
    docker network rm "$left_network" "$right_network" "$network" >/dev/null 2>&1 || true
    # Do not report PASS if Docker refused to remove any test resources.
    local resource
    for resource in "$r1" "$r2"; do
        if docker container inspect "$resource" >/dev/null 2>&1; then
            echo "FAIL: temporary container remains: $resource" >&2
            status=1
        fi
    done
    for resource in "$v1" "$v2"; do
        if docker volume inspect "$resource" >/dev/null 2>&1; then
            echo "FAIL: temporary volume remains: $resource" >&2
            status=1
        fi
    done
    for resource in "$network" "$left_network" "$right_network"; do
        if docker network inspect "$resource" >/dev/null 2>&1; then
            echo "FAIL: temporary network remains: $resource" >&2
            status=1
        fi
    done
    if (( status == 0 )); then
        local after_image_id
        after_image_id="$(docker image inspect -f '{{.Id}}' "$image" 2>/dev/null)" || {
            echo 'FAIL: source image was removed or its tag was changed during verification' >&2
            exit 1
        }
        if [[ "$after_image_id" != "$original_image_id" ]]; then
            echo 'FAIL: source image reference changed during verification' >&2
            exit 1
        fi
        echo "PASS: $image — startup, AS 4453, /16 transit adjacency, 3 topology networks (two /24), cleanup; image unchanged"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

check_cli_output() {
    local output="$1"
    if grep -Eqi '(% Unknown command|% Ambiguous command|% Command incomplete|% Can.t open configuration file|Configuration file\[.*\] processing failure)' <<< "$output"; then
        echo "$output" >&2
        fail 'vtysh reported a CLI/configuration error'
    fi
}

# Section 1 of docker/README.md: full existing integration test. It uses
# its own throwaway volume and verifies save/restart and release reporting.
echo '== 1/3: automated Docker integration test =='
"$root/tools/docker-test.sh" "$image"

# Sections 2 and 3: three Docker bridges provide three actual connected
# networks, not IP aliases on a single interface. Only the /16 transit is
# shared. Each /24 LAN attaches to its respective router alone.
# All containers, networks and /etc/frr volumes are disposable.
echo '== 2/3: temporary deployment and FRR inspection =='
docker network create --driver bridge --subnet 10.56.0.0/16 "$network" >/dev/null ||
    fail 'cannot create 10.56.0.0/16 transit bridge; check overlapping subnets'
docker network create --driver bridge --subnet 10.53.41.0/24 "$left_network" >/dev/null ||
    fail 'cannot create R1 10.53.41.0/24 LAN bridge; check overlapping subnets'
docker network create --driver bridge --subnet 10.47.45.0/24 "$right_network" >/dev/null ||
    fail 'cannot create R2 10.47.45.0/24 LAN bridge; check overlapping subnets'
docker volume create "$v1" >/dev/null
docker volume create "$v2" >/dev/null
for node in "$r1" "$r2"; do
    if [[ "$node" == "$r1" ]]; then address=10.56.41.11; volume="$v1";
    else address=10.56.41.12; volume="$v2"; fi
    docker run -d --name "$node" --network "$network" --ip "$address" \
        --cap-add NET_ADMIN --cap-add NET_RAW --cap-add SYS_ADMIN \
        --mount "type=volume,src=$volume,dst=/etc/frr" \
        "$image" >/dev/null || fail "cannot start $node"
done
# Attach the two different /24 edge LANs before configuring EIGRP.
docker network connect --ip 10.53.41.11 "$left_network" "$r1" ||
    fail 'cannot attach R1 10.53.41.11/24 LAN interface'
docker network connect --ip 10.47.45.12 "$right_network" "$r2" ||
    fail 'cannot attach R2 10.47.45.12/24 LAN interface'
# Confirm the actual Linux interface addresses and both prefix lengths.
for spec in "$r1:10.56.41.11/16" "$r1:10.53.41.11/24" \
            "$r2:10.56.41.12/16" "$r2:10.47.45.12/24"; do
    node="${spec%%:*}"
    address="${spec#*:}"
    # Do not pipe Docker output into grep -q under pipefail: an early match
    # can close the pipe and make docker exec fail with SIGPIPE (false negative).
    interface_addresses="$(docker exec "$node" ip -o -4 addr show)" ||
        fail "$node: unable to inspect IPv4 interface addresses"
    grep -Fq "inet $address " <<< "$interface_addresses" ||
        fail "$node: expected IPv4 interface $address not installed"
done

ready() {
    local node="$1" daemon
    [[ "$(docker inspect -f '{{.State.Running}}' "$node" 2>/dev/null)" == true ]] || return 1
    docker exec "$node" test -r /etc/frr/vtysh.conf >/dev/null 2>&1 || return 1
    for daemon in watchfrr zebra mgmtd staticd eigrpd; do
        docker exec "$node" pidof "$daemon" >/dev/null 2>&1 || return 1
    done
    docker exec "$node" vtysh -c 'show version' >/dev/null 2>&1 || return 1
    docker exec "$node" vtysh -d eigrpd -c 'show eigrp tech-support' >/dev/null 2>&1 || return 1
}
for ((attempt=1; attempt<=60; attempt++)); do
    if ready "$r1" && ready "$r2"; then break; fi
    [[ "$(docker inspect -f '{{.State.Running}}' "$r1" 2>/dev/null)" == true &&
       "$(docker inspect -f '{{.State.Running}}' "$r2" 2>/dev/null)" == true ]] ||
        fail 'one or both FRR containers exited during startup'
    ((attempt < 60)) || fail 'FRR daemons or vtysh not ready within 120 seconds'
    sleep 2
done

# These correspond to the noninteractive versions of the README's Docker ps,
# pidof, vtysh and log inspection commands.
docker ps --filter "name=$r1" --filter "name=$r2"
for node in "$r1" "$r2"; do
    echo "--- $node: FRR processes ---"
    docker exec "$node" pidof watchfrr zebra mgmtd staticd eigrpd
    echo "--- $node: show version / daemons / running-config ---"
    for cmd in 'show version' 'show daemons' 'show running-config'; do
        output="$(docker exec "$node" vtysh -c "$cmd" 2>&1)" || fail "$node: $cmd failed"
        check_cli_output "$output"
        printf '%s\n' "$output"
    done
    echo "--- $node: EIGRP tech-support ---"
    output="$(docker exec "$node" vtysh -d eigrpd -c 'show eigrp tech-support' 2>&1)" ||
        fail "$node: show eigrp tech-support failed"
    check_cli_output "$output"
    printf '%s\n' "$output"
    echo "--- $node: recent startup logs ---"
    docker logs --tail 30 "$node"
done

# Section 3: configure transit and edge networks on both routers. Each router
# must advertise its own /24 into the shared /16 adjacency, and learn the
# opposite /24. Three distinct destination prefixes must appear on BOTH ends.
echo '== 3/3: EIGRP AS 4453 — adjacency and three-network route exchange =='
for node in "$r1" "$r2"; do
    if [[ "$node" == "$r1" ]]; then
        router_id=10.56.41.11
        lan_network='network 10.53.41.0 0.0.0.255'
        lan_prefix=10.53.41.0/24
    else
        router_id=10.56.41.12
        lan_network='network 10.47.45.0 0.0.0.255'
        lan_prefix=10.47.45.0/24
    fi
    output="$(docker exec "$node" vtysh -d eigrpd \
        -c 'configure terminal' \
        -c 'router eigrp savage' \
        -c 'address-family ipv4 unicast autonomous-system 4453' \
        -c "eigrp router-id $router_id" \
        -c 'network 10.56.0.0 0.0.255.255' \
        -c "$lan_network" \
        -c 'end' 2>&1)" || fail "$node: named-mode EIGRP configuration failed"
    check_cli_output "$output"
    running="$(docker exec "$node" vtysh -d eigrpd -c 'show running-config' 2>&1)" ||
        fail "$node: cannot read EIGRP running configuration"
    check_cli_output "$running"
    grep -Fq 'router eigrp savage' <<< "$running" || fail "$node: named instance not retained"
    grep -Fq 'address-family ipv4 unicast autonomous-system 4453' <<< "$running" || fail "$node: AS 4453 not retained"
    grep -Fq "eigrp router-id $router_id" <<< "$running" || fail "$node: router ID not retained"
    grep -Fq 'network 10.56.0.0/16' <<< "$running" ||
        fail "$node: transit /16 EIGRP network not retained"
    grep -Fq "network $lan_prefix" <<< "$running" ||
        fail "$node: edge /24 EIGRP network not retained"
done

neighbor_command='show eigrp address-family ipv4 4453 neighbors'
topology_command='show eigrp address-family ipv4 topology 4453'
neighbor_present() {
    local node="$1" peer="$2" output
    output="$(docker exec "$node" vtysh -d eigrpd -c "$neighbor_command" 2>&1)" || return 1
    check_cli_output "$output"
    # An established peer appears at the beginning of a neighbor data row.
    grep -Eq "^[[:space:]]*${peer//./\.}[[:space:]]+" <<< "$output"
}
topology_present() {
    local node="$1" output prefix count
    output="$(docker exec "$node" vtysh -d eigrpd -c "$topology_command" 2>&1)" || return 1
    check_cli_output "$output"
    for prefix in 10.56.0.0/16 10.53.41.0/24 10.47.45.0/24; do
        grep -Eq "^[[:space:]]*[PA][[:space:]]+${prefix//./\.}," <<< "$output" || return 1
    done
    # Three total destination networks, using both /16 and /24 masks.
    count="$(grep -Ec '^[[:space:]]*[PA][[:space:]]+[0-9]+(\.[0-9]+){3}/[0-9]+,' <<< "$output" || true)"
    [[ "$count" == 3 ]]
}
learned_via_peer() {
    local node="$1" prefix="$2" peer="$3" output
    output="$(docker exec "$node" vtysh -d eigrpd -c "$topology_command $prefix" 2>&1)" || return 1
    check_cli_output "$output"
    grep -Eq "^[[:space:]]*[PA][[:space:]]+${prefix//./\.}," <<< "$output" || return 1
    grep -Fq "via $peer (" <<< "$output"
}

# Adjacency first: both peers must discover one another via the /16 transit.
adjacent=0
for ((attempt=1; attempt<=60; attempt++)); do
    if neighbor_present "$r1" 10.56.41.12 && neighbor_present "$r2" 10.56.41.11; then
        adjacent=1
        break
    fi
    [[ "$(docker inspect -f '{{.State.Running}}' "$r1" 2>/dev/null)" == true &&
       "$(docker inspect -f '{{.State.Running}}' "$r2" 2>/dev/null)" == true ]] ||
        fail 'router exited while waiting for EIGRP adjacency'
    sleep 2
done
(( adjacent == 1 )) || fail 'EIGRP adjacency did not form on 10.56.0.0/16 in both directions within 120 seconds'

# Next require route propagation, not merely a Hello neighbor relationship.
converged=0
for ((attempt=1; attempt<=60; attempt++)); do
    if topology_present "$r1" && topology_present "$r2" &&
       learned_via_peer "$r1" 10.47.45.0/24 10.56.41.12 &&
       learned_via_peer "$r2" 10.53.41.0/24 10.56.41.11; then
        converged=1
        break
    fi
    [[ "$(docker inspect -f '{{.State.Running}}' "$r1" 2>/dev/null)" == true &&
       "$(docker inspect -f '{{.State.Running}}' "$r2" 2>/dev/null)" == true ]] ||
        fail 'router exited while waiting for three-network route convergence'
    sleep 2
done
(( converged == 1 )) ||
    fail 'EIGRP did not learn all three /16 and /24 prefixes with remote /24 next hops within 120 seconds'

# Confirm stable adjacency and retained remote routes after another interval.
sleep 5
neighbor_present "$r1" 10.56.41.12 && neighbor_present "$r2" 10.56.41.11 &&
topology_present "$r1" && topology_present "$r2" &&
learned_via_peer "$r1" 10.47.45.0/24 10.56.41.12 &&
learned_via_peer "$r2" 10.53.41.0/24 10.56.41.11 ||
    fail 'EIGRP adjacency or three-network topology was not stable across two checks'

for node in "$r1" "$r2"; do
    echo "--- $node: established EIGRP neighbors ---"
    docker exec "$node" vtysh -d eigrpd -c "$neighbor_command"
    echo "--- $node: three-network EIGRP topology (/16 plus two /24) ---"
    docker exec "$node" vtysh -d eigrpd -c "$topology_command"
    echo "--- $node: interfaces ---"
    docker exec "$node" vtysh -d eigrpd -c 'show eigrp address-family ipv4 4453 interfaces'
    echo "--- $node: packet counters ---"
    docker exec "$node" vtysh -d eigrpd -c 'show eigrp address-family ipv4 4453 traffic'
done
# EXIT trap removes both temporary containers, both config volumes and the
# three isolated bridges, then confirms the image ID is unchanged.
