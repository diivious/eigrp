#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# FRR build helper for the EIGRP project.

set -euo pipefail

script_name="$(basename "$0")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eigrp_root="$(cd "$script_dir/.." && pwd)"

command="build"
frr_root=""
jobs=""
install_first=1
configure_first=0
extra_configure_args=()

usage() {
	cat <<USAGE
usage: $script_name [command] [options] [-- configure-args]

commands:
  smoke       Run the standalone compile-smoke harness only.
  install     Copy eigrpd/ and test/frr/ into FRR only.
  config      Run bootstrap.sh and configure in FRR.
  build       Install EIGRP into FRR, then run make. Default.
  check       Install EIGRP into FRR, then run make check.
  all         Install EIGRP into FRR, configure, build, and run make check.
  clean       Run make clean in FRR.

options:
  -frr-root PATH       FRR checkout root. Default: ../frr or ~/devel/frr.
  -j N                 make parallelism. Default: detected CPU count.
  -no-install          Do not stage EIGRP into FRR before build/check/all.
  -configure           Run configure before build/check.
  -h, -help, --help    Show this help.

examples:
  tools/build.sh smoke
  tools/build.sh config -frr-root ~/devel/frr
  tools/build.sh build -j 8
  tools/build.sh all -- --enable-snmp
USAGE
}

fail() {
	echo "error: $*" >&2
	exit 1
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

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		smoke|install|config|build|check|all|clean)
			command="$1"
			shift
			;;
		-frr-root)
			[[ "$#" -ge 2 ]] || fail "-frr-root requires a path"
			frr_root="$(resolve_existing_path "$2")"
			shift 2
			;;
		-j)
			[[ "$#" -ge 2 ]] || fail "-j requires a value"
			jobs="$2"
			shift 2
			;;
		-no-install)
			install_first=0
			shift
			;;
		-configure)
			configure_first=1
			shift
			;;
		-h|-help|--help)
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

case "$command" in
	smoke)
		make -C "$eigrp_root/build"
		exit 0
		;;
esac

if [[ -z "$frr_root" ]]; then
	if ! frr_root="$(infer_frr_root)"; then
		fail "FRR root could not be inferred; use -frr-root /path/to/frr"
	fi
fi

[[ -f "$frr_root/bootstrap.sh" ]] || fail "not an FRR checkout root: $frr_root"

case "$command" in
	install)
		"$script_dir/install.sh" -frr-root "$frr_root"
		;;
	config)
		if [[ "$install_first" -eq 1 ]]; then
			"$script_dir/install.sh" -frr-root "$frr_root"
		fi
		configure_frr
		;;
	build)
		if [[ "$install_first" -eq 1 ]]; then
			"$script_dir/install.sh" -frr-root "$frr_root"
		fi
		if [[ "$configure_first" -eq 1 ]]; then
			configure_frr
		fi
		run_make
		;;
	check)
		if [[ "$install_first" -eq 1 ]]; then
			"$script_dir/install.sh" -frr-root "$frr_root"
		fi
		if [[ "$configure_first" -eq 1 ]]; then
			configure_frr
		fi
		run_make check
		;;
	all)
		if [[ "$install_first" -eq 1 ]]; then
			"$script_dir/install.sh" -frr-root "$frr_root"
		fi
		configure_frr
		run_make
		run_make check
		;;
	clean)
		run_make clean
		;;
	*)
		fail "unhandled command: $command"
		;;
esac
