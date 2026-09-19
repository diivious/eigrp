#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Live FRR/vtysh acceptance and writeback test for EIGRP named-mode
# configuration.  This test intentionally drives only named mode; classic
# numeric-AS CLI behavior is outside this test's scope.

set -euo pipefail

script_name="$(basename "$0")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eigrp_root="$(cd "$script_dir/.." && pwd)"

uut_name="savage"
uut_name_case="SAVAGE"
uut_if="${EIGRP_UUT_INTERFACE:-enp0s8}"
keep_config="${EIGRP_UUT_KEEP_CONFIG:-0}"
log_dir="${EIGRP_UUT_LOG_DIR:-$eigrp_root/test/build/logs}"
log_file="$log_dir/eigrp-named-uut.log"
assertions=0

usage() {
	cat <<USAGE
usage: $script_name [options]

Runs the live named-mode EIGRP configuration/writeback test against eigrpd
using only commands of this form:

  sudo vtysh -d eigrpd -c '...'

options:
  --interface IFNAME    Interface used for af-interface tests.
                        Default: EIGRP_UUT_INTERFACE or enp0s8.
  --keep-config         Leave the final savage/SAVAGE case-sensitivity test
                        configuration in running-config.
  --help                Show this help.

environment:
  EIGRP_UUT_INTERFACE   Default interface name for af-interface tests.
  EIGRP_UUT_KEEP_CONFIG Set to 1 to retain final test configuration.
  EIGRP_UUT_LOG_DIR     Directory for the detailed vtysh transcript.

The daemon must already be running the EIGRP build under test.  The script
never writes memory; it validates running-config and cleans up its test state
unless --keep-config is requested.
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

trim_config() {
	sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
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

	# A valid but not-yet-implemented EIGRP target may return a warning after
	# retaining configuration.  Storage checks below are authoritative for
	# that case.  Parser/connection failures are never acceptable.
	if printf '%s\n' "$output" | grep -Eqi \
		'(% ?Unknown command|% ?Ambiguous command|% ?Command incomplete|unknown command|ambiguous command|command incomplete|failed to connect|connection refused|can.t connect to.*eigrpd)'; then
		printf '%s\n' "$output" >&2
		fail "vtysh rejected command sequence"
	fi

	VTY_LAST_RC=$rc
	printf '%s' "$output"
}

vty_apply() {
	local output
	output="$(vty_capture "$@")"
	if [[ -n "$output" ]]; then
		printf '%s\n' "$output"
	fi
}

# Best-effort cleanup is deliberately non-asserting.  Cleanup must not use a
# command that is itself under test as a prerequisite for reaching the test.
# In particular, an unsupported `no address-family ...` must be discovered in
# the explicit removal phase, not while establishing the initial baseline.
vty_cleanup() {
	local -a argv=(sudo vtysh -d eigrpd)
	local cmd output rc

	for cmd in "$@"; do
		argv+=(-c "$cmd")
	done

	{
		printf '+ cleanup: '
		printf '%q ' "${argv[@]}"
		printf '\n'
	} >>"$log_file"

	set +e
	output="$("${argv[@]}" 2>&1)"
	rc=$?
	set -e

	printf '%s\n' "$output" >>"$log_file"
	printf 'cleanup rc=%d\n' "$rc" >>"$log_file"
	return 0
}

show_run() {
	vty_capture "show running-config"
}

extract_af_block() {
	local config="$1"
	local name="$2"
	local family="$3"
	local asn="$4"
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

extract_mode_block() {
	local block="$1"
	local start="$2"
	local finish="$3"

	printf '%s\n' "$block" | awk -v start="$start" -v finish="$finish" '
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
	'
}

block_has_line() {
	local block="$1"
	local expected="$2"

	printf '%s\n' "$block" | trim_config | grep -Fqx -- "$expected"
}

# Check only commands that are direct children of an address-family block.
# Nested af-interface/topology commands can legitimately use the same command
# text (for example, "shutdown"), so a flat grep of the whole AF block would
# produce false positives while testing removal of the AF-level command.
af_direct_has_line() {
	local block="$1"
	local expected="$2"

	printf '%s\n' "$block" | awk -v expected="$expected" '
		function trim(s) {
			gsub(/^[[:space:]]+/, "", s)
			gsub(/[[:space:]]+$/, "", s)
			return s
		}
		{
			t = trim($0)

			if (t ~ /^af-interface / || t == "topology base") {
				nested = 1
				next
			}

			if (nested) {
				if (t == "exit-af-interface" || t == "exit-af-topology")
					nested = 0
				next
			}

			if (t == expected)
				found = 1
		}
		END { exit found ? 0 : 1 }
	'
}

assert_af_exists_in() {
	local config="$1" name="$2" family="$3" asn="$4"
	local block

	block="$(extract_af_block "$config" "$name" "$family" "$asn")"
	[[ -n "$block" ]] || fail "missing '$name' $family address-family AS $asn in running-config"
	assertions=$((assertions + 1))
}

assert_af_absent_in() {
	local config="$1" name="$2" family="$3" asn="$4"
	local block

	block="$(extract_af_block "$config" "$name" "$family" "$asn")"
	[[ -z "$block" ]] || fail "'$name' $family address-family AS $asn still exists"
	assertions=$((assertions + 1))
}

assert_af_line_in() {
	local config="$1" name="$2" family="$3" asn="$4" expected="$5"
	local block

	block="$(extract_af_block "$config" "$name" "$family" "$asn")"
	[[ -n "$block" ]] || fail "missing '$name' $family address-family AS $asn while checking '$expected'"
	af_direct_has_line "$block" "$expected" || fail "'$expected' was not retained in '$name' $family AS $asn"
	assertions=$((assertions + 1))
}

assert_af_line_absent_in() {
	local config="$1" name="$2" family="$3" asn="$4" unexpected="$5"
	local block

	block="$(extract_af_block "$config" "$name" "$family" "$asn")"
	[[ -n "$block" ]] || fail "missing '$name' $family address-family AS $asn while checking removal of '$unexpected'"
	if af_direct_has_line "$block" "$unexpected"; then
		fail "'$unexpected' is still retained in '$name' $family AS $asn"
	fi
	assertions=$((assertions + 1))
}

assert_mode_line_in() {
	local config="$1" name="$2" family="$3" asn="$4"
	local mode="$5" finish="$6" expected="$7"
	local af_block mode_block

	af_block="$(extract_af_block "$config" "$name" "$family" "$asn")"
	[[ -n "$af_block" ]] || fail "missing '$name' $family AS $asn while checking mode '$mode'"
	mode_block="$(extract_mode_block "$af_block" "$mode" "$finish")"
	[[ -n "$mode_block" ]] || fail "mode '$mode' was not written under '$name' $family AS $asn"
	block_has_line "$mode_block" "$expected" || fail "'$expected' was not retained under '$mode' in '$name' $family AS $asn"
	assertions=$((assertions + 1))
}

assert_mode_line_absent_in() {
	local config="$1" name="$2" family="$3" asn="$4"
	local mode="$5" finish="$6" unexpected="$7"
	local af_block mode_block

	af_block="$(extract_af_block "$config" "$name" "$family" "$asn")"
	[[ -n "$af_block" ]] || fail "missing '$name' $family AS $asn while checking removal under '$mode'"
	mode_block="$(extract_mode_block "$af_block" "$mode" "$finish")"
	if [[ -n "$mode_block" ]] && block_has_line "$mode_block" "$unexpected"; then
		fail "'$unexpected' is still retained under '$mode' in '$name' $family AS $asn"
	fi
	assertions=$((assertions + 1))
}

apply_af() {
	local name="$1" family="$2" asn="$3" cmd="$4"
	vty_apply \
		"configure terminal" \
		"router eigrp $name" \
		"address-family $family unicast autonomous-system $asn" \
		"$cmd" \
		"end"
}

apply_af_interface() {
	local name="$1" family="$2" asn="$3" iface="$4" cmd="$5"
	vty_apply \
		"configure terminal" \
		"router eigrp $name" \
		"address-family $family unicast autonomous-system $asn" \
		"af-interface $iface" \
		"$cmd" \
		"end"
}

apply_topology() {
	local name="$1" family="$2" asn="$3" cmd="$4"
	vty_apply \
		"configure terminal" \
		"router eigrp $name" \
		"address-family $family unicast autonomous-system $asn" \
		"topology base" \
		"$cmd" \
		"end"
}

set_af_expect() {
	local name="$1" family="$2" asn="$3" cmd="$4" expected="${5:-$4}"
	local config

	echo "set: $name $family/$asn: $cmd"
	apply_af "$name" "$family" "$asn" "$cmd"
	config="$(show_run)"
	assert_af_line_in "$config" "$name" "$family" "$asn" "$expected"
}

set_if_expect() {
	local name="$1" family="$2" asn="$3" iface="$4" cmd="$5" expected="${6:-$5}"
	local config

	echo "set: $name $family/$asn af-interface $iface: $cmd"
	apply_af_interface "$name" "$family" "$asn" "$iface" "$cmd"
	config="$(show_run)"
	assert_mode_line_in "$config" "$name" "$family" "$asn" \
		"af-interface $iface" "exit-af-interface" "$expected"
}

set_topology_expect() {
	local name="$1" family="$2" asn="$3" cmd="$4" expected="${5:-$4}"
	local config

	echo "set: $name $family/$asn topology base: $cmd"
	apply_topology "$name" "$family" "$asn" "$cmd"
	config="$(show_run)"
	assert_mode_line_in "$config" "$name" "$family" "$asn" \
		"topology base" "exit-af-topology" "$expected"
}

remove_af_expect() {
	local name="$1" family="$2" asn="$3" cmd="$4" removed="$5"
	local config

	echo "remove: $name $family/$asn: $cmd"
	apply_af "$name" "$family" "$asn" "$cmd"
	config="$(show_run)"
	assert_af_line_absent_in "$config" "$name" "$family" "$asn" "$removed"
}

remove_if_expect() {
	local name="$1" family="$2" asn="$3" iface="$4" cmd="$5" removed="$6"
	local config

	echo "remove: $name $family/$asn af-interface $iface: $cmd"
	apply_af_interface "$name" "$family" "$asn" "$iface" "$cmd"
	config="$(show_run)"
	assert_mode_line_absent_in "$config" "$name" "$family" "$asn" \
		"af-interface $iface" "exit-af-interface" "$removed"
}

remove_topology_expect() {
	local name="$1" family="$2" asn="$3" cmd="$4" removed="$5"
	local config

	echo "remove: $name $family/$asn topology base: $cmd"
	apply_topology "$name" "$family" "$asn" "$cmd"
	config="$(show_run)"
	assert_mode_line_absent_in "$config" "$name" "$family" "$asn" \
		"topology base" "exit-af-topology" "$removed"
}

create_af_expect() {
	local name="$1" family="$2" asn="$3"
	local config

	echo "create: router eigrp $name / address-family $family AS $asn"
	vty_apply \
		"configure terminal" \
		"router eigrp $name" \
		"address-family $family unicast autonomous-system $asn" \
		"exit-address-family" \
		"end"
	config="$(show_run)"
	assert_af_exists_in "$config" "$name" "$family" "$asn"
}

remove_af_context_expect() {
	local name="$1" family="$2" asn="$3"
	local config

	echo "remove AF: router eigrp $name / $family AS $asn"
	vty_apply \
		"configure terminal" \
		"router eigrp $name" \
		"no address-family $family unicast autonomous-system $asn" \
		"end"
	config="$(show_run)"
	assert_af_absent_in "$config" "$name" "$family" "$asn"
}

cleanup_names() {
	local name config

	# Delete only the parent named process.  Entering `router eigrp <name>`
	# here would itself create/select the parent and would force cleanup to
	# exercise `no address-family` before the no-form test phase.
	for name in "$uut_name" "$uut_name_case"; do
		vty_cleanup \
			"configure terminal" \
			"no router eigrp $name" \
			"end"
	done

	# Do not silently run on top of retained state from an earlier UUT.  A
	# parser failure is harmless when no such process exists, but if the old
	# named process is still present the test no longer has a clean baseline.
	config="$(show_run)"
	for name in "$uut_name" "$uut_name_case"; do
		if printf '%s\n' "$config" | trim_config | \
			grep -Fqx -- "router eigrp $name"; then
			fail "stale 'router eigrp $name' remains after best-effort cleanup"
		fi
	done
}

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		--interface)
			[[ "$#" -ge 2 ]] || fail "--interface requires IFNAME"
			uut_if="$2"
			shift 2
			;;
		--keep-config)
			keep_config=1
			shift
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

