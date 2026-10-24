#!/usr/bin/env bash
# Copyright (C) 2026 Donnie V. Savage
# Debian 13 bare-metal netlab/Containerlab host provisioning.
# The provisioner never deletes Docker images, volumes, or existing labs.
set -Eeuo pipefail

readonly EIGRP_IMAGE='ghcr.io/diivious/openeigrp-frr'
readonly CSR_IMAGE='vrnetlab/cisco_csr1000v'
readonly CSR_NETLAB_IMAGE='vrnetlab/vr-csr'
readonly C8000V_IMAGE='vrnetlab/cisco_c8000v'
readonly C8000V_NETLAB_IMAGE='vrnetlab/vr-c8000v'
readonly VENV="${NETLAB_VENV:-$HOME/netlab-venv}"
readonly IMAGE_DIR="${NETLAB_CSR_DIR:-$HOME/netlab/images/csr1kv}"
readonly C8000V_DIR="${NETLAB_C8000V_DIR:-$HOME/netlab/images/csr8kv}"
readonly VRNETLAB_DIR="${NETLAB_VRNETLAB_DIR:-$HOME/netlab/vrnetlab}"
readonly ISO_TIMEOUT="${NETLAB_CSR_ISO_TIMEOUT:-45m}"
readonly CSR_BUILD_TIMEOUT="${NETLAB_CSR_BUILD_TIMEOUT:-60m}"
readonly ISO_MEMORY="${NETLAB_CSR_ISO_MEMORY:-4096}"
readonly ISO_VCPUS="${NETLAB_CSR_ISO_VCPUS:-2}"
readonly EXEC_USER='netlab'
readonly EXEC_DATA_ROOT="${NETLAB_DATA_ROOT:-/home/netlab-data}"
readonly SCRIPT_PATH="$(realpath "${BASH_SOURCE[0]}")"

error() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
note()  { printf '\n==> %s\n' "$*"; }
warn()  { printf 'WARNING: %s\n' "$*" >&2; }
need()  { command -v "$1" >/dev/null 2>&1 || error "Required command missing: $1 (run --setup first)"; }

usage() {
  cat <<'HELP'
Usage:
  ./netlab-provision.sh --setup [--csr-source FILE|URL]... [--c8000v-source FILE|URL]...
  ./netlab-provision.sh --update
  ./netlab-provision.sh --image  [--csr-source FILE|URL]... [--c8000v-source FILE|URL]...
  ./netlab-provision.sh --images [--csr-source FILE|URL]... [--c8000v-source FILE|URL]...
  ./netlab-provision.sh --help

--setup   Install prerequisite Debian packages, netlab, Docker/Containerlab,
          Ansible, and graph tools. Create/update the password-unset netlab
          execution account, separate Python environment, run/results folders,
          and scoped noninteractive networking privileges. Stage images when possible.
--update  Upgrade Debian packages, netlab/Ansible/Containerlab, and vrnetlab
          build tooling (if previously cloned), plus netlab execution account
          dependencies. Does not update router images.
--image   Discover highest published X.Y.Z OpenEIGRP GHCR release, pull AMD64,
          preserve all earlier version tags, and optionally build CSR1000v and Catalyst 8000V.
--images  Alias for --image.

Cisco source directories (ISO, QCOW2, VMDK):
  CSR1000v:          ~/netlab/images/csr1kv/
  Catalyst 8000V:    ~/netlab/images/csr8kv/
Supply files directly or with --csr-source/--c8000v-source PATH|file://|https://.
Cisco installation ISOs are installed onto 8G QCOW2 disks with KVM/QEMU,
then packaged with the matching vrnetlab builder. A local VNC console is exposed during ISO install
(see README). A failed/partial ISO install never gets a release Docker tag.
The script creates netlab aliases vrnetlab/vr-csr:VERSION and
vrnetlab/vr-c8000v:VERSION. Catalyst 8000V builds autonomous routing mode
for EIGRP labs. All versioned images are retained.

Environment overrides:
  NETLAB_VENV, NETLAB_CSR_DIR, NETLAB_C8000V_DIR, NETLAB_VRNETLAB_DIR
  NETLAB_CSR_ISO_TIMEOUT (default 45m), NETLAB_CSR_ISO_MEMORY (MiB),
  NETLAB_CSR_ISO_VCPUS, NETLAB_CSR_BUILD_TIMEOUT (default 60m)
  GHCR_USER and GHCR_TOKEN (both, only for private GHCR packages)
  NETLAB_DATA_ROOT (default /home/netlab-data; run/results only)

Run as your administrative Debian user with sudo access; do NOT run as root.
A new `netlab` execution account is created with no usable password; existing
accounts/SSH keys and passwords are preserved. SSH key setup is manual.
Containerlab/docker groups are root-equivalent. A narrowly scoped sudoers
entry allows noninteractive Linux bridge/network commands (review README).
A new login after --setup may be required to activate Docker group membership.
Run netlab test clab after that login to validate the installation.
No Docker cleanup/prune commands are run by this script.
HELP
}

check_host() {
  [[ $(id -u) -ne 0 ]] || error 'Run as your normal Debian user, not root/sudo.'
  [[ -r /etc/os-release ]] || error 'Cannot determine operating system.'
  # shellcheck disable=SC1091
  . /etc/os-release
  [[ "${ID:-}" == debian && "${VERSION_ID:-}" == 13 ]] ||
    error "This provisioner targets Debian 13; found ${PRETTY_NAME:-unknown}."
  [[ $(uname -m) == x86_64 ]] || error 'This provisioner requires x86_64/AMD64.'
}

# Debian places some administration tools in /usr/sbin. Inherit this PATH
# for netlab subprocesses as well as invocations from non-login shells.
export PATH="/usr/local/sbin:/usr/sbin:/sbin:$PATH"

sudo_ready() { need sudo; sudo -v; }

