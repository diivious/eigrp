#!/usr/bin/env bash
# Copyright (C) 2026 Donnie V. Savage
# Simulated account setup: no sudo, passwords, system changes, or network use.
set -Eeuo pipefail
script="$(cd "$(dirname "$0")" && pwd)/netlab-provision.sh"
tmp="$(mktemp -d)"; trap 'rm -rf -- "$tmp"' EXIT
export HOME="$tmp/admin" NETLAB_DATA_ROOT="$tmp/netlab-data"
export ACCOUNT_HOME="$tmp/netlab" ACCOUNT_EVENTS="$tmp/events"
mkdir -p "$HOME" "$tmp/bin" "$NETLAB_DATA_ROOT"
export PATH="$tmp/bin:$PATH"
for name in ip brctl containerlab visudo; do
  printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/$name"
  chmod 755 "$tmp/bin/$name"
done

source <(sed '$d' "$script")

getent() {
  case "$1:$2" in
    passwd:netlab)
      [[ -f "$tmp/account-created" ]] || return 2
      printf 'netlab:x:1019:1019:,,,: %s:/bin/bash\n' "$ACCOUNT_HOME" | sed 's/: /:/' ;;
    group:docker|group:kvm|group:clab_admins) echo "$2:x:999:netlab" ;;
    *) return 2 ;;
  esac
}
id() {
  [[ "${1:-}" == -u && "${2:-}" == netlab ]] && { echo 1019; return 0; }
  command id "$@"
}
stat() {
  [[ "${1:-}" == -c && "${2:-}" == %u ]] && { echo 0; return 0; }
  command stat "$@"
}
sudo() {
  local op="${1:-}"; shift || true
  printf '%s %s\n' "$op" "$*" >> "$ACCOUNT_EVENTS"
  case "$op" in
    useradd)
      [[ ! -e "$tmp/account-created" ]]
      mkdir -p "$ACCOUNT_HOME"
      touch "$tmp/account-created" ;;
    groupadd|usermod|systemctl|chmod) : ;;
    visudo) : ;;
    test) test "$@" ;;
    install)
      local -a args=()
      while (($#)); do
        case "$1" in
          -o|-g|-m) shift 2 ;;
          -d) args+=("-d"); shift ;;
          *) args+=("$1"); shift ;;
        esac
      done
      if [[ "${args[-1]}" == /etc/sudoers.d/netlab-network ]]; then
        cp "${args[-2]}" "$tmp/sudoers-installed"
      else
        command install "${args[@]}"
      fi ;;
    -u)
      [[ "$1" == netlab && "$2" == -H ]]; shift 2
      case "$1" in
        python3)
          [[ "$2" == -m && "$3" == venv ]]
          local last="${@: -1}"
          mkdir -p "$last/bin"
          printf '#!/bin/sh\nexit 0\n' > "$last/bin/python3"
          printf '#!/bin/sh\nexit 0\n' > "$last/bin/netlab"
          chmod +x "$last/bin/python3" "$last/bin/netlab" ;;
        bash) HOME="$ACCOUNT_HOME" bash -c "$3" ;;
        docker) : ;;
        *) [[ "$1" == "$ACCOUNT_HOME/netlab-venv/bin/python3" || "$1" == "$ACCOUNT_HOME/netlab-venv/bin/netlab" ]] ;;
      esac ;;
    *) echo "Unexpected mock sudo operation: $op" >&2; return 2 ;;
  esac
}

ensure_execution_account > "$tmp/first-run"
[[ -d "$ACCOUNT_HOME/labs" && -d "$NETLAB_DATA_ROOT/runs" && -d "$NETLAB_DATA_ROOT/results" ]]
[[ -x "$ACCOUNT_HOME/netlab-venv/bin/netlab" ]]
[[ -f "$ACCOUNT_HOME/.ssh/authorized_keys" ]]
grep -Fq 'netlab ALL=(root) NOPASSWD:' "$tmp/sudoers-installed"
! grep -Fq 'ALL=(ALL)' "$tmp/sudoers-installed"
[[ "$(grep -c '^useradd ' "$ACCOUNT_EVENTS")" == 1 ]]
[[ "$(grep -c '^usermod -aG docker,clab_admins,kvm netlab$' "$ACCOUNT_EVENTS")" == 1 ]]
grep -Fq 'export PATH="$HOME/bin:$HOME/netlab-venv/bin:/usr/local/sbin:/usr/sbin:/sbin:$PATH"' "$ACCOUNT_HOME/.bashrc"
grep -Fq 'export PATH="$HOME/bin:$HOME/netlab-venv/bin:/usr/local/sbin:/usr/sbin:/sbin:$PATH"' "$ACCOUNT_HOME/.profile"
# User-provided SSH keys and password state cannot be changed by subsequent setup.
printf 'ssh-ed25519 retained-test-key\n' >> "$ACCOUNT_HOME/.ssh/authorized_keys"
ensure_execution_account > "$tmp/second-run"
[[ "$(grep -c '^useradd ' "$ACCOUNT_EVENTS")" == 1 ]]
[[ "$(grep -c 'retained-test-key' "$ACCOUNT_HOME/.ssh/authorized_keys")" == 1 ]]
[[ "$(grep -c '# netlab-provision execution PATH v3' "$ACCOUNT_HOME/.bashrc")" == 1 ]]
[[ "$(grep -c '# netlab-provision execution PATH v3' "$ACCOUNT_HOME/.profile")" == 1 ]]
! grep -Eq 'passwd|chpasswd|userdel' "$ACCOUNT_EVENTS"
echo 'PASS: dedicated account creation, no password, SSH key retention, per-user venv, group assignment, scoped sudo policy, runtime dirs, PATH and repeatability'
