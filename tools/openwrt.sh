#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# OpenWrt development driver: setup, install, config, clean, build and tests.

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
menuconfig=0
minimal_feeds=0
build_jobs=2
build_jobs_specified=0
qemu_image=""
qemu_firmware=""
qemu_log_dir=""

usage() {
	cat <<USAGE
usage: $(basename "$0") --setup|--install|--config|--build|--clean|--test|--uut|--kernel|--image-build|--qemu|--verify [options]

actions:
  --setup               Install host build and QEMU UUT dependencies, clone
                        OpenWrt if needed, and initialize OpenWrt feeds.
  --install             Register the local OpenEIGRP package feed safely.
  --verify              Check host, checkout, feeds and target state; change nothing.
  --config              Select/preserve target and OpenEIGRP package; run defconfig/prereq.
  --build               Build OpenWrt host tools, target toolchain, then OpenEIGRP.
  --clean               Clean only the OpenEIGRP package build outputs.
  --test                Run host-neutral component, configuration and recipe tests.
  --uut                 Run all nonprivileged tests, including Linux interface,
                        packet, kernel RIB and daemon unit-under-test scenarios.
  --kernel              Run the isolated privileged kernel RIB test (sudo is
                        used only for the disposable network namespace).
  --image-build         Build an OpenWrt disk image suitable for two-node QEMU
                        UUT (enables ext4, iproute2 and dummy interface support).
  --qemu                Boot two disposable OpenWrt nodes and require EIGRP
                        neighbor Up and exchanged Linux kernel routes.
                        Linux setup installs qemu-system-arm and qemu-system-x86.

options:
  --devel-root PATH     Parent directory of the OpenWrt checkout.
                        Default: parent of the EIGRP checkout.
  --openwrt-root PATH   Exact OpenWrt checkout location (overrides --devel-root).
  --skip-packages       Do not run apt-get/brew install.
  --skip-clone          Require an existing OpenWrt checkout.
  --skip-feeds          Do not download or install OpenWrt feeds.
  --target BOARD/SUB    With --config, override automatic target selection.
                        Generic defaults: armsr/armv8 or x86/64.
  --menuconfig          With --config, launch interactive OpenWrt menuconfig
                        instead of automatic target selection.
  --jobs N              With --build or --image-build, jobs (default: 2).
                        Direct test scripts remain available in openwrt/test/.
  --image PATH          With --qemu, OpenWrt combined ext4 disk (.img[.gz]);
                        defaults to image from selected build target.
  --firmware PATH       With --qemu on ARM64, QEMU U-Boot firmware .bin;
                        defaults to buildroot u-boot-qemu_armv8/u-boot.bin.
  --log-dir PATH        With --qemu, preserve QEMU logs at this directory.
  --minimal-feeds       With --config, remove installed feed package links
                        before configuring (feed source checkouts stay intact).
                        Explicit recovery for broken optional-feed Kconfig.
  --help                Show this help.

The source baseline is https://github.com/diivious/openwrt (branch main).
Existing checkouts are never reset, pulled or recloned. OpenWrt builds must
run as an unprivileged user, on a case-sensitive filesystem without spaces.

The driver orchestrates the same independent scripts under openwrt/test/.
On macOS, the OpenWrt buildroot is experimental. Use a case-sensitive APFS
volume for OpenWrt sources; native portable EIGRP builds/tests run normally.
Run --install and --config after --setup, then --build. Existing target
selections are preserved; the host CPU supplies an initial generic default.
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
		--setup|--verify|--config|--install|--build|--clean|--test|--uut|--kernel|--image-build|--qemu) set_action "${1#--}"; shift ;;
		--devel-root)
			(($# >= 2)) || fail "--devel-root needs a path"
			devel_root="$(absolute_path "$2")"; shift 2 ;;
		--openwrt-root)
			(($# >= 2)) || fail "--openwrt-root needs a path"
			openwrt_root="$(absolute_path "$2")"; shift 2 ;;
		--skip-packages) skip_packages=1; shift ;;
		--skip-clone) skip_clone=1; shift ;;
		--skip-feeds) skip_feeds=1; shift ;;
		--menuconfig) menuconfig=1; shift ;;
		--minimal-feeds) minimal_feeds=1; shift ;;
		--jobs)
			(($# >= 2)) || fail "--jobs needs a positive integer"
			[[ "$2" =~ ^[1-9][0-9]*$ ]] || fail "--jobs needs a positive integer"
			((${#2} <= 3 && 10#$2 <= 256)) || fail "--jobs must be 1..256"
			build_jobs="$2"; build_jobs_specified=1; shift 2 ;;
		--target)
			(($# >= 2)) || fail "--target needs a value"
			selected_target="$2"; shift 2 ;;
		--image)
			(($# >= 2)) || fail "--image needs a path"
			qemu_image="$(absolute_path "$2")"; shift 2 ;;
		--firmware)
			(($# >= 2)) || fail "--firmware needs a path"
			qemu_firmware="$(absolute_path "$2")"; shift 2 ;;
		--log-dir)
			(($# >= 2)) || fail "--log-dir needs a path"
			qemu_log_dir="$(absolute_path "$2")"; shift 2 ;;
		--help) usage; exit 0 ;;
		--patch|--check)
			fail "$1 is not implemented; see openwrt/TASKS.md" ;;
		*) fail "unknown option: $1" ;;
	esac
done

[[ -n "$action" ]] || { usage >&2; exit 2; }
[[ "$(id -u)" != 0 ]] || fail "do not run OpenWrt operations as root"
[[ -z "$selected_target" || "$action" == config ]] || fail "--target requires --config"
(( !menuconfig )) || [[ "$action" == config ]] || fail "--menuconfig requires --config"
(( !minimal_feeds )) || [[ "$action" == config ]] || fail "--minimal-feeds requires --config"
(( !build_jobs_specified )) || [[ "$action" == build || "$action" == image-build ]] || fail "--jobs requires --build or --image-build"
[[ -z "$qemu_image$qemu_firmware$qemu_log_dir" || "$action" == qemu ]] || fail "--image, --firmware and --log-dir require --qemu"
[[ -z "$selected_target" ]] || (( !menuconfig )) || fail "--target and --menuconfig cannot be combined"
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
			diffutils file flex gawk gettext git iproute2 libncurses-dev libssl-dev \
			make patch perl pkg-config python3 python3-setuptools qemu-system-arm \
			qemu-system-x86 rsync subversion swig time unzip wget xz-utils zlib1g-dev
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

# The feed lives in this checkout; OpenWrt is only a consumer of its recipe.
feed_root="$eigrp_root/openwrt/feed"
package_recipe="$feed_root/openeigrp/Makefile"
package_link="${openwrt_root%/}/package/feeds/openeigrp/openeigrp"

verify_package_feed() {
	local registered target
	if [[ ! -e "$package_recipe" ]]; then
		log "package: OpenEIGRP recipe absent: $package_recipe"
		return 1
	fi
	if [[ ! -L "$package_link" || ! -f "$package_link/Makefile" ]]; then
		if [[ -L "$package_link" ]]; then
			target="$(readlink "$package_link")"
			log "package: stale OpenEIGRP package link: $package_link -> $target (run --install to repair)"
		else
			log 'package: not installed (run --install)'
		fi
		return 1
	fi
	registered="$(cd "$(dirname "$package_link")" && cd -P openeigrp && pwd -P)" || return 1
	if [[ "$registered" != "$(cd "$feed_root/openeigrp" && pwd -P)" ]]; then
		log "package: unexpected package source at $registered"
		return 1
	fi
	log "package: OpenEIGRP feed registered ($registered)"
}

# A checkout renamed from eigrp/ to openeigrp/ may leave the src-link and
# package registration pointing at an absent sibling checkout. Never take over
# an existing checkout, a foreign feed path, or a developer-owned file.
relocated_feed_source() {
	local old="$1" old_root
	[[ "$old" == /* && "$old" != "$feed_root" && ! -e "$old" && ! -L "$old" ]] || return 1
	[[ "$old" == */openwrt/feed ]] || return 1
	old_root="${old%/openwrt/feed}"
	[[ "$(dirname "$old_root")" == "$(dirname "$eigrp_root")" ]] || return 1
	case "$(basename "$old_root")" in eigrp|openeigrp) ;; *) return 1 ;; esac
}

# Only remove broken links owned by the OpenEIGRP feed registration. A live
# symlink or a regular file/directory is never overwritten by this repair.
prepare_broken_feed_link() {
	local path="$1"
	if [[ -L "$path" ]]; then
		if [[ -e "$path" ]]; then
			fail "refusing to overwrite live feed link: $path -> $(readlink "$path")"
		fi
		log "feeds: removing broken symlink $path -> $(readlink "$path")"
		rm -- "$path" || fail "could not remove broken feed link: $path"
	elif [[ -e "$path" ]]; then
		fail "refusing to replace non-symlink feed path: $path"
	fi
}

# A normal package install should not silently change configuration. Only
# --config changes OpenWrt's .config, including the package selection.
config_package_enable() {
	local config=".config" tmp
	if [[ ! -L "$package_link" || ! -f "$package_link/Makefile" ]]; then
		log 'config: OpenEIGRP feed is not registered; run --install first'
		return 0
	fi
	if grep -qx 'CONFIG_PACKAGE_openeigrp=y' "$config" 2>/dev/null; then
		log 'config: OpenEIGRP core package already selected'
		return 0
	fi
	tmp="$(mktemp "$openwrt_root/.config.package.XXXXXXXX")"
	if [[ -f "$config" ]]; then
		awk '!/^CONFIG_PACKAGE_openeigrp=|^# CONFIG_PACKAGE_openeigrp is not set$/' "$config" > "$tmp"
	fi
	printf 'CONFIG_PACKAGE_openeigrp=y\n' >> "$tmp"
	mv "$tmp" "$config"
	log 'config: selected CONFIG_PACKAGE_openeigrp=y (core and OpenWrt host libraries)'
}

# Register the local feed, or repair a proven relocation from an absent sibling
# checkout. An existing feed from another checkout is always a hard conflict.
install_openwrt() {
	local feed_conf="$openwrt_root/feeds.conf" existing old_source backup tmp
	local feed_link="$openwrt_root/feeds/openeigrp"
	[[ ! -L "$feed_conf" ]] || fail 'refusing symlinked feeds.conf'
	verify_fs || fail 'OpenWrt checkout must be on a case-sensitive filesystem'
	verify_checkout || fail 'OpenWrt checkout is missing or invalid'
	[[ -f "$package_recipe" && -f "$eigrp_root/eigrp/code/Makefile" ]] ||
		fail 'OpenEIGRP core or package recipe missing from checkout'
	case "$feed_root" in *' '*) fail "OpenEIGRP feed path contains spaces: $feed_root" ;; esac
	if [[ ! -f "$feed_conf" ]]; then
		[[ ! -e "$feed_conf" && ! -L "$feed_conf" ]] || fail 'feeds.conf is not a regular file'
		[[ -f "$openwrt_root/feeds.conf.default" ]] || fail 'feeds.conf.default is missing'
		cp "$openwrt_root/feeds.conf.default" "$feed_conf"
		log 'feeds: created feeds.conf from feeds.conf.default'
	fi
	# Preserve the existing source of truth and reject duplicates, malformed
	# declarations, or unrelated registrations before changing anything.
	existing="$(awk '$1 !~ /^#/ && $1 ~ /^src-/ && $2 == "openeigrp" {print $1 " " $3; if (NF != 3) print "INVALID"}' "$feed_conf")"
	[[ "$existing" != *$'\n'* && "$existing" != *INVALID* ]] ||
		fail "duplicate or malformed OpenEIGRP feed entry in $feed_conf"
	if [[ -n "$existing" && "$existing" != "src-link $feed_root" ]]; then
		[[ "$existing" == 'src-link '* ]] || fail "conflicting OpenEIGRP feed entry: $existing"
		old_source="${existing#src-link }"
		relocated_feed_source "$old_source" ||
			fail "conflicting OpenEIGRP feed entry: $existing (existing checkout or foreign source)"
		# An existing, functioning package registration must not be silently
		# replaced even when the old src-link points to an absent directory.
		if [[ -e "$package_link" || -e "$feed_link" ]]; then
			fail 'OpenEIGRP feed has live paths; refusing automatic relocation'
		fi
		[[ ! -e "$package_link" || -L "$package_link" ]] ||
			fail 'non-symlink OpenEIGRP package registration conflicts with this checkout'
		backup="$(mktemp "$openwrt_root/feeds.conf.before-openeigrp-relocation.XXXXXXXX")"
		cp -p "$feed_conf" "$backup" || fail 'cannot back up feeds.conf before relocation'
		tmp="$(mktemp "$openwrt_root/feeds.conf.relocation.XXXXXXXX")"
		awk -v replacement="src-link openeigrp $feed_root" '
			$1 == "src-link" && $2 == "openeigrp" {print replacement; next}
			{print}
		' "$feed_conf" > "$tmp" || fail 'cannot prepare relocated feed declaration'
		chmod --reference="$feed_conf" "$tmp"
		mv -- "$tmp" "$feed_conf" || fail 'cannot update feeds.conf'
		log "feeds: repaired relocated OpenEIGRP src-link $old_source -> $feed_root"
		log "feeds: previous feeds.conf saved as $backup"
		prepare_broken_feed_link "$package_link"
		prepare_broken_feed_link "$feed_link"
	elif [[ -z "$existing" ]]; then
		[[ ! -e "$package_link" && ! -L "$package_link" ]] ||
			fail 'unregistered OpenEIGRP package path already exists'
		[[ ! -e "$feed_link" && ! -L "$feed_link" ]] ||
			fail 'unregistered OpenEIGRP feed path already exists'
		printf '\nsrc-link openeigrp %s\n' "$feed_root" >> "$feed_conf"
		log "feeds: registered src-link openeigrp $feed_root"
	else
		# Idempotent repair when a previous registration was interrupted:
		# discard only broken symlinks, never a functioning foreign link.
		if [[ -L "$package_link" && ! -e "$package_link" ]]; then
			prepare_broken_feed_link "$package_link"
		elif [[ -e "$package_link" ]]; then
			verify_package_feed || fail 'existing OpenEIGRP package registration conflicts with this checkout'
		else
			[[ ! -e "$package_link" ]] || fail 'unexpected package registration'
		fi
		if [[ -L "$feed_link" && ! -e "$feed_link" ]]; then
			prepare_broken_feed_link "$feed_link"
		fi
		log 'feeds: OpenEIGRP src-link already registered'
	fi
	(
		cd "$openwrt_root"
		./scripts/feeds update openeigrp
		./scripts/feeds install -p openeigrp openeigrp
	) || fail 'OpenWrt feed registration failed'
	verify_package_feed || fail 'OpenEIGRP feed registration did not produce the expected package symlink'
	log 'OpenEIGRP package installed into OpenWrt feed index. Run --config to select it.'
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
	log "target: ${target/_//} (selected; Kconfig health is checked by --config)"
}

# The selected OpenWrt target is not the build host's CPU. We use the CPU
# solely as a convenient default when no target has been configured yet.
configured_target() {
	local config="$1"
	[[ -s "$config" ]] || return 0
	sed -n -E 's/^CONFIG_TARGET_([a-z0-9]+)_([a-z0-9]+)=y$/\1\/\2/p' "$config" | head -n 1
}

host_default_target() {
	case "$(uname -m)" in
		arm64|aarch64) printf '%s\n' armsr/armv8 ;;
		x86_64|amd64) printf '%s\n' x86/64 ;;
		*) return 1 ;;
	esac
}

