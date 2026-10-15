#!/usr/bin/env bash
# Copyright (C) 2026 Donnie V. Savage
# Alpine FRR/OpenEIGRP Docker build, deployment and verification entrypoint.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
eigrp_url=https://github.com/diivious/eigrp.git
frr_url=https://github.com/diivious/frr.git
# Compatible with the managed FRR patches in the October 3 reference archive.
development_frr_sha=ebfd886fd46b19f9886b0872b39ad3edf5e6c053
alpine_version=3.22

usage() {
    cat <<'USAGE'
Usage:
  tools/docker.sh --local [VERSION] [--eigrp-root PATH] [--frr-root PATH]
                    [--platform linux/amd64|linux/arm64] [--image NAME:TAG]
                    [--jobs N] [--pull-base]
  tools/docker.sh --build VERSION [--eigrp-ref REF] [--frr-ref REF]
                    [--platform linux/amd64|linux/arm64] [--image NAME:TAG]
                    [--jobs N] [--pull-base]
  tools/docker.sh --deploy IMAGE:TAG [--name NAME] [--volume NAME]
                    [--network bridge|host]
  tools/docker.sh --test IMAGE:TAG
  tools/docker.sh --verify [IMAGE:TAG]  # default openeigrp:$USER

  --local   Build from local EIGRP and FRR checkouts (including working edits).
            Default VERSION: $USER; image: openeigrp:VERSION.
            Default FRR root: sibling ../frr (or ~/projects/frr).
  --build   Fetch both repositories from GitHub; release VERSION must match
            the immutable major-minor-edit-beta tags in BOTH repositories.
            Nonrelease versions require --eigrp-ref; FRR defaults to a pinned SHA.
  --deploy  Start a persistent container. Pull an image only if not present.
  --test    Run automated, destructive-on-temporary-volume integration tests;
            requires an existing local image; cleans up its test container.
  --verify  Run --test and a disposable two-router IP protocol 88 adjacency
            check, with startup/CLI/log diagnostics. Always remove temporary
            containers, volumes and network; never modify the source image.
  --pull-base  Refresh the pinned Alpine base before building; otherwise reuse
            BuildKit's cached layers where available (no forced pull).

The Alpine base defaults to alpine:3.22 for both architectures. Docker caches
base layers and build dependencies. --local does not contact GitHub; --build
requires GitHub. Neither build mode pushes an image to GHCR.
USAGE
}

die() { echo "error: $*" >&2; exit 1; }
release_version() { [[ "$1" =~ ^[0-9]+-[0-9]+-[0-9]+-beta$ ]]; }
version_valid() { [[ "$1" =~ ^[a-zA-Z0-9][a-zA-Z0-9._+-]*$ ]]; }
ref_valid() {
    release_version "$1" ||
      [[ "$1" =~ ^[0-9a-f]{40}$ || "$1" =~ ^refs/pull/[0-9]+/merge$ ||
         "$1" =~ ^refs/heads/[a-zA-Z0-9._/-]+$ ]]
}

# Resolve annotated tags to their peeled commits. Never trust a moving ref
# between the GitHub lookup and the checkout that enters the build context.
resolve_sha() {
    local url="$1" ref="$2" result sha=""
    if [[ "$ref" =~ ^[0-9a-f]{40}$ ]]; then
        printf '%s\n' "$ref"
        return 0
    fi
    if release_version "$ref"; then
        result="$(git ls-remote "$url" "refs/tags/$ref" "refs/tags/$ref^{}")" || return 1
        sha="$(awk -v x="refs/tags/$ref^{}" '$2 == x {print $1}' <<< "$result")"
        [[ -n "$sha" ]] || sha="$(awk -v x="refs/tags/$ref" '$2 == x {print $1}' <<< "$result")"
    else
        result="$(git ls-remote "$url" "$ref")" || return 1
        sha="$(awk -v x="$ref" '$2 == x {print $1}' <<< "$result")"
    fi
    [[ "$sha" =~ ^[0-9a-f]{40}$ ]] || return 1
    printf '%s\n' "$sha"
}

