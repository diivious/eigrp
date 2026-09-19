# Post Command Audit Item 5 Closure

Copyright (C) 2026 Donnie V. Savage

## Scope

Post Command Audit item 5 is the command-target pass for these module families:

```text
interface controls/auth
filtering/redistribution
summaries/metrics/timers
neighbor policy/logging
```

The closure criterion is the project feature-target rule: every command terminates
at a feature-owned EIGRP target, configuration is retained independently of runtime
availability, and an incomplete runtime path reports the structured EIGRP
`NOT_IMPLEMENTED` result from that real target rather than from a generic CLI stub.

This item does not redefine a command-target pass into implementation of every
missing protocol/runtime subsystem.

## Completed runtime behavior

The item-5 target pass includes live runtime behavior where the required protocol
machinery already exists:

- interface bandwidth/delay, hello/hold, passive state, split horizon, and interface shutdown/start handling;
- named MD5/key-chain authentication;
- distribute-list runtime filter state and host policy evaluation;
- metric K-values, variance, and maximum-hop enforcement;
- neighbor-change logging enable/disable.

Maximum-hop enforcement now increments the accumulated metric-vector hop count and
marks a path inaccessible when the configured limit is exceeded. Delay accumulation
uses saturating arithmetic rather than allowing the 64-bit metric delay to wrap.

## Retained targets with incomplete runtime machinery

The following commands still intentionally return `EIGRP_RESULT_NOT_IMPLEMENTED`
when an active runtime is present. Their real module targets retain/remove
configuration first, so they are no longer command-target stubs:

- interface bandwidth-percent pacing;
- `next-hop-self` behavior requiring the remaining next-hop/AddPath wire support;
- direct-password HMAC-SHA-256 authentication;
- offset-list metric application;
- redistribution route injection/route-map processing and redistribution prefix enforcement;
- manual/automatic summary synthesis and summary-metric application;
- default-metric runtime consumption, traffic-share behavior, and metric holddown;
- ACTIVE-time enforcement;
- neighbor prefix-limit enforcement and warning-rate limiting.

These statuses are deliberate boundaries for later runtime work. They must not be
silently reported as implemented.

## Ownership and lifecycle

Address-family configuration owns the new retained module state. AF teardown frees
that state through module-owned cleanup functions so configuration does not survive
an AF delete accidentally.

FRR northbound code only normalizes management values and calls the EIGRP-owned
module targets. Host/YANG objects are not introduced into the portable target APIs.
IPv6 named-mode continues to retain the same configuration through these targets
without pretending that the IPv6 runtime/data path exists.

## Regression guards

`test/portable/cli/eigrp_command_targets/` guards item-5 ownership, target routing,
truthful incomplete-runtime results, supported runtime mutations, cleanup, hop-limit
behavior, and neighbor-change logging integration.