# QEMU is a Linux-only live-UUT dependency, not a prerequisite for ordinary
# package builds or native host tests. Debian/Ubuntu split the system
# emulators into these packages; aarch64 lives in qemu-system-arm.
qemu_requirements() {
	local binary package
	case "$1" in
		armsr/armv8) binary=qemu-system-aarch64; package=qemu-system-arm ;;
		x86/64) binary=qemu-system-x86_64; package=qemu-system-x86 ;;
		*) log "qemu: unsupported target $1" >&2; return 1 ;;
	esac
	if command -v "$binary" >/dev/null 2>&1; then
		log "qemu: $binary available"
		return 0
	fi
	log "qemu: $binary is missing (Debian/Ubuntu: sudo apt-get install $package)" >&2
	return 1
}

# Some OpenWrt Kconfig invocations emit fatal dependency diagnostics but
# return success via their make wrappers. Preserve their output and exit on
# either a nonzero return code or an explicit Kconfig error diagnostic.
run_config_make() {
	local step="$1" transcript
	transcript="$(mktemp "${TMPDIR:-/tmp}/openeigrp-owrt-config.XXXXXXXX")"
	if ! (make "$step" 2>&1 | tee "$transcript"); then
		log "error: OpenWrt make $step failed" >&2
		rm -f "$transcript"
		return 1
	fi
	if grep -Eiq '(^|[[:space:]:])error:[[:space:]]|recursive dependency detected' "$transcript"; then
		log "error: OpenWrt make $step reported Kconfig errors despite a successful exit status" >&2
		log 'error: configuration was NOT validated. Optional package feeds may be responsible.' >&2
		log 'error: review those feed dependencies, or use --config --minimal-feeds for a base-only validation.' >&2
		rm -f "$transcript"
		return 1
	fi
	rm -f "$transcript"
}

