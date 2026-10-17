#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# OpenWrt development driver: setup, verification, and target configuration.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
eigrp_root="$(cd "$script_dir/.." && pwd -P)"
invocation_dir="$(pwd -P)"
devel_root="$eigrp_root/.."
openwrt_root=""
action=""
skip_packages=0
skip_clone=0
skip_feeds=0
selected_target=""

usage() {
	cat <<USAGE
usage: $(basename "$0") --setup|--verify|--config [options]

actions:
  --setup               Install host dependencies, clone OpenWrt if needed,
                        and initialize OpenWrt feeds.
  --verify              Check host, checkout, feeds and target state; change nothing.
  --config              Select a target, run OpenWrt defconfig and prereq.

options:
  --devel-root PATH     Parent directory of the OpenWrt checkout.
                        Default: parent of the EIGRP checkout.
  --openwrt-root PATH   Exact OpenWrt checkout location (overrides --devel-root).
  --skip-packages       Do not run apt-get/brew install.
  --skip-clone          Require an existing OpenWrt checkout.
  --skip-feeds          Do not download or install OpenWrt feeds.
  --target armsr/armv8  With --config, select generic ARMv8 EFI/QEMU without
                        launching interactive menuconfig.
  --help                Show this help.

The source baseline is https://github.com/diivious/openwrt (branch main).
Existing checkouts are never reset, pulled or recloned. OpenWrt builds must
run as an unprivileged user, on a case-sensitive filesystem without spaces.

On macOS, the OpenWrt buildroot is experimental. Use a case-sensitive APFS
volume for OpenWrt sources; native portable EIGRP builds/tests run normally.
Run --config (or --config --target armsr/armv8) after --setup.
See openwrt/README.md for setup and macOS environment guidance.
USAGE
}

fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
log() { printf '%s\n' "$*"; }

