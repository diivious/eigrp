EIGRP
=====

EIGRP is a portable EIGRP implementation project based on RFC 7868.

The current production host is FRR. The repository is organized so the daemon
source can be staged into an FRR checkout while project specs, tools, standalone
compile smoke tests, and portable tests stay outside the FRR daemon directory.

Project layout
--------------

```text
eigrp/
  eigrpd/         Common EIGRP daemon/core source
  frr/            FRR-specific adapters and integration payload
    patch/        Required changes outside FRR/eigrpd/
    test/         FRR-native test payload
  bsd/            Future BSD-specific adapters/integration
  test/
    build/        Standalone compile-smoke harness
    common/       Shared fixtures and packet samples
    portable/     Python/source-level tests that do not need FRR
  specs/          Project design, code conventions, refactor parking lot, and protocol notes
  tools/          Build, install, setup, backup, and UUT helpers
```


Project guidance
----------------

`PROJECT-INSTRUCTIONS.md` is the concise project-instruction version of the
governing rules. Detailed development naming/navigation rules live in
`specs/code-conventions.md`; deferred pre-production cleanup is tracked in
`specs/refactor-work.md`.

FRR staging
-----------

Clone FRR and this project as siblings:

```sh
git clone https://github.com/frrouting/frr.git frr
git clone git@github.com:diivious/eigrp.git eigrp
```

Install EIGRP into FRR and apply the required managed FRR-wide patches:

```sh
cd eigrp
tools/frr.sh --install --frr-root ../frr
```

To update only the managed FRR-wide patch state:

```sh
tools/frr.sh --patch --frr-root ../frr
```

The installer projects the repository layout into FRR:

```text
eigrpd/ + frr/*    -> ../frr/eigrpd/
frr/test/           -> ../frr/tests/eigrpd/
frr/patch/series    -> ordered patches applied at ../frr/ repository root
```

FRR-wide patches are applied idempotently.  An already-applied patch is left
unchanged; a patch that is neither applicable nor reverse-applicable stops the
install so source drift can be reviewed.

Build FRR normally, or use the helper. Build/configure/UUT staging never applies FRR-wide patches; use `--install` or `--patch` explicitly for that:

```sh
tools/frr.sh --configure --frr-root ../frr
tools/frr.sh --build --frr-root ../frr
```

Standalone compile smoke
------------------------

The standalone smoke harness catches syntax/prototype drift in selected daemon
files without requiring a full FRR build:

```sh
make -C test/build
```

or:

```sh
make smoke
```

This is not a replacement for the FRR build/link gate.

Tests
-----

`tools/frr-uut.sh` is the FRR UUT driver. A normal UUT run stages the current
EIGRP source into FRR without applying patches, builds FRR/eigrpd, and only then
runs the selected tests. Required managed patches must already be installed.
The default test selection is the portable and FRR-native test suites.

On a Linux machine that is itself the UUT:

```sh
tools/frr-uut.sh --frr-root ../frr
```

From another development host, sync the project to a remote Linux UUT and run
the complete build/test cycle there:

```sh
tools/frr-uut.sh --host uut --frr-root '~/devel/frr'
```

Select a narrower test set when needed:

```sh
tools/frr-uut.sh --packet   --frr-root ../frr
tools/frr-uut.sh --portable --frr-root ../frr
tools/frr-uut.sh --frr      --frr-root ../frr
```

Those UUT commands still build FRR first. To run portable tests without an FRR
build, use the project-local target directly:

```sh
make portable-test
```

To re-run bootstrap/configure before the UUT build:

```sh
tools/frr-uut.sh --configure-first --frr-root ../frr
```

To build without executing tests:

```sh
tools/frr-uut.sh --build-only --frr-root ../frr
```

Debugging
---------

Stop the service-managed EIGRP daemon before launching a debug copy:

```sh
sudo systemctl stop frr
sudo systemctl start frr
sudo kill -9 "$(pidof eigrpd)" 2>/dev/null || true
```

Then, from the FRR checkout:

```sh
sudo gdb eigrpd/.libs/eigrpd
```

Useful `vtysh` smoke commands:

```sh
vtysh -c 'configure terminal' -c 'router eigrp 4453' -c 'network 0.0.0.0/0'
vtysh -c 'show running-config'
vtysh -c 'show ip eigrp topology'
vtysh -c 'show ip eigrp neighbors'
```

Contributing
------------

Small, reviewable commits are preferred. Existing author/copyright history must
be preserved when refactoring existing FRR-derived files.

Security
--------

To report security issues, email:

```text
diivious [at] hotmail.com
```
