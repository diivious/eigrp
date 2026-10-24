# 2R-neighbor: IPv4 and IPv6 named-mode interoperability

Two real routers are deployed and configured by **native netlab**:

| Node | Device | Image | Data interface | IPv4 | IPv6 |
|---|---|---|---|---|---|
| oe | FRR + OpenEIGRP | `ghcr.io/diivious/openeigrp-frr:0.3.0` | `eth1` | `10.44.53.1/30` | `2001:db8:4473:1::1/64` |
| csr | CSR1000v | `vrnetlab/vr-csr:17.03.06` | `GigabitEthernet2` | `10.44.53.2/30` | `2001:db8:4473:1::2/64` |

**Both routers:** named process `OpenEIGRP`, AS **4473**, separate IPv4
and IPv6 unicast address families. No secondary EIGRP process is defined.

The native netlab topology is `topology.yml`; it enables `frr.daemons: [ eigrpd ]`
and applies `../../config/eigrp-named.j2` to **both** routers through native
FRR/IOS XE configuration delivery. The built-in netlab EIGRP module is omitted
because its IOS template generates classic mode. One shared template generates
named-mode EIGRP for each router using netlab-provided interface names and
addresses. The UUT driver never configures EIGRP or interface addresses.

Run from macOS or Debian development checkout:

```bash
netlabs/tools/remote-uut.sh --scenario 2R-neighbor
```

StormTrooper is the execution platform. The run checks both routers' actual
IPv4 and IPv6 neighbor tables, using observed link-local IPv6 addresses; it
records PASS/FAIL independently, CLI logs, deployment output, and cleanup
results. The remote runner invokes `netlab down` after execution, whether the
scenario passes or fails. It retains session state if cleanup fails.

**Startup noise:** Cisco IOS XE takes time to boot. Transient SSH connection
errors are recorded in `cisco-readiness.log` rather than printed repeatedly;
if readiness never succeeds, the final error includes the last failure.
Remote staging disables macOS xattr archiving to prevent harmless
`LIBARCHIVE.xattr.com.apple.quarantine` messages.

The scenario currently qualifies **neighbor formation only**, not route
advertisement, withdrawal, DUAL convergence, or descriptor ownership. It is
independent of the portable `make uut` regression gate.

This native netlab topology/template combination has **not** been run on
StormTrooper in this delivery; actual Cisco/OpenEIGRP CLI compatibility
with the shared template still requires the live test.
