#!/usr/bin/env bash
# Copyright (C) 2026 Donnie V. Savage
# Executable regression checks with mocked external tools. No host modifications.
set -Eeuo pipefail
script="$(cd "$(dirname "$0")" && pwd)/netlab-provision.sh"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin" "$tmp/home"
export MOCK_ROOT="$tmp" HOME="$tmp/home" PATH="$tmp/bin:$PATH"
cat > "$tmp/bin/docker" <<'MOCK'
#!/usr/bin/env bash
set -eu
case "${1:-}" in
  info) exit 0;;
  image)
    case "${2:-}" in
      inspect)
        img="${@: -1}"
        grep -Fxq "$img" "$MOCK_ROOT/images" 2>/dev/null || exit 1
        if [[ "${3:-}" == -f ]]; then
          [[ "${4:-}" == '{{.Architecture}}' ]] && echo amd64 || echo sha256:mock
        fi;;
      ls) cat "$MOCK_ROOT/images" 2>/dev/null || true;;
      *) exit 2;;
    esac;;
  pull)
    img="${@: -1}"
    echo "$img" >> "$MOCK_ROOT/images"
    echo "pulled $img" >> "$MOCK_ROOT/events";;
  tag)
    echo "$3" >> "$MOCK_ROOT/images"
    echo "tagged $2 $3" >> "$MOCK_ROOT/events";;
  *) exit 2;;
esac
MOCK
cat > "$tmp/bin/curl" <<'MOCK'
#!/usr/bin/env bash
set -eu
outfile=''; headers=''
while (($#)); do
  case "$1" in
    -o|--output) outfile="$2"; shift 2;;
    -D) headers="$2"; shift 2;;
    *) shift;;
  esac
done
if [[ -z "$outfile" ]]; then
  echo '{"token":"anonymous-token"}'
else
  echo '{"tags":["0.0.3","0.0.3-amd64","0.0.4-amd64","0.0.4"]}' > "$outfile"
  printf 'HTTP/2 200\r\n' > "$headers"
fi
MOCK
cat > "$tmp/bin/qemu-img" <<'MOCK'
#!/usr/bin/env bash
set -eu
case "$1" in
  create)
    # qemu-img create -f qcow2 DEST 8G
    [[ "$2" == -f && "$3" == qcow2 ]]
    printf 'mock qcow2' > "$4"
    # Mirror real qemu-img output. The provisioner must not capture this
    # formatting announcement as part of the resulting disk filename.
    printf "Formatting '%s', fmt=qcow2 cluster_size=65536 size=8589934592\n" "$4"
    echo create >> "$MOCK_ROOT/qemu-events";;
  info)
    if [[ "${MOCK_QEMU_SMALL:-}" == 1 ]]; then
      echo '{"format":"qcow2","actual-size":4096}'
    else
      echo '{"format":"qcow2","actual-size":536870912}'
    fi;;
  *) exit 2;;
esac
MOCK
cat > "$tmp/bin/qemu-system-x86_64" <<'MOCK'
#!/usr/bin/env bash
set -eu
echo run >> "$MOCK_ROOT/qemu-events"
[[ "${MOCK_QEMU_FAIL:-}" != 1 ]] || exit 1
MOCK
chmod +x "$tmp/bin/"*

# Release selection and version retention are ordinary script-level tests.
"$script" --image > "$tmp/run1"
grep -Fxq 'pulled ghcr.io/diivious/openeigrp-frr:0.0.4' "$tmp/events"
"$script" --images > "$tmp/run2"
[[ "$(grep -c '^pulled ' "$tmp/events")" -eq 1 ]]
echo 'ghcr.io/diivious/openeigrp-frr:0.0.3' >> "$tmp/images"
"$script" --image > "$tmp/run3"
grep -Fxq 'ghcr.io/diivious/openeigrp-frr:0.0.3' "$tmp/images"
[[ "$(grep -c '^pulled ' "$tmp/events")" -eq 1 ]]

