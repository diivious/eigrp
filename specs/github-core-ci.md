# EIGRP GitHub Core CI

Copyright (C) 2026 Donnie V. Savage

## 1. Purpose

GitHub Actions is a remote runner for the EIGRP-owned portable/core developer
pipeline. It does not define a second build or test pipeline. The repository
root `Makefile` remains the execution authority, and CI runs the same command a
developer runs locally:

```sh
make test
```

This keeps local development and hosted CI aligned. Changes to the portable/core
gate belong in the root `Makefile` and its owned test targets, not as duplicated
build or test orchestration in workflow YAML.

## 2. Workflow

The workflow is `.github/workflows/core.yml` and is named `EIGRP Core CI`.
It runs on:

- pushes;
- pull requests; and
- explicit `workflow_dispatch` requests.

The job matrix runs the same gate on:

- `ubuntu-latest`; and
- `macos-latest`.

Matrix fail-fast is disabled so a failure on one operating system does not
prevent the other operating system from reporting its result.

## 3. CI execution contract

Each matrix job performs only the runner preparation needed to execute the
repository-owned gate:

1. check out the repository;
2. provide Python 3;
3. install the Python test dependencies `pytest` and `PyYAML`; and
4. execute `make test` from the repository root.

The workflow must not expand `make test` into individual GitHub Actions steps.
The root Makefile owns ordering, continuation across gate components, and the
final success/failure result.

At the time this specification was written, `make test` covers:

- portable `eigrpd/code` build validation;
- `eigrpd/test`;
- Unix host `unix/code` build validation;
- `unix/test`;
- IPv4 Basic Core Gate;
- IPv6 Basic Core Gate; and
- RTP Core Gate.

If that developer pipeline changes, CI inherits the change through `make test`.
The workflow should change only when hosted-runner preparation or CI policy
itself changes.

## 4. Dependency boundary

Core CI qualifies EIGRP-owned portable/core code and the native Unix host used
by that pipeline. It does not qualify an external routing stack.

The workflow therefore must not:

- download, stage, build, or test FRR;
- invoke `tools/frr.sh`;
- run FRR Topotests;
- download, stage, build, or test BIRD;
- invoke BIRD platform tooling; or
- require another routing-stack checkout, VM, privileged networking, or host
  VLAN configuration.

FRR and future BIRD integration remain explicit platform validation performed
outside this core workflow. Native platform tests may have their own platform
workflows, but they are consumers of the portable/core contract rather than
prerequisites for `EIGRP Core CI`.

## 5. Failure diagnostics

The workflow pipes `make test` through `tee` into `make-test.log` and enables
shell `pipefail`. This preserves the real `make test` exit status while retaining
the complete console transcript.

On failure, the workflow uploads `make-test.log` as an artifact for the failing
operating-system job. UUT scenario failures already emit their expectation and
state diagnostics into the test output, so those diagnostics are retained in
the same artifact without adding CI-specific UUT behavior.

A failing protocol or scenario gate is a CI failure. The workflow must not
weaken, skip, or rewrite repository tests to make hosted CI green.

## 6. Status badge

The repository README displays the GitHub Actions status for
`.github/workflows/core.yml`. The badge reports the hosted result of the same
`make test` gate described here; it is not a separate quality metric.

## 7. Change rule

When changing core CI:

- keep `make test` as the primary CI operation;
- keep Linux and macOS running the same root target;
- put build/test orchestration in the root Makefile, not workflow YAML;
- keep FRR/BIRD and other external routing-stack dependencies out of this
  workflow; and
- preserve useful failure output or artifacts where practical.

The local acceptance check for CI pipeline changes remains:

```sh
make test
```
