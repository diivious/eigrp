# OpenEIGRP OpenWrt configuration, retention, reload and host policy (OW-08)

Copyright (C) 2026 Donnie V. Savage

## 1. Boundaries

OpenWrt owns UCI syntax, configuration storage, keychain lookup and prefix
policy. The portable core owns EIGRP semantic configuration and neighbor/
DUAL decisions. All OpenWrt-specific symbols are `eigrp_owrt_*` except the
required `eigrp_sys_*` host contract implementations. The portable core never
sees `uci_context`, `uci_package`, `uci_section`, `uci_option`, or OpenWrt
package objects.

```text
/etc/config/openeigrp   ──────►  libuci
                                  │
                       eigrp_owrt_uci_load()
                                  │
                       eigrp_owrt_config_validate()
                          all sections, first
                                  │
                       eigrp_owrt_config_replace()
                                  │
                 ┌────────────────┴────────────────┐
                 ▼                                 ▼
         portable eigrp_cli.h             eigrp_owrt_policy.c
         owning semantic targets          prefix rules + keychain
                 │                                 │
         portable EIGRP state          eigrp_sys_* public hooks
```

The loader and normalized configuration model are implemented **now**.
OW-09 now supplies the procd-managed daemon and UCI SIGHUP reload. The
configuration semantic APIs remain OW-08-owned. Custom ubus management RPCs
are not currently exposed; the operational console uses a local socket.

## 2. UCI sections

- `config process 'NAME'`: `option name` (optional; defaults to section id),
  `shutdown` (optional Boolean). Parent identifiers are **case-sensitive**.
- `config address_family`: `process`, `afi` (`ipv4` or `ipv6`), `asn` (1..65535),
  `vrf` (`default` only), optional `shutdown`, `router_id`, `distance_internal`,
  `distance_external`, `variance`, `maximum_paths`, `active_time`.
- `config network`: `process`, `afi`, `asn`, optional `vrf`, and IPv4 `prefix`
  (`10.0.0.0/8`). IPv6 `network` statements are not part of this API.
- `config interface`: `process`, `afi`, `asn`, optional `vrf`, `ifname`,
  optional `hello_interval`, `hold_time`, `passive`, `split_horizon`,
  `next_hop_self`, `shutdown`, `bandwidth`, `delay`, `bandwidth_percent`,
  `keychain`, `auth_mode` (`none`, `md5`, `hmac-sha256`) and `hmac_password`.
  Use kernel interface names such as `br-lan`, not netifd logical names
  such as `lan`. A `default` interface template is supported by the portable interface API.
- `config keychain`: `name`, `key_id`, `secret`. The currently selected
  keychain model uses one active key per section; protect this UCI file.
- `config prefix_rule`: `name`, `prefix`, `action` (`permit` or `deny`).
  Rules of a given name are evaluated in UCI section order; a known list with
  no matching rule denies. Prefix rules apply to IPv4 and IPv6 CIDRs.

Unknown section types/options, unsupported VRFs and malformed values are
rejected. The loader never silently discards fields or treats an incomplete
feature as configured runtime behavior. Every rejected operation carries an
`eigrp_result_t` and a section-specific diagnostic.

### Example: named IPv4 + retained IPv6, multiple AS, case-sensitive parents

```text
config process 'savage'
    option name 'savage'

config address_family
    option process 'savage'
    option afi 'ipv4'
    option asn '4453'
    option router_id '10.1.2.3'

config network
    option process 'savage'
    option afi 'ipv4'
    option asn '4453'
    option prefix '10.0.0.0/8'

config interface
    option process 'savage'
    option afi 'ipv4'
    option asn '4453'
    option ifname 'br-lan'
    option hello_interval '5'
    option hold_time '15'

config address_family
    option process 'savage'
    option afi 'ipv6'
    option asn '4453'

config address_family
    option process 'savage'
    option afi 'ipv4'
    option asn '4454'

config process 'SAVAGE'
    option name 'SAVAGE'

config address_family
    option process 'SAVAGE'
    option afi 'ipv4'
    option asn '4453'
```

