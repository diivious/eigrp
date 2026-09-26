#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Live FRR/vtysh acceptance test for EIGRP mode navigation and classic
# multi-instance configuration.

set -euo pipefail

script_name="$(basename "$0")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eigrp_root="$(cd "$script_dir/.." && pwd)"

uut_if="${EIGRP_UUT_INTERFACE:-enp0s8}"
name_a="EIGRP-NAV-A"
name_b="EIGRP-NAV-B"
log_dir="${EIGRP_UUT_LOG_DIR:-$eigrp_root/test/build/logs}"
log_file="$log_dir/eigrp-cli-navigation-uut.log"
assertions=0

usage() {
	cat <<USAGE
usage: $script_name [options]

Runs live EIGRP CLI hierarchy tests against eigrpd using sudo vtysh -d eigrpd.

options:
  --interface IFNAME    Interface used for af-interface navigation.
                        Default: EIGRP_UUT_INTERFACE or enp0s8.
  --help                Show this help.
USAGE
}

fail() {
	echo "FAIL: $*" >&2
	echo "transcript: $log_file" >&2
	exit 1
}

phase() {
	echo
	echo "== $* =="
	printf '\n== %s ==\n' "$*" >>"$log_file"
}

vty_capture() {
	local -a argv=(sudo vtysh -d eigrpd)
	local cmd output rc

	for cmd in "$@"; do
		argv+=(-c "$cmd")
	done

	{
		printf '+ '
		printf '%q ' "${argv[@]}"
		printf '\n'
	} >>"$log_file"

	set +e
	output="$("${argv[@]}" 2>&1)"
	rc=$?
	set -e
	printf '%s\n' "$output" >>"$log_file"
	printf '%s' "$output"
	return "$rc"
}

vty_apply() {
	local output rc
	set +e
	output="$(vty_capture "$@")"
	rc=$?
	set -e
	if printf '%s\n' "$output" | grep -Eqi \
		'(% ?Unknown command|% ?Ambiguous command|% ?Command incomplete|unknown command|ambiguous command|command incomplete|failed to connect|connection refused|can.t connect to.*eigrpd|% Configuration failed)'; then
		printf '%s\n' "$output" >&2
		fail "vtysh rejected command sequence"
	fi
	[[ "$rc" -eq 0 ]] || fail "vtysh command sequence returned $rc"
	if [[ -n "$output" ]]; then
		printf '%s\n' "$output"
	fi
}

vty_cleanup() {
	local -a argv=(sudo vtysh -d eigrpd)
	local cmd

	for cmd in "$@"; do
		argv+=(-c "$cmd")
	done
	"${argv[@]}" >/dev/null 2>&1 || true
}

show_run() {
	vty_capture "show running-config"
}

assert_has_line() {
	local config="$1" expected="$2"
	printf '%s\n' "$config" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
		| grep -Fqx -- "$expected" || fail "missing running-config line: $expected"
	assertions=$((assertions + 1))
}

extract_named_af_block() {
	local config="$1" name="$2" family="$3" asn="$4"
	local router_line="router eigrp $name"
	local af_line="address-family $family unicast autonomous-system $asn"

	printf '%s\n' "$config" | awk -v router="$router_line" -v af="$af_line" '
		function trim(s) {
			gsub(/^[[:space:]]+/, "", s)
			gsub(/[[:space:]]+$/, "", s)
			return s
		}
		{
			t = trim($0)
			if (t ~ /^router eigrp /)
				in_router = (t == router)
			if (!capture && in_router && t == af)
				capture = 1
			if (capture)
				print $0
			if (capture && t == "exit-address-family")
				exit
		}
	'
}

assert_named_af_line() {
	local config="$1" name="$2" family="$3" asn="$4" expected="$5"
	local block

	block="$(extract_named_af_block "$config" "$name" "$family" "$asn")"
	[[ -n "$block" ]] || fail "missing $name $family AS $asn"
	assert_has_line "$block" "$expected"
}

assert_named_mode_line() {
	local config="$1" name="$2" family="$3" asn="$4"
	local mode="$5" finish="$6" expected="$7"
	local block mode_block

	block="$(extract_named_af_block "$config" "$name" "$family" "$asn")"
	[[ -n "$block" ]] || fail "missing $name $family AS $asn"
	mode_block="$(printf '%s\n' "$block" | awk -v start="$mode" -v finish="$finish" '
		function trim(s) {
			gsub(/^[[:space:]]+/, "", s)
			gsub(/[[:space:]]+$/, "", s)
			return s
		}
		{
			t = trim($0)
			if (!capture && t == start)
				capture = 1
			if (capture)
				print $0
			if (capture && t == finish)
				exit
		}
	')"
	[[ -n "$mode_block" ]] || fail "missing mode '$mode' under $name $family AS $asn"
	assert_has_line "$mode_block" "$expected"
}