# Exercise the *actual* ISO conversion implementation using mocked KVM/QEMU.
# Source the script without its command-line entrypoint and override only the
# /dev/kvm availability check, which a test host may not provide.
source <(sed '$d' "$script")
require_kvm() { :; }
mkdir -p "$IMAGE_DIR"
iso="$IMAGE_DIR/csr1000v-universalk9.17.03.06.iso"
printf 'mock ISO' > "$iso"
qcow2="$(install_csr_iso "$iso")"
[[ -f "$qcow2" && -f "$qcow2.installed-sha256" ]]
[[ "$qcow2" == "$IMAGE_DIR/csr1000v-universalk9.17.03.06-serial.qcow2" ]]
# Regression for the real-world failure: qemu-img "Formatting ..." stdout
# must be logged, not captured as part of the QCOW2 filename.
[[ "$qcow2" != *Formatting* && "$qcow2" != *$'\n'* ]]
grep -Fq 'Formatting ' "$IMAGE_DIR/csr1000v-universalk9.17.03.06.install.log"
[[ "$(grep -c '^run$' "$tmp/qemu-events")" -eq 1 ]]
[[ "$(install_csr_iso "$iso")" == "$qcow2" ]]
[[ "$(grep -c '^run$' "$tmp/qemu-events")" -eq 1 ]]

# Changing a previously installed ISO must not replace an immutable version.
printf 'changed ISO' > "$iso"
if (install_csr_iso "$iso" > /dev/null 2>"$tmp/changed.err"); then
  echo 'FAIL: ISO version checksum mismatch was accepted' >&2; exit 1
fi
grep -q 'source has changed' "$tmp/changed.err"

# A failed VM installation leaves no completed disk/tag.
bad="$IMAGE_DIR/csr1000v-universalk9.17.03.07.iso"
printf 'mock ISO' > "$bad"
if (export MOCK_QEMU_FAIL=1; install_csr_iso "$bad" > /dev/null 2>"$tmp/failed.err"); then
  echo 'FAIL: QEMU installation failure was accepted' >&2; exit 1
fi
[[ ! -e "${bad%.iso}-serial.qcow2" ]]

# Even a clean QEMU exit is not sufficient without populated disk data.
small="$IMAGE_DIR/csr1000v-universalk9.17.03.08.iso"
printf 'mock ISO' > "$small"
if (export MOCK_QEMU_SMALL=1; install_csr_iso "$small" > /dev/null 2>"$tmp/small.err"); then
  echo 'FAIL: empty disk was accepted' >&2; exit 1
fi
[[ ! -e "${small%.iso}-serial.qcow2" ]]

# A user-supplied disk takes priority over a companion ISO; never overwrite it.
pre="$IMAGE_DIR/csr1000v-universalk9.17.03.09-serial.qcow2"
printf 'provided QCOW2' > "$pre"
printf 'mock ISO' > "${pre%-serial.qcow2}.iso"
[[ "$(install_csr_iso "${pre%-serial.qcow2}.iso")" == "$pre" ]]
[[ "$(cat "$pre")" == 'provided QCOW2' ]]

# Mock vrnetlab checkout and Docker build to exercise ISO -> QCOW2 -> tag.
cat > "$tmp/bin/git" <<'MOCK'
#!/usr/bin/env bash
set -eu
[[ "$1" == clone ]] || exit 2
out="${@: -1}"
mkdir -p "$out/cisco/csr1000v" "$out/cisco/c8000v"
MOCK
cat > "$tmp/bin/make" <<'MOCK'
#!/usr/bin/env bash
set -eu
if [[ "${*: -1}" == version-test ]]; then
  printf 'Version: 17.03.06\n'
elif [[ "${*: -1}" == print-version ]]; then
  printf '17.13.01a\n'
elif [[ "${1:-}" == docker-image ]]; then
  echo 'vrnetlab/cisco_csr1000v:17.03.06' >> "$MOCK_ROOT/images"
  echo build-csr >> "$MOCK_ROOT/vrnetlab-events"
elif [[ "${*: -1}" == docker-build ]]; then
  [[ " ${*} " == *' MODE=autonomous '* ]]
  echo 'vrnetlab/cisco_c8000v:17.13.01a' >> "$MOCK_ROOT/images"
  echo build-c8000v >> "$MOCK_ROOT/vrnetlab-events"
else
  exit 2
fi
MOCK
chmod +x "$tmp/bin/git" "$tmp/bin/make"
mkdir -p "$VRNETLAB_DIR/cisco/csr1000v" "$VRNETLAB_DIR/cisco/c8000v"
: > "$VRNETLAB_DIR/cisco/csr1000v/Makefile"
: > "$VRNETLAB_DIR/cisco/c8000v/Makefile"
ensure_vrnetlab() { :; }
printf 'mock ISO' > "$iso"
build_csr "$iso" > "$tmp/build-out"
grep -Fxq 'vrnetlab/cisco_csr1000v:17.03.06' "$tmp/images"
grep -Fxq 'vrnetlab/vr-csr:17.03.06' "$tmp/images"
[[ "$(grep -c '^build-csr$' "$tmp/vrnetlab-events")" -eq 1 ]]
build_csr "$iso" > "$tmp/build-again"
[[ "$(grep -c '^build-csr$' "$tmp/vrnetlab-events")" -eq 1 ]]

