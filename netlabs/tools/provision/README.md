# Netlab appliance provisioning

Copyright (C) 2026 Donnie V. Savage

`netlab-provision.sh` prepares a **dedicated Debian 13 AMD64** host for
OpenEIGRP interoperability testing with netlab, Containerlab, Docker, and
QEMU/KVM-backed Cisco IOS XE routers. It also creates a separate `netlab`
execution account for the [OpenEIGRP remote lab tools](../README.md).

This guide uses **StormTrooper** as an example appliance hostname and
**diivious** as an example Debian administrator username. Replace those two
values with the actual hostname/address and your own sudo-capable login. The
**`netlab` account name is not an example**: it is the dedicated execution
account created by this provisioner and expected by the current remote tools.

The provisioner is intended for a trusted, dedicated laboratory host. It does
not prune Docker images, delete lab data, or replace existing versioned router
image tags. **Provisioning and especially updating can still install or change
system packages and tool versions.**

## Requirements

- Debian **13** on **x86_64/AMD64** with a sudo-capable non-root administrator.
  Other distributions and architectures are rejected by the script.
- CPU virtualization enabled in firmware, with functional **`/dev/kvm`** for
  Cisco IOS XE virtual routers. Docker and Containerlab are installed during
  `--setup`.
- Network access to Debian package repositories, Python packages, GitHub,
  container registries, and tool installers.
- Adequate disk space for container images, Cisco virtual disks, installation
  logs, and repeated lab results. Existing Docker/containerd data locations
  are preserved, not relocated by this script.
- Separately obtained, appropriately licensed Cisco installation images for
  any Cisco routers you plan to build. The script does not supply Cisco images.

Run provisioning **as your normal administrator**, not with `sudo` in front of
the script and not from a root shell. The script uses `sudo` internally for
operations that require it.

## Install and verify the appliance

From an OpenEIGRP checkout on your development machine, copy the provisioner
to the appliance. The example uses `diivious@StormTrooper`; substitute your
administrator login and appliance address as appropriate.

```bash
scp netlabs/provision/netlab-provision.sh diivious@StormTrooper:~/
ssh diivious@StormTrooper
```

On the appliance, run:

```bash
chmod +x ~/netlab-provision.sh
~/netlab-provision.sh --setup
```

`--setup` installs the prerequisites, creates separate Python environments
for the administrator and `netlab`, configures its runtime directories and
privileges, and attempts image preparation. If Docker group membership was
just added, **log out and reconnect** before further Docker work. Some
installation steps might also require a reboot.

Verify the runtime as the new account:

```bash
id netlab
sudo -iu netlab
netlab version
docker info --format '{{.DockerRootDir}}'
mkdir -p ~/netlab-smoke && cd ~/netlab-smoke
netlab test clab
exit
```

Run the smoke test from a clean directory; it generates temporary topology
files and may conflict with an existing directory named `test`. The
provisioner deliberately does **not** run this test automatically because it
could interfere with an active lab.

If image preparation was deferred due to group membership or no Cisco sources
were available, run `~/netlab-provision.sh --image` after reconnecting and/or
providing the images. You do not need to rerun `--setup` simply to add an
image to an already provisioned appliance.

## SSH access for remote execution

The provisioner creates `/home/netlab/.ssh/authorized_keys` with restrictive
permissions but **does not set a password, generate authentication keys, or
copy another user's keys**. It preserves existing SSH keys and account
password state when rerun.

Use SSH public-key authentication for unattended execution:

1. On each development client, use an existing SSH key pair or create one if
   needed. Keep its **private key on that client**.
2. Copy the contents of the client's **public** key (for example,
   `~/.ssh/id_ed25519.pub`) into
   `/home/netlab/.ssh/authorized_keys` on StormTrooper. The administrator can
   edit this file with `sudoedit`; use one public key per line.
3. Verify the server-side ownership and permissions:

   ```bash
   sudo chown netlab:netlab /home/netlab/.ssh /home/netlab/.ssh/authorized_keys
   sudo chmod 700 /home/netlab/.ssh
   sudo chmod 600 /home/netlab/.ssh/authorized_keys
   ```

4. From the development client, confirm unattended login:

   ```bash
   ssh -o BatchMode=yes netlab@StormTrooper whoami
   # expected: netlab
   ```

If the `netlab` account's initially locked password prevents public-key login
under your sshd/PAM policy, resolve the account policy deliberately; do not
weaken SSH security globally. A password can be established explicitly with
`sudo passwd netlab` if password authentication is required. It is **not**
created by the provisioner.

### Configure and run OpenEIGRP tests

In the **client's** OpenEIGRP checkout, edit `netlabs/remote-lab.cfg`:

```bash
REMOTE_HOST=StormTrooper
REMOTE_USER=netlab
REMOTE_SSH_PORT=22
```

The existing config also defines `REMOTE_PASSWORD`. It is not used when SSH
key authentication succeeds. **Never commit an actual password or token.**
For password-based access, keep the credentials in a private config outside
the repository and set `REMOTE_LAB_CONFIG` to its path (see
[`netlabs/README.md`](../README.md)). The remote client sources this file as
Bash, so only use a trusted file.

