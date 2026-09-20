# FRR integration patches

This directory contains the small set of changes that EIGRP requires outside
FRR's `eigrpd/` directory.

`tools/frr-install.sh` owns patch installation. `series` defines patch order
when one patch depends on another. Each patch is applied from the FRR repository
root with `git apply` and is handled idempotently:

- reverse `git apply --check` succeeds: the patch is already present;
- for known managed patches, a strict semantic marker/schema check can also
  establish that an earlier patch is already present after later patches have
  changed adjacent context;
- forward `git apply --check` succeeds: apply the patch;
- none of those checks succeeds: stop and require review of the FRR source
  drift.

Patches must not be applied with fuzz or silently rewritten by the installer.

Current integration patches:

- `vtysh-named-eigrp.patch` lets FRR `vtysh` enter `EIGRP_NODE` for both the
  numeric and named `router eigrp` forms.
- `eigrp-named-yang.patch` adds the named-process parent and IPv4/IPv6
  address-family keys to the authoritative FRR EIGRP YANG model. The legacy
  numeric-AS model remains unchanged.
- `eigrp-named-af-config.patch` extends that named address-family schema with
  retained address-family configuration for router ID, IPv4 networks, static
  IPv4/IPv6 neighbors, and address-family shutdown state.
- `eigrp-named-af-interface.patch` adds retained `af-interface default` and
  concrete-interface configuration, including interface bandwidth/delay,
  bandwidth percentage, hello/hold timers, passive state, authentication
  references, next-hop-self, split horizon, IPv4 summary addresses, and
  interface shutdown state.
- `eigrp-named-topology.patch` adds the retained named `topology base`
  hierarchy and named topology configuration nodes.
- `eigrp-named-topology-callbacks.patch` tightens compound topology command
  leaves to mandatory values so FRR northbound callback validation matches the
  parent create/destroy plus child modify ownership model.
- `frr-eigrp-yang.patch` extends the retained EIGRP named-mode
  configuration schema for the classic-inherited command surface: logging,
  neighbor metadata/prefix limits, full prefix-limit policy, maximum paths,
  metric holddown/maximum hops, event-log size, distribute-list, redistribution
  route-map/prefix limits, HMAC-SHA-256 credentials, and complete summary forms.
- `eigrp-named-ipv6.patch` generalizes the managed summary-address and
  summary-metric keys to `inet:ip-prefix`, allowing the same named-mode summary
  command/runtime targets to retain IPv4 and IPv6 prefixes without enabling the
  IPv6 packet data path.

The YANG patches modify only `yang/frr-eigrpd.yang`. After patch installation,
`tools/frr-install.sh` removes FRR's generated `yang/frr-eigrpd.yang.c`; the FRR
build regenerates it from the authoritative patched YANG source.