apt_packages() {
  export DEBIAN_FRONTEND=noninteractive
  sudo apt-get update
  sudo apt-get install -y \
    ca-certificates curl git jq make rsync wget openssh-server sudo \
    python3 python3-pip python3-venv python3-dev build-essential \
    libffi-dev libssl-dev pkg-config \
    iproute2 iputils-ping bridge-utils tcpdump traceroute sshpass \
    qemu-utils qemu-system-x86 kmod acl file coreutils util-linux
}

ensure_shell_path() {
  local profile line
  line='export PATH="$HOME/bin:$HOME/netlab-venv/bin:$HOME/.local/bin:/usr/local/sbin:/usr/sbin:/sbin:$PATH"'
  mkdir -p "$HOME/bin" "$HOME/.local/bin"
  for profile in "$HOME/.bashrc" "$HOME/.profile"; do
    if ! grep -Fq '# netlab-provision managed PATH v2' "$profile" 2>/dev/null; then
      {
        printf '\n# netlab-provision managed PATH v2\n'
        printf '%s\n' "$line"
      } >> "$profile"
    fi
  done
  if [[ "$VENV" != "$HOME/netlab-venv" ]]; then
    warn 'Custom NETLAB_VENV: append its bin directory to PATH yourself.'
  fi
}

ensure_venv() {
  need python3
  if [[ ! -x "$VENV/bin/python3" ]]; then
    note "Creating Python environment at $VENV"
    python3 -m venv --system-site-packages "$VENV"
  fi
  export PATH="$VENV/bin:$PATH"
  "$VENV/bin/python3" -m pip install --upgrade pip networklab
  ensure_shell_path
  ln -sfn "$VENV/bin/netlab" "$HOME/.local/bin/netlab"
}

netlab_install() {
  need netlab
  # Run separately: avoids APT lock contention in vendor installers.
  netlab install -y ubuntu
  netlab install -y containerlab
  netlab install -y ansible
  netlab install -y graph
}

# The netlab execution account is distinct from the human administrator. Never
# copy the administrator's SSH keys or change an existing user's password.
# Its working files live on the large /home filesystem already hosting images.
ensure_execution_account() {
  local exec_home exec_venv pkg_version name
  local -a runtime_pkgs=(networklab ansible ansible-core ansible-pylibssh netmiko paramiko jmespath textfsm ttp ntc-templates yamllint yq)
  if getent passwd "$EXEC_USER" >/dev/null; then
    [[ "$(id -u "$EXEC_USER")" != 0 ]] || error "$EXEC_USER unexpectedly has UID 0"
    note "Preserving existing account: $EXEC_USER (password/SSH settings unchanged)"
  else
    note "Creating $EXEC_USER account without a password (SSH keys added separately)"
    sudo useradd --create-home --shell /bin/bash "$EXEC_USER"
  fi
  exec_home="$(getent passwd "$EXEC_USER" | cut -d: -f6)"
  [[ -n "$exec_home" && -d "$exec_home" && "$exec_home" != / ]] ||
    error "Invalid home for $EXEC_USER: $exec_home"
  exec_venv="$exec_home/netlab-venv"

  # netlab's Containerlab provider requires both groups. KVM is included so
  # native host-side QEMU tools work for ISO conversion under the account.
  sudo groupadd -f clab_admins
  for name in docker kvm clab_admins; do
    getent group "$name" >/dev/null || error "Expected system group not found: $name"
  done
  sudo usermod -aG docker,clab_admins,kvm "$EXEC_USER"
  sudo install -d -o "$EXEC_USER" -g "$EXEC_USER" -m 0750 \
    "$exec_home/bin" "$exec_home/labs" "$exec_home/.local" "$exec_home/.local/bin"
  sudo install -d -o "$EXEC_USER" -g "$EXEC_USER" -m 0700 "$exec_home/.ssh"
  # Empty key file on first setup, preserving any authorized keys on reruns.
  if [[ ! -e "$exec_home/.ssh/authorized_keys" ]]; then
    sudo install -o "$EXEC_USER" -g "$EXEC_USER" -m 0600 /dev/null \
      "$exec_home/.ssh/authorized_keys"
  fi
  # Do not chown or reset the existing shared Docker/containerd/images trees.
  # A newly created directory is owned by the execution user; a rerun simply
  # keeps the intended run/results ownership and permissions.
  sudo install -d -o "$EXEC_USER" -g "$EXEC_USER" -m 0750 \
    "$EXEC_DATA_ROOT/runs" "$EXEC_DATA_ROOT/results"

  if ! sudo test -x "$exec_venv/bin/python3"; then
    note "Creating execution Python environment: $exec_venv"
    sudo -u "$EXEC_USER" -H python3 -m venv --system-site-packages "$exec_venv"
  fi
  # Match the administrator's known-working Python versions when possible.
  # This prevents a first-time execution account from silently installing a
  # different netlab/Ansible release than the one already verified on this host.
  if [[ -x "$VENV/bin/python3" ]]; then
    for name in "${!runtime_pkgs[@]}"; do
      pkg_version="$("$VENV/bin/python3" -m pip show "${runtime_pkgs[$name]}" 2>/dev/null |
        sed -n 's/^Version: //p' | head -n 1 || true)"
      if [[ "$pkg_version" =~ ^[0-9]+([.][0-9]+)*([a-zA-Z0-9.-]+)?$ ]]; then
        runtime_pkgs[$name]="${runtime_pkgs[$name]}==$pkg_version"
      fi
    done
  fi
  note "Installing runtime packages for $EXEC_USER (matching administrative netlab versions)"
  sudo -u "$EXEC_USER" -H "$exec_venv/bin/python3" -m pip install --upgrade \
    pip "${runtime_pkgs[@]}"

  # Non-login SSH commands and interactive Bash must both locate netlab. The
  # remote runner should still use the absolute path for deterministic launch.
  sudo -u "$EXEC_USER" -H bash -c '
    for profile in "$HOME/.bashrc" "$HOME/.profile"; do
      if ! grep -Fq "# netlab-provision execution PATH v3" "$profile" 2>/dev/null; then
        printf "\n%s\n%s\n" "# netlab-provision execution PATH v3" \
          "export PATH=\"\$HOME/bin:\$HOME/netlab-venv/bin:/usr/local/sbin:/usr/sbin:/sbin:\$PATH\"" >> "$profile"
      fi
    done
  '
  # Enable only known network tools. Membership in docker/clab_admins already
  # confers root-equivalent authority; don't grant sudo ALL to this account.
  configure_execution_sudoers
  sudo systemctl enable --now ssh
  sudo -u "$EXEC_USER" -H "$exec_venv/bin/netlab" version >/dev/null
  if sudo -u "$EXEC_USER" -H docker info >/dev/null 2>&1; then
    note "$EXEC_USER can access the existing Docker engine and images"
  else
    warn "$EXEC_USER cannot access Docker yet: check group membership and service"
  fi
  note "Runtime: $EXEC_USER:$exec_home; work: $EXEC_DATA_ROOT/runs; results: $EXEC_DATA_ROOT/results"
  note "SSH keys are not copied. Set an account password later, or add keys to $exec_home/.ssh/authorized_keys."
}

