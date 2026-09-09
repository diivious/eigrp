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
  --install            Stage eigrpd/ and test/frr/ into FRR only.
  --configure          Stage EIGRP, then run bootstrap.sh and configure in FRR.
  --build              Stage EIGRP, then run make. Default.
  --check              Stage EIGRP, then run make check.
  --all                Stage EIGRP, configure, build, and run make check.
  --clean              Run make clean in FRR.

options:
  --frr-root PATH       FRR checkout root. Default: ../frr or ~/devel/frr.
  --jobs N              make parallelism. Default: detected CPU count.
  --no-install          Do not stage EIGRP before configure/build/check/all.
  --configure-first     Run configure before --build or --check.
  --help                Show this help.

Only one action may be specified. Arguments after -- are passed to FRR configure.

examples:
  tools/frr.sh --smoke
  tools/frr.sh --configure --frr-root ~/devel/frr
  tools/frr.sh --build --jobs 8
  tools/frr.sh --all -- --enable-snmp
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
	"$script_dir/frr-install.sh" --frr-root "$frr_root"
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

if [[ "$configure_first" -eq 1 && "$action" != "build" && "$action" != "check" ]]; then
	fail "--configure-first is valid only with --build or --check"
fi
if [[ "${#extra_configure_args[@]}" -gt 0 ]]; then
	case "$action" in
		configure|all) ;;
		build|check)
			[[ "$configure_first" -eq 1 ]] || fail "configure arguments require --configure-first with --$action"
			;;
		*)
			fail "configure arguments are not valid with --$action"
			;;
	esac
fi

if [[ "$action" == "smoke" ]]; then
	make -C "$eigrp_root/build"
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
	clean)
		run_make clean
		;;
	*)
		fail "unhandled action: $action"
		;;
esac
