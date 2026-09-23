# Contributing to EIGRP

Copyright (C) 2026 Donnie V. Savage

This is the process document. Architecture rules live in `specs/design-spec.md`.
The public host contract lives in `specs/integration-spec.md`. Do not treat this
file as a second design spec.

If you are new to the tree, start with `README.md`. It tells you which spec to
open for your job.

## 1. What you are expected to know

You do not need to know every EIGRP packet type on day one. You do need to know
which directory you are changing:

```text
eigrpd/   portable protocol. same behavior on every host
frr/      FRR adapter, CLI, YANG, Zebra, process main
bird/     reserved BIRD adapter. not an implementation yet
test/     host-independent tests and compile smoke
specs/    project rules. read before you invent a new pattern
tools/    FRR stage/build/UUT helpers
```

If the change is protocol behavior, it belongs in `eigrpd/`.
If the change is a public host API, update `specs/integration-spec.md` in the same patch as the header.
if the change is in a shim, then it belongs in the shim folder `eigrp/frr eigrp/bird, etc)`.

## 2. How work is delivered

This is a normal git project. Canonical source is
https://github.com/diivious/eigrpd

Do not send zip files, recursive copies, or `tools/backup.sh` output as a
contribution. Do not develop against a staged FRR copy of `eigrpd/` and copy
edits back.

Flow:

```text
1. Fork https://github.com/diivious/eigrpd
2. Clone your fork
3. Branch from current master
4. Make the change in this tree
5. Run the test gate in Section 7
6. Push the branch
7. Open a pull request against diivious/eigrpd master
```

Small, reviewable changes beat large mixed diffs. One feature or one defect per
pull request unless the work cannot be split.

### 2.1 Pull request text

The PR description is part of the review. It must answer:

```text
Issue
  What was wrong, missing, or incomplete.
  Point at a spec section, command, test, or failure if you have one.

Change
  What you edited and why that is the right place.
  Call out public API or spec updates.

Test
  Commands you ran.
  What passed.
  What you could not run and why.
```

A PR with no issue/change/test writeup will be sent back. We review the PR, ask questions, and merge it when it is accepted. Do not assume a green smoke run means it will be merged.

If you used AI to draft code, say so in the PR and be ready to defend the diff. See Section 4.

## 3. Talk to repo moderators first when

Ask before you:
- rename public or widely used internal symbols
- add a compatibility wrapper or second code path
- change whitespace, comment style, or include order across a file
- add a new public header or public function
- move code between `eigrpd/` and a host directory
- take an item from `specs/refactor-work.md`
- change managed patches under `frr/patch/`

Do the work first only when the change is local, obvious, and already covered by
an existing spec rule.

## 4. AI generated code

AI generated code is allowed. The contributor still owns the result. If you use an assistant, you must:
- understand every line you submit
- be able to explain why the change is correct
- be able to defend the design against `specs/design-spec.md` and, for host
  APIs, `specs/integration-spec.md`
- remove code you cannot explain
- run the test gate yourself

"The model wrote it" is not a review answer. If you cannot walk a moderator
through control flow, ownership, and failure paths, the change is not ready.

Do not paste generated code that also "cleans up" nearby functions. That is how
gratuitous diffs get into the tree.

## 5. Style and diff hygiene

Core code changes must follow the style already used in the file you touch.

Rules:
- Match local indent, brace, comment, and naming style.
- The repo `.clang-format` is the format reference. Do not invent a new one.
- Do not reformat a file because a tool wants to.
- Do not add or remove whitespace on lines you did not change.
- Do not sort includes, rename locals, or wrap comments as a side effect.
- Do not land drive-by cleanups in a feature or bug fix.

If a file is messy and you want to clean it, say so to the repo moderators
before you touch it. Unrelated style churn will be rejected.

Naming for new portable APIs follows `specs/design-spec.md`:

```text
eigrp_<module>.c
eigrp_<module>_<object>_<action>()
```

Do not derive portable names from Cisco CLI nesting or from FRR YANG paths.

## 6. Copyright and history

Keep existing copyright, SPDX, and author lines. Refactoring a file is not permission to erase prior authorship.
New project files use Donnie V. Savage as copyright owner unless another author is the actual writer and is identified on purpose.

## 7. Test gate

From the repository root:

```sh
make test
```

That is the local fast gate. It is not the whole gate.

For protocol or FRR adapter changes, also:

```sh
tools/frr.sh --install --frr-root ../frr
tools/frr.sh --build --frr-root ../frr
```

If you changed managed FRR-wide files, apply patches on a fresh FRR checkout
with `tools/frr.sh --patch` first. Do not fuzz or force a patch.

If you changed named-mode CLI, config retention, or writeback, run the live UUT:

```sh
tools/frr.sh --uut --frr-root ../frr
```

CLI work is not done until parse, mutation, running-config writeback, and the
documented `no` form are shown to work.

## 8. Public API changes

The installed public headers are:

```text
eigrpd/eigrp.h
eigrpd/eigrp_cli.h
eigrpd/eigrp_mgnt.h
eigrpd/eigrp_rib.h
eigrpd/eigrp_sys.h
```

If you add or change a public type or function:

1. Update the header.
2. Update the matching entry in `specs/integration-spec.md`.
3. Update the host adapter that implements or calls it.
4. Add or extend a test that would fail if the contract regresses.

A public symbol with no spec entry is not a stable API.

Host code must not include private `eigrpd/` module headers to "get it working."
If the public contract is missing a call you need, that is a spec change, not an
excuse to reach into DUAL or packet objects.

## 9. What not to work on

EIGRP Stub routing is out of scope. Do not implement it, import it, or add
tests for it.

`specs/refactor-work.md` is a parking lot. Items there are not an invitation to
start a rename sweep.

## 10. Security reports

Send security reports to:

```text
diivious [at] hotmail.com
```

Do not open a public issue with an exploitable protocol or parser defect.
