# EIGRP Standalone Compile Smoke

This directory contains the lightweight compile-only harness for the FRR-backed
EIGRP source projection.

The repository keeps common source in `eigrpd/` and FRR-specific adapter source
in `frr/`.  The real FRR build receives those as one flattened `frr/eigrpd/`
directory.  This harness mirrors that projection under `test/build/obj/eigrpd/`
before compiling with `-fsyntax-only` and the narrow FRR stub headers under
`test/build/include/`.

It does not link, does not run FRR clippy, and is not a replacement for
`tools/frr.sh --build` against a complete FRR checkout.

Run from the repository root:

```sh
make -C test/build
```

or:

```sh
make smoke
```

Optional aliases:

```sh
make -C test/build cli
make -C test/build list
make -C test/build all-sources
```