cleanup() {
	vty_cleanup "configure terminal" "no router eigrp 10" "end"
	vty_cleanup "configure terminal" "no router eigrp 4453" "end"
	vty_cleanup "configure terminal" "no router eigrp $name_a" "end"
	vty_cleanup "configure terminal" "no router eigrp $name_b" "end"
}

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		--interface)
			[[ "$#" -ge 2 ]] || fail "--interface requires a value"
			uut_if="$2"
			shift 2
			;;
		--help)
			usage
			exit 0
			;;
		*)
			fail "unknown option: $1"
			;;
	esac
done

mkdir -p "$log_dir"
: >"$log_file"
command -v sudo >/dev/null 2>&1 || fail "required command not found: sudo"
command -v vtysh >/dev/null 2>&1 || fail "required command not found: vtysh"
trap cleanup EXIT

phase "preflight"
set +e
output="$(vty_capture "show running-config")"
preflight_rc=$?
set -e
[[ "$preflight_rc" -eq 0 ]] || fail "cannot read eigrpd running-config"
echo "eigrpd vtysh connection: OK"
echo "af-interface test interface: $uut_if"
cleanup

phase "classic router command rewinds to top-level configuration"
vty_apply \
	"configure terminal" \
	"router eigrp 10" \
	"router eigrp 4453" \
	"eigrp router-id 10.44.53.45" \
	"end"
config="$(show_run)"
assert_has_line "$config" "router eigrp 10"
assert_has_line "$config" "router eigrp 4453"
assertions=$((assertions + 1))
if printf '%s\n' "$config" | awk '
	function trim(s) { gsub(/^[[:space:]]+/, "", s); gsub(/[[:space:]]+$/, "", s); return s }
	{ t=trim($0); if (t=="router eigrp 4453") in_as=1; else if (in_as && t ~ /^router eigrp /) in_as=0; if (in_as && t=="eigrp router-id 10.44.53.45") found=1 }
	END { exit found ? 0 : 1 }
'; then
	:
else
	fail "router-id entered after router eigrp 4453 was not stored under AS 4453"
fi

phase "named commands require an existing named parent"
set +e
output="$(vty_capture \
	"configure terminal" \
	"router eigrp 10" \
	"address-family ipv4 unicast autonomous-system 4453" \
	"end")"
set -e
printf '%s\n' "$output" | grep -Fq "Enter named EIGRP router mode first" \
	|| fail "address-family under classic EIGRP did not reject the missing named parent"
assertions=$((assertions + 1))

# Classic AS 4453 and named IPv4 AS 4453 cannot simultaneously own the
# same {AF, VRF, AS} protocol runtime.  The classic navigation checks above
# are complete, so remove their temporary state before exercising named mode.
cleanup

phase "named sibling mode navigation"
vty_apply \
	"configure terminal" \
	"router eigrp $name_a" \
	"address-family ipv4 unicast autonomous-system 4453" \
	"eigrp router-id 10.44.53.1" \
	"address-family ipv4 unicast autonomous-system 6473" \
	"eigrp router-id 10.64.73.1" \
	"af-interface default" \
	"bandwidth 100000" \
	"af-interface $uut_if" \
	"bandwidth 200000" \
	"topology base" \
	"variance 2" \
	"af-interface default" \
	"delay 100" \
	"address-family ipv6 unicast autonomous-system 4453" \
	"eigrp router-id 10.44.53.6" \
	"router eigrp $name_b" \
	"address-family ipv4 unicast autonomous-system 4453" \
	"eigrp router-id 10.44.53.2" \
	"end"

config="$(show_run)"
assert_named_af_line "$config" "$name_a" ipv4 4453 "eigrp router-id 10.44.53.1"
assert_named_af_line "$config" "$name_a" ipv4 6473 "eigrp router-id 10.64.73.1"
assert_named_mode_line "$config" "$name_a" ipv4 6473 \
	"af-interface default" "exit-af-interface" "bandwidth 100000"
assert_named_mode_line "$config" "$name_a" ipv4 6473 \
	"af-interface $uut_if" "exit-af-interface" "bandwidth 200000"
assert_named_mode_line "$config" "$name_a" ipv4 6473 \
	"topology base" "exit-af-topology" "variance 2"
assert_named_mode_line "$config" "$name_a" ipv4 6473 \
	"af-interface default" "exit-af-interface" "delay 100"
assert_named_af_line "$config" "$name_a" ipv6 4453 "eigrp router-id 10.44.53.6"
assert_named_af_line "$config" "$name_b" ipv4 4453 "eigrp router-id 10.44.53.2"

phase "PASS"
echo "EIGRP CLI navigation live UUT passed: $assertions assertions"
echo "transcript: $log_file"

cleanup
trap - EXIT