phase "preflight"
set +e
preflight="$(vty_capture "show running-config")"
preflight_rc=$?
set -e
[[ "$preflight_rc" -eq 0 ]] || fail "cannot read eigrpd running-config with sudo vtysh -d eigrpd"
echo "eigrpd vtysh connection: OK"
echo "af-interface test interface: $uut_if"

phase "clean previous named-mode test state"
cleanup_names

# Stage 1 deliberately creates only one named address-family context.  Do not
# introduce IPv6 or a second AS until the complete IPv4/4453 command surface,
# mutation behavior, no forms, and writeback have passed.
phase "stage 1: create only router eigrp savage / IPv4 AS 4453"
create_af_expect "$uut_name" ipv4 4453

phase "stage 1: configure and verify every Step-1 named IPv4/4453 command"
set_af_expect "$uut_name" ipv4 4453 "eigrp router-id 10.44.53.1"
set_af_expect "$uut_name" ipv4 4453 "network 10.44.0.0 0.0.255.255" "network 10.44.0.0/16"
set_af_expect "$uut_name" ipv4 4453 "neighbor 10.0.0.1 $uut_if"
set_af_expect "$uut_name" ipv4 4453 "neighbor 10.0.0.1 description STEP1-PEER"
set_af_expect "$uut_name" ipv4 4453 "neighbor 10.0.0.1 maximum-prefix 100 80"
set_af_expect "$uut_name" ipv4 4453 "neighbor maximum-prefix 500 75 warning-only"
# Neighbor-change logging is enabled by default, so its meaningful retained
# configuration is the documented no form.
set_af_expect "$uut_name" ipv4 4453 "no eigrp log-neighbor-changes"
set_af_expect "$uut_name" ipv4 4453 "eigrp log-neighbor-warnings 30"
set_af_expect "$uut_name" ipv4 4453 "shutdown"