configure_execution_sudoers() {
  local sudo_file='/etc/sudoers.d/netlab-network' temp ip_cmd brctl_cmd clab_cmd
  need visudo
  ip_cmd="$(command -v ip)"
  brctl_cmd="$(command -v brctl)"
  clab_cmd="$(command -v containerlab)"
  [[ -x "$ip_cmd" && -x "$brctl_cmd" && -x "$clab_cmd" ]] ||
    error 'Cannot configure remote lab networking sudo rules: missing ip/brctl/containerlab'
  temp="$(mktemp)"
  {
    printf '# Managed by netlab-provision.sh: Containerlab and bridge operations\n'
    printf '# Members of docker and clab_admins already have root-equivalent access.\n'
    printf '%s ALL=(root) NOPASSWD: %s, %s, %s\n' \
      "$EXEC_USER" "$ip_cmd" "$brctl_cmd" "$clab_cmd"
  } > "$temp"
  if ! sudo visudo -cf "$temp" >/dev/null; then
    rm -f "$temp"
    error 'Refusing to install invalid sudoers policy'
  fi
  sudo install -o root -g root -m 0440 "$temp" "$sudo_file"
  rm -f "$temp"
  sudo visudo -cf "$sudo_file" >/dev/null || error 'Installed sudoers policy failed validation'
  # Containerlab's official sudo-less model requires setuid-root plus group
  # membership (v0.63+). Do not change an unexpectedly non-root-owned binary.
  if [[ ! -u "$clab_cmd" ]]; then
    [[ "$(stat -c %u "$clab_cmd")" == 0 ]] ||
      error "Containerlab executable is not root-owned: $clab_cmd"
    note 'Enabling root-owned Containerlab setuid helper for clab_admins'
    sudo chmod u+s "$clab_cmd"
  fi
}

check_tools() {
  for tool in netlab docker containerlab ansible-playbook; do
    if command -v "$tool" >/dev/null 2>&1; then
      printf '  %-18s %s\n' "$tool" "$(command -v "$tool")"
    else
      warn "$tool not available on PATH"
    fi
  done
  if [[ -e /dev/kvm ]]; then
    note '/dev/kvm is present; CSR1000v hardware acceleration is available.'
  else
    warn '/dev/kvm absent: check BIOS Intel VT-x and kvm_intel; CSR1000v requires KVM.'
  fi
  if [[ -f /var/run/reboot-required ]]; then
    warn 'A reboot is recommended (kernel or system components were updated).'
  fi
}

setup() {
  sudo_ready
  note 'Installing Debian 13 prerequisite packages'
  apt_packages
  ensure_venv
  note 'Installing netlab dependencies'
  netlab_install
  sudo systemctl enable --now docker
  ensure_execution_account
  if getent group kvm >/dev/null 2>&1; then
    sudo usermod -aG kvm "$(id -un)"
  fi
  sudo modprobe kvm_intel 2>/dev/null || true
  if [[ -c /dev/kvm && ! -w /dev/kvm ]]; then
    # The newly added kvm group becomes active only after re-login. Allow
    # initial ISO installation now; durable permission comes from kvm group.
    sudo setfacl -m "u:$(id -un):rw" /dev/kvm ||
      warn 'Cannot grant initial /dev/kvm access; reconnect before processing Cisco ISOs.'
  fi
  mkdir -p "$IMAGE_DIR" "$C8000V_DIR" "$HOME/netlab/labs"
  # The first setup login may not yet have the Docker supplementary group.
  # sg runs a one-shot image operation with newly assigned Docker group.
  # Don't silently claim images were installed when access is unavailable.
  note 'Preparing OpenEIGRP and any locally available Cisco images'
  if docker info >/dev/null 2>&1; then
    images
  else
    sudo usermod -aG docker "$(id -un)"
    if command -v sg >/dev/null 2>&1; then
      local args=() src cmd
      for src in "${csr_sources[@]}"; do args+=(--csr-source "$src"); done
      for src in "${c8000v_sources[@]}"; do args+=(--c8000v-source "$src"); done
      printf -v cmd '%q ' "$SCRIPT_PATH" --images "${args[@]}"
      if ! sg docker -c "$cmd"; then
        warn 'Image preparation not completed; reconnect to activate the Docker group, then run --images.'
      fi
    else
      warn 'Docker group not yet active; reconnect, then run --images.'
    fi
  fi
  note 'Installation summary'
  check_tools
  note 'Re-login to activate Docker group if not already active. Run: netlab test clab'
}

