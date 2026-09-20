#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# FRR development driver for the EIGRP project.

set -euo pipefail

script_name="$(basename "$0")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eigrp_root="$(cd "$script_dir/.." && pwd)"

action="build"
action_set=0
frr_root=""
jobs=""
configure_first=0
extra_configure_args=()
eigrpd_uut_owned=0

usage() {
	cat <<USAGE
usage: $script_name [action] [options] [-- configure-args]

actions:
  --smoke              Run the standalone compile-smoke harness only.
  --install            Assemble/stage EIGRP source and FRR tests without patching.
  --patch              Apply only the managed FRR patches from frr/patch/.
  --configure          Stage EIGRP without patching, then bootstrap/configure FRR.
  --build              Stage EIGRP without patching, then run make. Default.
  --check              Stage EIGRP without patching, then run make check.
  --all                Stage EIGRP without patching, configure, build, and check.
  --uut                Stage, build/install/restart, then run UUT.
  --clean              Run make clean in FRR.

options:
  --frr-root PATH       FRR checkout root. Default: ../frr or ~/devel/frr.
  --jobs N              make parallelism. Default: detected CPU count.
  --configure-first     Run configure before --build, --check, or --uut.
  --help                Show this help.

Only one action may be specified. Arguments after -- are passed to FRR configure.

examples:
  tools/frr.sh --smoke
  tools/frr.sh --install --frr-root ~/devel/frr
  tools/frr.sh --patch --frr-root ~/devel/frr
  tools/frr.sh --configure --frr-root ~/devel/frr
  tools/frr.sh --build --jobs 8
  tools/frr.sh --all -- --enable-snmp
  tools/frr.sh --uut --frr-root ~/devel/frr
USAGE
}

fail() {
	echo "error: $*" >&2
	exit 1
}

set_action() {
	local new_action="$1"
	if [[ "$action_set" -eq 1 ]]; then
		fail "only one action may be specified"
	fi
	action="$new_action"
	action_set=1
}

