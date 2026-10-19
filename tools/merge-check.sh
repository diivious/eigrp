#!/usr/bin/env bash
# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
# Pre-merge integration gate for portable EIGRP and all present host shims.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
profile=full
jobs=2
privileged=0
frr_root="${MERGE_FRR_ROOT:-}"
openwrt_root="${MERGE_OPENWRT_ROOT:-}"
log_dir="${MERGE_LOG_DIR:-}"

usage() {
    cat <<'USAGE'
usage: tools/merge-check.sh [--full|--host] [options]

  --full                 Strict pre-merge qualification (default). Requires
                         full FRR and OpenWrt builds/UUT, guest image build,
                         two-node QEMU UUT, and isolated kernel UUT.
  --host                 Host-only regression gate: portable/Unix build and UUT,
                         FRR compile/link guard and OpenWrt host UUT. Does NOT
                         constitute full merge qualification.
  --privileged           Explicitly authorize sudo-backed integration UUTs.
                         Refuses interactive sudo; never runs the whole gate as root.
  --frr-root PATH        FRR checkout (default: ../frr or ~/devel/frr).
  --openwrt-root PATH    OpenWrt buildroot (default: ../openwrt).
  --jobs N               Parallel compilation jobs (default 2).
  --log-dir PATH         Directory for separate per-stage logs (default: temp dir).
  --help                 This help.

All present integrations are discovered. Missing mandatory prerequisites BLOCK
--full (nonzero exit); stages continue so the final summary lists all failures.
No external repository is cloned, reset, patched or configured automatically.
Only present BIRD/BSD adapters are tested; if BSD is added without a driver,
the gate fails rather than silently passing. Each shim retains its own direct
build/test entry points.
USAGE
}

fail_arg() { printf 'error: %s\n' "$*" >&2; exit 2; }
while (($#)); do
    case "$1" in
        --full) profile=full; shift ;;
        --host) profile=host; shift ;;
        --privileged) privileged=1; shift ;;
        --frr-root|--openwrt-root|--log-dir|--jobs)
            (($# >= 2)) || fail_arg "$1 needs a value"
            case "$1" in
                --frr-root) frr_root="$2" ;;
                --openwrt-root) openwrt_root="$2" ;;
                --log-dir) log_dir="$2" ;;
                --jobs) jobs="$2" ;;
            esac
            shift 2 ;;
        --help) usage; exit 0 ;;
        *) fail_arg "unknown argument: $1" ;;
    esac
done
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || fail_arg '--jobs must be a positive integer'
(( ${#jobs} <= 3 && 10#$jobs <= 256 )) || fail_arg '--jobs must be 1..256'
[[ "$(id -u)" != 0 ]] || fail_arg 'run this gate as an unprivileged user'

if [[ -z "$frr_root" ]]; then
    for candidate in "$root/../frr" "$HOME/devel/frr"; do
        if [[ -f "$candidate/bootstrap.sh" ]]; then
            frr_root="$candidate"
            break
        fi
    done
fi
[[ -n "$openwrt_root" ]] || openwrt_root="$root/../openwrt"
[[ -n "$log_dir" ]] || log_dir="$(mktemp -d "${TMPDIR:-/tmp}/openeigrp-merge-check.XXXXXXXX")"
mkdir -p "$log_dir"
log_dir="$(cd "$log_dir" && pwd -P)"

labels=()
states=()
logs=()
pass_count=0
fail_count=0
blocked_count=0
skip_count=0
stage_number=0
last_stage_ok=0

record() {
    labels+=("$1")
    states+=("$2")
    logs+=("$3")
    case "$2" in
        PASS) ((pass_count+=1)) ;;
        FAIL) ((fail_count+=1)) ;;
        BLOCKED) ((blocked_count+=1)) ;;
        SKIP) ((skip_count+=1)) ;;
    esac
    printf '%-8s %s%s\n' "$2" "$1" "${4:+: $4}"
}

# Every stage is independent. Failures are aggregated and never masked by a
# subsequent successful stage or by the shell status of tee.
run_stage() {
    local label="$1"; shift
    local filename="$(printf '%02d' "$stage_number")-$(printf '%s' "$label" | tr ' /' '--' | tr -cd 'A-Za-z0-9._-').log"
    local logfile="$log_dir/$filename"
    local rc
    ((stage_number+=1))
    printf '\n==> %s\n' "$label"
    if (cd "$root" && "$@") >"$logfile" 2>&1; then
        last_stage_ok=1
        record "$label" PASS "$logfile"
    else
        rc=$?
        last_stage_ok=0
        record "$label" FAIL "$logfile" "exit $rc"
        # Show an actionable error while preserving the complete stage log.
        tail -n 35 "$logfile" >&2 || true
        printf 'Full log: %s\n' "$logfile" >&2
    fi
}

blocked() { record "$1" BLOCKED "" "$2"; }
skipped() { record "$1" SKIP "" "$2"; }

# Gate self-test exercises aggregation, stage ordering and prereq handling.
run_stage 'merge-driver tests' bash "$root/test/merge/merge-check-test.sh"
run_stage 'portable and Unix build' make build
run_stage 'portable protocol UUT' make uut
run_stage 'portable pacing/backpressure UUT' make scale
if [[ "$(uname -s)" == Linux ]]; then
    run_stage 'FRR adapter compile/link' make -C frr/test/build all