update() {
  sudo_ready
  note 'Updating Debian 13 packages'
  apt_packages
  sudo apt-get -y full-upgrade
  ensure_venv
  note 'Refreshing netlab and configuration utilities'
  netlab install -y ubuntu
  netlab install -y ansible
  netlab install -y graph
  if ! command -v containerlab >/dev/null 2>&1; then
    note 'Containerlab not found; installing it'
    netlab install -y containerlab
  else
    note 'Upgrading Containerlab'
    sudo containerlab version upgrade
  fi
  if [[ -d "$VRNETLAB_DIR/.git" ]]; then
    note 'Updating vrnetlab build scripts (fast-forward only)'
    git -C "$VRNETLAB_DIR" pull --ff-only
  fi
  sudo systemctl enable --now docker
  ensure_execution_account
  check_tools
  note 'Update complete. No router images or lab data removed.'
}

check_docker() {
  need curl; need jq; need docker
  docker info >/dev/null 2>&1 ||
    error 'Cannot access Docker. Start Docker and log out/in after --setup for group membership.'
}

# GHCR does not publish a moving :latest tag for OpenEIGRP. Discover immutable
# release tags through the OCI distribution tags/list endpoint instead.
registry_token() {
  local args=(-fsSL --retry 2 --connect-timeout 15 --max-time 40)
  if [[ -n ${GHCR_USER:-} || -n ${GHCR_TOKEN:-} ]]; then
    [[ -n ${GHCR_USER:-} && -n ${GHCR_TOKEN:-} ]] ||
      error 'Both GHCR_USER and GHCR_TOKEN must be supplied for private images.'
    args+=(--user "$GHCR_USER:$GHCR_TOKEN")
  fi
  curl "${args[@]}" \
    'https://ghcr.io/token?service=ghcr.io&scope=repository:diivious/openeigrp-frr:pull' |
      jq -er '.token // .access_token // empty'
}

latest_eigrp_tag() {
  local token="$1" page=0 last='' headers payload tags tag next ver
  headers="$(mktemp)"
  payload="$(mktemp)"
  while ((page < 100)); do
    local url='https://ghcr.io/v2/diivious/openeigrp-frr/tags/list?n=100'
    [[ -z "$last" ]] || url+="&last=$last"
    if ! curl -fsSL --retry 2 --connect-timeout 15 --max-time 60 \
      -D "$headers" -o "$payload" \
      -H "Authorization: Bearer $token" "$url"; then
      rm -f "$headers" "$payload"
      error 'Cannot list GHCR tags. Check package visibility or GHCR credentials.'
    fi
    tags="$(jq -er '(.tags // []) | if type == "array" then .[]? else error("invalid tags") end' "$payload" || true)"
    if [[ -n "$tags" ]]; then
      printf '%s\n' "$tags" | grep -E '^[0-9]+\.[0-9]+\.[0-9]+$' || true
    fi
    next="$(grep -i '^link:' "$headers" | grep -i 'rel="next"' || true)"
    [[ -n "$next" ]] || { rm -f "$headers" "$payload"; return 0; }
    tag="$(jq -r '.tags[-1] // empty' "$payload")"
    [[ -n "$tag" && "$tag" != "$last" && "$tag" =~ ^[a-zA-Z0-9_.-]+$ ]] || {
      rm -f "$headers" "$payload"; error 'Invalid GHCR tags pagination response.';
    }
    last="$tag"
    ((page += 1))
  done
  rm -f "$headers" "$payload"
  error 'GHCR tags pagination exceeded 100 pages; refusing an incomplete release search.'
}

pull_eigrp() {
  note 'Discovering highest published OpenEIGRP release in GHCR'
  local token version image arch
  token="$(registry_token)" || error 'Unable to authenticate to GHCR for tag discovery.'
  version="$(latest_eigrp_tag "$token" | sort -Vu | tail -n 1)"
  [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] ||
    error 'No published X.Y.Z release tags found for OpenEIGRP.'
  image="$EIGRP_IMAGE:$version"
  if docker image inspect "$image" >/dev/null 2>&1; then
    note "Already available: $image (keeping all previous versions)"
  else
    note "Pulling native AMD64 image: $image"
    docker pull --platform linux/amd64 "$image"
  fi
  arch="$(docker image inspect -f '{{.Architecture}}' "$image")"
  [[ "$arch" == amd64 ]] || error "$image has architecture $arch instead of amd64"
  # Versioned tags are retained. A convenience alias moves to current release.
  docker tag "$image" openeigrp-frr:current
  printf '\nOpenEIGRP selected: %s\n' "$image"
  printf 'Convenience alias:  openeigrp-frr:current\n'
}

ensure_vrnetlab() {
  need git; need make
  if [[ ! -d "$VRNETLAB_DIR/.git" ]]; then
    [[ ! -e "$VRNETLAB_DIR" ]] || error "Cannot initialize vrnetlab: path exists: $VRNETLAB_DIR"
    mkdir -p "$(dirname "$VRNETLAB_DIR")"
    note 'Cloning Containerlab-compatible srl-labs/vrnetlab'
    git clone --depth 1 https://github.com/srl-labs/vrnetlab.git "$VRNETLAB_DIR"
  fi
  [[ -f "$VRNETLAB_DIR/cisco/csr1000v/Makefile" ]] ||
    error 'vrnetlab checkout is missing cisco/csr1000v/Makefile'
}

