# Remote netlab execution (StormTrooper)

These commands are invoked from an OpenEIGRP checkout on macOS or Debian.
They require only local `ssh` and `tar` (or `sshpass` if not using SSH keys).
No local netlab/Containerlab/Docker installation is required.

## Layout

All remote netlab source files live inside `netlabs/`: this README and the
entry points are under `netlabs/tools/`, the default configuration is
`netlabs/remote-lab.cfg`, and scenarios and instructional labs are under
`netlabs/uut/` and `netlabs/labs/`. The repository root `tools/` is not used.

## Authentication and configuration

Edit `netlabs/remote-lab.cfg` (a trusted Bash-sourced configuration file):

```sh
REMOTE_HOST=StormTrooper
REMOTE_USER=netlab
REMOTE_PASSWORD=netlab
REMOTE_SSH_PORT=22
```

The requested default password is included **only as a default setting**. The
provisioned `netlab` account currently has no password. To use password SSH,
set a password on StormTrooper and enable password authentication in its SSH
server, then install `sshpass` on the development machine. The scripts use
`SSHPASS` via the environment, not a password on the command line.

**SSH keys are preferred**: the client first tests noninteractive public-key
authentication, then falls back to `sshpass`. Config files containing real
secrets should not be shared or committed. Use `REMOTE_LAB_CONFIG=/path/to/private.cfg`
for a private override, and `chmod 600` for it.

## Interactive labs

```sh
netlabs/tools/remote-lab.sh --lab 101
netlabs/tools/remote-lab.sh --status
netlabs/tools/remote-lab.sh --connect
netlabs/tools/remote-lab.sh --stop
```

`--lab NNN` only accepts implemented labs with a
`netlabs/labs/NNN/topology.yml`. The `netlabs/README.md` curriculum is currently
**a planned catalog, not executable labs**, so no lab numbers are deployable in
this initial framework. A successful start retains the lab until `--stop`.
`--connect` prompts for a router and runs `netlab connect` interactively.

## Automated UUT

```sh
netlabs/tools/remote-uut.sh --list
netlabs/tools/remote-uut.sh --scenario 2R-neighbor
```

The initial UUT tests actual bidirectional IPv4 and IPv6 EIGRP adjacency
between OpenEIGRP-FRR 0.3.0 and CSR1000v 17.03.06. It uses native netlab
provisioning and the shared `netlabs/config/eigrp-named.j2` template.
Pinned image tags must already exist on StormTrooper; it never substitutes a
mutable `current` tag. The UUT produces JSON PASS/FAIL results and CLI/log
artifacts in `netlabs/results/` on the development host.

The initial UUT is a **live compatibility scenario** with its own YAML schema.
It does not replace portable YAML UUT tests run by `make uut`; that harness has
internal and broker assertions unavailable to physical VM peers.

## Exclusivity, recovery, and security

StormTrooper alone owns the session marker and an exclusive `flock` lock under
`/home/netlab/.remote-lab/`. Both commands are serialized across clients.
A running lab persists after SSH disconnect. A live UUT holds the lock through
validation, evidence collection, and teardown. Unrecoverable cleanup errors
retain the session marker instead of reporting success or permitting a second
run. After a terminated UUT has released its lock, `remote-lab.sh --stop` can
clean up stale UUT resources before starting again.

Uploaded execution files go to `/home/netlab-data/runs/`; results remain in
`/home/netlab-data/results/`. The shell runner is installed at
`/home/netlab/.remote-lab/runner.sh` on first use.

Docker/Containerlab groups on StormTrooper are effectively root-equivalent.
Grant SSH access only to trusted development machines. Lab inputs and remote
commands are repository-controlled, not downloaded from mutable upstream URLs.

## Test and qualification status

```sh
python3 -m unittest discover -s netlabs/uut/tests -p 'test_2r_neighbor.py' -v
netlabs/tests/test_remote_runtime.sh
```

The tests exercise Python neighbor validation and Bash session transitions with
controlled substitutes for remote tools. They cannot prove Cisco IOS XE boots,
OpenEIGRP interoperates, or cleanup works on StormTrooper. Those require an
actual run of `remote-uut.sh` on the provisioned hardware.

## Moving from earlier remote-lab overlays

A complete-file overlay does not delete files in the receiving checkout. If the
first remote-lab overlay has already been applied, the following are obsolete
**after verifying the replacement files**:

```text
tools/remote/live-neighbor.py
eigrp/test/uut/tests/test_remote_neighbor.py
eigrp/test/uut/tests/test_remote_runtime.sh
netlabs/uut/neighbor-two-router/
```

Remove only these earlier remote-lab additions, after checking for local edits.
Leave the existing portable UUT framework and its other tests untouched.

If the earlier layout overlay was applied, after checking for local edits
remove its obsolete `tools/remote-lab.sh`, `tools/remote-uut.sh`,
`tools/remote-lab.cfg`, and `tools/remote/` files. Do not remove unrelated
repository `tools/` scripts.

## Native netlab integration

For live UUTs, netlab owns topology, interface addressing, Docker/KVM startup,
and the initial configuration of both platform types.
The shared `netlabs/config/eigrp-named.j2` is a named-mode EIGRP configuration
rendered once per router; its output is delivered by netlab's platform-specific
FRR/IOS XE transport. The UUT driver performs **read-only** CLI validation.

`remote-uut.sh` stages `netlabs/config/` along with the selected scenario.
Remote cleanup invokes `netlab down`, while interactive labs retain the
existing explicit `remote-lab.sh --stop` lifecycle.

## Live execution progress

The UUT client prints remote progress immediately (staging, deployment and
configuration, readiness, IPv4 validation, IPv6 validation, and teardown). Both
SSH and runner output are streamed through `tee`; the remote report still contains
`runner.log` and diagnostics. Final PASS/FAIL is based on the original runner
exit code, not `tee` or successful cleanup.

Native `netlab up` performs deployment **and** initial configuration as one
operation; progress reports this combined phase rather than implying there is a
separately timed configuration step. Details from netlab are saved to `deploy.log`.