# Only explicitly requested. OpenWrt's feeds uninstall -a removes symlinked
# package registrations, not the downloaded feed sources. Do not permit it
# to remove arbitrary developer-owned regular files under package/feeds.
remove_optional_feed_packages() {
	local unexpected backup
	if [[ -d package/feeds ]]; then
		unexpected="$(find package/feeds -type f -print -quit)"
		[[ -z "$unexpected" ]] || fail "refusing to remove developer-owned feed file: $unexpected"
	fi
	if [[ -f .config ]]; then
		backup="$(mktemp "$openwrt_root/.config.before-openeigrp-feed-reset.XXXXXXXX")"
		cp -p .config "$backup"
		log "config: previous configuration saved as $backup"
	fi
	log 'config: removing installed feed package links; feed sources remain unchanged'
	./scripts/feeds uninstall -a
}

config_openwrt() {
	local config="$openwrt_root/.config" tmp backup target existing board subtarget
	verify_host_tools || fail 'host prerequisites are incomplete'
	verify_fs || fail 'OpenWrt checkout must be on a case-sensitive filesystem'
	verify_checkout || fail 'OpenWrt checkout is missing or invalid'
	(
		cd "$openwrt_root"
		if ((minimal_feeds)); then
			remove_optional_feed_packages
		fi
		if ((menuconfig)); then
			[[ -t 0 && -t 1 ]] || fail '--menuconfig requires an interactive terminal'
			log 'config: opening OpenWrt menuconfig'
			make menuconfig
		else
			existing="$(configured_target "$config")"
			if [[ -n "$selected_target" ]]; then
				target="$selected_target"
				log "config: explicit target override: $target"
			elif [[ -n "$existing" && -f "target/linux/$existing/target.mk" ]]; then
				target="$existing"
				log "config: preserving existing target: $target"
			else
				target="$(host_default_target)" ||
					fail "cannot infer target from $(uname -s)/$(uname -m); use --target or --menuconfig"
				log "config: auto-selected generic target $target from host $(uname -s)/$(uname -m)"
			fi
			# A device-specific target can be retained from an existing .config.
			# Automated target overrides are limited to known generic profiles.
			if [[ "$target" != "$existing" ]]; then
				case "$target" in
					armsr/armv8|x86/64) ;;
					*) fail "automatic override supports armsr/armv8 or x86/64; use --menuconfig for a device-specific target" ;;
				esac
			fi
			board="${target%%/*}"
			subtarget="${target#*/}"
			[[ -f "target/linux/$board/$subtarget/target.mk" ]] ||
				fail "target missing from OpenWrt checkout: $target"
			if [[ "$existing" != "$target" ]]; then
				# A deliberate target switch preserves a backup and keeps all
				# non-target package configuration values.
				tmp="$(mktemp "$openwrt_root/.config.new.XXXXXXXX")"
				if [[ -e "$config" ]]; then
					awk '!/^(# )?CONFIG_TARGET_[a-z0-9]/' "$config" > "$tmp"
					backup="$(mktemp "$openwrt_root/.config.before-openeigrp.XXXXXXXX")"
					cp -p "$config" "$backup"
					log "config: previous configuration saved as $backup"
				else
					: > "$tmp"
				fi
				printf '%s\n' \
					"CONFIG_TARGET_${board}=y" \
					"CONFIG_TARGET_${board}_${subtarget}=y" \
					"CONFIG_TARGET_${board}_${subtarget}_DEVICE_generic=y" >> "$tmp"
				mv "$tmp" "$config"
			fi
		fi
		config_package_enable
		run_config_make defconfig || fail 'OpenWrt defconfig failed validation'
		run_config_make prereq || fail 'OpenWrt prerequisite validation failed'
		if ((!menuconfig)); then
			grep -qx "CONFIG_TARGET_${board}_${subtarget}=y" "$config" ||
				fail "OpenWrt rejected the requested target: $target"
			if [[ "$target" == armsr/armv8 || "$target" == x86/64 ]]; then
				grep -qx "CONFIG_TARGET_${board}_${subtarget}_DEVICE_generic=y" "$config" ||
					fail "OpenWrt did not enable the generic device for $target"
			fi
		fi
	)
	log 'OpenWrt target configuration complete. Run --verify to inspect it.'
}