# Preserve source artifacts so --image can be repeated without a NAS mount.
store_csr_source() {
  local source="$1" base path tmp
  local target_dir="${2:-$IMAGE_DIR}"
  mkdir -p "$target_dir"
  case "$source" in
    http://*|https://*)
      need curl
      base="${source%%\?*}"; base="${base##*/}"
      [[ "$base" =~ ^[a-zA-Z0-9_.-]+$ ]] || error "Invalid CSR download filename: $base"
      path="$target_dir/$base"
      if [[ ! -f "$path" ]]; then
        tmp="$(mktemp "$target_dir/.cisco-download.XXXXXXXX")"
        if ! curl -fL --retry 2 --connect-timeout 20 --output "$tmp" "$source"; then
          rm -f "$tmp"; error "CSR image download failed: $base"
        fi
        mv -n "$tmp" "$path"
        rm -f "$tmp"
      else
        note "Reusing cached CSR source: $path"
      fi
      ;;
    file://*) source="${source#file://}";;&
    *)
      [[ -f "$source" ]] || error "CSR source is not a regular file: $source"
      base="${source##*/}"
      [[ "$base" =~ ^[a-zA-Z0-9_.-]+$ ]] || error "Invalid CSR filename: $base"
      path="$target_dir/$base"
      if [[ "$source" != "$path" ]]; then
        if [[ -e "$path" ]]; then
          [[ "$(sha256sum "$source" | cut -d' ' -f1)" == "$(sha256sum "$path" | cut -d' ' -f1)" ]] ||
            error "Different CSR file already cached under $base; refusing overwrite"
        else
          cp -n -- "$source" "$path"
        fi
      fi
      ;;
  esac
  printf '%s\n' "$path"
}

require_kvm() {
  [[ -c /dev/kvm ]] || {
    # Loading the module is harmless when already loaded; in a Docker-only
    # environment /dev/kvm legitimately might not exist.
    sudo modprobe kvm_intel 2>/dev/null || true
    [[ -c /dev/kvm ]] || error "/dev/kvm is missing; enable Intel VT-x in BIOS and load kvm_intel."
  }
  [[ -r /dev/kvm && -w /dev/kvm ]] ||
    error "/dev/kvm is not accessible to $(id -un); add your user to kvm and reconnect."
}

# A Cisco installation ISO is bootable media, not a CSR router disk. Cisco's
# KVM installation procedure installs it onto an 8G QCOW2. Run the first boot
# in a private QEMU/KVM VM and stop on its first reboot or shutdown. The
# generated disk is only published once QEMU exits cleanly and enough data
# has been written to exclude an untouched qcow2 header. The subsequent
# vrnetlab --install phase configures CSR for serial-console operation.
install_csr_iso() {
  local iso="$1" basename target tmp logfile exitcode=0 bytes format
  local -a args
  basename="${iso##*/}"
  [[ "$basename" == *.iso ]] || error "Not an ISO filename: $basename"
  [[ "$basename" =~ ^csr1000v[-_].*[0-9]+\.[0-9]+\.[0-9]+.*\.iso$ ]] ||
    error "Cisco ISO filename does not contain CSR1000v and a release version: $basename"
  target="$IMAGE_DIR/${basename%.iso}-serial.qcow2"
  # Do not treat a stale incomplete ISO installation as a completed image.
  if [[ -f "$target" ]]; then
    if [[ -f "$target.installed-sha256" ]]; then
      local expected
      expected="$(cat "$target.installed-sha256")"
      [[ "$expected" == "$(sha256sum "$iso" | awk '{print $1}')" ]] ||
        error "ISO source has changed since $target was prepared; refusing to overwrite it."
    else
      # A user-supplied serial QCOW2 takes precedence over a companion ISO.
      note "Using preexisting CSR disk $target; not replacing it with ISO installation." >&2
    fi
    note "Reusing installed Cisco QCOW2: $target" >&2
    printf '%s\n' "$target"
    return 0
  fi
  require_kvm
  need qemu-img
  need qemu-system-x86_64
  need timeout
  [[ "$ISO_MEMORY" =~ ^[1-9][0-9]*$ && "$ISO_VCPUS" =~ ^[1-9][0-9]*$ ]] ||
    error 'CSR ISO memory/vCPU settings must be positive integers.'
  tmp="$target.installing"
  [[ ! -e "$tmp" ]] || error "An incomplete ISO disk remains: $tmp. Inspect/remove it before retrying."
  logfile="$IMAGE_DIR/${basename%.iso}.install.log"
  note "Installing CSR1000v ISO $basename onto a new 8 GB disk" >&2
  note "QEMU serial log: $logfile" >&2
  note "Headless QEMU VNC: 127.0.0.1:5999 (SSH forward this port if console intervention is needed)" >&2
  # Locally bound VNC is not remotely reachable without an explicit SSH tunnel.
  # Keep the network disconnected from the LAN; installation needs no uplink.
  # qemu-img prints "Formatting ..." to stdout. install_csr_iso is used in a
  # command substitution, where stdout is reserved exclusively for the final
  # QCOW2 pathname. Put tool diagnostics in the ISO installation log instead.
  qemu-img create -f qcow2 "$tmp" 8G >>"$logfile" 2>&1 ||
    error "Cannot create CSR installation disk. See $logfile."
  args=(
    -enable-kvm -machine pc -cpu host -m "$ISO_MEMORY" -smp "$ISO_VCPUS"
    -drive "if=virtio,format=qcow2,file=$tmp"
    -drive "if=ide,media=cdrom,readonly=on,file=$iso"
    -netdev user,id=csrnet -device virtio-net-pci,netdev=csrnet
    -boot order=d -no-reboot -display none -vnc 127.0.0.1:99
    -serial "file:$logfile" -monitor none
  )
  # ISO install normally shuts down/reboots automatically. A timeout or QEMU
  # failure is *not* success; leave source ISO unchanged and no final QCOW2.
  if timeout --foreground --signal=TERM --kill-after=30s "$ISO_TIMEOUT" \
      qemu-system-x86_64 "${args[@]}" >>"$logfile" 2>&1; then
    :
  else
    exitcode=$?
    rm -f -- "$tmp"
    error "CSR ISO installation failed (status $exitcode). See $logfile. No image was published."
  fi
  format="$(qemu-img info --output=json "$tmp" | jq -r '.format // empty')"
  bytes="$(qemu-img info --output=json "$tmp" | jq -r '.["actual-size"] // 0')"
  [[ "$format" == qcow2 && "$bytes" =~ ^[0-9]+$ && "$bytes" -ge 134217728 ]] || {
    rm -f -- "$tmp"
    error "CSR installer exited but disk was not populated (allocated: $bytes bytes). See $logfile."
  }
  mv -n -- "$tmp" "$target"
  [[ ! -e "$tmp" ]] || error "Could not promote completed CSR disk: $tmp"
  sha256sum "$iso" | awk '{print $1}' > "$target.installed-sha256"
  note "Cisco installation disk created: $target" >&2
  printf '%s\n' "$target"
}