# Exercise both named af-interface forms.  The default template carries
# inherited metric/interface values; the concrete interface carries the full
# interface command surface.
set_if_expect "$uut_name" ipv4 4453 default "bandwidth 100000"
set_if_expect "$uut_name" ipv4 4453 default "delay 100"
set_if_expect "$uut_name" ipv4 4453 default "bandwidth-percent 60"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "bandwidth 200000"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "delay 200"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "bandwidth-percent 75"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "hello-interval 7"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "hold-time 21"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "passive-interface"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "authentication key-chain EIGRP-UUT-4453"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "authentication mode md5"
# Exercise the full named HMAC-SHA-256 form, including encryption selector
# and password retention/writeback.
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "authentication mode hmac-sha-256 0 Step1Secret"
# next-hop-self and split-horizon are enabled by default.  Their documented
# no forms are the non-default configurations that must survive writeback.
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "no next-hop-self"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "no split-horizon"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "summary-address 10.44.0.0 255.255.0.0 5 leak-map STEP1-LEAK"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "shutdown"

set_topology_expect "$uut_name" ipv4 4453 "auto-summary"
set_topology_expect "$uut_name" ipv4 4453 "default-information in STEP1-IN"
set_topology_expect "$uut_name" ipv4 4453 "default-information out STEP1-OUT"
set_topology_expect "$uut_name" ipv4 4453 "default-metric 10000 100 255 1 1500"
set_topology_expect "$uut_name" ipv4 4453 "distance eigrp 91 171"
set_topology_expect "$uut_name" ipv4 4453 "maximum-prefix 1000 80 dampened reset-time 15 restart 5 restart-count 3"
set_topology_expect "$uut_name" ipv4 4453 "maximum-paths 8"
set_topology_expect "$uut_name" ipv4 4453 "metric maximum-hops 200"
set_topology_expect "$uut_name" ipv4 4453 "metric holddown"
set_topology_expect "$uut_name" ipv4 4453 "eigrp event-log-size 1000"
set_af_expect "$uut_name" ipv4 4453 "metric weights 0 1 0 1 0 0"
set_topology_expect "$uut_name" ipv4 4453 "distribute-list STEP1-ACL-IN in"
set_topology_expect "$uut_name" ipv4 4453 "distribute-list prefix STEP1-PFX-OUT out"
set_topology_expect "$uut_name" ipv4 4453 "offset-list EIGRP-UUT in 100 $uut_if"
set_topology_expect "$uut_name" ipv4 4453 "redistribute connected metric 10000 100 255 1 1500 route-map STEP1-RM"
set_topology_expect "$uut_name" ipv4 4453 "redistribute maximum-prefix 300 70 dampened reset-time 20 restart 6 restart-count 4"
set_topology_expect "$uut_name" ipv4 4453 "summary-metric 10.44.0.0 255.255.0.0 10000 100 255 1 1500 distance 20"
set_topology_expect "$uut_name" ipv4 4453 "timers active-time 180"
# balanced is the default.  The no form is the retained non-default state and
# proves the documented no form survives configuration writeback.
set_topology_expect "$uut_name" ipv4 4453 "no traffic-share balanced"
set_topology_expect "$uut_name" ipv4 4453 "variance 2"

phase "stage 1: change IPv4/4453 retained values"
set_af_expect "$uut_name" ipv4 4453 "eigrp router-id 10.44.53.2"
config="$(show_run)"
assert_af_line_absent_in "$config" "$uut_name" ipv4 4453 "eigrp router-id 10.44.53.1"

set_af_expect "$uut_name" ipv4 4453 "neighbor 10.0.0.1 description STEP1-PEER-UPDATED"
config="$(show_run)"
assert_af_line_absent_in "$config" "$uut_name" ipv4 4453 "neighbor 10.0.0.1 description STEP1-PEER"
set_af_expect "$uut_name" ipv4 4453 "neighbor 10.0.0.1 maximum-prefix 120 85 warning-only"
config="$(show_run)"
assert_af_line_absent_in "$config" "$uut_name" ipv4 4453 "neighbor 10.0.0.1 maximum-prefix 100 80"
set_af_expect "$uut_name" ipv4 4453 "eigrp log-neighbor-warnings 45"
config="$(show_run)"
assert_af_line_absent_in "$config" "$uut_name" ipv4 4453 "eigrp log-neighbor-warnings 30"

