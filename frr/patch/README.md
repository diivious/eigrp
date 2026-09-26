# FRR integration patches

This directory contains the small set of changes that EIGRP requires outside
FRR's `eigrpd/` directory.

`tools/frr-install.sh` owns patch installation. `series` defines patch order.
Each upstream FRR file modified by this project is owned by exactly one managed
patch. A patch is applied from the FRR repository root with `git apply` and is
handled idempotently:

- reverse `git apply --check` succeeds: the patch is already present;
- forward `git apply --check` succeeds: apply the patch;
- neither check succeeds: stop and require review of the FRR source drift.

Patches must not be applied with fuzz, partial context, or force. A partially
modified or otherwise ambiguous target is a conflict, not an already-applied
state.

Current integration patches:

- `vtysh-named-eigrp.patch` owns the project changes to `vtysh/vtysh.c`. It
  lets FRR `vtysh` enter `EIGRP_NODE` for both the numeric and named
  `router eigrp` forms, including re-entering `router eigrp` directly from an
  EIGRP submode.
- `frr-eigrp-yang.patch` owns all project changes to `yang/frr-eigrpd.yang`.
  It is the flattened cumulative YANG patch and preserves alpha-15 functionality outside the redistribution schema correction:
  classic multi-instance support; named parent/address-family keys; retained
  address-family, `af-interface`, and `topology base` configuration; compound
  callback-required leaves; logging, neighbor policy/prefix limits, metric and
  filter controls, redistribution route-map/prefix-limit and protocol +
  route-instance identity, HMAC-SHA-256 credentials, and IPv4/IPv6 summary
  forms. Redistribution protocol/route-instance constraints follow the source
  identities FRR actually carries rather than treating every protocol as if it
  had a generic numeric process instance.

The YANG source is authoritative. After patch installation,
`tools/frr-install.sh` validates the expected grammar/schema shape and removes
FRR's generated `yang/frr-eigrpd.yang.c`; the FRR build regenerates that file
from the patched YANG source. More than one named-mode schema node is treated
as source drift and fails rather than being repaired by applying an overlapping
patch.

The flattened YANG patch remains the only managed owner of
`yang/frr-eigrpd.yang`. TASK 2 builds on the alpha-15 flattened result by
correcting redistribution source identity in that same patch; no second YANG
patch is layered on top.
