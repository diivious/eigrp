# EIGRP Tools

These scripts support the project layout where `eigrpd/` is staged into an FRR
checkout and project-only material stays in the EIGRP repository. FRR-specific
development tooling uses the `frr-` prefix so a future host can provide its own
parallel driver, such as `bsd.sh`.

## Scripts

```text
frr.sh          Primary FRR development driver: smoke, stage, configure, build, check, clean.
frr-install.sh  Stage eigrpd/ and test/frr/ into an FRR checkout.
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

## UUT workflow

`frr-uut.sh` is the single UUT entry point. A UUT test run always stages and
builds the current EIGRP source before executing the selected tests. If the
build fails, tests are not run.

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