absolute_path() {
	case "$1" in
		/*) printf '%s\n' "$1" ;;
		*) printf '%s/%s\n' "$invocation_dir" "$1" ;;
	esac
}

set_action() {
	[[ -z "$action" ]] || fail "select exactly one action"
	action="$1"
}

while (($#)); do
	case "$1" in
		--setup|--verify|--config) set_action "${1#--}"; shift ;;
		--devel-root)
			(($# >= 2)) || fail "--devel-root needs a path"
			devel_root="$(absolute_path "$2")"; shift 2 ;;
		--openwrt-root)
			(($# >= 2)) || fail "--openwrt-root needs a path"
			openwrt_root="$(absolute_path "$2")"; shift 2 ;;
		--skip-packages) skip_packages=1; shift ;;
		--skip-clone) skip_clone=1; shift ;;
		--skip-feeds) skip_feeds=1; shift ;;
		--target)
			(($# >= 2)) || fail "--target needs a value"
			selected_target="$2"; shift 2 ;;
		--help) usage; exit 0 ;;
		--patch|--install|--uut|--build|--check)
			fail "$1 is not implemented in the setup phase; see openwrt/TASKS.md" ;;
		*) fail "unknown option: $1" ;;
	esac
done

[[ -n "$action" ]] || { usage >&2; exit 2; }
[[ "$(id -u)" != 0 ]] || fail "do not run OpenWrt operations as root"
[[ -z "$selected_target" || "$action" == config ]] || fail "--target requires --config"
if [[ "$action" != setup ]]; then
	((!skip_packages && !skip_clone && !skip_feeds)) ||
		fail "--skip-packages, --skip-clone and --skip-feeds apply only to --setup"
fi

if [[ -z "$openwrt_root" ]]; then
	openwrt_root="${devel_root%/}/openwrt"
fi
case "$openwrt_root" in
	*' '*) fail "OpenWrt path cannot contain spaces: $openwrt_root" ;;
esac

host="$(uname -s)"
case "$host" in
	Linux)
		[[ -r /etc/os-release ]] || fail "Linux distribution not detected"
		# /etc/os-release is OS-supplied system metadata, not user input.
		# shellcheck disable=SC1091
		. /etc/os-release
		case " ${ID:-} ${ID_LIKE:-} " in
			*debian*|*ubuntu*) ;;
			*) fail "automated setup supports Debian/Ubuntu only (found ${ID:-unknown})" ;;
		esac
		command -v apt-get >/dev/null 2>&1 || fail "apt-get was not found"
		;;
	Darwin)
		if [[ -r "$eigrp_root/openwrt/dev-env.sh" ]]; then
			# Ensure GNU userland tools supplied by Homebrew are available.
			# shellcheck source=../openwrt/dev-env.sh
			source "$eigrp_root/openwrt/dev-env.sh"
		fi
		;;
	*) fail "unsupported setup host: $host" ;;
esac

install_packages() {
	if ((skip_packages)); then
		log 'packages: skipped by request'
		return
	fi
	if [[ "$host" == Linux ]]; then
		command -v sudo >/dev/null 2>&1 || fail "sudo is required to install Debian packages"
		log 'packages: installing Debian/Ubuntu OpenWrt build prerequisites'
		sudo apt-get update
		sudo apt-get install -y \
			build-essential binutils bison bzip2 ca-certificates clang curl \
			diffutils file flex gawk gettext git libncurses-dev libssl-dev \
			make patch perl pkg-config python3 python3-setuptools rsync \
			subversion swig time unzip wget xz-utils zlib1g-dev
	else
		command -v xcode-select >/dev/null 2>&1 || fail "Xcode Command Line Tools are required"
		xcode-select -p >/dev/null 2>&1 || fail "run 'xcode-select --install' before setup"
		command -v brew >/dev/null 2>&1 || fail "install Homebrew before running --setup"
		log 'packages: installing Homebrew OpenWrt build prerequisites'
		brew install \
			bash bison bzip2 coreutils diffutils findutils flex gawk \
			gettext git gnu-getopt gnu-sed gnu-tar grep make \
			ncurses pkg-config python3 rsync subversion swig unzip \
			wget xz
		# Newly installed GNU tools can be keg-only.
		# shellcheck source=../openwrt/dev-env.sh
		source "$eigrp_root/openwrt/dev-env.sh"
	fi
}

verify_host_tools() {
	local missing=0 cmd make_version major minor
	for cmd in bash git make gcc g++ python3 perl rsync gawk flex bison unzip wget; do
		if ! command -v "$cmd" >/dev/null 2>&1; then
			log "missing executable: $cmd"
			missing=1
		fi
	done
	# OpenWrt expects GNU Make >= 4.1; Apple's /usr/bin/make is usually 3.81.
	if command -v make >/dev/null 2>&1; then
		make_version="$(make --version 2>/dev/null | sed -n '1s/^GNU Make //p')"
		if [[ "$make_version" =~ ^([0-9]+)\.([0-9]+) ]]; then
			major="${BASH_REMATCH[1]}"
			minor="${BASH_REMATCH[2]}"
			if ((major < 4 || (major == 4 && minor < 1))); then
				log "GNU Make 4.1+ is needed (found $make_version)"
				missing=1
			fi
		else
			log 'GNU Make 4.1+ is required'
			missing=1
		fi
	fi
	if [[ "$host" == Darwin ]]; then
		if ! command -v brew >/dev/null 2>&1; then
			log 'missing executable: brew (Homebrew)'
			missing=1
		fi
		if ! xcode-select -p >/dev/null 2>&1; then
			log 'missing: Xcode Command Line Tools'
			missing=1
		fi
	fi
	return "$missing"
}

case_sensitive() {
	local dir="$1" probe
	# Use an existing ancestor; the checkout itself may not exist yet.
	while [[ ! -d "$dir" ]]; do
		[[ "$dir" != / ]] || break
		dir="$(dirname "$dir")"
	done
	probe="$(mktemp -d "$dir/.openwrt-case.XXXXXXXX")" || return 1
	: > "$probe/OpenWrtProbe"
	if [[ -e "$probe/openwrtprobe" ]]; then
		rm -rf "$probe"
		return 1
	fi
	rm -rf "$probe"
}

verify_fs() {
	if case_sensitive "$openwrt_root"; then
		log 'filesystem: case-sensitive (OK)'
	else
		log "filesystem: NOT case-sensitive for $openwrt_root"
		if [[ "$host" == Darwin ]]; then
			log 'Use a case-sensitive APFS volume and --openwrt-root /Volumes/<volume>/openwrt.'
		fi
		return 1
	fi
}

verify_checkout() {
	local origin
	[[ -d "$openwrt_root/.git" || -f "$openwrt_root/.git" ]] || {
		log "checkout: absent at $openwrt_root"
		return 1
	}
	[[ -f "$openwrt_root/Makefile" && -x "$openwrt_root/scripts/feeds" ]] || {
		log "checkout: does not look like an OpenWrt buildroot: $openwrt_root"
		return 1
	}
	origin="$(git -C "$openwrt_root" remote get-url origin 2>/dev/null)" || {
		log 'checkout: missing origin remote'
		return 1
	}
	case "$origin" in
		https://github.com/diivious/openwrt|https://github.com/diivious/openwrt.git|git@github.com:diivious/openwrt|git@github.com:diivious/openwrt.git|ssh://git@github.com/diivious/openwrt|ssh://git@github.com/diivious/openwrt.git) ;;
		*) log "checkout: unexpected origin '$origin' (expected diivious/openwrt)"; return 1 ;;
	esac
	log "checkout: $openwrt_root ($(git -C "$openwrt_root" rev-parse --short HEAD))"
}

verify_config() {
	local config="$openwrt_root/.config" target
	if [[ ! -s "$config" ]]; then
		log 'target: not configured (run --config); setup checks can still pass'
		return 0
	fi
	target="$(sed -n -E 's/^CONFIG_TARGET_([a-z0-9]+_[a-z0-9]+)=y$/\1/p' "$config" | head -n 1)"
	if [[ -z "$target" ]]; then
		log "target: no selected target/subtarget in $config (run --config)"
		return 1
	fi
	log "target: ${target/_//} (configured)"
}

config_openwrt() {
	local config="$openwrt_root/.config" tmp backup
	if [[ -n "$selected_target" && "$selected_target" != armsr/armv8 ]]; then
		fail "--target currently supports armsr/armv8; use --config (interactive) for other targets"
	fi
	verify_host_tools || fail 'host prerequisites are incomplete'
	verify_fs || fail 'OpenWrt checkout must be on a case-sensitive filesystem'
	verify_checkout || fail 'OpenWrt checkout is missing or invalid'
	(
		cd "$openwrt_root"
		if [[ -z "$selected_target" ]]; then
			[[ -t 0 && -t 1 ]] || fail "interactive --config requires a terminal; use --config --target armsr/armv8"
			log 'config: opening OpenWrt menuconfig'
			make menuconfig
		else
			[[ -f target/linux/armsr/armv8/target.mk ]] ||
				fail 'armsr/armv8 target is missing from this OpenWrt checkout'
			log 'config: selecting OpenWrt armsr/armv8 Generic EFI target'
			# Preserve package/build settings; replace only target selection.
			tmp="$(mktemp "$openwrt_root/.config.new.XXXXXXXX")"
			if [[ -e "$config" ]]; then
				awk '!/^(# )?CONFIG_TARGET_[a-z0-9]/' "$config" > "$tmp"
			else
				: > "$tmp"
			fi
			printf '%s\n' \
				'CONFIG_TARGET_armsr=y' \
				'CONFIG_TARGET_armsr_armv8=y' \
				'CONFIG_TARGET_armsr_armv8_DEVICE_generic=y' >> "$tmp"
			if [[ -e "$config" ]]; then
				backup="$(mktemp "$openwrt_root/.config.before-openeigrp.XXXXXXXX")"
				cp -p "$config" "$backup"
				log "config: previous configuration saved as $backup"
			fi
			mv "$tmp" "$config"
		fi
		make defconfig
		make prereq
		if [[ "$selected_target" == armsr/armv8 ]]; then
			grep -qx 'CONFIG_TARGET_armsr_armv8=y' "$config" ||
				fail 'OpenWrt rejected the requested armsr/armv8 target'
			grep -qx 'CONFIG_TARGET_armsr_armv8_DEVICE_generic=y' "$config" ||
				fail 'OpenWrt did not enable the generic ARMv8 device'
		fi
	)
	log 'OpenWrt target configuration complete. Run --verify to inspect it.'
}

if [[ "$action" == verify ]]; then
	status=0
	verify_host_tools || status=1
	verify_fs || status=1
	verify_checkout || status=1
	if [[ -d "$openwrt_root" ]]; then
		if [[ -d "$openwrt_root/feeds" ]]; then
			log 'feeds: directory present'
		else
			log 'feeds: not initialized (run --setup)'
		fi
		verify_config || status=1
	fi
	exit "$status"
fi

if [[ "$action" == config ]]; then
	config_openwrt
	exit 0
fi

verify_fs || fail 'OpenWrt checkout must be on a case-sensitive filesystem'
install_packages
verify_host_tools || fail 'host prerequisites are incomplete'

if [[ ! -e "$openwrt_root" ]]; then
	((!skip_clone)) || fail "checkout is missing and --skip-clone was requested: $openwrt_root"
	mkdir -p "$(dirname "$openwrt_root")"
	log "clone: https://github.com/diivious/openwrt.git (main) -> $openwrt_root"
	git clone --branch main https://github.com/diivious/openwrt.git "$openwrt_root"
fi
verify_checkout || fail 'the existing checkout is not the requested OpenWrt source baseline'

if ((skip_feeds)); then
	log 'feeds: skipped by request'
else
	log 'feeds: update and install using the checkout feed configuration'
	(
		cd "$openwrt_root"
		./scripts/feeds update -a
		./scripts/feeds install -a
	)
fi

log 'OpenWrt host setup complete. Configure a target with: tools/openwrt.sh --config'
log 'Portable tests are available separately: make -C <eigrp-checkout> test'