# vrnetlab's Makefile computes the image version from the QCOW2 filename.
csr_version() {
  local file="$1" result
  result="$(make -s -C "$VRNETLAB_DIR/cisco/csr1000v" \
    IMAGE="${file##*/}" version-test)"
  result="${result##*Version: }"
  [[ "$result" =~ ^[0-9]+\.[0-9]+\.[0-9]+[a-zA-Z0-9.-]*$ ]] ||
    error "Cannot determine CSR version from filename ${file##*/}; use Cisco's original image filename"
  printf '%s\n' "$result"
}

csr_tag_alias() {
  local version="$1" actual="$CSR_IMAGE:$version" alias="$CSR_NETLAB_IMAGE:$version"
  local a b
  a="$(docker image inspect -f '{{.Id}}' "$actual")" || error "Cannot inspect $actual"
  if docker image inspect "$alias" >/dev/null 2>&1; then
    b="$(docker image inspect -f '{{.Id}}' "$alias")" || error "Cannot inspect $alias"
    [[ "$a" == "$b" ]] || error "Conflicting image exists: $alias; refusing to replace it"
  else
    docker tag "$actual" "$alias" || error "Cannot tag $alias"
  fi
  note "CSR1000v ready: $alias (native image: $actual)"
}

build_csr() {
  local file="$1" original ext version image scratch buildfile archive sha marker
  original="${file##*/}"
  case "${original,,}" in
    *.iso) file="$(install_csr_iso "$file")" || return 1; original="${file##*/}" ;;
    *.qcow2|*.vmdk) ;;
    *) error "Unsupported Cisco input format: $original (expected .qcow2 or .vmdk)" ;;
  esac
  require_kvm
  need qemu-img
  ensure_vrnetlab
  archive="$file"
  if [[ "${original,,}" == *.vmdk ]]; then
    buildfile="$IMAGE_DIR/${original%.*}.qcow2"
    if [[ ! -f "$buildfile" ]]; then
      note "Converting VMDK to QCOW2: $original"
      qemu-img convert -p -f vmdk -O qcow2 "$archive" "$buildfile.tmp" || error "VMDK conversion failed: $archive"
      mv "$buildfile.tmp" "$buildfile" || error "Unable to save converted QCOW2"
    fi
    file="$buildfile"
  fi
  version="$(csr_version "$file")" || return 1
  image="$CSR_IMAGE:$version"
  marker="$IMAGE_DIR/.csr-$version.sha256"
  sha="$(sha256sum "$file" | awk '{print $1}')"
  if [[ -f "$marker" ]]; then
    [[ "$(cat "$marker")" == "$sha" ]] || error "CSR version $version has a different source checksum; refusing image overwrite"
  fi
  if docker image inspect "$image" >/dev/null 2>&1; then
    note "CSR image already built: $image"
    csr_tag_alias "$version"
    return 0
  fi
  scratch="$(mktemp -d "$HOME/netlab/.csr-build.XXXXXXXX")" || error "Cannot create CSR build workspace"
  # Work in a disposable checkout: never leave Cisco assets in the vrnetlab tree.
  note "Preparing isolated CSR build workspace for $version"
  if ! git clone --quiet --no-hardlinks "$VRNETLAB_DIR" "$scratch/vrnetlab"; then
    rm -rf "$scratch"; error 'Could not create a local vrnetlab build checkout'
  fi
  if ! cp -- "$file" "$scratch/vrnetlab/cisco/csr1000v/${file##*/}"; then
    rm -rf -- "$scratch"
    error "Cannot stage CSR input: $file"
  fi
  note "Building Cisco CSR1000v version $version (requires KVM)"
  # vrnetlab waits for IOS XE's first-boot configuration to finish. Bound the
  # build so an unbootable or incompatible disk cannot hang the provisioner.
  if ! (cd "$scratch/vrnetlab/cisco/csr1000v" &&
        timeout --signal=TERM --kill-after=30s "$CSR_BUILD_TIMEOUT" make docker-image); then
    if [[ -f "$scratch/vrnetlab/cisco/csr1000v/cidfile" ]]; then
      local cid
      cid="$(head -n 1 "$scratch/vrnetlab/cisco/csr1000v/cidfile")"
      [[ "$cid" =~ ^[a-f0-9]{12,64}$ ]] && docker rm -f "$cid" >/dev/null 2>&1 || true
    fi
    rm -rf "$scratch"
    error "CSR1000v image build failed for $version"
  fi
  rm -rf "$scratch"
  docker image inspect "$image" >/dev/null 2>&1 || error "Build completed but expected image missing: $image"
  printf '%s\n' "$sha" > "$marker" || error "Cannot record CSR checksum: $marker"
  csr_tag_alias "$version"
}