set_if_expect "$uut_name" ipv4 4453 "$uut_if" "hello-interval 9"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "af-interface $uut_if" "exit-af-interface" "hello-interval 7"

set_if_expect "$uut_name" ipv4 4453 "$uut_if" "bandwidth-percent 80"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "af-interface $uut_if" "exit-af-interface" "bandwidth-percent 75"

set_if_expect "$uut_name" ipv4 4453 "$uut_if" "bandwidth 250000"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "af-interface $uut_if" "exit-af-interface" "bandwidth 200000"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "delay 250"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "af-interface $uut_if" "exit-af-interface" "delay 200"

set_if_expect "$uut_name" ipv4 4453 "$uut_if" "authentication mode hmac-sha-256 7 Step1Secret7"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "af-interface $uut_if" "exit-af-interface" "authentication mode hmac-sha-256 0 Step1Secret"
# Re-enter the same summary without options; omitted options must be removed,
# not left stale in YANG.
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "summary-address 10.44.0.0 255.255.0.0"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "af-interface $uut_if" "exit-af-interface" "summary-address 10.44.0.0 255.255.0.0 5 leak-map STEP1-LEAK"

set_topology_expect "$uut_name" ipv4 4453 "default-metric 20000 200 250 2 1400"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "topology base" "exit-af-topology" "default-metric 10000 100 255 1 1500"

set_topology_expect "$uut_name" ipv4 4453 "distance eigrp 93 173"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "topology base" "exit-af-topology" "distance eigrp 91 171"

set_topology_expect "$uut_name" ipv4 4453 "default-information in STEP1-IN-2"
set_topology_expect "$uut_name" ipv4 4453 "maximum-prefix 1100 85 warning-only"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "topology base" "exit-af-topology" "maximum-prefix 1000 80 dampened reset-time 15 restart 5 restart-count 3"
set_topology_expect "$uut_name" ipv4 4453 "maximum-paths 12"
set_topology_expect "$uut_name" ipv4 4453 "metric maximum-hops 220"
set_topology_expect "$uut_name" ipv4 4453 "eigrp event-log-size 1200"
set_topology_expect "$uut_name" ipv4 4453 "redistribute connected metric 15000 150 252 2 1450 route-map STEP1-RM-2"
set_topology_expect "$uut_name" ipv4 4453 "redistribute maximum-prefix 350 75 warning-only"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "topology base" "exit-af-topology" "redistribute maximum-prefix 300 70 dampened reset-time 20 restart 6 restart-count 4"
set_topology_expect "$uut_name" ipv4 4453 "summary-metric 10.44.0.0 255.255.0.0 distance 25"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "topology base" "exit-af-topology" "summary-metric 10.44.0.0 255.255.0.0 10000 100 255 1 1500 distance 20"

set_af_expect "$uut_name" ipv4 4453 "metric weights 0 2 1 3 1 0 7"
config="$(show_run)"
assert_af_line_absent_in "$config" "$uut_name" ipv4 4453 "metric weights 0 1 0 1 0 0"

set_topology_expect "$uut_name" ipv4 4453 "redistribute connected metric 20000 200 250 2 1400 route-map STEP1-RM-3"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "topology base" "exit-af-topology" "redistribute connected metric 15000 150 252 2 1450 route-map STEP1-RM-2"

set_topology_expect "$uut_name" ipv4 4453 "timers active-time 240"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "topology base" "exit-af-topology" "timers active-time 180"

set_topology_expect "$uut_name" ipv4 4453 "variance 4"
config="$(show_run)"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 "topology base" "exit-af-topology" "variance 2"

phase "stage 1: remove every IPv4/4453 child command individually"
remove_af_expect "$uut_name" ipv4 4453 "no eigrp router-id" "eigrp router-id 10.44.53.2"
remove_af_expect "$uut_name" ipv4 4453 "no network 10.44.0.0 0.0.255.255" "network 10.44.0.0/16"
remove_af_expect "$uut_name" ipv4 4453 "no neighbor 10.0.0.1 description" "neighbor 10.0.0.1 description STEP1-PEER-UPDATED"
remove_af_expect "$uut_name" ipv4 4453 "no neighbor 10.0.0.1 maximum-prefix" "neighbor 10.0.0.1 maximum-prefix 120 85 warning-only"
remove_af_expect "$uut_name" ipv4 4453 "no neighbor maximum-prefix" "neighbor maximum-prefix 500 75 warning-only"
# The positive form restores the default and removes the retained no form.
remove_af_expect "$uut_name" ipv4 4453 "eigrp log-neighbor-changes" "no eigrp log-neighbor-changes"
# First prove the warnings no form is itself retained, then restore the default.
set_af_expect "$uut_name" ipv4 4453 "no eigrp log-neighbor-warnings"
remove_af_expect "$uut_name" ipv4 4453 "eigrp log-neighbor-warnings" "no eigrp log-neighbor-warnings"
remove_af_expect "$uut_name" ipv4 4453 "no neighbor 10.0.0.1 $uut_if" "neighbor 10.0.0.1 $uut_if"
remove_af_expect "$uut_name" ipv4 4453 "no shutdown" "shutdown"

remove_if_expect "$uut_name" ipv4 4453 default "no bandwidth 100000" "bandwidth 100000"
remove_if_expect "$uut_name" ipv4 4453 default "no delay 100" "delay 100"
remove_if_expect "$uut_name" ipv4 4453 default "no bandwidth-percent" "bandwidth-percent 60"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no bandwidth" "bandwidth 250000"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no delay" "delay 250"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no bandwidth-percent" "bandwidth-percent 80"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no hello-interval" "hello-interval 9"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no hold-time" "hold-time 21"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no passive-interface" "passive-interface"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no authentication key-chain EIGRP-UUT-4453" "authentication key-chain EIGRP-UUT-4453"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no authentication mode" "authentication mode hmac-sha-256 7 Step1Secret7"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "next-hop-self" "no next-hop-self"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "split-horizon" "no split-horizon"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no summary-address 10.44.0.0 255.255.0.0" "summary-address 10.44.0.0 255.255.0.0"
remove_if_expect "$uut_name" ipv4 4453 "$uut_if" "no shutdown" "shutdown"

