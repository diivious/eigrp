#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# EIGRP project test runner.

set -euo pipefail

script_name="$(basename "$0")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eigrp_root="$(cd "$script_dir/.." && pwd)"

run_portable=0
run_frr=0
install_tests=1
install_only=0
list_only=0
frr_root=""
pytest_args=()
portable_target="test/portable"

usage() {
	cat <<USAGE
usage: $script_name [options] [-- pytest-args]

primary options:
  -all                 Run portable tests and FRR-native tests when present.
  -packet              Run portable packet tests.
  -portable            Run all portable tests.
  -frr                 Run FRR-native tests from frr/tests/eigrpd.
  -install             Copy test/frr/ into frr/tests/eigrpd and exit.
  -list                List available EIGRP tests and exit.

path/options:
  -frr-root PATH       FRR checkout root. Default: ../frr or ~/devel/frr.
  -no-install          Do not copy test/frr/ before running FRR tests.
  -h, -help, --help    Show this help.

examples:
  tools/unittest.sh -portable
  tools/unittest.sh -packet -- -k checksum
  tools/unittest.sh -install -frr-root ~/devel/frr
  tools/unittest.sh -frr -frr-root ~/devel/frr

layout:
  portable tests: test/portable/
  FRR test source: test/frr/
  FRR install path: frr/tests/eigrpd/
USAGE
}

fail() {
	echo "error: $*" >&2
	exit 1
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
	for candidate in "$eigrp_root/../frr" "$HOME/devel/frr" "$eigrp_root/.."; do
		if [[ -d "$candidate/tests" && -f "$candidate/bootstrap.sh" ]]; then
			cd "$candidate" && pwd
			return 0
		fi
	done
	return 1
}

has_frr_tests() {
	local src="$1"
	[[ -d "$src" ]] || return 1
	find "$src" -mindepth 1 -type f ! -name 'README.md' | grep -q .
}

print_available_tests() {
	echo "portable tests:"
	if [[ -d "$eigrp_root/test/portable" ]]; then
		find "$eigrp_root/test/portable" -mindepth 1 -maxdepth 3 -type d ! -name __pycache__ | sed "s#^$eigrp_root/test/##" | sort
	else
		echo "  none"
	fi

	echo
	echo "FRR-native test payload:"
	if has_frr_tests "$eigrp_root/test/frr"; then
		find "$eigrp_root/test/frr" -mindepth 1 -maxdepth 2 -type f | sed "s#^$eigrp_root/test/frr/#  #" | sort
	else
		echo "  none"
	fi
}

install_frr_tests() {
	local root="$1"
	"$script_dir/install.sh" -frr-root "$root" -no-eigrpd
}

run_frr_tests() {
	local root="$1"
	local frr_test_dir="$root/tests/eigrpd"

	[[ -d "$frr_test_dir" ]] || fail "FRR EIGRP test directory not found: $frr_test_dir"
	if ! has_frr_tests "$frr_test_dir"; then
		echo "warning: no FRR-native EIGRP tests are installed yet at $frr_test_dir" >&2
		return 0
	fi

	require_command python3
	(
		cd "$root"
		python3 tests/runtests.py -v tests/eigrpd "${pytest_args[@]}"
	)
}

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		-all)
			run_portable=1
			run_frr=1
			shift
			;;
		-packet)
			run_portable=1
			portable_target="test/portable/packet"
			shift
			;;
		-portable)
			run_portable=1
			shift
			;;
		-frr)
			run_frr=1
			shift
			;;
		-install)
			install_only=1
			shift
			;;
		-list)
			list_only=1
			shift
			;;
		-frr-root)
			[[ "$#" -ge 2 ]] || fail "-frr-root requires a path"
			frr_root="$(resolve_existing_path "$2")"
			shift 2
			;;
		-no-install)
			install_tests=0
			shift
			;;
		-h|-help|--help)
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
	print_available_tests
	exit 0
fi

if [[ "$run_portable" -eq 0 && "$run_frr" -eq 0 && "$install_only" -eq 0 ]]; then
	usage
	exit 2
fi

if [[ "$run_frr" -eq 1 || "$install_only" -eq 1 ]]; then
	if [[ -z "$frr_root" ]]; then
		if ! frr_root="$(infer_frr_root)"; then
			fail "FRR root could not be inferred; use -frr-root /path/to/frr"
		fi
	fi
	[[ -f "$frr_root/bootstrap.sh" ]] || fail "not an FRR checkout root: $frr_root"
	if [[ "$install_tests" -eq 1 || "$install_only" -eq 1 ]]; then
		install_frr_tests "$frr_root"
	fi
fi

if [[ "$install_only" -eq 1 ]]; then
	exit 0
fi

if [[ "$run_portable" -eq 1 ]]; then
	require_command python3
	(
		cd "$eigrp_root"
		python3 -m pytest "$portable_target" "${pytest_args[@]}"
	)
fi

if [[ "$run_frr" -eq 1 ]]; then
	run_frr_tests "$frr_root"
fi
