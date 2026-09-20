# EIGRP

Portable EIGRP implementation based on RFC 7868.

RFC 7868 and Donnie V. Savage's protocol design decisions define EIGRP
behavior. FRR and BIRD are host/integration frameworks; neither is the protocol
authority.

## Repository layout

```text
eigrp/
  eigrpd/         Portable/common EIGRP protocol code
  frr/            FRR-specific adapters and integration
    patch/        Managed changes required outside FRR/eigrpd/
    test/         FRR-native integration/UUT material
  bird/           BIRD-specific adapters and integration
  test/
    build/        Lightweight compile-smoke harness
    common/       Host-independent fixtures and packet samples
    portable/     Host-independent source/behavior tests
  specs/          Architecture, CLI, naming, process, and packetizing design
  tools/          Build, staging, patch, UUT, and packaging helpers
```

The canonical source tree is this repository. FRR staging creates a projection
of `eigrpd/` plus the FRR adapter files; do not develop against the staged FRR
copy and then copy changes back.

## Design documents

The normative project documents are:

- `specs/design-spec.md` - architecture, portability, ownership boundaries, and
  protocol implementation rules.
- `specs/code-conventions.md` - human-navigation naming and public target rules.
- `specs/cli-spec.md` - classic/named CLI scope, management flow, configuration
  retention, and operational command rules.
- `specs/process-spec.md` - named parent/address-family runtime ownership and
  packet demultiplexing model.
- `specs/packetizing-spec.md` - DUAL-to-packet pipeline, TLV dispatch, packet
  queues, pacing, and reliable transport ownership.
- `specs/refactor-work.md` - deliberately deferred pre-production cleanup. Work
  listed there is not permission for unrelated rename-only churn.

`specs/EIGRP-Config-Guide.md` and `specs/EIGRP-Named-Mode.md` are command/reference
notes. They are not protocol or implementation authority.

## Development rules

Portable protocol behavior belongs in `eigrpd/`. FRR-specific CLI/YANG,
management, Zebra/RIB, event-loop, interface, and operating-system adaptation
belongs under `frr/`. BIRD-specific integration belongs under `bird/`.

Portable APIs use EIGRP-owned types and structured result codes. FRR VTY, YANG,
Zebra, interface, event, stream, route-map, and equivalent BIRD objects must not
leak into portable protocol APIs.

Every configuration or operational feature terminates at its own real EIGRP
target. An incomplete feature keeps that target and returns the structured
EIGRP `NOT_IMPLEMENTED` result where required; it is not routed through a
generic CLI stub or unrelated dispatcher. Retained configuration must remain
writeable even when its runtime behavior is incomplete.

EIGRP Stub routing is outside project scope. Do not implement, import, or add
runtime tests for the EIGRP Stub feature.

Function/module naming is optimized for human navigation. The normal form is:

```text
eigrp_<module>.c
eigrp_<module>_<object>_<action>()
```

See `specs/code-conventions.md` before adding or renaming public APIs.

## Normal development workflow

### 1. Work in the canonical project tree

Make source changes in `eigrpd/`, `frr/`, or `bird/` according to ownership.
Changes required outside FRR's `eigrpd/` directory are exceptional and are
carried as managed patches under `frr/patch/`.

### 2. Run the local fast gate

From the repository root:

```sh
make test
```

That runs:

```text
make smoke          standalone compile-smoke harness
make portable-test  host-independent pytest suite
```

Use the narrower targets while iterating:

```sh
make smoke
make portable-test
```

The smoke harness catches syntax/prototype drift but does not replace the full
FRR build/link gate.

### 3. Stage into an FRR checkout

Keep the EIGRP and FRR repositories as siblings when practical:

```text
~/devel/eigrp/
~/devel/frr/
```

Stage the current EIGRP source and FRR-native test payload:

```sh
tools/frr.sh --install --frr-root ../frr
```

The projection is:

```text
eigrpd/ + FRR adapter files in frr/  -> ../frr/eigrpd/
frr/test/                            -> ../frr/tests/eigrpd/
```

Staging uses `--no-patches`. It never modifies FRR-wide source.

### 4. Apply managed FRR patches explicitly

When the managed patch state changes, or when preparing a fresh FRR checkout:

```sh
tools/frr.sh --patch --frr-root ../frr
```

Patch application is idempotent. An already-applied patch is left alone. A
patch that is neither cleanly applicable nor recognized as already applied
stops the operation for source-drift review. Do not fuzz or force managed
patches.

`--install`, `--configure`, `--build`, `--check`, `--all`, and `--uut` do not
apply FRR-wide patches.

### 5. Run the FRR build gate

For an already configured FRR checkout:

```sh
tools/frr.sh --build --frr-root ../frr
```

For the complete configure/build/check gate:

```sh
tools/frr.sh --all --frr-root ../frr
```

Use `--configure-first` with `--build`, `--check`, or `--uut` when the FRR
configure state needs to be regenerated.

The minimum code-change gate is a successful FRR build with `eigrpd` linked.

### 6. Run the live named-mode UUT

On the Linux FRR UUT:

```sh
tools/frr.sh --uut --frr-root ../frr
```

This stages the current EIGRP source, builds and installs FRR, restarts FRR,
starts the just-built `eigrpd`, and drives the daemon with
`sudo vtysh -d eigrpd`.

Named-mode validation is intentionally ordered:

1. `router eigrp savage`, IPv4 AF AS 4453 - applicable commands, mutation,
   writeback, and documented `no` forms.
2. IPv6 AF AS 4453 - the same configuration/writeback discipline for commands
   applicable to IPv6.
3. Additional AS contexts under the same named parent.
4. Case-sensitive named parents such as `savage` and `SAVAGE`.

Do not attribute a parser/config-retention failure to runtime worker code until
the command has actually crossed the CLI/northbound boundary.

Set `EIGRP_UUT_INTERFACE` when the UUT interface is not `enp0s8`.

### 7. Run aggregate or remote UUT testing

`tools/frr-uut.sh` builds before running the selected test set. Locally:

```sh
tools/frr-uut.sh --frr-root ../frr
```

From another development host:

```sh
tools/frr-uut.sh --host USER@HOST --frr-root '~/devel/frr'
```

Useful subsets:

```sh
tools/frr-uut.sh --packet   --frr-root ../frr
tools/frr-uut.sh --portable --frr-root ../frr
tools/frr-uut.sh --frr      --frr-root ../frr
```

Required managed FRR patches must already be present on the UUT checkout.

## Debugging

For daemon debugging, stop any service-managed/manual EIGRP daemon before
starting another copy. From the FRR checkout, a typical GDB launch is:

```sh
sudo gdb eigrpd/.libs/eigrpd
```

Useful smoke commands include:

```sh
sudo vtysh -d eigrpd -c 'show running-config'
sudo vtysh -d eigrpd -c 'show ip eigrp topology'
sudo vtysh -d eigrpd -c 'show ip eigrp neighbors'
```

The UUT scripts contain the authoritative daemon-start and cleanup mechanics;
prefer them over hand-maintained launch sequences when validating a change.

## Packaging

Create a clean project ZIP with:

```sh
tools/backup.sh --zip eigrp.zip
```

Delivery ZIPs are rooted at `eigrp/` and omit local VCS/build/cache noise so
they can be copied into another checkout with normal recursive copy tools.

## Copyright and contribution history

Preserve existing copyright, SPDX, and author history in files derived from
FRR or earlier EIGRP implementations. New project files use Donnie V. Savage as
the copyright owner unless another author is intentionally identified.

Small, reviewable changes are preferred. Do not add alias wrappers, duplicate
old/new paths, or compatibility layers without an explicit approved migration
reason.

## Security

Security reports may be sent to:

```text
diivious [at] hotmail.com
```
