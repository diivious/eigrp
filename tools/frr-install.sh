#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Assemble and stage this EIGRP project into an FRR checkout.
#
# The project keeps common EIGRP source in eigrpd/ and FRR-specific source in
# frr/.  FRR itself still expects one flattened eigrpd/ source directory, so
# this installer owns that projection.  Any required modification outside
# FRR's eigrpd/ directory is carried as a patch under frr/patch/.

set -euo pipefail

script_name="$(basename "$0")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
eigrp_root="$(cd "$script_dir/.." && pwd)"

frr_root=""
install_eigrpd=1
install_tests=1
install_patches=1
dry_run=0

common_src="$eigrp_root/eigrpd"
frr_src="$eigrp_root/frr"
frr_test_src="$frr_src/test"
frr_patch_src="$frr_src/patch"

usage() {
	cat <<USAGE
usage: $script_name [options]

options:
  --frr-root PATH       FRR checkout root. Default inference order:
                        ../frr, ~/devel/frr, or parent when run from an FRR tree.
  --no-eigrpd           Do not assemble/install FRR/eigrpd/.
  --no-tests            Do not copy frr/test/ into FRR tests/eigrpd/.
  --no-patches          Do not apply required patches from frr/patch/.
  --dry-run             Validate and print actions without changing FRR.
  --help                Show this help.

installs:
  eigrpd/ + frr/*       -> FRR/eigrpd/
  frr/test/             -> FRR/tests/eigrpd/
  frr/patch/*.patch     -> applied at the FRR repository root

Patch handling is idempotent.  frr/patch/series defines dependency order when
present.  Each patch is checked in reverse first; an already-applied patch is
left alone, a clean forward patch is applied, and any conflict stops the
install rather than guessing at an FRR source change.
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

	for candidate in \
		"$eigrp_root/../frr" \
		"$HOME/devel/frr" \
		"$eigrp_root/.."; do
		if [[ -d "$candidate/tests" && -f "$candidate/bootstrap.sh" ]]; then
			cd "$candidate" && pwd
			return 0
		fi
	done

	return 1
}

rsync_project_tree() {
	local src="$1"
	local dst="$2"
	shift 2

	[[ -d "$src" ]] || fail "source directory not found: $src"
	mkdir -p "$dst"
	rsync -a "$@" \
		--exclude '.git/' \
		--exclude '.pytest_cache/' \
		--exclude '__pycache__/' \
		--exclude '__MACOSX/' \
		--exclude '.DS_Store' \
		--exclude '*.o' \
		--exclude '*.lo' \
		--exclude '*.la' \
		--exclude '*_clippy.c' \
		--exclude '*~' \
		--exclude 'obj/' \
		--exclude 'logs/' \
		"$src"/ "$dst"/
}

assemble_eigrpd_tree() {
	local stage="$1"

	[[ -d "$common_src" ]] || fail "missing common EIGRP source: $common_src"
	[[ -d "$frr_src" ]] || fail "missing FRR adapter source: $frr_src"

	mkdir -p "$stage"

	# Common source establishes the base daemon tree.
	rsync_project_tree "$common_src" "$stage"

	# FRR-specific daemon files overlay the common tree.  frr/patch and
	# frr/test are integration payloads and are never copied into eigrpd/.
	rsync_project_tree "$frr_src" "$stage" \
		--exclude '/patch/' \
		--exclude '/patches/' \
		--exclude '/test/'
}

install_eigrpd_tree() {
	local dst="$frr_root/eigrpd"
	local stage

	stage="$(mktemp -d)"
	assemble_eigrpd_tree "$stage"

	if [[ -L "$dst" ]]; then
		fail "FRR eigrpd destination is a symlink; split common/FRR source requires a real staging directory: $dst"
	fi

	if [[ "$dry_run" -eq 1 ]]; then
		echo "would assemble: eigrpd/ + frr/ -> $dst/"
		rm -rf "$stage"
		return 0
	fi

	mkdir -p "$dst"
	rsync -a --delete "$stage"/ "$dst"/
	rm -rf "$stage"
	echo "installed: eigrpd/ + frr/ -> $dst/"
}

install_test_tree() {
	local dst="$frr_root/tests/eigrpd"

	if [[ ! -d "$frr_test_src" ]]; then
		echo "warning: no FRR test payload exists at $frr_test_src" >&2
		return 0
	fi

	if [[ "$dry_run" -eq 1 ]]; then
		echo "would sync: $frr_test_src/ -> $dst/"
		return 0
	fi

	mkdir -p "$dst"
	rsync_project_tree "$frr_test_src" "$dst" --delete
	echo "installed: frr/test/ -> $dst/"
}

patch_semantically_applied() {
	local patch_name="$1"
	local yang="$frr_root/yang/frr-eigrpd.yang"

	case "$patch_name" in
		vtysh-named-eigrp.patch)
			grep -Fq 'router eigrp <(1-65535)|WORD> [vrf NAME]' \
				"$frr_root/vtysh/vtysh.c"
			;;
		eigrp-named-yang.patch)
			grep -Fq 'EIGRP named-mode configuration.' "$yang" &&
			grep -Fq 'list named {' "$yang" &&
			grep -Fq 'key "afi vrf asn";' "$yang"
			;;
		eigrp-named-af-config.patch)
			grep -Fq 'Named-mode address-family configuration is augmented separately' "$yang" &&
			grep -Fq 'leaf-list network {' "$yang" &&
			grep -Fq 'list neighbor {' "$yang"
			;;
		eigrp-named-af-interface.patch)
			grep -Fq 'Named-mode address-family interface configuration.' "$yang" &&
			grep -Fq 'list af-interface {' "$yang" &&
			grep -Fq 'leaf hello-interval {' "$yang"
			;;
		eigrp-named-topology.patch)
			grep -Fq 'Named-mode base topology configuration.' "$yang" &&
			grep -Fq 'container topology {' "$yang" &&
			grep -Fq 'list redistribute {' "$yang" &&
			grep -Fq 'leaf variance {' "$yang"
			;;
		eigrp-named-topology-callbacks.patch)
			grep -Fq 'EIGRP_STEP1_TOPOLOGY_COMPOUND_MANDATORY' "$yang"
			;;
		frr-eigrp-yang.patch)
			grep -Fq 'EIGRP_STEP1_CONFIG_COMPLETE' "$yang"
			;;
		*)
			return 1
			;;
	esac
}

patch_state() {
	local patch_file="$1"
	local patch_name

	patch_name="$(basename "$patch_file")"

	# Check the reverse direction first.  Some insertion-only patches can still
	# find another valid forward context after they have already been applied.
	# Forward-first detection can therefore apply the same logical patch twice.
	if git -C "$frr_root" apply --reverse --check "$patch_file" >/dev/null 2>&1; then
		printf '%s\n' applied
		return 0
	fi

	# Later managed patches may legitimately add schema text adjacent to an
	# earlier insertion and make git's reverse context check too strict.  For
	# known project patches, verify the complete feature marker/schema shape
	# before deciding that the earlier patch is in conflict.
	if patch_semantically_applied "$patch_name"; then
		printf '%s\n' applied
		return 0
	fi

	if git -C "$frr_root" apply --check "$patch_file" >/dev/null 2>&1; then
		printf '%s\n' pending
		return 0
	fi

	printf '%s\n' conflict
}

install_patch_file() {
	local patch_file="$1"
	local state

	state="$(patch_state "$patch_file")"
	case "$state" in
		pending)
			if [[ "$dry_run" -eq 1 ]]; then
				echo "would apply patch: $(basename "$patch_file")"
			else
				echo "apply patch: $(basename "$patch_file")"
				git -C "$frr_root" apply "$patch_file"
			fi
			;;
		applied)
			echo "already patched: $(basename "$patch_file")"
			;;
		conflict)
			cat >&2 <<MSG
error: FRR patch does not apply cleanly and does not appear to be already applied:
  $patch_file

The FRR source may have changed or the same feature may have been modified
independently.  Refusing to patch with fuzz or partial context.
MSG
			exit 1
			;;
		*)
			fail "unknown patch state '$state' for $patch_file"
			;;
	esac
}

invalidate_eigrp_yang_embed() {
	local count
	local previous_count
	local yang_source="$frr_root/yang/frr-eigrpd.yang"
	local yang_embed="$frr_root/yang/frr-eigrpd.yang.c"
	local named_patch="$frr_patch_src/eigrp-named-yang.patch"

	[[ -f "$yang_source" ]] || fail "FRR EIGRP YANG source not found: $yang_source"
	[[ -f "$named_patch" ]] || fail "managed EIGRP named YANG patch not found: $named_patch"

	# The .yang source is authoritative.  FRR generates frr-eigrpd.yang.c as a
	# build artifact.  Keep duplicate-schema repair here, but never generate or
	# copy the derived .yang.c file during installation.
	count="$(grep -Ec '^[[:space:]]*list[[:space:]]+named[[:space:]]*\{' "$yang_source" || true)"

	# Older versions of the installer checked forward applicability before the
	# reverse/already-applied state.  Because this patch is insertion-only, a
	# second valid context could be found and the named schema block could be
	# inserted more than once.  Repair only duplicates that can be removed by
	# reversing this exact managed patch; otherwise stop rather than edit an
	# unknown FRR/YANG change.
	while [[ "$count" -gt 1 ]]; do
		if ! git -C "$frr_root" apply --reverse --check "$named_patch" >/dev/null 2>&1; then
			fail "duplicate EIGRP named YANG nodes exist but the managed patch cannot safely remove one"
		fi

		if [[ "$dry_run" -eq 1 ]]; then
			echo "would repair: remove one duplicate managed EIGRP named YANG schema block"
			break
		fi

		previous_count="$count"
		echo "repair: remove one duplicate managed EIGRP named YANG schema block"
		git -C "$frr_root" apply --reverse "$named_patch"
		count="$(grep -Ec '^[[:space:]]*list[[:space:]]+named[[:space:]]*\{' "$yang_source" || true)"
		[[ "$count" -eq $((previous_count - 1)) ]] || \
			fail "managed YANG duplicate repair did not remove exactly one named schema block"
	done

	if [[ "$dry_run" -eq 0 && "$count" -ne 1 ]]; then
		fail "expected exactly one EIGRP named-mode schema node after patch installation; found $count"
	fi

	if [[ "$dry_run" -eq 1 ]]; then
		echo "would remove generated: yang/frr-eigrpd.yang.c"
		return 0
	fi

	rm -f "$yang_embed"
	echo "invalidated generated: yang/frr-eigrpd.yang.c (FRR build will regenerate it)"
}

install_frr_patches() {
	local -a patch_files=()
	local patch_file
	local patch_name
	local series_file="$frr_patch_src/series"

	[[ -d "$frr_patch_src" ]] || {
		echo "warning: FRR patch directory not found: $frr_patch_src" >&2
		return 0
	}

	# Some integration patches depend on earlier schema/infrastructure patches.
	# Prefer an explicit series file so patch order is stable and reviewable.
	if [[ -f "$series_file" ]]; then
		while IFS= read -r patch_name || [[ -n "$patch_name" ]]; do
			patch_name="${patch_name%%#*}"
			patch_name="$(printf '%s' "$patch_name" | xargs)"
			[[ -n "$patch_name" ]] || continue
			[[ "$patch_name" == *.patch ]] || fail "invalid patch series entry: $patch_name"
			patch_file="$frr_patch_src/$patch_name"
			[[ -f "$patch_file" ]] || fail "patch series file references missing patch: $patch_file"
			patch_files+=("$patch_file")
		done < "$series_file"
	else
		shopt -s nullglob
		patch_files=("$frr_patch_src"/*.patch)
		shopt -u nullglob
	fi

	if [[ "${#patch_files[@]}" -eq 0 ]]; then
		echo "patches: none"
		return 0
	fi

	for patch_file in "${patch_files[@]}"; do
		install_patch_file "$patch_file"
	done

	# Keep FRR's generated embedded EIGRP model synchronized with the patched
	# authoritative .yang source, including upgrades from older project patches.
	invalidate_eigrp_yang_embed
}

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		--frr-root)
			[[ "$#" -ge 2 ]] || fail "--frr-root requires a path"
			frr_root="$(resolve_existing_path "$2")"
			shift 2
			;;
		--no-eigrpd)
			install_eigrpd=0
			shift
			;;
		--no-tests)
			install_tests=0
			shift
			;;
		--no-patches)
			install_patches=0
			shift
			;;
		--dry-run)
			dry_run=1
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

require_command rsync
require_command git

if [[ -z "$frr_root" ]]; then
	if ! frr_root="$(infer_frr_root)"; then
		fail "FRR root could not be inferred; use --frr-root /path/to/frr"
	fi
fi

[[ -d "$frr_root" ]] || fail "FRR root does not exist: $frr_root"
[[ -f "$frr_root/bootstrap.sh" ]] || fail "not an FRR checkout root: $frr_root"
git -C "$frr_root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || \
	fail "FRR root is not a Git working tree: $frr_root"
[[ -d "$common_src" ]] || fail "missing project eigrpd directory: $common_src"
[[ -d "$frr_src" ]] || fail "missing project FRR directory: $frr_src"

# Global FRR modifications are checked/applied before daemon staging so a
# version-drift conflict fails without first replacing FRR/eigrpd/.
if [[ "$install_patches" -eq 1 ]]; then
	install_frr_patches
fi

if [[ "$install_eigrpd" -eq 1 ]]; then
	install_eigrpd_tree
fi

if [[ "$install_tests" -eq 1 ]]; then
	install_test_tree
fi
