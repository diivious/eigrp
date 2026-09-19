#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Build and test the FRR-backed EIGRP unit under test (UUT).
#
# When --host is supplied, the project is synced to the remote Linux UUT and
# this same script is invoked there.  The remote invocation performs the FRR
# build first and only runs tests after the build succeeds.

set -euo pipefail

script_name="$(basename "$0")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eigrp_root="$(cd "$script_dir/.." && pwd)"

test_mode="all"
test_mode_set=0
build_only=0
list_only=0
configure_first=0
host=""
remote_root="eigrp-uut"
remote_root_set=0
frr_root=""
jobs=""
pytest_args=()

usage() {
	cat <<USAGE
usage: $script_name [options] [-- pytest-args]

Default workflow:
  1. Sync to the remote UUT when --host is supplied.
  2. Stage EIGRP into the FRR checkout without modifying FRR-wide patches.
  3. Build FRR/eigrpd.
  4. Run portable and FRR-native EIGRP tests.

Required FRR-wide patches must already have been applied explicitly with
  tools/frr.sh --patch

Test selection:
  --all                 Build, then run portable and FRR-native tests. Default.
  --packet              Build, then run portable packet tests only.
  --portable            Build, then run all portable tests only.
  --frr                 Build, then run live named-mode UUT and FRR-native tests.
  --build-only          Build the UUT and do not run tests.
  --list                List available EIGRP tests and exit without building.

Build/UUT options:
  --host USER@HOST      Remote Linux UUT. If omitted, this machine is the UUT.
  --remote-root PATH    Remote parent directory used for project sync.
                        Default: eigrp-uut.
  --frr-root PATH       FRR checkout root on the UUT.
                        Default: ../frr or ~/devel/frr when detectable.
  --jobs N              make parallelism. Default: detected on the UUT.
  --configure-first     Run FRR bootstrap/configure before the build.
  --help                Show this help.

Arguments after -- are passed to pytest/FRR test execution.

examples:
  # Build and test on the current Linux UUT.
  tools/frr-uut.sh --frr-root ~/devel/frr

  # Sync, build, and test on a remote Linux UUT.
  tools/frr-uut.sh --host uut --frr-root '~/devel/frr'

  # Remote packet-test cycle.
  tools/frr-uut.sh --packet --host donnie@lab-linux --frr-root /home/donnie/frr

  # Reconfigure before building and testing.
  tools/frr-uut.sh --configure-first --frr-root ~/devel/frr

  # Build only.
  tools/frr-uut.sh --build-only --frr-root ~/devel/frr

layout:
  portable tests:       test/portable/
  FRR test source:      frr/test/
  FRR installed tests:  frr/tests/eigrpd/
USAGE
}

fail() {
	echo "error: $*" >&2
	exit 1
}

set_test_mode() {
	local new_mode="$1"
	if [[ "$test_mode_set" -eq 1 ]]; then
		fail "only one test selection may be specified"
	fi
	test_mode="$new_mode"
	test_mode_set=1
}

require_command() {
	command -v "$1" >/dev/null 2>&1 || fail "required command not found: $1"
}