require_git_checkout() {
    local src="$1" description="$2"
    [[ -d "$src" ]] || die "$description checkout not found: $src"
    git -C "$src" rev-parse --show-toplevel >/dev/null 2>&1 || die "$description is not a Git checkout: $src"
}

# Local release builds must come from the matching release of both trees.
# Development builds intentionally accept local edits and untracked files.
verify_local_release() {
    local src="$1" description="$2" version="$3"
    git -C "$src" tag --points-at HEAD --list "$version" | grep -Fxq "$version" ||
        die "$description HEAD is not tagged $version"
    [[ -z "$(git -C "$src" status --porcelain --untracked-files=normal)" ]] ||
        die "$description has working changes; release builds require clean checkouts (use a nonrelease version for development)"
}

# GitHub checkout is performed before building, then BuildKit receives those
# exact sources. This also ensures the --local and --build modes compile through
# precisely the same Dockerfile and tools/frr.sh stages.
checkout_remote() {
    local url="$1" ref="$2" sha="$3" dst="$4"
    git init -q "$dst"
    git -C "$dst" remote add origin "$url"
    if release_version "$ref"; then
        git -C "$dst" fetch -q --no-tags --depth 1 origin "+refs/tags/$ref:refs/tags/$ref"
        git -C "$dst" checkout -q --detach "$ref"
    else
        git -C "$dst" fetch -q --no-tags --depth 1 origin "$ref"
        git -C "$dst" checkout -q --detach FETCH_HEAD
    fi
    [[ "$(git -C "$dst" rev-parse HEAD)" == "$sha" ]] ||
        die "GitHub checkout changed during build: $url ($ref)"
}

# Local FRR must retain REAL Git metadata because the managed patch installer
# invokes git apply and verifies it is inside an FRR Git checkout. Fetch only
# HEAD from the local repository (no GitHub access, no historical clone), then
# overlay working modifications/new files and remove locally deleted files.
stage_local_frr() {
    local src="$1" dst="$2" version="$3" sha="$4" names path
    names="$workdir/frr-files.list"
    git init -q "$dst"
    git -C "$dst" remote add origin "$src"
    git -C "$dst" fetch -q --no-tags --depth 1 origin "$sha"
    git -C "$dst" checkout -q --detach FETCH_HEAD
    if release_version "$version"; then
        git -C "$dst" fetch -q --no-tags --depth 1 origin \
            "+refs/tags/$version:refs/tags/$version"
    fi
    (
        cd "$src"
        git ls-files --cached --others --exclude-standard -z | \
            while IFS= read -r -d '' path; do
                if [[ -f "$path" || -L "$path" ]]; then
                    printf '%s\0' "$path"
                fi
            done > "$names"
        tar --null -T "$names" -cf -
    ) | (cd "$dst" && tar -xf -)
    while IFS= read -r -d '' path; do
        rm -f -- "$dst/$path"
    done < <(git -C "$src" diff --name-only --diff-filter=D -z HEAD)
    rm -f "$names"
    [[ "$(git -C "$dst" rev-parse HEAD)" == "$sha" ]] ||
        die 'staged FRR revision does not match the local checkout'
}

# Resolve platform from the build machine, rather than defaulting an ARM Mac
# to amd64. The workflow passes its matrix platform explicitly.
host_platform() {
    case "$(uname -m)" in
        x86_64|amd64) printf 'linux/amd64\n' ;;
        aarch64|arm64) printf 'linux/arm64\n' ;;
        *) die 'cannot infer platform; specify --platform linux/amd64|linux/arm64' ;;
    esac
}