remove_topology_expect "$uut_name" ipv4 4453 "no auto-summary" "auto-summary"
remove_topology_expect "$uut_name" ipv4 4453 "no default-information in" "default-information in STEP1-IN-2"
remove_topology_expect "$uut_name" ipv4 4453 "no default-information out" "default-information out STEP1-OUT"
remove_topology_expect "$uut_name" ipv4 4453 "no default-metric 20000 200 250 2 1400" "default-metric 20000 200 250 2 1400"
remove_topology_expect "$uut_name" ipv4 4453 "no distance eigrp" "distance eigrp 93 173"
remove_topology_expect "$uut_name" ipv4 4453 "no maximum-prefix" "maximum-prefix 1100 85 warning-only"
remove_topology_expect "$uut_name" ipv4 4453 "no maximum-paths" "maximum-paths 12"
remove_topology_expect "$uut_name" ipv4 4453 "no metric maximum-hops" "metric maximum-hops 220"
remove_topology_expect "$uut_name" ipv4 4453 "no metric holddown" "metric holddown"
remove_topology_expect "$uut_name" ipv4 4453 "no eigrp event-log-size" "eigrp event-log-size 1200"
remove_af_expect "$uut_name" ipv4 4453 "no metric weights" "metric weights 0 2 1 3 1 0 7"
remove_topology_expect "$uut_name" ipv4 4453 "no distribute-list STEP1-ACL-IN in" "distribute-list STEP1-ACL-IN in"
remove_topology_expect "$uut_name" ipv4 4453 "no distribute-list prefix STEP1-PFX-OUT out" "distribute-list prefix STEP1-PFX-OUT out"
remove_topology_expect "$uut_name" ipv4 4453 "no offset-list EIGRP-UUT in 100 $uut_if" "offset-list EIGRP-UUT in 100 $uut_if"
remove_topology_expect "$uut_name" ipv4 4453 "no redistribute maximum-prefix" "redistribute maximum-prefix 350 75 warning-only"
remove_topology_expect "$uut_name" ipv4 4453 "no redistribute connected" "redistribute connected metric 20000 200 250 2 1400 route-map STEP1-RM-3"
remove_topology_expect "$uut_name" ipv4 4453 "no summary-metric 10.44.0.0 255.255.0.0" "summary-metric 10.44.0.0 255.255.0.0 distance 25"
remove_topology_expect "$uut_name" ipv4 4453 "no timers active-time" "timers active-time 240"
remove_topology_expect "$uut_name" ipv4 4453 "traffic-share balanced" "no traffic-share balanced"
remove_topology_expect "$uut_name" ipv4 4453 "no variance" "variance 4"

vty_apply "configure terminal" "router eigrp $uut_name" \
	"address-family ipv4 unicast autonomous-system 4453" \
	"no af-interface default" "no af-interface $uut_if" "end"
config="$(show_run)"
assert_af_exists_in "$config" "$uut_name" ipv4 4453

# Only after the complete IPv4 test has passed do we introduce IPv6.
phase "stage 2: create only IPv6 AS 4453 under the proven named process"
create_af_expect "$uut_name" ipv6 4453

phase "stage 2: configure and verify the applicable IPv4-equivalent IPv6/4453 matrix"
# IPv6 owns a control/runtime context, but its packet data path is deliberately
# not ready.  Configuration and control targets must retain/write back exactly
# as IPv4 does.  Only intrinsically IPv4 commands (network and auto-summary)
# are omitted.
set_af_expect "$uut_name" ipv6 4453 "eigrp router-id 10.44.53.6"
set_af_expect "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::2 $uut_if"
set_af_expect "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::2 description STEP2-PEER"
set_af_expect "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::2 maximum-prefix 100 80"
set_af_expect "$uut_name" ipv6 4453 "neighbor maximum-prefix 500 75 warning-only"
set_af_expect "$uut_name" ipv6 4453 "no eigrp log-neighbor-changes"
set_af_expect "$uut_name" ipv6 4453 "eigrp log-neighbor-warnings 30"
set_af_expect "$uut_name" ipv6 4453 "shutdown"

set_if_expect "$uut_name" ipv6 4453 default "bandwidth 110000"
set_if_expect "$uut_name" ipv6 4453 default "delay 110"
set_if_expect "$uut_name" ipv6 4453 default "bandwidth-percent 61"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "bandwidth 210000"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "delay 210"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "bandwidth-percent 76"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "hello-interval 8"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "hold-time 24"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "passive-interface"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "authentication key-chain EIGRP-UUT6-4453"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "authentication mode md5"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "authentication mode hmac-sha-256 0 Step2Secret"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "no next-hop-self"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "no split-horizon"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "summary-address 2001:db8:4453::/48 6 leak-map STEP2-LEAK"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "shutdown"

set_topology_expect "$uut_name" ipv6 4453 "default-information in STEP2-IN"
set_topology_expect "$uut_name" ipv6 4453 "default-information out STEP2-OUT"
set_topology_expect "$uut_name" ipv6 4453 "default-metric 11000 110 254 2 1492"
set_topology_expect "$uut_name" ipv6 4453 "distance eigrp 92 172"
set_topology_expect "$uut_name" ipv6 4453 "maximum-prefix 1001 80 dampened reset-time 16 restart 6 restart-count 4"
set_topology_expect "$uut_name" ipv6 4453 "maximum-paths 9"
set_topology_expect "$uut_name" ipv6 4453 "metric maximum-hops 201"
set_topology_expect "$uut_name" ipv6 4453 "metric holddown"
set_topology_expect "$uut_name" ipv6 4453 "eigrp event-log-size 1001"
set_af_expect "$uut_name" ipv6 4453 "metric weights 0 2 0 2 0 0"
set_topology_expect "$uut_name" ipv6 4453 "distribute-list STEP2-ACL-IN in"
set_topology_expect "$uut_name" ipv6 4453 "distribute-list prefix STEP2-PFX-OUT out"
set_topology_expect "$uut_name" ipv6 4453 "offset-list EIGRP-UUT6 in 101 $uut_if"
set_topology_expect "$uut_name" ipv6 4453 "redistribute connected metric 11000 110 254 2 1492 route-map STEP2-RM"
set_topology_expect "$uut_name" ipv6 4453 "redistribute maximum-prefix 301 71 dampened reset-time 21 restart 7 restart-count 5"
set_topology_expect "$uut_name" ipv6 4453 "summary-metric 2001:db8:4453::/48 11000 110 254 2 1492 distance 21"
set_topology_expect "$uut_name" ipv6 4453 "timers active-time 181"
set_topology_expect "$uut_name" ipv6 4453 "no traffic-share balanced"
set_topology_expect "$uut_name" ipv6 4453 "variance 3"

