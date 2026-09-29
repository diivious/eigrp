# TASK1-6 metric-holddown decision

## Status

Resolved: the legacy `metric holddown` configuration feature is not part of this
EIGRP implementation. The CLI/YANG surface, retained configuration state,
portable target API, FRR northbound callbacks, writeback support, and associated
UUT expectations have been removed.

This decision does not affect the EIGRP neighbor hold-time mechanism. Neighbor
`hold_time`, `v_holddown`, and `t_holddown` remain protocol liveness state used
for adjacency expiration.