[[ $# -gt 0 ]] || { usage; exit 2; }
operation="$1"
shift
case "$operation" in
    --help|-h) usage; exit 0 ;;
    --test)
        [[ $# -eq 1 ]] || die 'usage: --test IMAGE:TAG'
        command -v docker >/dev/null 2>&1 || die 'Docker CLI is required'
        docker image inspect "$1" >/dev/null 2>&1 ||
            die "image not found locally: $1; build it with --local/--build or pull it first"
        exec "$root/tools/docker-test.sh" "$1" ;;
    --verify)
        [[ $# -le 1 ]] || die 'usage: --verify [IMAGE:TAG]'
        image="${1:-openeigrp:${USER:-$(id -un)}}"
        command -v docker >/dev/null 2>&1 || die 'Docker CLI is required'
        docker image inspect "$image" >/dev/null 2>&1 ||
            die "image not found locally: $image; build with --local/--build or pull it first"
        exec "$root/tools/docker-verify.sh" "$image" ;;
    --deploy)
        [[ $# -gt 0 ]] || die '--deploy requires IMAGE:TAG'
        image="$1"; shift
        name=openeigrp; volume=openeigrp-vol; network=bridge
        while [[ $# -gt 0 ]]; do
            case "$1" in
                --name) [[ $# -ge 2 ]] || die 'missing --name'; name="$2"; shift 2 ;;
                --volume) [[ $# -ge 2 ]] || die 'missing --volume'; volume="$2"; shift 2 ;;
                --network) [[ $# -ge 2 ]] || die 'missing --network'; network="$2"; shift 2 ;;
                *) die "unknown --deploy option: $1" ;;
            esac
        done
        [[ "$network" == bridge || "$network" == host ]] || die '--network must be bridge or host'
        [[ "$name" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || die 'invalid container name'
        [[ "$volume" =~ ^[a-zA-Z0-9][a-zA-Z0-9_.-]*$ ]] || die 'invalid volume name'
        command -v docker >/dev/null 2>&1 || die 'Docker CLI is required'
        if ! docker image inspect "$image" >/dev/null 2>&1; then
            echo "Image $image is not local; pulling it first."
            docker pull "$image" || die "cannot pull $image (build locally first or check GHCR access)"
        fi
        docker volume create "$volume" >/dev/null
        container_id="$(docker run -d --name "$name" --network "$network" \
            --cap-add NET_ADMIN --cap-add NET_RAW --cap-add SYS_ADMIN \
            --mount "type=volume,src=$volume,dst=/etc/frr" "$image")"
        # A live watchfrr process alone does not establish operational FRR.
        # In particular, zebra/mgmtd can fail their privilege setup while
        # eigrpd and staticd remain alive. Gate deployment on all daemons.
        deployment_ready() {
            local daemon
            for daemon in watchfrr mgmtd zebra staticd eigrpd; do
                docker exec "$name" pidof "$daemon" >/dev/null 2>&1 || return 1
            done
            docker exec "$name" vtysh -c 'show version' >/dev/null 2>&1 || return 1
            docker exec "$name" vtysh -d eigrpd -c 'show eigrp tech-support' >/dev/null 2>&1 || return 1
        }
        ready=0
        for ((i = 0; i < 30; i++)); do
            if [[ "$(docker inspect -f '{{.State.Running}}' "$name" 2>/dev/null)" != true ]]; then
                echo "error: container $name exited during startup (ID: $container_id)" >&2
                docker logs "$name" >&2 || true
                exit 1
            fi
            if deployment_ready; then ready=1; break; fi
            sleep 2
        done
        if [[ "$ready" -ne 1 ]]; then
            echo "error: required FRR daemons or vtysh not ready in $name" >&2
            docker logs "$name" >&2 || true
            docker exec "$name" ps -ef >&2 || true
            exit 1
        fi
        printf 'Container: %s\nImage:     %s\nConfig:    %s\nOpen CLI:  docker exec -it %s vtysh\n' \
            "$name" "$image" "$volume" "$name"
        exit 0 ;;
    --local|--build) ;;
    *) usage >&2; exit 2 ;;
esac

version="${USER:-$(id -un)}"
if [[ "$operation" == --build ]]; then
    [[ $# -gt 0 && "$1" != --* ]] || die '--build requires a version'
    version="$1"; shift
elif [[ $# -gt 0 && "$1" != --* ]]; then
    version="$1"; shift
fi
version_valid "$version" || die "invalid image version: $version"
if [[ "$operation" == --build ]]; then
    image="ghcr.io/diivious/eigrp-frr:$version"
else
    image="openeigrp:$version"
fi
platform="$(host_platform)"
jobs=4
pull_base=0
eigrp_ref="$version"; frr_ref="$version"
eigrp_ref_set=0; frr_ref_set=0
eigrp_root="$root"; frr_root=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform) [[ $# -ge 2 ]] || die 'missing --platform'; platform="$2"; shift 2 ;;
        --image) [[ $# -ge 2 ]] || die 'missing --image'; image="$2"; shift 2 ;;
        --jobs) [[ $# -ge 2 ]] || die 'missing --jobs'; jobs="$2"; shift 2 ;;
        --eigrp-ref) [[ $# -ge 2 ]] || die 'missing --eigrp-ref'; eigrp_ref="$2"; eigrp_ref_set=1; shift 2 ;;
        --frr-ref) [[ $# -ge 2 ]] || die 'missing --frr-ref'; frr_ref="$2"; frr_ref_set=1; shift 2 ;;
        --eigrp-root) [[ $# -ge 2 ]] || die 'missing --eigrp-root'; eigrp_root="$2"; shift 2 ;;
        --frr-root) [[ $# -ge 2 ]] || die 'missing --frr-root'; frr_root="$2"; shift 2 ;;
        --pull-base) pull_base=1; shift ;;
        *) die "unknown option: $1" ;;
    esac
done
[[ "$platform" == linux/amd64 || "$platform" == linux/arm64 ]] || die 'only linux/amd64 and linux/arm64 are supported'
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die '--jobs requires a positive integer'
[[ -n "$image" && "$image" == *:* ]] || die '--image requires an image name and tag'
command -v docker >/dev/null 2>&1 || die 'Docker CLI is required'
command -v git >/dev/null 2>&1 || die 'git is required'

if [[ "$operation" == --local ]]; then
    [[ "$eigrp_ref_set" == 0 && "$frr_ref_set" == 0 ]] || die '--local does not accept remote refs'
    [[ -d "$eigrp_root" ]] || die "local EIGRP checkout not found: $eigrp_root"
    eigrp_root="$(cd "$eigrp_root" && pwd)"
    if [[ -z "$frr_root" ]]; then
        for candidate in "$eigrp_root/../frr" "$HOME/projects/frr"; do
            if [[ -f "$candidate/bootstrap.sh" && -d "$candidate/tests" ]]; then
                frr_root="$candidate"
                break
            fi
        done
    fi
    [[ -n "$frr_root" && -d "$frr_root" ]] || die 'local FRR checkout not found; supply --frr-root PATH'
    frr_root="$(cd "$frr_root" && pwd)"
    require_git_checkout "$eigrp_root" EIGRP
    require_git_checkout "$frr_root" FRR
    [[ -f "$eigrp_root/tools/frr.sh" && -f "$frr_root/bootstrap.sh" ]] || die 'incorrect local source roots'
    eigrp_sha="$(git -C "$eigrp_root" rev-parse HEAD)"
    frr_sha="$(git -C "$frr_root" rev-parse HEAD)"
    if release_version "$version"; then
        verify_local_release "$eigrp_root" EIGRP "$version"
        verify_local_release "$frr_root" FRR "$version"
    fi
else
    [[ -z "$frr_root" && "$eigrp_root" == "$root" ]] || die 'source roots apply only to --local'
    if release_version "$version"; then
        [[ "$eigrp_ref_set" == 0 && "$frr_ref_set" == 0 ]] || \
            die 'release builds use matching GitHub tags in both repositories'
    else
        [[ "$eigrp_ref_set" == 1 ]] || die 'development builds require --eigrp-ref'
        [[ "$frr_ref_set" == 1 ]] || frr_ref="$development_frr_sha"
        release_version "$eigrp_ref" && die 'development version cannot use a release tag'
    fi
    ref_valid "$eigrp_ref" || die "invalid EIGRP ref: $eigrp_ref"
    ref_valid "$frr_ref" || die "invalid FRR ref: $frr_ref"
    eigrp_sha="$(resolve_sha "$eigrp_url" "$eigrp_ref")" || die "cannot resolve EIGRP ref $eigrp_ref"
    frr_sha="$(resolve_sha "$frr_url" "$frr_ref")" || die "cannot resolve FRR ref $frr_ref"
fi

# Temporary source contexts are not part of the committed repository or the
# resulting runtime image. Local EIGRP is cloned locally to keep Git tag data
# available for the existing release injector, then working edits are overlaid.
command -v rsync >/dev/null 2>&1 || die 'rsync is required to prepare build sources'
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
mkdir -p "$workdir/eigrp" "$workdir/frr"
if [[ "$operation" == --local ]]; then
    git clone -q --local --no-hardlinks "$eigrp_root" "$workdir/eigrp"
    git -C "$workdir/eigrp" checkout -q --detach "$eigrp_sha"
    rsync -a --delete --exclude='.git/' --exclude='__pycache__/' \
        --exclude='.pytest_cache/' --exclude='*.o' --exclude='*.lo' \
        --exclude='*.la' --exclude='obj/' --exclude='logs/' \
        "$eigrp_root/" "$workdir/eigrp/"
    stage_local_frr "$frr_root" "$workdir/frr" "$version" "$frr_sha"
    eigrp_ref="local:$eigrp_root"
    frr_ref="local:$frr_root"
else
    checkout_remote "$eigrp_url" "$eigrp_ref" "$eigrp_sha" "$workdir/eigrp"
    checkout_remote "$frr_url" "$frr_ref" "$frr_sha" "$workdir/frr"
fi
# Named local BuildKit contexts honor their own .dockerignore files. The
# eigrp checkout's packaging-only .dockerignore would otherwise discard its
# protocol source tree; remove it only from these temporary source snapshots.
rm -f "$workdir/eigrp/.dockerignore" "$workdir/frr/.dockerignore"
# Local clones and GitHub fetches generate volatile index/reflog metadata that
# would needlessly invalidate BuildKit caches even when the source is unchanged.
# HEAD, tags and Git objects are retained for release detection/revision checks.
for src in "$workdir/eigrp" "$workdir/frr"; do
    rm -rf "$src/.git/logs"
    rm -f "$src/.git/index" "$src/.git/FETCH_HEAD"
done

printf 'OpenEIGRP: %s @ %s\nFRR:       %s @ %s\nAlpine:    %s\nImage:     %s [%s]\n' \
    "${eigrp_ref:-local}" "$eigrp_sha" "${frr_ref:-local}" "$frr_sha" "$alpine_version" "$image" "$platform"

build_args=(buildx build --load --platform "$platform")
if [[ "$pull_base" -eq 1 ]]; then
    build_args+=(--pull)
fi
docker "${build_args[@]}" \
    --build-context "eigrp-source=$workdir/eigrp" \
    --build-context "frr-source=$workdir/frr" \
    -f "$root/docker/Dockerfile" \
    --build-arg "ALPINE_VERSION=$alpine_version" \
    --build-arg "EIGRP_SHA=$eigrp_sha" \
    --build-arg "FRR_SHA=$frr_sha" \
    --build-arg "EIGRP_VERSION=$version" \
    --build-arg "BUILD_JOBS=$jobs" \
    --build-arg "SOURCE_MODE=${operation#--}" \
    -t "$image" "$root"