# Catalyst 8000V is distinct from CSR1000v: vrnetlab/cisco/c8000v uses a
# different build and netlab expects device cat8000v (vrnetlab/vr-c8000v).
# Keep its source disks, checksums, and image tags independent of CSR1000v.
install_c8000v_iso() {
  local iso="$1" basename target tmp logfile exitcode=0 bytes format expected
  local -a args
  basename="${iso##*/}"
  [[ "$basename" =~ ^c8000v[-_].*[0-9]+\.[0-9]+\.[0-9]+.*\.iso$ ]] ||
    error "Catalyst 8000V installer filename must start with c8000v and include a version: $basename"
  target="$C8000V_DIR/${basename%.iso}-serial.qcow2"
  if [[ -f "$target" ]]; then
    if [[ -f "$target.installed-sha256" ]]; then
      expected="$(cat "$target.installed-sha256")"
      [[ "$expected" == "$(sha256sum "$iso" | awk '{print $1}')" ]] ||
        error "8000V ISO source changed after $target was prepared; refusing overwrite."
    else
      note "Reusing existing Catalyst disk $target rather than overwriting it." >&2
    fi
    printf '%s\n' "$target"
    return 0
  fi
  require_kvm
  need qemu-img; need qemu-system-x86_64; need timeout
  [[ "$ISO_MEMORY" =~ ^[1-9][0-9]*$ && "$ISO_VCPUS" =~ ^[1-9][0-9]*$ ]] ||
    error 'Cisco ISO memory/vCPU settings must be positive integers.'
  tmp="$target.installing"
  [[ ! -e "$tmp" ]] || error "An incomplete Catalyst disk remains: $tmp"
  logfile="$C8000V_DIR/${basename%.iso}.install.log"
  note "Installing Catalyst 8000V ISO $basename onto a new 8 GB disk" >&2
  note "QEMU serial log: $logfile" >&2
  note 'QEMU VNC: 127.0.0.1:5999 (SSH tunnel if interactive installation required)' >&2
  # Cisco's KVM guide specifies an 8G QCOW2 using virtio, with ISO attached.
  # Keep all QEMU diagnostics on stderr or in the log: stdout is the return path.
  qemu-img create -f qcow2 "$tmp" 8G >>"$logfile" 2>&1 ||
    error "Unable to create Catalyst installation disk. See $logfile"
  args=(
    -enable-kvm -machine pc -cpu host -m "$ISO_MEMORY" -smp "$ISO_VCPUS"
    -drive "if=virtio,format=qcow2,file=$tmp"
    -drive "if=ide,media=cdrom,readonly=on,file=$iso"
    -netdev user,id=c8net -device virtio-net-pci,netdev=c8net
    -boot order=d -no-reboot -display none -vnc 127.0.0.1:99
    -serial "file:$logfile" -monitor none
  )
  if timeout --foreground --signal=TERM --kill-after=30s "$ISO_TIMEOUT" \
      qemu-system-x86_64 "${args[@]}" >>"$logfile" 2>&1; then
    :
  else
    exitcode=$?
    rm -f -- "$tmp"
    error "Catalyst 8000V ISO installation failed ($exitcode). See $logfile"
  fi
  format="$(qemu-img info --output=json "$tmp" | jq -r '.format // empty')"
  bytes="$(qemu-img info --output=json "$tmp" | jq -r '.["actual-size"] // 0')"
  [[ "$format" == qcow2 && "$bytes" =~ ^[0-9]+$ && "$bytes" -ge 134217728 ]] || {
    rm -f -- "$tmp"
    error "Catalyst ISO installer exited but did not populate disk ($bytes bytes). See $logfile"
  }
  mv -n -- "$tmp" "$target"
  [[ ! -e "$tmp" ]] || error "Could not save installed Catalyst disk: $tmp"
  sha256sum "$iso" | awk '{print $1}' > "$target.installed-sha256"
  note "Catalyst 8000V installation disk created: $target" >&2
  printf '%s\n' "$target"
}

