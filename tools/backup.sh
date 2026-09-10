#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
#
# Copyright (C) 2026 Donnie V. Savage
#
# Create a clean project zip without local VCS/build/cache noise.

set -euo pipefail

script_name="$(basename "$0")"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
project_name="$(basename "$project_root")"

download_dir="$HOME/Downloads"
zip_name=""

usage() {
	cat <<USAGE
usage: $script_name --zip ZIPFILE

examples:
  tools/backup.sh --zip eigrp.zip
  tools/backup.sh --zip eigrp-active-code.zip

Creates a clean copy of the project at ~/Downloads/$project_name and writes the
requested zip file to ~/Downloads unless ZIPFILE is an absolute path.
USAGE
}

while [[ "$#" -gt 0 ]]; do
	case "$1" in
		--zip)
			shift
			[[ "$#" -gt 0 && "${1:-}" != -* ]] || { echo "error: missing zip file name" >&2; usage >&2; exit 2; }
			zip_name="$1"
			shift
			;;
		--help)
			usage
			exit 0
			;;
		*)
			echo "error: unknown option: $1" >&2
			usage >&2
			exit 2
			;;
	esac
done

[[ -n "$zip_name" ]] || { echo "error: zip file name is required" >&2; usage >&2; exit 2; }
[[ "$zip_name" == *.zip ]] || zip_name="$zip_name.zip"

if [[ "$zip_name" = /* ]]; then
	zip_path="$zip_name"
else
	zip_path="$download_dir/$zip_name"
fi

backup_dir="$download_dir/$project_name"
cleanup_backup=0
mkdir -p "$download_dir"

if [[ "$(cd "$project_root" && pwd)" == "$(mkdir -p "$backup_dir" && cd "$backup_dir" && pwd)" ]]; then
	echo "error: source project and backup target are the same path: $project_root" >&2
	exit 1
fi

rm -rf "$backup_dir"
cleanup_backup=1
trap 'if [[ "${cleanup_backup:-0}" -eq 1 ]]; then rm -rf "$backup_dir"; fi' EXIT
cp -R "$project_root" "$backup_dir"

find "$backup_dir" \
	-name .git -o \
	-name .pytest_cache -o \
	-name __pycache__ -o \
	-name __MACOSX -o \
	-name .DS_Store -o \
	-name '*.o' -o \
	-name '*.lo' -o \
	-name '*.la' -o \
	-name '*~' | while IFS= read -r path; do
		rm -rf "$path"
	done

rm -rf "$backup_dir/test/build/obj" "$backup_dir/test/build/logs"
rm -f "$zip_path"
(
	cd "$download_dir"
	zip -qr "$zip_path" "$project_name"
)

rm -rf "$backup_dir"
cleanup_backup=0
trap - EXIT

echo "created: $zip_path"
echo "removed: $backup_dir"