# Build stages run in the same OpenWrt checkout selected by --config.
# OpenWrt's stamps make a second invocation incremental. Do not silently
# alter .config, install packages/feeds, update source checkouts or patch the OS.
build_openwrt() {
	local config="$openwrt_root/.config" target target_board target_subtarget stage rc
	verify_host_tools || fail 'host prerequisites are incomplete; run --setup'
	verify_fs || fail 'OpenWrt checkout must be on a case-sensitive filesystem'
	verify_checkout || fail 'OpenWrt checkout is missing or invalid'
	verify_package_feed || fail 'OpenEIGRP package is not installed; run --install'
	[[ -f "$config" ]] || fail 'OpenWrt target is not configured; run --config'
	target="$(configured_target "$config")"
	[[ -n "$target" ]] || fail 'OpenWrt target/subtarget missing; run --config'
	target_board="${target%%/*}"
	target_subtarget="${target#*/}"
	[[ -f "$openwrt_root/target/linux/$target_board/$target_subtarget/target.mk" ]] ||
		fail "selected OpenWrt target does not exist: $target"
	grep -qx 'CONFIG_PACKAGE_openeigrp=y' "$config" ||
		fail 'OpenEIGRP package is not selected; run --config'

	log "build: OpenWrt target $target, $build_jobs parallel job(s)"
	log "build: OpenWrt $(git -C "$openwrt_root" rev-parse --short HEAD), EIGRP $(git -C "$eigrp_root" rev-parse --short HEAD 2>/dev/null || printf 'local checkout')"
	for stage in tools/install toolchain/install package/openeigrp/compile; do
		log "build: starting $stage"
		if (cd "$openwrt_root" && make "-j${build_jobs}" "$stage" V=s); then
			log "build: completed $stage"
		else
			rc=$?
			fail "OpenWrt $stage failed (exit $rc); later stages were not started"
		fi
		if [[ "$stage" == tools/install ]]; then
			# These are the exact host executables needed in the Aurora failure.
			# Check immediately so failures point to the responsible stage.
			for tool in openssl libdeflate-gzip; do
				[[ -x "$openwrt_root/staging_dir/host/bin/$tool" ]] ||
					fail "tools/install completed but staging_dir/host/bin/$tool is missing"
			done
		fi
	done
	log 'OpenEIGRP OpenWrt package and daemon build complete.'
	log 'Package output (if produced): OpenWrt bin/packages/<target-arch>/*/openeigrp*'
}