phase "stage 2: change IPv6/4453 retained values"
set_af_expect "$uut_name" ipv6 4453 "eigrp router-id 10.44.53.7"
config="$(show_run)"
assert_af_line_absent_in "$config" "$uut_name" ipv6 4453 "eigrp router-id 10.44.53.6"
set_af_expect "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::2 description STEP2-PEER-UPDATED"
set_af_expect "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::2 maximum-prefix 120 85 warning-only"
set_af_expect "$uut_name" ipv6 4453 "eigrp log-neighbor-warnings 45"
set_af_expect "$uut_name" ipv6 4453 "metric weights 0 3 0 3 0 0 6"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "hello-interval 10"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "bandwidth-percent 81"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "bandwidth 260000"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "delay 260"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "authentication mode hmac-sha-256 7 Step2Secret7"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "summary-address 2001:db8:4453::/48"
set_topology_expect "$uut_name" ipv6 4453 "default-information in STEP2-IN-2"
set_topology_expect "$uut_name" ipv6 4453 "default-metric 21000 210 249 3 1401"
set_topology_expect "$uut_name" ipv6 4453 "distance eigrp 94 174"
set_topology_expect "$uut_name" ipv6 4453 "maximum-prefix 1101 85 warning-only"
set_topology_expect "$uut_name" ipv6 4453 "maximum-paths 13"
set_topology_expect "$uut_name" ipv6 4453 "metric maximum-hops 221"
set_topology_expect "$uut_name" ipv6 4453 "eigrp event-log-size 1201"
set_topology_expect "$uut_name" ipv6 4453 "redistribute connected metric 21000 210 249 3 1401 route-map STEP2-RM-3"
set_topology_expect "$uut_name" ipv6 4453 "redistribute maximum-prefix 351 76 warning-only"
set_topology_expect "$uut_name" ipv6 4453 "summary-metric 2001:db8:4453::/48 distance 26"
set_topology_expect "$uut_name" ipv6 4453 "timers active-time 241"
set_topology_expect "$uut_name" ipv6 4453 "variance 5"

phase "stage 2: remove every applicable IPv6/4453 child command individually"
remove_af_expect "$uut_name" ipv6 4453 "no eigrp router-id" "eigrp router-id 10.44.53.7"
remove_af_expect "$uut_name" ipv6 4453 "no neighbor 2001:db8:4453::2 description" "neighbor 2001:db8:4453::2 description STEP2-PEER-UPDATED"
remove_af_expect "$uut_name" ipv6 4453 "no neighbor 2001:db8:4453::2 maximum-prefix" "neighbor 2001:db8:4453::2 maximum-prefix 120 85 warning-only"
remove_af_expect "$uut_name" ipv6 4453 "no neighbor maximum-prefix" "neighbor maximum-prefix 500 75 warning-only"
remove_af_expect "$uut_name" ipv6 4453 "eigrp log-neighbor-changes" "no eigrp log-neighbor-changes"
set_af_expect "$uut_name" ipv6 4453 "no eigrp log-neighbor-warnings"
remove_af_expect "$uut_name" ipv6 4453 "eigrp log-neighbor-warnings" "no eigrp log-neighbor-warnings"
remove_af_expect "$uut_name" ipv6 4453 "no neighbor 2001:db8:4453::2 $uut_if" "neighbor 2001:db8:4453::2 $uut_if"
remove_af_expect "$uut_name" ipv6 4453 "no shutdown" "shutdown"

remove_if_expect "$uut_name" ipv6 4453 default "no bandwidth 110000" "bandwidth 110000"
remove_if_expect "$uut_name" ipv6 4453 default "no delay 110" "delay 110"
remove_if_expect "$uut_name" ipv6 4453 default "no bandwidth-percent" "bandwidth-percent 61"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no bandwidth" "bandwidth 260000"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no delay" "delay 260"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no bandwidth-percent" "bandwidth-percent 81"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no hello-interval" "hello-interval 10"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no hold-time" "hold-time 24"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no passive-interface" "passive-interface"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no authentication key-chain EIGRP-UUT6-4453" "authentication key-chain EIGRP-UUT6-4453"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no authentication mode" "authentication mode hmac-sha-256 7 Step2Secret7"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "next-hop-self" "no next-hop-self"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "split-horizon" "no split-horizon"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no summary-address 2001:db8:4453::/48" "summary-address 2001:db8:4453::/48"
remove_if_expect "$uut_name" ipv6 4453 "$uut_if" "no shutdown" "shutdown"

remove_topology_expect "$uut_name" ipv6 4453 "no default-information in" "default-information in STEP2-IN-2"
remove_topology_expect "$uut_name" ipv6 4453 "no default-information out" "default-information out STEP2-OUT"
remove_topology_expect "$uut_name" ipv6 4453 "no default-metric 21000 210 249 3 1401" "default-metric 21000 210 249 3 1401"
remove_topology_expect "$uut_name" ipv6 4453 "no distance eigrp" "distance eigrp 94 174"
remove_topology_expect "$uut_name" ipv6 4453 "no maximum-prefix" "maximum-prefix 1101 85 warning-only"
remove_topology_expect "$uut_name" ipv6 4453 "no maximum-paths" "maximum-paths 13"
remove_topology_expect "$uut_name" ipv6 4453 "no metric maximum-hops" "metric maximum-hops 221"
remove_topology_expect "$uut_name" ipv6 4453 "no metric holddown" "metric holddown"
remove_topology_expect "$uut_name" ipv6 4453 "no eigrp event-log-size" "eigrp event-log-size 1201"
remove_af_expect "$uut_name" ipv6 4453 "no metric weights" "metric weights 0 3 0 3 0 0 6"
remove_topology_expect "$uut_name" ipv6 4453 "no distribute-list STEP2-ACL-IN in" "distribute-list STEP2-ACL-IN in"
remove_topology_expect "$uut_name" ipv6 4453 "no distribute-list prefix STEP2-PFX-OUT out" "distribute-list prefix STEP2-PFX-OUT out"
remove_topology_expect "$uut_name" ipv6 4453 "no offset-list EIGRP-UUT6 in 101 $uut_if" "offset-list EIGRP-UUT6 in 101 $uut_if"
remove_topology_expect "$uut_name" ipv6 4453 "no redistribute maximum-prefix" "redistribute maximum-prefix 351 76 warning-only"
remove_topology_expect "$uut_name" ipv6 4453 "no redistribute connected" "redistribute connected metric 21000 210 249 3 1401 route-map STEP2-RM-3"
remove_topology_expect "$uut_name" ipv6 4453 "no summary-metric 2001:db8:4453::/48" "summary-metric 2001:db8:4453::/48 distance 26"
remove_topology_expect "$uut_name" ipv6 4453 "no timers active-time" "timers active-time 241"
remove_topology_expect "$uut_name" ipv6 4453 "traffic-share balanced" "no traffic-share balanced"
remove_topology_expect "$uut_name" ipv6 4453 "no variance" "variance 5"

