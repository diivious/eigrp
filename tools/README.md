# EIGRP Tools

These scripts support the split source layout where common EIGRP code lives in
`eigrpd/`, FRR adapters live in `frr/`, and `frr-install.sh` assembles the two
into the FRR checkout. FRR-specific
development tooling uses the `frr-` prefix so a future host can provide its own
parallel driver, such as `bsd.sh`.

## Scripts

```text
frr.sh          Primary FRR development driver: smoke, install, patch, configure, build, check, UUT, clean.
frr-named-uut.sh  Live named-mode config/writeback test using sudo vtysh -d eigrpd.
frr-install.sh  Assemble eigrpd/ + frr/ into FRR, install frr/test/, and apply frr/patch/.
frr-setup.sh    Debian development-machine setup helper.
frr-uut.sh      Build the FRR UUT, then run EIGRP tests; can drive a remote Linux UUT.
backup.sh       Create a clean project zip.
```

All script options use GNU-style `--long-option` arguments.

## FRR development workflow

Stage source into FRR directly when needed:

```sh
tools/frr-install.sh --frr-root ~/devel/frr
```

This assembles common `eigrpd/` source plus the top-level FRR adapter files in
`frr/` into FRR's single `eigrpd/` directory.  It also stages `frr/test/` and
applies the patches listed in `frr/patch/series` (falling back to `*.patch`
when no series file exists). Patch application is idempotent: an already-applied
patch is detected with a reverse `git apply --check`, while an FRR source
conflict stops the install for review.


Patch ownership is explicit. `--install` applies the managed FRR-wide patches as
part of installation. `--patch` applies only those patches. Configure, build,
check, and UUT actions may restage EIGRP source/test payloads, but they invoke
the installer with `--no-patches` and therefore never modify FRR-wide source.

```sh
tools/frr.sh --install --frr-root ~/devel/frr
tools/frr.sh --patch --frr-root ~/devel/frr
```

Run the standalone compile smoke:

```sh
tools/frr.sh --smoke
```

Configure and build FRR after staging EIGRP:

```sh
tools/frr.sh --configure --frr-root ~/devel/frr
tools/frr.sh --build --frr-root ~/devel/frr
```

Run the full configure/build/check gate:

```sh
tools/frr.sh --all --frr-root ~/devel/frr
```

Build, install, restart FRR, and run the live named-mode configuration UUT
against `eigrpd` without applying or changing FRR-wide patches:

```sh
tools/frr.sh --uut --frr-root ~/devel/frr
```

The live UUT uses `sudo vtysh -d eigrpd -c ...` for every operation. It is
intentionally staged so configuration/runtime defects are isolated instead of
being hidden by a large matrix. Stage 1 creates only `router eigrp savage` /
IPv4 AS 4453 and runs the complete applicable command, mutation, no-form, and
writeback suite. Only after IPv4/4453 passes does Stage 2 add IPv6 AS 4453 and
run the applicable IPv6 suite. Stage 3 then adds IPv4/IPv6 AS 6473 to verify
multiple autonomous-system contexts and `no address-family`. Stage 4 finally
verifies that `savage` and `SAVAGE` are distinct named processes.
`frr.sh --uut` installs the just-built FRR tree and restarts the `frr`
systemd service before invoking vtysh, so the test cannot accidentally exercise
an older installed daemon. The required patch state is a prerequisite established
with `--install` or `--patch`; UUT does not mutate it. Set `EIGRP_UUT_INTERFACE` when the test interface is
not `enp0s8`.

## UUT workflow

`frr-uut.sh` is the aggregate/local-or-remote UUT entry point. A UUT test run
always stages and builds the current EIGRP source before executing the selected
tests. This staging is performed without patch application. If the build fails,
tests are not run. The `--all` and `--frr` selections
run the same live named-mode configuration test used by `frr.sh --uut` before
any FRR-native pytest payload.

On a Linux machine that is itself the UUT:

```sh
tools/frr-uut.sh --frr-root ~/devel/frr
```

From another development host, sync the project to a remote Linux UUT, build
there, and run the tests there:

```sh
tools/frr-uut.sh --host uut --frr-root '~/devel/frr'
```

The quotes around a remote `~/...` path keep the local shell from expanding it
before the path is sent to the UUT.

The default test set is `--all`. Test subsets are available when needed:

```sh
tools/frr-uut.sh --packet   --frr-root ~/devel/frr
tools/frr-uut.sh --portable --frr-root ~/devel/frr
tools/frr-uut.sh --frr      --frr-root ~/devel/frr
```

These still build the UUT first. For a source-only portable test without an FRR
build, use the project test target directly:

```sh
make portable-test
```

To force bootstrap/configure before the UUT build:

```sh
tools/frr-uut.sh --configure-first --frr-root ~/devel/frr
```

To build the UUT without executing tests:

```sh
tools/frr-uut.sh --build-only --frr-root ~/devel/frr
```

## Project packaging

Create a clean project zip:

```sh
tools/backup.sh --zip eigrp.zip
```

## Local FRR service commands

```sh
sudo systemctl restart frr
sudo systemctl status frr
```

## Handy `vtysh` commands

```sh
vtysh -c 'show running-config'
vtysh -c 'configure terminal' -c 'service integrated-vtysh-config' -c 'end' -c 'write memory'
vtysh -c 'debug eigrp packets hello' -c 'debug eigrp packets update' -c 'debug eigrp transmit send' -c 'debug eigrp transmit recv'
vtysh -c 'configure terminal' -c 'router eigrp 4453' -c 'network 10.0.0.0/8' -c 'network 172.16.0.0/24' -c 'network 192.168.1.0/24'
```

## Test topology notes

```text
                         NAT to Host
                             |
                          (enp0s3)
                             |
RTR1(E1) ---- (enp0s8) FRR (enp0s9) ---- (E2)RTR2
                             |
                          (enp0s10)
```

FRR sample interfaces:

```text
enp0s3:  N/A
enp0s8:  10.0.0.250/8
enp0s9:  172.16.0.250/20
enp0s10: 192.168.1.250/24
```


## Manual eigrpd UUT startup

For UUT runs, `/etc/frr/daemons` is not changed. `eigrpd` may remain `no`; after the FRR service is restarted, the UUT starts `eigrpd` manually through FRR's installed `frrcommon.sh` daemon helper. This deliberately keeps `eigrpd` outside `watchfrr`, so killing the daemon during development does not cause watchfrr to respawn it.