is_abs_path() {
	case "$1" in
		/*) return 0 ;;
		*) return 1 ;;
	esac
}

expand_home_path() {
	local path="$1"
	case "$path" in
		'~') printf '%s\n' "$HOME" ;;
		'~/'*) printf '%s/%s\n' "$HOME" "${path:2}" ;;
		*) printf '%s\n' "$path" ;;
	esac
}

resolve_existing_path() {
	local path
	path="$(expand_home_path "$1")"
	if is_abs_path "$path"; then
		cd "$path" && pwd
	else
		cd "$PWD" && cd "$path" && pwd
	fi
}

infer_frr_root() {
	local candidate
	for candidate in "$eigrp_root/../frr" "$HOME/devel/frr" "$eigrp_root/.."; do
		if [[ -d "$candidate/tests" && -f "$candidate/bootstrap.sh" ]]; then
			cd "$candidate" && pwd
			return 0
		fi
	done
	return 1
}

default_jobs() {
	if command -v nproc >/dev/null 2>&1; then
		nproc
	elif command -v sysctl >/dev/null 2>&1; then
		sysctl -n hw.ncpu
	else
		echo 4
	fi
}

has_frr_tests() {
	local src="$1"
	[[ -d "$src" ]] || return 1
	find "$src" -mindepth 1 -type f ! -name 'README.md' | grep -q .
}

print_available_tests() {
	echo "portable tests:"
	if [[ -d "$eigrp_root/test/portable" ]]; then
		find "$eigrp_root/test/portable" -mindepth 1 -maxdepth 3 -type d ! -name __pycache__ \
			| sed "s#^$eigrp_root/test/##" | sort
	else
		echo "  none"
	fi

	echo
	echo "FRR-native test payload:"
	if has_frr_tests "$eigrp_root/frr/test"; then
		find "$eigrp_root/frr/test" -mindepth 1 -maxdepth 2 -type f \
			| sed "s#^$eigrp_root/frr/test/#  #" | sort
	else
		echo "  none"
	fi
}

shell_quote() {
	printf '%q' "$1"
}

remote_path_quote() {
	local path="$1"
	case "$path" in
		'~')
			printf '~'
			;;
		'~/'*)
			printf '~/%q' "${path:2}"
			;;
		*)
			shell_quote "$path"
			;;
	esac
}

sync_remote_project() {
	local remote_project="$remote_root/eigrp"

	require_command ssh
	require_command rsync

	echo "sync: $eigrp_root/ -> $host:$remote_project/"
	ssh "$host" "mkdir -p $(remote_path_quote "$remote_project")"
	rsync -az --delete \
		--exclude '.git/' \
		--exclude '.pytest_cache/' \
		--exclude '__pycache__/' \
		--exclude '__MACOSX/' \
		--exclude '.DS_Store' \
		--exclude '*.o' \
		--exclude '*.lo' \
		--exclude '*.la' \
		--exclude '*~' \
		--exclude 'test/build/obj/' \
		--exclude 'test/build/logs/' \
		"$eigrp_root"/ "$host:$remote_project"/
}

run_remote_uut() {
	local remote_project="$remote_root/eigrp"
	local remote_cmd

	sync_remote_project

	remote_cmd="cd $(remote_path_quote "$remote_project") && tools/frr-uut.sh"

	if [[ "$build_only" -eq 1 ]]; then
		remote_cmd+=" --build-only"
	else
		case "$test_mode" in
			all) remote_cmd+=" --all" ;;
			packet) remote_cmd+=" --packet" ;;
			portable) remote_cmd+=" --portable" ;;
			frr) remote_cmd+=" --frr" ;;
			*) fail "unhandled test mode: $test_mode" ;;
		esac
	fi
	if [[ "$configure_first" -eq 1 ]]; then
		remote_cmd+=" --configure-first"
	fi
	if [[ -n "$frr_root" ]]; then
		remote_cmd+=" --frr-root $(shell_quote "$frr_root")"
	fi
	if [[ -n "$jobs" ]]; then
		remote_cmd+=" --jobs $(shell_quote "$jobs")"
	fi
	if [[ "${#pytest_args[@]}" -gt 0 ]]; then
		remote_cmd+=" --"
		local arg
		for arg in "${pytest_args[@]}"; do
			remote_cmd+=" $(shell_quote "$arg")"
		done
	fi

	echo "run: ssh $host $remote_cmd"
	ssh "$host" "$remote_cmd"
}

build_local_uut() {
	local build_args=(--build --frr-root "$frr_root" --jobs "$jobs")

	if [[ "$configure_first" -eq 1 ]]; then
		build_args+=(--configure-first)
	fi

	"$script_dir/frr.sh" "${build_args[@]}"
}

run_portable_tests() {
	local target="$1"
	require_command python3
	(
		cd "$eigrp_root"
		python3 -m pytest "$target" "${pytest_args[@]}"
	)
}

cleanup_stale_eigrpd_uut() {
	local pids remaining live pid state i

	pids="$(pgrep -x eigrpd 2>/dev/null || true)"
	if [[ -z "$pids" ]]; then
		echo "preflight: no existing eigrpd processes"
		return 0
	fi

	echo "preflight: existing eigrpd processes"
	ps -C eigrpd -o pid=,ppid=,stat=,user=,etime=,args= 2>/dev/null || true
	echo "preflight: terminate existing eigrpd processes"
	# pgrep returns one numeric PID per line; word splitting is intentional here.
	# shellcheck disable=SC2086
	sudo kill -TERM $pids 2>/dev/null || true

	for i in $(seq 1 5); do
		sleep 1
		remaining="$(pgrep -x eigrpd 2>/dev/null || true)"
		[[ -z "$remaining" ]] && {
			echo "preflight: eigrpd cleanup complete"
			return 0
		}
	done

	echo "preflight: force-kill surviving eigrpd processes"
	# shellcheck disable=SC2086
	sudo kill -KILL $remaining 2>/dev/null || true
	sleep 1

	remaining="$(pgrep -x eigrpd 2>/dev/null || true)"
	[[ -z "$remaining" ]] && {
		echo "preflight: eigrpd cleanup complete"
		return 0
	}

	# A true zombie cannot be killed and does not retain sockets or other daemon
	# runtime resources.  Fail only when a live/stopped eigrpd survived cleanup.
	live=""
	while read -r pid; do
		[[ -n "$pid" ]] || continue
		state="$(ps -o stat= -p "$pid" 2>/dev/null | awk '{print $1}' || true)"
		[[ -n "$state" ]] || continue
		case "$state" in
			Z*) ;;
			*) live="${live}${live:+ }${pid}" ;;
		esac
	done <<< "$remaining"

	if [[ -n "$live" ]]; then
		echo "error: eigrpd processes survived UUT preflight cleanup: $live" >&2
		ps -C eigrpd -o pid=,ppid=,stat=,user=,etime=,args= >&2 || true
		return 1
	fi

	echo "preflight: only defunct eigrpd process entries remain; continuing"
	ps -C eigrpd -o pid=,ppid=,stat=,user=,etime=,args= 2>/dev/null || true
}

start_eigrpd_uut() {
	local frrcommon="/usr/lib/frr/frrcommon.sh"
	local i

	[[ -r "$frrcommon" ]] || fail "FRR daemon helper not found: $frrcommon"

	echo "start: eigrpd manually for UUT (watchfrr remains disabled for eigrpd)"
	sudo bash -s -- "$frrcommon" <<'EOS'
frrcommon="$1"
log_success_msg() { echo "$@"; }
log_warning_msg() { echo "$@" >&2; }
log_failure_msg() { echo "$@" >&2; }
. "$frrcommon"

# The UUT owns this daemon instance.  Stop a stale manually-started eigrpd,
# then start the freshly installed binary directly through FRR's daemon helper.
# daemon_start() does not require eigrpd=yes and therefore does not register the
# daemon with watchfrr.
if daemon_status eigrpd >/dev/null 2>&1; then
	daemon_stop eigrpd --quiet || true
fi
daemon_start eigrpd
EOS

	for i in $(seq 1 30); do
		if sudo vtysh -d eigrpd -c 'show version' >/dev/null 2>&1; then
			echo "ready: eigrpd"
			return 0
		fi
		sleep 1
	done

	echo "error: eigrpd did not become VTY-ready within 30 seconds" >&2
	echo "diagnostic: /etc/frr/daemons" >&2
	grep '^eigrpd=' /etc/frr/daemons 2>/dev/null >&2 || true
	echo "diagnostic: eigrpd process" >&2
	pgrep -a eigrpd >&2 || true
	echo "diagnostic: FRR service" >&2
	sudo systemctl status frr --no-pager >&2 || true
	echo "diagnostic: recent FRR journal" >&2
	sudo journalctl -u frr -n 80 --no-pager >&2 || true
	return 1
}
activate_local_uut() {
	echo "install: activating FRR build under test"
	sudo make -C "$frr_root" install
	echo "restart: frr"
	sudo systemctl restart frr
	start_eigrpd_uut
}

run_frr_tests() {
	local frr_test_dir="$frr_root/tests/eigrpd"

	# The live VTY test must exercise the binary just built, not an older
	# installed daemon.  Activate the build and restart FRR before vtysh.
	activate_local_uut

	# First prove the built daemon accepts, retains, changes, and removes the
	# complete Step-1 named-mode configuration surface through the real VTY.
	# This intentionally uses sudo vtysh -d eigrpd rather than a parser-only
	# harness.
	"$script_dir/frr-named-uut.sh"

	[[ -d "$frr_test_dir" ]] || fail "FRR EIGRP test directory not found: $frr_test_dir"
	if ! has_frr_tests "$frr_test_dir"; then
		echo "warning: no FRR-native EIGRP tests are installed yet at $frr_test_dir" >&2
		return 0
	fi

	require_command python3
	(
		cd "$frr_root"
		python3 tests/runtests.py -v tests/eigrpd "${pytest_args[@]}"
	)
}

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		--all)
			set_test_mode all
			shift
			;;
		--packet)
			set_test_mode packet
			shift
			;;
		--portable)
			set_test_mode portable
			shift
			;;
		--frr)
			set_test_mode frr
			shift
			;;
		--build-only)
			build_only=1
			shift
			;;
		--list)
			list_only=1
			shift
			;;
		--host)
			[[ "$#" -ge 2 ]] || fail "--host requires USER@HOST"
			host="$2"
			shift 2
			;;
		--remote-root)
			[[ "$#" -ge 2 ]] || fail "--remote-root requires a path"
			remote_root="$2"
			remote_root_set=1
			shift 2
			;;
		--frr-root)
			[[ "$#" -ge 2 ]] || fail "--frr-root requires a path"
			frr_root="$2"
			shift 2
			;;
		--jobs)
			[[ "$#" -ge 2 ]] || fail "--jobs requires a value"
			jobs="$2"
			shift 2
			;;
		--configure-first)
			configure_first=1
			shift
			;;
		--help)
			usage
			exit 0
			;;
		--)
			shift
			pytest_args+=("$@")
			break
			;;
		*)
			fail "unknown option: $1"
			;;
	esac
done

if [[ "$list_only" -eq 1 ]]; then
	[[ "$build_only" -eq 0 ]] || fail "--list cannot be combined with --build-only"
	[[ "$test_mode_set" -eq 0 ]] || fail "--list cannot be combined with a test selection"
	[[ -z "$host" ]] || fail "--list is local; omit --host"
	[[ "$remote_root_set" -eq 0 ]] || fail "--remote-root requires --host"
	[[ "${#pytest_args[@]}" -eq 0 ]] || fail "pytest arguments are not valid with --list"
	print_available_tests
	exit 0
fi

if [[ "$build_only" -eq 1 ]]; then
	[[ "$test_mode_set" -eq 0 ]] || fail "--build-only cannot be combined with a test selection"
	[[ "${#pytest_args[@]}" -eq 0 ]] || fail "pytest arguments are not valid with --build-only"
fi
if [[ -z "$host" && "$remote_root_set" -eq 1 ]]; then
	fail "--remote-root requires --host"
fi
if [[ -n "$jobs" ]]; then
	[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"
fi

if [[ -n "$host" ]]; then
	run_remote_uut
	exit 0
fi

if [[ -z "$frr_root" ]]; then
	if ! frr_root="$(infer_frr_root)"; then
		fail "FRR root could not be inferred; use --frr-root /path/to/frr"
	fi
else
	frr_root="$(resolve_existing_path "$frr_root")"
fi
[[ -f "$frr_root/bootstrap.sh" ]] || fail "not an FRR checkout root: $frr_root"

if [[ -z "$jobs" ]]; then
	jobs="$(default_jobs)"
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"

if [[ "$build_only" -eq 0 && ( "$test_mode" == "all" || "$test_mode" == "frr" ) ]]; then
	cleanup_stale_eigrpd_uut
fi

build_local_uut

if [[ "$build_only" -eq 1 ]]; then
	exit 0
fi

case "$test_mode" in
	all)
		run_portable_tests test/portable
		run_frr_tests
		;;
	packet)
		run_portable_tests test/portable/packet
		;;
	portable)
		run_portable_tests test/portable
		;;
	frr)
		run_frr_tests
		;;
	*)
		fail "unhandled test mode: $test_mode"
		;;
esac