vty_apply "configure terminal" "router eigrp $uut_name" \
    "address-family ipv6 unicast autonomous-system 4453" \
    "no af-interface default" "no af-interface $uut_if" "end"
config="$(show_run)"
assert_af_exists_in "$config" "$uut_name" ipv6 4453

# Only after both single-AS address families have passed do we exercise
# multiple AS contexts under one named parent.
phase "stage 3: add second AS 6473 contexts"
create_af_expect "$uut_name" ipv4 6473
create_af_expect "$uut_name" ipv6 6473

# The important multiple-AS test is not merely that two AF nodes can coexist.
# Give the IPv4 4453 and 6473 contexts distinct AF-, interface-, and topology-
# level configuration and prove writeback remains keyed to the selected AS.
phase "stage 3: verify IPv4 AS 4453/6473 configuration isolation"
set_af_expect "$uut_name" ipv4 4453 "eigrp router-id 10.44.53.31"
set_af_expect "$uut_name" ipv4 4453 "network 10.44.0.0 0.0.255.255" "network 10.44.0.0/16"
set_if_expect "$uut_name" ipv4 4453 "$uut_if" "hello-interval 13"
set_topology_expect "$uut_name" ipv4 4453 "variance 5"

set_af_expect "$uut_name" ipv4 6473 "eigrp router-id 10.64.73.31"
set_af_expect "$uut_name" ipv4 6473 "network 10.64.0.0 0.0.255.255" "network 10.64.0.0/16"
set_if_expect "$uut_name" ipv4 6473 "$uut_if" "hello-interval 17"
set_topology_expect "$uut_name" ipv4 6473 "variance 7"

config="$(show_run)"
assert_af_exists_in "$config" "$uut_name" ipv4 4453
assert_af_exists_in "$config" "$uut_name" ipv4 6473
assert_af_line_in "$config" "$uut_name" ipv4 4453 "eigrp router-id 10.44.53.31"
assert_af_line_in "$config" "$uut_name" ipv4 6473 "eigrp router-id 10.64.73.31"
assert_af_line_in "$config" "$uut_name" ipv4 4453 "network 10.44.0.0/16"
assert_af_line_in "$config" "$uut_name" ipv4 6473 "network 10.64.0.0/16"
assert_af_line_absent_in "$config" "$uut_name" ipv4 4453 "eigrp router-id 10.64.73.31"
assert_af_line_absent_in "$config" "$uut_name" ipv4 6473 "eigrp router-id 10.44.53.31"
assert_af_line_absent_in "$config" "$uut_name" ipv4 4453 "network 10.64.0.0/16"
assert_af_line_absent_in "$config" "$uut_name" ipv4 6473 "network 10.44.0.0/16"
assert_mode_line_in "$config" "$uut_name" ipv4 4453 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 13"
assert_mode_line_in "$config" "$uut_name" ipv4 6473 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 17"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 17"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 6473 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 13"
assert_mode_line_in "$config" "$uut_name" ipv4 4453 \
	"topology base" "exit-af-topology" "variance 5"
assert_mode_line_in "$config" "$uut_name" ipv4 6473 \
	"topology base" "exit-af-topology" "variance 7"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 4453 \
	"topology base" "exit-af-topology" "variance 7"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 6473 \
	"topology base" "exit-af-topology" "variance 5"

# Mutate only 6473.  The 4453 values must remain byte-for-byte distinct in
# running-config, proving update lookup is scoped by the AF/AS context.
phase "stage 3: mutate IPv4 AS 6473 without changing AS 4453"
set_af_expect "$uut_name" ipv4 6473 "eigrp router-id 10.64.73.32"
set_if_expect "$uut_name" ipv4 6473 "$uut_if" "hello-interval 19"
set_topology_expect "$uut_name" ipv4 6473 "variance 9"

config="$(show_run)"
assert_af_line_absent_in "$config" "$uut_name" ipv4 6473 "eigrp router-id 10.64.73.31"
assert_af_line_in "$config" "$uut_name" ipv4 6473 "network 10.64.0.0/16"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 6473 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 17"
assert_mode_line_absent_in "$config" "$uut_name" ipv4 6473 \
	"topology base" "exit-af-topology" "variance 7"
assert_af_line_in "$config" "$uut_name" ipv4 4453 "eigrp router-id 10.44.53.31"
assert_af_line_in "$config" "$uut_name" ipv4 4453 "network 10.44.0.0/16"
assert_mode_line_in "$config" "$uut_name" ipv4 4453 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 13"
assert_mode_line_in "$config" "$uut_name" ipv4 4453 \
	"topology base" "exit-af-topology" "variance 5"

# Exercise the same multiple-AS isolation for IPv6.  IPv6 does not have the
# IPv4 network command, so use a configured neighbor as the AF-level keyed
# object in addition to router-id.
phase "stage 3: verify IPv6 AS 4453/6473 configuration isolation"
set_af_expect "$uut_name" ipv6 4453 "eigrp router-id 10.44.53.61"
set_af_expect "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::31 $uut_if"
set_if_expect "$uut_name" ipv6 4453 "$uut_if" "hello-interval 23"
set_topology_expect "$uut_name" ipv6 4453 "variance 11"