# Test scripts remain independent. Driver is the single discoverable entry
# point; there is exactly one implementation of each test under openwrt/test/.
run_openwrt_test() {
	local filename="$1"
	local path="$eigrp_root/openwrt/test/$filename"
	[[ -f "$path" ]] || fail "test missing: $path"
	log "test: starting $filename"
	if bash "$path"; then
		log "test: PASS $filename"
	else
		fail "test FAILED: $filename"
	fi
}

run_openwrt_tests() {
	local filename
	local -a components=(setup-test.sh runtime-test.sh config-test.sh qemu-test.sh)
	if [[ "$host" == Linux ]]; then
		components+=(recipe-test.sh image-test.sh)
	fi
	for filename in "${components[@]}"; do
		run_openwrt_test "$filename"
	done
	if [[ "$action" == uut ]]; then
		if [[ "$host" == Linux ]]; then
			for filename in interface-uut.sh packet-uut.sh rib-uut.sh daemon-uut.sh; do
				run_openwrt_test "$filename"
			done
		else
			log 'uut: Linux-specific interface, packet and RIB scenarios skipped on macOS'
		fi
	fi
	log "$action: PASS (all applicable tests)"
}

# OpenWrt scripts/config/ is a DIRECTORY containing Kconfig sources, not
# Linux kernel's scripts/config executable. Follow the existing --config
# implementation: edit .config atomically and let OpenWrt make defconfig
# resolve dependencies. Preserve unrelated settings and the previous config.
image_profile_enable() {
	local config="$openwrt_root/.config" target="$1" symbol selected=1 tmp backup
	local -a flags=(TARGET_ROOTFS_EXT4FS TARGET_IMAGES_GZIP PACKAGE_kmod-dummy PACKAGE_ip-full)
	[[ "$target" != x86/64 ]] || flags+=(GRUB_IMAGES)
	[[ -f "$config" && ! -L "$config" ]] || fail 'refusing nonregular or symlinked OpenWrt .config'

	for symbol in "${flags[@]}"; do
		if ! grep -qx "CONFIG_${symbol}=y" "$config"; then
			selected=0
			break
		fi
	done
	if ((selected)); then
		log 'image-build: required image options already selected'
		return 0
	fi

	tmp="$(mktemp "$openwrt_root/.config.image.XXXXXXXX")"
	# Replace both enabled/disabled forms, without duplicates or an unrelated
	# configuration change. The name test uses exact keys, not prefix matches.
	awk -v requested="${flags[*]}" '
		BEGIN {
			n = split(requested, names, " ")
			for (i = 1; i <= n; i++) enabled["CONFIG_" names[i]] = 1
		}
		/^CONFIG_[^=]+=/ {
			key = $0
			sub(/=.*/, "", key)
			if (key in enabled) next
		}
		/^# CONFIG_.* is not set$/ {
			key = $0
			sub(/^# /, "", key)
			sub(/ is not set$/, "", key)
			if (key in enabled) next
		}
		{ print }
	' "$config" > "$tmp" || { rm -f "$tmp"; fail 'could not prepare OpenWrt image configuration'; }
	printf 'CONFIG_%s=y\n' "${flags[@]}" >> "$tmp"
	backup="$(mktemp "$openwrt_root/.config.before-openeigrp-image.XXXXXXXX")"
	cp -p "$config" "$backup" || { rm -f "$tmp" "$backup"; fail 'could not back up OpenWrt configuration'; }
	chmod --reference="$config" "$tmp"
	mv "$tmp" "$config" || fail 'could not apply OpenWrt image configuration'
	log "image-build: previous configuration saved as $backup"
}