From the repository root on the client:

```bash
netlabs/tools/remote-lab.sh --status
netlabs/tools/remote-uut.sh --list
netlabs/tools/remote-uut.sh --scenario 2R-neighbor
```

`remote-uut.sh` provisions the scenario, validates the outcome, copies logs
and results into `netlabs/results/`, and tears down the topology. The
interactive `remote-lab.sh` maintains a lab until explicitly stopped; numbered
instructional labs are not yet implemented. The two tools share an exclusive
session lock, so run **only one** lab or UUT session at a time. The separate
portable YAML UUT harness in `eigrp/test/uut/` is not a Containerlab workload.

The `2R-neighbor` scenario uses **specific** image tags:
`ghcr.io/diivious/openeigrp-frr:0.3.0` and `vrnetlab/vr-csr:17.03.06`.
Installing the newest available release with `--image` does **not** guarantee
these exact tags exist; check the images on the appliance before running the
scenario.

### Running on the appliance itself

A separate development server is optional. On a **provisioned Linux appliance**,
run the OpenEIGRP client commands locally with `REMOTE_HOST=127.0.0.1` (or
`localhost`). SSH is still used, and `netlab@127.0.0.1` must allow unattended
login. Loopback on a Mac points to the Mac, not to StormTrooper, and does not
provide a native Cisco KVM appliance.

## Provisioner commands

Run the following **on the appliance**, as the non-root administrator:

| Command | Action |
| --- | --- |
| `./netlab-provision.sh --setup` | Install prerequisites, provision the `netlab` account, prepare images when possible. |
| `./netlab-provision.sh --image` | Discover/pull the latest published OpenEIGRP image and prepare any supplied Cisco images. |
| `./netlab-provision.sh --images` | Alias for `--image`. |
| `./netlab-provision.sh --update` | Upgrade Debian packages, netlab/Ansible/Containerlab, selected tooling, and runtime packages. **Does not update router images.** |
| `./netlab-provision.sh --help` | Show command-line help and environment overrides. |

### Important: `--update` changes the appliance

**Do not run `--update` while an interactive lab or automated UUT is active.**
It runs `apt-get update`, installs/upgrades prerequisite packages, performs
**`apt-get -y full-upgrade`**, refreshes netlab and associated tooling,
upgrades Containerlab, and fast-forwards the local vrnetlab checkout when
present. These operations can change behavior, introduce regressions, restart
services, and require a reboot. Versions are not pinned to the last successful
qualification run.

For a reproducible test appliance, record the working Debian/kernel, Docker,
Containerlab, netlab, Ansible, and router image versions before an upgrade.
Prefer a system backup or snapshot and rerun smoke tests and relevant EIGRP
UUTs afterward. **Use `--image`, not `--update`, when the only goal is to add a
Cisco image or refresh the OpenEIGRP release.** Neither command is a rollback
mechanism. `--update` preserves existing versioned router image tags and lab
data; that does not guarantee compatibility with newer host-side tools.

## The privileged `netlab` account

`--setup` creates or maintains a dedicated `netlab` user with its own
`/home/netlab/netlab-venv`, `~/bin`, and `~/labs` directories. The execution
workspace and collected results reside under `/home/netlab-data/runs` and
`/home/netlab-data/results`. The script does not rehome the existing
Docker/containerd image stores.

It grants `netlab` membership in `docker`, `clab_admins`, and `kvm`, and sets
up Containerlab's setuid mechanism for a root-owned installation as needed.
It also installs `/etc/sudoers.d/netlab-network`, allowing passwordless root
execution of only the detected `ip`, `brctl`, and `containerlab` commands.
It does **not** grant a blanket `sudo ALL` policy.

**Security consequence:** Docker and Containerlab administrative access is
already effectively **root-equivalent**. The limited sudoers rule does not
make this account unprivileged or isolated. Only grant the `netlab` account to
trusted developers and authorize SSH keys from trusted machines. Do not use a
shared account on a multi-tenant or otherwise untrusted host. Review the
resulting group memberships and sudoers file as part of installation.

## Cisco IOS XE images

| Family | Default source directory | vrnetlab builder | Built image tag | netlab image tag | netlab device |
| --- | --- | --- | --- | --- | --- |
| CSR1000v | `~/netlab/images/csr1kv/` | `cisco/csr1000v` | `vrnetlab/cisco_csr1000v:VERSION` | `vrnetlab/vr-csr:VERSION` | `csr` |
| Catalyst 8000V | `~/netlab/images/csr8kv/` | `cisco/c8000v` | `vrnetlab/cisco_c8000v:VERSION` | `vrnetlab/vr-c8000v:VERSION` | `cat8000v` |

`csr1kv` and `csr8kv` are local storage names, not netlab device types. The
Catalyst 8000V is IOS XE and is **not** the IOS XR-based Cisco 8000 in netlab
(`cisco8000v`). Catalyst 8000V images are prepared in **autonomous** routing
mode for these EIGRP labs.