set_af_expect "$uut_name" ipv6 6473 "eigrp router-id 10.64.73.61"
set_af_expect "$uut_name" ipv6 6473 "neighbor 2001:db8:6473::31 $uut_if"
set_if_expect "$uut_name" ipv6 6473 "$uut_if" "hello-interval 29"
set_topology_expect "$uut_name" ipv6 6473 "variance 13"

config="$(show_run)"
assert_af_exists_in "$config" "$uut_name" ipv6 4453
assert_af_exists_in "$config" "$uut_name" ipv6 6473
assert_af_line_in "$config" "$uut_name" ipv6 4453 "eigrp router-id 10.44.53.61"
assert_af_line_in "$config" "$uut_name" ipv6 6473 "eigrp router-id 10.64.73.61"
assert_af_line_in "$config" "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::31 $uut_if"
assert_af_line_in "$config" "$uut_name" ipv6 6473 "neighbor 2001:db8:6473::31 $uut_if"
assert_af_line_absent_in "$config" "$uut_name" ipv6 4453 "eigrp router-id 10.64.73.61"
assert_af_line_absent_in "$config" "$uut_name" ipv6 6473 "eigrp router-id 10.44.53.61"
assert_af_line_absent_in "$config" "$uut_name" ipv6 4453 "neighbor 2001:db8:6473::31 $uut_if"
assert_af_line_absent_in "$config" "$uut_name" ipv6 6473 "neighbor 2001:db8:4453::31 $uut_if"
assert_mode_line_in "$config" "$uut_name" ipv6 4453 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 23"
assert_mode_line_in "$config" "$uut_name" ipv6 6473 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 29"
assert_mode_line_absent_in "$config" "$uut_name" ipv6 4453 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 29"
assert_mode_line_absent_in "$config" "$uut_name" ipv6 6473 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 23"
assert_mode_line_in "$config" "$uut_name" ipv6 4453 \
	"topology base" "exit-af-topology" "variance 11"
assert_mode_line_in "$config" "$uut_name" ipv6 6473 \
	"topology base" "exit-af-topology" "variance 13"
assert_mode_line_absent_in "$config" "$uut_name" ipv6 4453 \
	"topology base" "exit-af-topology" "variance 13"
assert_mode_line_absent_in "$config" "$uut_name" ipv6 6473 \
	"topology base" "exit-af-topology" "variance 11"

phase "stage 3: mutate IPv6 AS 6473 without changing AS 4453"
set_af_expect "$uut_name" ipv6 6473 "eigrp router-id 10.64.73.62"
set_if_expect "$uut_name" ipv6 6473 "$uut_if" "hello-interval 31"
set_topology_expect "$uut_name" ipv6 6473 "variance 15"

config="$(show_run)"
assert_af_line_absent_in "$config" "$uut_name" ipv6 6473 "eigrp router-id 10.64.73.61"
assert_af_line_in "$config" "$uut_name" ipv6 6473 "neighbor 2001:db8:6473::31 $uut_if"
assert_mode_line_absent_in "$config" "$uut_name" ipv6 6473 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 29"
assert_mode_line_absent_in "$config" "$uut_name" ipv6 6473 \
	"topology base" "exit-af-topology" "variance 13"
assert_af_line_in "$config" "$uut_name" ipv6 4453 "eigrp router-id 10.44.53.61"
assert_af_line_in "$config" "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::31 $uut_if"
assert_mode_line_in "$config" "$uut_name" ipv6 4453 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 23"
assert_mode_line_in "$config" "$uut_name" ipv6 4453 \
	"topology base" "exit-af-topology" "variance 11"

phase "stage 3: remove second-AS contexts with no address-family"
remove_af_context_expect "$uut_name" ipv4 6473
remove_af_context_expect "$uut_name" ipv6 6473

# Deleting 6473 must not merely leave the 4453 node behind; its retained
# child configuration must remain intact as well.
config="$(show_run)"
assert_af_exists_in "$config" "$uut_name" ipv4 4453
assert_af_exists_in "$config" "$uut_name" ipv6 4453
assert_af_line_in "$config" "$uut_name" ipv4 4453 "eigrp router-id 10.44.53.31"
assert_af_line_in "$config" "$uut_name" ipv4 4453 "network 10.44.0.0/16"
assert_mode_line_in "$config" "$uut_name" ipv4 4453 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 13"
assert_mode_line_in "$config" "$uut_name" ipv4 4453 \
	"topology base" "exit-af-topology" "variance 5"
assert_af_line_in "$config" "$uut_name" ipv6 4453 "eigrp router-id 10.44.53.61"
assert_af_line_in "$config" "$uut_name" ipv6 4453 "neighbor 2001:db8:4453::31 $uut_if"
assert_mode_line_in "$config" "$uut_name" ipv6 4453 \
	"af-interface $uut_if" "exit-af-interface" "hello-interval 23"
assert_mode_line_in "$config" "$uut_name" ipv6 4453 \
	"topology base" "exit-af-topology" "variance 11"

# Clean the single-parent matrix before the final case-sensitive name test.
remove_af_context_expect "$uut_name" ipv4 4453
remove_af_context_expect "$uut_name" ipv6 4453

phase "stage 4: verify named process names are case-sensitive"
create_af_expect "$uut_name" ipv4 4453
create_af_expect "$uut_name_case" ipv4 6473
set_af_expect "$uut_name" ipv4 4453 "eigrp router-id 10.44.53.10"
set_af_expect "$uut_name_case" ipv4 6473 "eigrp router-id 10.64.73.10"

config="$(show_run)"
assert_af_exists_in "$config" "$uut_name" ipv4 4453
assert_af_exists_in "$config" "$uut_name_case" ipv4 6473
assert_af_absent_in "$config" "$uut_name" ipv4 6473
assert_af_absent_in "$config" "$uut_name_case" ipv4 4453

if [[ "$keep_config" -eq 0 ]]; then
	phase "cleanup"
	cleanup_names
else
	echo "leaving final savage/SAVAGE configuration in running-config (--keep-config)"
fi

phase "PASS"
echo "named-mode live UUT passed: $assertions running-config assertions"
echo "transcript: $log_file"
