# TASK1-8 — default-information design resolution

Copyright (C) 2026 Donnie V. Savage

## Resolution

The dependency recorded by this file is resolved by `TASK1-2.md`.

`eigrp_topology_default_information_update()` owns directional retained state
and applies the policy at the common prefix-filter boundary. `EIGRP_SET` enables
the selected direction and optionally attaches the configured standard access
list; `EIGRP_RESET` disables the direction and removes its policy reference.
Only the default prefix (`0.0.0.0/0` or `::/0`) is affected. Non-default routes
continue through the normal distribute/offset policy path.

A configured access-list is evaluated through `eigrp_sys_filter_evaluate()`. A
missing policy object, evaluation error, or explicit deny fails closed for the
default prefix. Inbound changes soft-resync neighbors; outbound changes trigger
the existing filter graceful-resync path so initialization/EOT/resync and normal
route advertisement use the same policy decision.
