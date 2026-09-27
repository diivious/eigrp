#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
#
# Live IPv6 EIGRP Hello capture gate.  Run on an FRR UUT with a dedicated
# IPv6-capable test interface.  This does not form an adjacency or exchange
# route TLVs; it verifies the Task-6 Hello wire image emitted by eigrpd.

set -euo pipefail

interface="${EIGRP_IPV6_HELLO_IF:-}"
asn="${EIGRP_IPV6_HELLO_AS:-4453}"
router_id="${EIGRP_IPV6_HELLO_ROUTER_ID:-192.0.2.6}"
process="${EIGRP_IPV6_HELLO_PROCESS:-TASK6HELLO}"
output="${EIGRP_IPV6_HELLO_CAPTURE:-/tmp/eigrp-ipv6-hello.txt}"

[[ -n "$interface" ]] || { echo "set EIGRP_IPV6_HELLO_IF to a dedicated UUT interface" >&2; exit 2; }
command -v tcpdump >/dev/null 2>&1 || { echo "tcpdump is required" >&2; exit 2; }
command -v vtysh >/dev/null 2>&1 || { echo "vtysh is required" >&2; exit 2; }

link_local="$(ip -6 -o addr show dev "$interface" scope link | awk 'NR==1 {sub(/\/.*/, "", $4); print $4}')"
[[ "$link_local" == fe80:* ]] || { echo "$interface has no IPv6 link-local address" >&2; exit 1; }

cleanup() {
	sudo vtysh -d eigrpd \
		-c 'configure terminal' \
		-c "no router eigrp $process" >/dev/null 2>&1 || true
}
trap cleanup EXIT

sudo vtysh -d eigrpd \
	-c 'configure terminal' \
	-c "router eigrp $process" \
	-c "address-family ipv6 unicast autonomous-system $asn" \
	-c "eigrp router-id $router_id" \
	-c 'no shutdown' \
	-c "af-interface $interface" \
	-c 'no passive-interface' \
	-c 'no shutdown' >/dev/null

rm -f "$output"
# Capture the first protocol-88 IPv6 packet sourced by this interface.
sudo timeout 12 tcpdump -i "$interface" -c 1 -nn -vv -XX 'ip6 and ip6[6] == 88 and dst host ff02::a' >"$output" 2>&1

grep -qi "$link_local" "$output"
grep -qi 'ff02::a' "$output"
echo "IPv6 EIGRP Hello capture passed: $output"