# Catalyst 8000V ISO-to-QCOW2 and independent image tagging.
mkdir -p "$C8000V_DIR"
catiso="$C8000V_DIR/c8000v-universalk9.17.13.01a.iso"
printf 'mock catalyst ISO' > "$catiso"
catdisk="$(install_c8000v_iso "$catiso")"
[[ "$catdisk" == "$C8000V_DIR/c8000v-universalk9.17.13.01a-serial.qcow2" ]]
[[ -f "$catdisk" && -f "$catdisk.installed-sha256" ]]
[[ "$catdisk" != *Formatting* && "$catdisk" != *$'\n'* ]]
grep -q 'Formatting ' "$C8000V_DIR/c8000v-universalk9.17.13.01a.install.log"
build_c8000v "$catiso" > "$tmp/build-cat"
grep -Fxq 'vrnetlab/cisco_c8000v:17.13.01a' "$tmp/images"
grep -Fxq 'vrnetlab/vr-c8000v:17.13.01a' "$tmp/images"
[[ "$(grep -c '^build-c8000v$' "$tmp/vrnetlab-events")" -eq 1 ]]
build_c8000v "$catiso" > "$tmp/build-cat-again"
[[ "$(grep -c '^build-c8000v$' "$tmp/vrnetlab-events")" -eq 1 ]]
# A changed ISO must not overwrite an already installed release.
printf 'modified catalyst ISO' > "$catiso"
if (install_c8000v_iso "$catiso" > /dev/null 2> "$tmp/changed-cat.err"); then
  echo 'FAIL: changed Catalyst ISO was accepted' >&2; exit 1
fi
grep -q 'source changed' "$tmp/changed-cat.err"
# A failed 8000V installation must not promote an empty disk.
failedcat="$C8000V_DIR/c8000v-universalk9.17.13.02.iso"
printf 'another ISO' > "$failedcat"
if (export MOCK_QEMU_FAIL=1; install_c8000v_iso "$failedcat" > /dev/null 2> "$tmp/failed-cat.err"); then
  echo 'FAIL: failed Catalyst installation was accepted' >&2; exit 1
fi
[[ ! -e "${failedcat%.iso}-serial.qcow2" ]]
rm -f "$failedcat"
# Source-file staging into the correct directory must leave the CSR1k intact.
cat_source="$tmp/c8000v-universalk9.17.13.01a.qcow2"
printf 'standalone cat disk' > "$cat_source"
cat_cache="$(store_csr_source "$cat_source" "$C8000V_DIR")"
[[ "$cat_cache" == "$C8000V_DIR/${cat_source##*/}" ]]
[[ ! -e "$IMAGE_DIR/${cat_source##*/}" ]]
rm -f "$cat_cache"
printf 'mock catalyst ISO' > "$catiso"

# Exercise --images discovery/iteration against ISO and generated QCOW2.
rm -f "$bad" "$small" "$pre" "${pre%-serial.qcow2}.iso"
images > "$tmp/complete-images"
grep -Fq 'OpenEIGRP selected:' "$tmp/complete-images"
grep -Fxq 'vrnetlab/vr-csr:17.03.06' "$tmp/images"
grep -Fxq 'vrnetlab/vr-c8000v:17.13.01a' "$tmp/images"
[[ "$(grep -c '^build-csr$' "$tmp/vrnetlab-events")" -eq 1 ]]
[[ "$(grep -c '^build-c8000v$' "$tmp/vrnetlab-events")" -eq 1 ]]

# Upgrades of a previous installation must add the new PATH to both shell
# entrypoints exactly once, even if ~/.profile has an old managed block.
printf '# netlab-provision managed PATH\n' > "$HOME/.profile"
ensure_shell_path
ensure_shell_path
[[ "$(grep -Fc '# netlab-provision managed PATH v2' "$HOME/.profile")" -eq 1 ]]
[[ "$(grep -Fc '# netlab-provision managed PATH v2' "$HOME/.bashrc")" -eq 1 ]]
grep -Fq '$HOME/bin:$HOME/netlab-venv/bin' "$HOME/.bashrc"

printf 'PASS: retained GHCR tags, CSR1k ISO and v2.1 regression, Catalyst 8000V ISO build, checksums, separate folders, both vrnetlab aliases, autonomous mode, failure handling, repeatability, PATH\n'