# Full firmware images are an explicit action, not an unexpected side effect
# of --build/--config. Only local .config changes; never rewrite host source.
image_build_openwrt() {
	local config="$openwrt_root/.config" target symbol
	[[ "$host" == Linux ]] || fail '--image-build requires Linux'
	verify_host_tools || fail 'host prerequisites missing; run --setup'
	verify_fs || fail 'OpenWrt checkout must be on a case-sensitive filesystem'
	verify_checkout || fail 'OpenWrt checkout is missing or invalid'
	verify_package_feed || fail 'OpenEIGRP feed is missing; run --install'
	[[ -f "$config" ]] || fail 'configure the target before --image-build'
	target="$(configured_target "$config")"
	case "$target" in
		armsr/armv8|x86/64) ;;
		*) fail "QEMU image profile supports only armsr/armv8 or x86/64, got $target" ;;
	esac
	grep -qx 'CONFIG_PACKAGE_openeigrp=y' "$config" || fail 'select openeigrp first via --config'
	log "image-build: select ext4 disk and kernel dummy interface for $target"
	image_profile_enable "$target"
	(
		cd "$openwrt_root"
		run_config_make defconfig || fail 'OpenWrt image profile Kconfig validation failed'
	)
	for symbol in TARGET_ROOTFS_EXT4FS TARGET_IMAGES_GZIP PACKAGE_kmod-dummy PACKAGE_ip-full; do
		grep -qx "CONFIG_${symbol}=y" "$config" ||
			fail "OpenWrt rejected required image option CONFIG_${symbol}=y"
	done
	if [[ "$target" == x86/64 ]]; then
		grep -qx 'CONFIG_GRUB_IMAGES=y' "$config" ||
			fail 'OpenWrt rejected required image option CONFIG_GRUB_IMAGES=y'
	fi
	[[ "$(configured_target "$config")" == "$target" ]] ||
		fail "OpenWrt image defconfig changed selected target (expected $target)"
	grep -qx 'CONFIG_PACKAGE_openeigrp=y' "$config" ||
		fail 'OpenWrt image defconfig deselected openeigrp'
	log 'image-build: building complete OpenWrt firmware image (incremental toolchain reused)'
	(cd "$openwrt_root" && make "-j${build_jobs}" V=s) || fail 'OpenWrt firmware image build failed'
	find_qemu_image "$target" >/dev/null || fail "no combined ext4 QEMU image found for $target"
	log "image-build: PASS; image: $(find_qemu_image "$target")"
}