is_abs_path() {
	case "$1" in
		/*) return 0 ;;
		*) return 1 ;;
	esac
}

resolve_existing_path() {
	local path="$1"
	if is_abs_path "$path"; then
		cd "$path" && pwd
	else
		cd "$PWD" && cd "$path" && pwd
	fi
}

infer_frr_root() {
	local candidate
	for candidate in "$eigrp_root/../frr" "$HOME/devel/frr"; do
		if [[ -d "$candidate" && -f "$candidate/bootstrap.sh" ]]; then
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

configure_frr() {
	(
		cd "$frr_root"
		./bootstrap.sh
		./configure \
			--sysconfdir=/etc \
			--localstatedir=/var \
			--sbindir=/usr/lib/frr \
			--enable-multipath=64 \
			--enable-user=frr \
			--enable-group=frr \
			--enable-vty-group=frrvty \
			--enable-configfile-mask=0640 \
			--enable-logfile-mask=0640 \
			--enable-fpm \
			--with-pkg-git-version \
			--with-pkg-extra-version=-dVs-EIGRP-v0 \
			"${extra_configure_args[@]}"
	)
}

run_make() {
	(
		cd "$frr_root"
		make -j "$jobs" "$@"
	)
}

stage_eigrp() {
	# Staging may refresh the projected EIGRP daemon/test trees, but it must
	# never modify FRR-wide source. Managed patches are applied only by
	# the explicit --patch action.
	"$script_dir/frr-install.sh" --frr-root "$frr_root" --no-patches
}

patch_frr() {
	"$script_dir/frr-install.sh" --frr-root "$frr_root" --no-eigrpd --no-tests
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

eigrpd_uut_live_pids() {
	local pid state

	while read -r pid; do
		[[ -n "$pid" ]] || continue
		state="$(ps -o stat= -p "$pid" 2>/dev/null | awk '{print $1}' || true)"
		[[ -n "$state" ]] || continue
		case "$state" in
			Z*) ;;
			*) printf '%s\n' "$pid" ;;
		esac
	done < <(pgrep -x eigrpd 2>/dev/null || true)
}

assert_eigrpd_uut_alive() {
	local live

	if sudo vtysh -d eigrpd -c 'show version' >/dev/null 2>&1; then
		live="$(eigrpd_uut_live_pids)"
		if [[ -n "$live" ]]; then
			echo "lifecycle: eigrpd remains alive after UUT configuration cleanup (PID(s): ${live//$'\n'/ })"
			return 0
		fi
	fi

	echo "error: UUT-owned eigrpd exited before harness shutdown" >&2
	echo "diagnostic: eigrpd process" >&2
	ps -C eigrpd -o pid=,ppid=,stat=,user=,etime=,args= >&2 2>/dev/null || true
	echo "diagnostic: recent FRR journal" >&2
	sudo journalctl -u frr -n 80 --no-pager >&2 2>/dev/null || true
	return 1
}

stop_eigrpd_uut() {
	local frrcommon="/usr/lib/frr/frrcommon.sh"
	local live i

	[[ "$eigrpd_uut_owned" -eq 1 ]] || return 0
	echo "stop: UUT-owned eigrpd"

	if [[ -r "$frrcommon" ]]; then
		sudo bash -s -- "$frrcommon" <<'EOS'
frrcommon="$1"
log_success_msg() { echo "$@"; }
log_warning_msg() { echo "$@" >&2; }
log_failure_msg() { echo "$@" >&2; }
. "$frrcommon"

if daemon_status eigrpd >/dev/null 2>&1; then
	daemon_stop eigrpd --quiet || true
fi
EOS
	else
		live="$(eigrpd_uut_live_pids)"
		if [[ -n "$live" ]]; then
			# pgrep returns one numeric PID per line; word splitting is intentional here.
			# shellcheck disable=SC2086
			sudo kill -TERM $live 2>/dev/null || true
		fi
	fi

	for i in $(seq 1 10); do
		live="$(eigrpd_uut_live_pids)"
		if [[ -z "$live" ]]; then
			eigrpd_uut_owned=0
			echo "stop: eigrpd shutdown complete"
			return 0
		fi
		sleep 1
	done

	echo "stop: force-kill surviving UUT eigrpd PID(s): ${live//$'\n'/ }" >&2
	# pgrep returns one numeric PID per line; word splitting is intentional here.
	# shellcheck disable=SC2086
	sudo kill -KILL $live 2>/dev/null || true
	sleep 1
	live="$(eigrpd_uut_live_pids)"
	if [[ -n "$live" ]]; then
		echo "error: UUT-owned eigrpd survived harness shutdown: ${live//$'\n'/ }" >&2
		ps -C eigrpd -o pid=,ppid=,stat=,user=,etime=,args= >&2 2>/dev/null || true
		return 1
	fi

	eigrpd_uut_owned=0
	echo "stop: eigrpd shutdown complete after SIGKILL"
}

uut_eigrpd_exit_cleanup() {
	local rc=$?

	trap - EXIT
	if [[ "$eigrpd_uut_owned" -eq 1 ]]; then
		if ! stop_eigrpd_uut; then
			[[ "$rc" -ne 0 ]] || rc=1
		fi
	fi
	exit "$rc"
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

	eigrpd_uut_owned=1
	trap uut_eigrpd_exit_cleanup EXIT

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
activate_frr_uut() {
	echo "install: activating FRR build under test"
	sudo make -C "$frr_root" install
	echo "restart: frr"
	sudo systemctl restart frr
	start_eigrpd_uut
}

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		--smoke)
			set_action smoke
			shift
			;;
		--install)
			set_action install
			shift
			;;
		--patch)
			set_action patch
			shift
			;;
		--configure)
			set_action configure
			shift
			;;
		--build)
			set_action build
			shift
			;;
		--check)
			set_action check
			shift
			;;
		--all)
			set_action all
			shift
			;;
		--uut)
			set_action uut
			shift
			;;
		--clean)
			set_action clean
			shift
			;;
		--frr-root)
			[[ "$#" -ge 2 ]] || fail "--frr-root requires a path"
			frr_root="$(resolve_existing_path "$2")"
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
			extra_configure_args+=("$@")
			break
			;;
		*)
			fail "unknown option: $1"
			;;
	esac
done

if [[ -z "$jobs" ]]; then
	jobs="$(default_jobs)"
fi
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "--jobs must be a positive integer"

if [[ "$configure_first" -eq 1 && "$action" != "build" && "$action" != "check" && "$action" != "uut" ]]; then
	fail "--configure-first is valid only with --build, --check, or --uut"
fi
if [[ "${#extra_configure_args[@]}" -gt 0 ]]; then
	case "$action" in
		configure|all) ;;
		build|check|uut)
			[[ "$configure_first" -eq 1 ]] || fail "configure arguments require --configure-first with --$action"
			;;
		*)
			fail "configure arguments are not valid with --$action"
			;;
	esac
fi

if [[ "$action" == "smoke" ]]; then
	make -C "$eigrp_root/test/build"
	exit 0
fi

if [[ -z "$frr_root" ]]; then
	if ! frr_root="$(infer_frr_root)"; then
		fail "FRR root could not be inferred; use --frr-root /path/to/frr"
	fi
fi

[[ -f "$frr_root/bootstrap.sh" ]] || fail "not an FRR checkout root: $frr_root"

case "$action" in
	install)
		stage_eigrp
		;;
	patch)
		patch_frr
		;;
	configure)
		stage_eigrp
		configure_frr
		;;
	build)
		stage_eigrp
		if [[ "$configure_first" -eq 1 ]]; then
			configure_frr
		fi
		run_make
		;;
	check)
		stage_eigrp
		if [[ "$configure_first" -eq 1 ]]; then
			configure_frr
		fi
		run_make check
		;;
	all)
		stage_eigrp
		configure_frr
		run_make
		run_make check
		;;
	uut)
		cleanup_stale_eigrpd_uut
		stage_eigrp
		if [[ "$configure_first" -eq 1 ]]; then
			configure_frr
		fi
		run_make
		activate_frr_uut
		"$script_dir/frr-named-uut.sh"
		assert_eigrpd_uut_alive
		stop_eigrpd_uut
		trap - EXIT
		;;
	clean)
		run_make clean
		;;
	*)
		fail "unhandled action: $action"
		;;
esac