c8000v_version() {
  local result
  [[ -f "$VRNETLAB_DIR/cisco/c8000v/Makefile" ]] ||
    error 'vrnetlab checkout lacks cisco/c8000v/Makefile'
  result="$(make -s -C "$VRNETLAB_DIR/cisco/c8000v" IMAGE="${1##*/}" print-version)" || return 1
  [[ "$result" =~ ^[0-9]+\.[0-9]+\.[0-9]+[a-zA-Z]?$ ]] ||
    error "Cannot determine Catalyst 8000V version from ${1##*/}: $result"
  printf '%s\n' "$result"
}

c8000v_tag_alias() {
  local version="$1" actual="$C8000V_IMAGE:$version" alias="$C8000V_NETLAB_IMAGE:$version" a b
  a="$(docker image inspect -f '{{.Id}}' "$actual")" || error "Cannot inspect $actual"
  if docker image inspect "$alias" >/dev/null 2>&1; then
    b="$(docker image inspect -f '{{.Id}}' "$alias")" || error "Cannot inspect $alias"
    [[ "$a" == "$b" ]] || error "Conflicting image exists: $alias; refusing overwrite"
  else
    docker tag "$actual" "$alias" || error "Cannot tag $alias"
  fi
  note "Catalyst 8000V ready: $alias (native image: $actual)"
}

build_c8000v() {
  local file="$1" original version image scratch buildfile sha marker cid
  original="${file##*/}"
  case "${original,,}" in
    *.iso) file="$(install_c8000v_iso "$file")" || return 1; original="${file##*/}" ;;
    *.qcow2|*.vmdk) ;;
    *) error "Unsupported Catalyst input: $original" ;;
  esac
  require_kvm
  need qemu-img
  ensure_vrnetlab
  if [[ "${original,,}" == *.vmdk ]]; then
    buildfile="$C8000V_DIR/${original%.*}.qcow2"
    if [[ ! -f "$buildfile" ]]; then
      note "Converting Catalyst VMDK to QCOW2: $original"
      qemu-img convert -p -f vmdk -O qcow2 "$file" "$buildfile.tmp" || {
        rm -f "$buildfile.tmp"; error "8000V VMDK conversion failed: $file";
      }
      mv "$buildfile.tmp" "$buildfile"
    fi
    file="$buildfile"
  fi
  version="$(c8000v_version "$file")" || return 1
  image="$C8000V_IMAGE:$version"
  marker="$C8000V_DIR/.c8000v-$version.sha256"
  sha="$(sha256sum "$file" | awk '{print $1}')"
  if [[ -f "$marker" ]]; then
    [[ "$(cat "$marker")" == "$sha" ]] ||
      error "Catalyst version $version has a different source checksum; refusing overwrite"
  fi
  if docker image inspect "$image" >/dev/null 2>&1; then
    note "Catalyst image already built: $image"
    c8000v_tag_alias "$version"
    return 0
  fi
  scratch="$(mktemp -d "$HOME/netlab/.c8000v-build.XXXXXXXX")" || error 'Unable to create build directory'
  note "Preparing isolated Catalyst 8000V build workspace for $version"
  if ! git clone --quiet --no-hardlinks "$VRNETLAB_DIR" "$scratch/vrnetlab"; then
    rm -rf -- "$scratch"; error 'Could not stage vrnetlab'
  fi
  if ! cp -- "$file" "$scratch/vrnetlab/cisco/c8000v/${file##*/}"; then
    rm -rf -- "$scratch"; error "Could not stage Catalyst input: $file"
  fi
  note "Building Catalyst 8000V $version (autonomous mode for EIGRP labs)"
  # Build only autonomous mode. vrnetlab's full docker-image target also
  # builds controller and ZTP variants, which are not needed for EIGRP labs.
  if ! (cd "$scratch/vrnetlab/cisco/c8000v" &&
        timeout --signal=TERM --kill-after=30s "$CSR_BUILD_TIMEOUT" \
          make IMAGE="${file##*/}" MODE=autonomous docker-build); then
    if [[ -f "$scratch/vrnetlab/cisco/c8000v/cidfile" ]]; then
      cid="$(head -n 1 "$scratch/vrnetlab/cisco/c8000v/cidfile")"
      if [[ "$cid" =~ ^[a-f0-9]{12,64}$ ]]; then docker rm -f "$cid" >/dev/null 2>&1 || true; fi
    fi
    rm -rf -- "$scratch"
    error "Catalyst 8000V image build failed for $version"
  fi
  rm -rf -- "$scratch"
  docker image inspect "$image" >/dev/null 2>&1 ||
    error "Catalyst build finished but expected image missing: $image"
  printf '%s\n' "$sha" > "$marker"
  c8000v_tag_alias "$version"
}

images() {
  check_docker
  pull_eigrp
  mkdir -p "$IMAGE_DIR" "$C8000V_DIR" "$HOME/netlab"
  local src file
  for src in "${csr_sources[@]}"; do
    store_csr_source "$src" "$IMAGE_DIR" >/dev/null
  done
  for src in "${c8000v_sources[@]}"; do
    store_csr_source "$src" "$C8000V_DIR" >/dev/null
  done
  shopt -s nullglob
  local inputs=("$IMAGE_DIR"/*.iso "$IMAGE_DIR"/*.qcow2 "$IMAGE_DIR"/*.vmdk)
  shopt -u nullglob
  local failed=0
  if ((${#inputs[@]} == 0)); then
    note "No CSR1000v inputs found in $IMAGE_DIR; skipping CSR1000v build."
  else
    for file in "${inputs[@]}"; do
      if ! ( build_csr "$file" ); then
        warn "CSR1000v image preparation failed for ${file##*/}; continuing."
        failed=1
      fi
    done
  fi
  shopt -s nullglob
  local cat_inputs=("$C8000V_DIR"/*.iso "$C8000V_DIR"/*.qcow2 "$C8000V_DIR"/*.vmdk)
  shopt -u nullglob
  if ((${#cat_inputs[@]} == 0)); then
    note "No Catalyst 8000V inputs found in $C8000V_DIR; skipping 8000V build."
  else
    for file in "${cat_inputs[@]}"; do
      if ! ( build_c8000v "$file" ); then
        warn "Catalyst 8000V image preparation failed for ${file##*/}; continuing."
        failed=1
      fi
    done
  fi
  ((failed == 0)) || error 'One or more Cisco image preparations failed; earlier versioned images were preserved.'
  note 'Available OpenEIGRP/Cisco tags (older versions retained):'
  docker image ls --format '{{.Repository}}:{{.Tag}}' | \
    grep -E '^(ghcr.io/diivious/openeigrp-frr|openeigrp-frr|vrnetlab/cisco_csr1000v|vrnetlab/vr-csr|vrnetlab/cisco_c8000v|vrnetlab/vr-c8000v):' || true
}

main() {
  [[ $# -gt 0 ]] || { usage >&2; exit 2; }
  local command="$1"; shift
  csr_sources=()
  c8000v_sources=()
  case "$command" in
    --help|-h) usage; return 0 ;;
    --setup|--update|--image|--images) ;;
    *) usage >&2; error "Unknown mode: $command" ;;
  esac
  while (($#)); do
    case "$1" in
      --csr-source)
        [[ $# -ge 2 && -n "$2" ]] || error '--csr-source requires a path or URL'
        csr_sources+=("$2"); shift 2 ;;
      --c8000v-source)
        [[ $# -ge 2 && -n "$2" ]] || error '--c8000v-source requires a path or URL'
        c8000v_sources+=("$2"); shift 2 ;;
      *) error "Unknown argument: $1" ;;
    esac
  done
  [[ "$command" == --image || "$command" == --images || "$command" == --setup || ( ${#csr_sources[@]} -eq 0 && ${#c8000v_sources[@]} -eq 0 ) ]] ||
    error 'Cisco sources are available only with --setup/--image/--images'
  check_host
  case "$command" in
    --setup) setup ;;
    --update) update ;;
    --image|--images) images ;;
  esac
}

main "$@"