This follows the project's development order: named IPv4 AS 4453, IPv6
AS 4453, multiple AS under the same parent, then distinct named parents.

## 3. Apply/reload and reset semantics

```text
UCI read ──► normalize ──► validate all ──► remove previous named owners
                                │                     │
                                └─────────► create named parents
                                             ▼
                                      create address families
                                             ▼
                                      apply network settings
                                             ▼
                                      apply interface settings
                                             ▼
                                      bind new host policies
```

`eigrp_owrt_uci_reload(directory, &active, &diagnostic)` parses and validates
new configuration **before** mutating portable state. During replacement it
calls the owning `eigrp_cli.h` functions; parents are created before AFs and
AFs before child configuration regardless of UCI section order. On a
semantic error, it reports the original result, removes partially created
candidate parents and attempts to replay the former snapshot. This is
**best-effort rollback**, not atomic kernel/network reconfiguration.

`eigrp_owrt_config_replace` retains an owned snapshot. The OW-09 daemon must
serialize reload and policy consumers to avoid use-after-free, start the
portable core before calling reload and call `eigrp_owrt_config_policy_bind(NULL)`
before disposing of the last snapshot.

UCI writeback uses its native commit semantics. Standard commands remain
available (`uci set`, `uci delete`, `uci commit openeigrp`).
`eigrp_owrt_uci_option_write` additionally validates the **prospective**
configuration before committing an option set/reset. In UCI, the documented
`no`/reset operation is deleting that option: the core's defaults are restored
when the retained configuration is replayed. Deleting an entire named process
or AF section deletes its owning configuration during reload.

OW-09 connects procd reload and semantic replay in a running daemon.
The reload path uses OW-08 best-effort rollback, not an atomic transaction.

## 4. Host policy and keychains

```text
portable EIGRP API
  ├─ eigrp_sys_filter_evaluate() ───► configured prefix_rule list
  │                                     first matching CIDR, default deny
  ├─ eigrp_sys_auth_key_lookup() ───► configured keychain name / id / secret
  ├─ eigrp_sys_summary_leak_map_evaluate() ► named prefix_rule list
  └─ route-map / access-list APIs ──► EIGRP_RESULT_NOT_IMPLEMENTED
```

Prefix-list evaluation uses containment semantics and respects address family.
Unsupported access-lists and arbitrary route-map evaluation **do not permit
traffic by default** and return structured `NOT_IMPLEMENTED`. Unknown prefix
list names return `NOT_FOUND`. The keychain lookup copies a single active key
into caller-owned storage; no UCI object escapes the adapter.

HMAC passwords are retained as protected UCI configuration. Do not place
secrets in world-readable scripts, logs, command examples or environment
variables. The packaged sample config has no active credentials. Runtime
HMAC capability remains governed by the portable core.

## 5. Build and tests

Native host-neutral tests:

```sh
./tools/openwrt.sh --test
bash openwrt/test/config-test.sh
```

Combined nonprivileged UUTs:

```sh
./tools/openwrt.sh --uut
```

For actual kernel route mutation the user must request an isolated test:

```sh
./tools/openwrt.sh --kernel
```

Cross-compilation of the real `libuci` target is verified by OpenWrt's package
build on Aurora. Native recipe tests set `OPENEIGRP_UCI_BUILD=0` because Debian
and macOS development hosts do not necessarily have libuci headers; they
still compile `eigrp_owrt_config.c` and `eigrp_owrt_policy.c` and confirm
source staging. The OW-09 live service gate will additionally verify
persistence across restart, reload of a running adjacency and error handling
for unsupported policy objects.

## OW-09 live service connection

The procd init script validates UCI before start/reload using
`openeigrpd --check`. A valid reload signals the daemon using `SIGHUP`; the
daemon reloads with `eigrp_owrt_uci_load` and then calls
`eigrp_owrt_config_replace` while holding its host runtime mutation lock.
Validation failure is logged without mutating the live snapshot; semantic
failure follows OW-08 best-effort rollback. `SIGTERM` gracefully stops the
watchers before terminating the core. See [daemon.md](daemon.md).