else
    skipped 'FRR adapter compile/link' 'Linux-only adapter compile check'
fi
run_stage 'OpenWrt host UUT' bash "$root/tools/openwrt.sh" --uut

# The repository currently has no bsd/ shim. Detect it if introduced, and
# require an independently runnable host adapter driver rather than reporting
# a fictional BIRD pass.
if [[ -d "$root/bsd" ]]; then
    if [[ -x "$root/tools/bsd.sh" ]]; then
        run_stage 'BIRD/BSD build' bash "$root/tools/bsd.sh" --build
        run_stage 'BIRD/BSD UUT' bash "$root/tools/bsd.sh" --uut
    else
        blocked 'BIRD/BSD integration' 'bsd/ exists but tools/bsd.sh is missing'
    fi
else
    skipped 'BIRD/BSD integration' 'not present in this checkout'
fi

if [[ "$profile" == full ]]; then
    if [[ -n "$frr_root" && -f "$frr_root/bootstrap.sh" ]]; then
        run_stage 'FRR full configure/build/check' bash "$root/tools/frr.sh" --all --frr-root "$frr_root" --jobs "$jobs"
        frr_build_ok=$last_stage_ok
        if (( !frr_build_ok )); then
            blocked 'FRR live UUT' 'FRR build/check failed; refusing stale executable'
        elif ((privileged)); then
            if sudo -n -v </dev/null >/dev/null 2>&1; then
                run_stage 'FRR live UUT' bash "$root/tools/frr.sh" --uut --frr-root "$frr_root" --jobs "$jobs"
            else
                blocked 'FRR live UUT' 'noninteractive sudo unavailable'
            fi
        else
            blocked 'FRR live UUT' 'requires --privileged'
        fi
    else
        blocked 'FRR full configure/build/check' 'FRR checkout missing; provide --frr-root'
        blocked 'FRR live UUT' 'FRR checkout missing; provide --frr-root'
    fi

    if [[ "$(uname -s)" != Linux ]]; then
        blocked 'OpenWrt target build' 'requires Linux build host'
        blocked 'OpenWrt guest image build' 'requires Linux build host'
        blocked 'OpenWrt two-node QEMU UUT' 'requires Linux build host'
        blocked 'OpenWrt isolated kernel UUT' 'requires Linux build host'
    elif [[ ! -f "$openwrt_root/rules.mk" || ! -f "$openwrt_root/.config" ]]; then
        blocked 'OpenWrt target build' 'configured buildroot missing; provide --openwrt-root'
        blocked 'OpenWrt guest image build' 'configured buildroot missing'
        blocked 'OpenWrt two-node QEMU UUT' 'configured buildroot missing'
        blocked 'OpenWrt isolated kernel UUT' 'configured buildroot missing'
    else
        run_stage 'OpenWrt target build' bash "$root/tools/openwrt.sh" --build --jobs "$jobs" --openwrt-root "$openwrt_root"
        if ((last_stage_ok)); then
            run_stage 'OpenWrt guest image build' bash "$root/tools/openwrt.sh" --image-build --jobs "$jobs" --openwrt-root "$openwrt_root"
            if ((last_stage_ok)); then
                run_stage 'OpenWrt two-node QEMU UUT' bash "$root/tools/openwrt.sh" --qemu --openwrt-root "$openwrt_root" --log-dir "$log_dir/qemu"
            else
                blocked 'OpenWrt two-node QEMU UUT' 'image build failed; refusing stale guest image'
            fi
        else
            blocked 'OpenWrt guest image build' 'target package build failed'
            blocked 'OpenWrt two-node QEMU UUT' 'target package build failed'
        fi
        if ((privileged)); then
            if sudo -n -v </dev/null >/dev/null 2>&1; then
                run_stage 'OpenWrt isolated kernel UUT' bash "$root/tools/openwrt.sh" --kernel --openwrt-root "$openwrt_root"
            else
                blocked 'OpenWrt isolated kernel UUT' 'noninteractive sudo unavailable'
            fi
        else
            blocked 'OpenWrt isolated kernel UUT' 'requires --privileged'
        fi
    fi
else
    skipped 'FRR and OpenWrt live acceptance' 'host-only profile; not merge qualified'
fi

printf '\n%-8s  %-38s %s\n' 'RESULT' 'STAGE' 'LOG'
printf '%s\n' '--------------------------------------------------------------------------------'
for ((i=0; i<${#labels[@]}; i++)); do
    printf '%-8s  %-38s %s\n' "${states[i]}" "${labels[i]}" "${logs[i]}"
done
printf '\nTotals: %d PASS, %d FAIL, %d BLOCKED, %d SKIP\nLogs: %s\n' \
    "$pass_count" "$fail_count" "$blocked_count" "$skip_count" "$log_dir"
if ((fail_count || blocked_count)); then
    printf 'MERGE GATE: FAIL (or incomplete)\n' >&2
    exit 1
fi
if [[ "$profile" == host ]]; then
    printf 'HOST REGRESSION: PASS; full merge acceptance NOT VERIFIED\n'
else
    printf 'MERGE GATE: PASS (all required stages completed)\n'
fi