The script accepts `.iso`, `.qcow2`, or `.vmdk` files. ISO installation uses
KVM/QEMU to produce an 8 GB QCOW2 disk before vrnetlab packaging; VMDK is
converted to QCOW2. Source files are cached, and a conflicting checksum is
rejected instead of silently replacing an existing file. By default the ISO
installation timeout is 45 minutes and the build timeout is 60 minutes;
`NETLAB_CSR_ISO_TIMEOUT` and `NETLAB_CSR_BUILD_TIMEOUT` apply to both Cisco
families.

Supply a legally obtained image directly, for example:

```bash
./netlab-provision.sh --image \
  --csr-source /path/to/csr1000v-image.qcow2 \
  --c8000v-source /path/to/c8000v-image.iso
```

Either option can be used on its own. Both accept local paths, `file://` URLs,
and HTTP(S) URLs. Alternatively place source images in their respective
default directories before running `--image`. Override those directories with
`NETLAB_CSR_DIR` and `NETLAB_C8000V_DIR` if needed.

Verify which versions are actually present:

```bash
docker image ls | grep -E 'openeigrp|cisco_csr1000v|vr-csr|cisco_c8000v|vr-c8000v'
```

For an installed Catalyst image, a native netlab topology can specify:

```yaml
name: iosxe-smoke
provider: clab
nodes:
  r1:
    device: cat8000v
    image: vrnetlab/vr-c8000v:17.13.01a
```

The version is an example; use your **actually built tag**. CSR1000v uses
`device: csr`.

### Optional migration of older image directories

**New installations should skip this section.** Only use it if you previously
stored CSR1000v images under `/home/netlab-data/images/csr` and maintained a
symlink `~/netlab/images/csr` pointing there. Stop all image builds and verify
the paths before changing anything; this migration is not performed by the
provisioner.

```bash
# On the appliance, inspect the old paths first.
ls -ld ~/netlab/images/csr /home/netlab-data/images/csr \
       /home/netlab-data/images/csr1kv 2>/dev/null || true
```

If and only if the old directory is the intended image store and the new
destination does not exist, rename it and update the symlink:

```bash
test ! -e /home/netlab-data/images/csr1kv && \
  sudo mv /home/netlab-data/images/csr /home/netlab-data/images/csr1kv

# Remove only a symlink, never an actual source directory.
test -L ~/netlab/images/csr && rm ~/netlab/images/csr
ln -s /home/netlab-data/images/csr1kv ~/netlab/images/csr1kv
```

If any existing destination, symlink, or owner differs from those assumptions,
stop and reconcile it manually. The original source ISO/QCOW2 files, build logs,
and checksum markers should be preserved. The normal `--image` path does not
require this migration.

## Operational limits and troubleshooting

- **Hardware acceleration:** `/dev/kvm` is required to prepare ISO-based Cisco
  images and to run IOS XE virtual routers efficiently. Verify firmware and
  KVM permissions if missing.
- **ISO installation console:** A VNC console is bound to
  `127.0.0.1:5999` on the appliance during QEMU installation. To access it
  from a workstation, use
  `ssh -L 5999:127.0.0.1:5999 diivious@StormTrooper` (substitute your
  administrator and host).
- **Image build logs:** Installation logs are retained as `*.install.log`.
  A successful QEMU exit and nonempty QCOW2 disk do not establish that IOS XE
  boots correctly. Validate the resulting router with a live `netlab up`,
  `netlab connect`, and `show version`.
- **Router tags:** Older OpenEIGRP and Cisco tags are preserved. The
  `openeigrp-frr:current` convenience tag tracks the newest pulled release;
  GHCR does not supply a moving OpenEIGRP `latest` tag.
- **Storage overrides:** `NETLAB_VENV`, `NETLAB_CSR_DIR`,
  `NETLAB_C8000V_DIR`, `NETLAB_VRNETLAB_DIR`, and related build parameters can
  customize provisioning. `NETLAB_DATA_ROOT` changes the provisioner's
  run/results root, **but the current remote runner expects
  `/home/netlab-data`**, so do not override it without updating that runner.
- **Version-specific lab requirements:** A successful `--setup` or `--image`
  does not prove that a particular scenario's pinned router versions exist,
  nor that EIGRP interoperability works.

## Regression tests

The provisioner and its two tests live together in `netlabs/provision/`. Run
from that directory on a development system:

```bash
bash -n netlab-provision.sh
bash test-netlab-provision.sh
bash test-netlab-account.sh
```

Both tests mock network, QEMU, Docker, vrnetlab, installation, and account
operations in temporary directories. They do **not** modify a real appliance
and cannot establish hardware readiness or live Cisco interoperability.
Execute the actual `netlab test clab` smoke test and the relevant remote EIGRP
UUTs separately on a provisioned appliance.

## References

- https://netlab.tools/platforms/
- https://netlab.tools/labs/clab/
- https://github.com/srl-labs/vrnetlab/tree/master/cisco/csr1000v
- https://github.com/srl-labs/vrnetlab/tree/master/cisco/c8000v
- https://www.cisco.com/c/en/us/td/docs/routers/C8000V/config/c8000v-installation-configuration-guide/install-cisco-catalyst-8000v-in-kvm-environment/create-vm-using-cli.html