find_qemu_image() {
	local target="$1" directory pattern
	directory="$openwrt_root/bin/targets/$target"
	[[ -d "$directory" ]] || return 1
	if [[ "$target" == armsr/armv8 ]]; then
		pattern='*generic-ext4-combined-efi.img*'
	else
		pattern='*generic-ext4-combined.img*'
	fi
	find "$directory" -maxdepth 1 -type f -size +0c -name "$pattern" \
		\( -name '*.img' -o -name '*.img.gz' \) -print | LC_ALL=C sort | sed -n '1p' | grep .
}

run_openwrt_qemu() {
	local target image firmware
	[[ "$host" == Linux ]] || fail '--qemu requires Linux'
	command -v python3 >/dev/null || fail '--qemu requires Python 3'
	[[ -f "$openwrt_root/.config" ]] || fail 'OpenWrt target missing; run --config'
	target="$(configured_target "$openwrt_root/.config")"
	case "$target" in
		x86/64|armsr/armv8) ;;
		*) fail "QEMU UUT unsupported target: $target" ;;
	esac
	qemu_requirements "$target" || fail 'QEMU host prerequisites missing; rerun --setup or install the package shown above'
	image="$qemu_image"
	if [[ -z "$image" ]]; then
		image="$(find_qemu_image "$target")" || fail 'no matching QEMU image; run --image-build or supply --image'
	fi
	[[ -s "$image" ]] || fail "QEMU disk image missing: $image"
	local -a arguments=(--image "$image" --arch "$target")
	if [[ "$target" == armsr/armv8 ]]; then
		firmware="${qemu_firmware:-$openwrt_root/bin/targets/armsr/armv8/u-boot-qemu_armv8/u-boot.bin}"
		[[ -s "$firmware" ]] || fail "ARM QEMU firmware missing: $firmware; use --firmware"
		arguments+=(--firmware "$firmware")
	fi
	if [[ -n "$qemu_log_dir" ]]; then
		arguments+=(--log-dir "$qemu_log_dir")
	fi
	log "qemu: two OpenWrt guests, target=$target image=$image"
	python3 "$eigrp_root/openwrt/test/qemu-uut.py" "${arguments[@]}" || fail 'OW-10 two-node QEMU UUT failed'
}

