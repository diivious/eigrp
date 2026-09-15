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
install_first=1
configure_first=0
extra_configure_args=()

usage() {
	cat <<USAGE
usage: $script_name [action] [options] [-- configure-args]

actions:
  --smoke              Run the standalone compile-smoke harness only.
  --install            Assemble/stage EIGRP and apply required frr/patch/ changes.
  --patch              Apply only the managed FRR patches from frr/patch/.
  --configure          Stage EIGRP without patching, then bootstrap/configure FRR.
  --build              Stage EIGRP without patching, then run make. Default.
  --check              Stage EIGRP without patching, then run make check.
  --all                Stage EIGRP without patching, configure, build, and check.
  --uut                Apply managed patches/stage, build/install/restart, then run UUT.
  --clean              Run make clean in FRR.

options:
  --frr-root PATH       FRR checkout root. Default: ../frr or ~/devel/frr.
  --jobs N              make parallelism. Default: detected CPU count.
  --no-install          Do not stage EIGRP before configure/build/check/all/uut.
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
	# Build/test actions may refresh the projected EIGRP source tree, but they
	# must never modify FRR-wide source.  Managed patches are owned by
	# --install / --patch.
	"$script_dir/frr-install.sh" --frr-root "$frr_root" --no-patches
}

install_eigrp() {
	"$script_dir/frr-install.sh" --frr-root "$frr_root"
}

patch_frr() {
	"$script_dir/frr-install.sh" --frr-root "$frr_root" --no-eigrpd --no-tests
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
		--no-install)
			install_first=0
			shift
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
		install_eigrp
		;;
	patch)
		patch_frr
		;;
	configure)
		if [[ "$install_first" -eq 1 ]]; then
			stage_eigrp
		fi
		configure_frr
		;;
	build)
		if [[ "$install_first" -eq 1 ]]; then
			stage_eigrp
		fi
		if [[ "$configure_first" -eq 1 ]]; then
			configure_frr
		fi
		run_make
		;;
	check)
		if [[ "$install_first" -eq 1 ]]; then
			stage_eigrp
		fi
		if [[ "$configure_first" -eq 1 ]]; then
			configure_frr
		fi
		run_make check
		;;
	all)
		if [[ "$install_first" -eq 1 ]]; then
			stage_eigrp
		fi
		configure_frr
		run_make
		run_make check
		;;
	uut)
		if [[ "$install_first" -eq 1 ]]; then
			# UUT must exercise the daemon against the matching managed FRR
			# schema/integration patches.  Source-only staging can otherwise
			# combine new northbound callbacks with an older YANG model.
			install_eigrp
		fi
		if [[ "$configure_first" -eq 1 ]]; then
			configure_frr
		fi
		run_make
		activate_frr_uut
		"$script_dir/frr-named-uut.sh"
		;;
	clean)
		run_make clean
		;;
	*)
		fail "unhandled action: $action"
		;;
esac
