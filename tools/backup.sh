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

Creates a clean, temporary project copy and writes the requested zip file
in ~/Downloads unless ZIPFILE is an absolute path. Existing folders under
~/Downloads are never deleted. An existing zip is replaced only after success.
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

# Unique staging directories are owned by this invocation. In particular,
# never delete or reuse ~/Downloads/$project_name: it may contain user files.
mkdir -p "$download_dir"
output_dir="$(dirname "$zip_path")"
[[ -d "$output_dir" ]] || {
    echo "error: output directory does not exist: $output_dir" >&2
    exit 1
}
output_dir="$(cd "$output_dir" && pwd)"
zip_path="$output_dir/$(basename "$zip_path")"
staging_dir=""
archive_dir=""
cleanup() {
    if [[ -n "$staging_dir" ]]; then rm -rf -- "$staging_dir"; fi
    if [[ -n "$archive_dir" ]]; then rm -rf -- "$archive_dir"; fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

staging_dir="$(mktemp -d "${TMPDIR:-/tmp}/${project_name}-backup.XXXXXXXX")"
archive_dir="$(mktemp -d "$output_dir/.${project_name}-archive.XXXXXXXX")"
backup_dir="$staging_dir/$project_name"
cp -R "$project_root" "$backup_dir"

# Prune generated directories before enumerating individual generated files.
# Keeping the traversal from entering .venv avoids including venv files in the zip.
find "$backup_dir" \
    \( -name .git -o -name .venv -o -name .pytest_cache -o \
       -name .deps -o -name .libs -o -name __pycache__ -o \
       -name __MACOSX \) \
    -prune -exec rm -rf -- {} +

find "$backup_dir" -type f \
    \( -name .DS_Store -o -name .dirstamp -o -name '*.pyc' -o \
       -name '*.o' -o -name '*.lo' -o -name '*.la' -o -name '*~' \) \
    -delete

rm -rf "$backup_dir/frr/test/build/obj" "$backup_dir/frr/test/build/logs"

# Write a complete archive alongside its final destination before replacing
# the old archive. Any failed zip leaves the previous backup intact.
temporary_zip="$archive_dir/$(basename "$zip_path")"
(
    cd "$staging_dir"
    zip -qr "$temporary_zip" "$project_name"
)
mv -f -- "$temporary_zip" "$zip_path"

echo "created: $zip_path"
echo "removed temporary staging: $backup_dir"