clean_openwrt() {
	local config="$openwrt_root/.config" target
	verify_checkout || fail 'OpenWrt checkout is missing or invalid'
	verify_package_feed || fail 'OpenEIGRP package not installed; run --install'
	# OpenWrt's package/clean target depends on .config. If it is absent,
	# OpenWrt falls through to interactive menuconfig, which fails on headless
	# hosts. Cleaning must not silently create or change the selected target.
	[[ -s "$config" ]] ||
		fail 'OpenWrt target is not configured; run ./tools/openwrt.sh --config before --clean'
	target="$(configured_target "$config")"
	[[ -n "$target" && -f "$openwrt_root/target/linux/$target/target.mk" ]] ||
		fail 'OpenWrt target selection is missing or invalid; run ./tools/openwrt.sh --config'
	grep -qx 'CONFIG_PACKAGE_openeigrp=y' "$config" ||
		fail 'OpenEIGRP package is not selected; run ./tools/openwrt.sh --config'
	log 'clean: removing only OpenEIGRP package outputs (OpenWrt toolchain preserved)'
	(cd "$openwrt_root" && make package/openeigrp/clean V=s) || fail 'OpenEIGRP package clean failed'
	log 'clean: PASS'
}

run_openwrt_kernel() {
	[[ "$host" == Linux ]] || fail '--kernel requires Linux'
	command -v ip >/dev/null 2>&1 || fail '--kernel requires iproute2 (ip)'
	command -v sudo >/dev/null 2>&1 || fail '--kernel requires sudo for isolated namespace'
	log 'kernel: isolated RIB UUT in disposable Linux network namespace'
	sudo bash "$eigrp_root/openwrt/test/rib-uut.sh" --kernel || fail 'isolated kernel RIB UUT failed'
	log 'kernel: PASS'
}

if [[ "$action" == image-build ]]; then
	image_build_openwrt
	exit 0
fi

if [[ "$action" == qemu ]]; then
	run_openwrt_qemu
	exit 0
fi

if [[ "$action" == test || "$action" == uut ]]; then
	run_openwrt_tests
	exit 0
fi

if [[ "$action" == kernel ]]; then
	run_openwrt_kernel
	exit 0
fi

if [[ "$action" == clean ]]; then
	clean_openwrt
	exit 0
fi

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
		if [[ "$host" == Linux && -s "$openwrt_root/.config" ]]; then
			# QEMU is an optional live-UUT facility, not a basic build/verify
			# dependency. Report readiness without rejecting package-only hosts.
			case "$(configured_target "$openwrt_root/.config")" in
				armsr/armv8|x86/64)
					qemu_requirements "$(configured_target "$openwrt_root/.config")" ||
						log 'qemu: optional for package builds; required by --qemu'
					;;
			esac
		fi
		if [[ -e "$package_link" || -L "$package_link" ]]; then
			verify_package_feed || status=1
			if [[ ! -s "$openwrt_root/.config" ]]; then
				log 'package: OpenWrt target is not configured; run --config before build or clean'
				status=1
			elif ! grep -qx 'CONFIG_PACKAGE_openeigrp=y' "$openwrt_root/.config"; then
				log 'package: not selected (run --config)'
				status=1
			fi
		fi
	fi
	exit "$status"
fi

if [[ "$action" == install ]]; then
	install_openwrt
	exit 0
fi

if [[ "$action" == build ]]; then
	build_openwrt
	exit 0
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
	log 'feeds: update source indexes only (no blanket package installation)'
	(
		cd "$openwrt_root"
		./scripts/feeds update -a
		# Do not install all feed packages here. Besides unnecessarily
		# increasing Kconfig complexity, unrelated packages can contain
		# dependency cycles. Future --install adds only OpenEIGRP packages.
	)
fi

# --skip-packages permits an intentionally lean host. Surface the missing
# optional emulator here instead of deferring discovery to a QEMU attempt.
if [[ "$host" == Linux && -s "$openwrt_root/.config" ]]; then
	case "$(configured_target "$openwrt_root/.config")" in
		armsr/armv8|x86/64)
			qemu_requirements "$(configured_target "$openwrt_root/.config")" ||
				log 'qemu: UUT prerequisites not installed; rerun --setup without --skip-packages'
			;;
	esac
fi
log 'OpenWrt host setup complete. Configure a target with: tools/openwrt.sh --config'
log 'Portable tests are available separately: make -C <eigrp-checkout> test'
