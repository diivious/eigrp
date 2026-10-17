# EIGRP route and fault scale testing

Base: GitHub `diivious/eigrp` branch `peer-scale`, commit
`1a7f15e1f78d749b42bc87c8b120e32f4c670e7f`.

The test overlay changes only Make targets and portable test harness files. It
uses actual checksummed EIGRP UPDATE/Hello/ACK packets on the Unix virtual wire
and real Unix-hosted EIGRP UUT processes. Route sources originate through the
actual host-RIB notification and redistribution APIs. No production module is
modified or simulated DUAL state inserted directly.

## Targets

| Root target | What it checks |
| --- | --- |
| `make scale` | Quick packet pacing, lost-ACK unicast retry, and reduced multicast ACK-backpressure/recovery gate |
| `make scale-peers` | Establish 2,048 peers and drain their initial RTP work |
| `make scale-internal` | Inject 100K classic internal routes into one R1 |
| `make scale-external` | Inject 100K classic external routes into one R1 |
| `make scale-line` | Propagate 100K mixed routes R1 → R2 → R3 |
| `make scale-kite` | Propagate 100K mixed routes over a three-router kite |
| `make scale-peer-routes` | 2,048 peers each send 50 routes (102,400 total) |
| `make scale-100k` | Run `scale-peers` followed by all five route-flood tests sequentially |
| `make scale-withdraw` | Install, withdraw and reinstall 100K redistributed routes; verify topology and RIB cleanup |
| `make scale-mcast` | Withhold ACKs from 300 peers during multicast, then release; verify CR MTU safety and RTP drain |
| `make scale-linkdown` | Drop/recover R1–R2 halfway through 100K-route ingress; verify all routers relearn |
| `make scale-mixed-tlv` | Use one TLV1 and two TLV2 neighbors; verify actual classic and MP UPDATE TLVs |
| `make scale-churn` | Change, withdraw and reinstate 10K routes repeatedly inside a 100K-route topology |
| `make scale-auth-md5` | Originate 100K redistributed static routes over a live MD5-authenticated R1–R2 adjacency |
| `make scale-stress` | Run the five non-authenticated stress tests sequentially |
| `make scale-auth` | Run MD5 authenticated flooding (`scale-auth-sha2` planned, not implemented) |
| `make scale-all` | Run `scale-100k`, `scale-stress`, then `scale-auth`, collecting suite failures |

The multicast test originates external routes through R1's real static-route
redistribution interface. Injecting routes from a peer on the same shared LAN
would test split horizon, not outbound multicast pacing. When a CR SEQUENCE
Hello fits the MTU, the test requires CR delivery before releasing slow-peer
ACKs. When the slow-peer exclusion list exceeds the MTU (the default
2,048-peer/300-slow-peer case), it instead requires multicast admission to
pause until those ACKs are released, then resume and drain.

The mixed-TLV test also originates redistributed routes on R1 rather than
expecting an UPDATE from a shared-LAN peer to be reflected onto the same
interface. It checks newly transmitted classic and multiprotocol TLVs after
the route injection, not merely the neighbor capability flags.

Every suite continues through its constituent tests after a failure, reports
PASS/FAIL for each, and exits unsuccessfully if any test failed. Tests execute
sequentially to avoid competing for RAM. `make scale` is deliberately the
quick TRANSMIT pacing/backpressure gate and does not run the full `make uut`.

All names are explicit. Old numbered targets and `scale-tests` are intentionally
not retained as aliases. SHA-256 authenticated flooding will be a separate
future `scale-auth-sha2` test after the required configuration path is ready.

## Running

```sh
make scale                         # Quick pacing/backpressure gate
make scale-peers
make scale-100k
make scale-mcast
make scale-stress
make scale-auth
make scale-all

# Lightweight development smoke (not a substitute for full qualification)
EIGRP_SCALE_ROUTES=1000 make scale-withdraw
EIGRP_SCALE_ROUTES=1000 EIGRP_SCALE_CHURN_ROUTES=100 make scale-churn
EIGRP_SCALE_ROUTES=1000 make scale-auth-md5
EIGRP_SCALE_PEERS=64 EIGRP_SCALE_MCAST_SLOW_PEERS=16 \
 EIGRP_SCALE_MCAST_ROUTES=48 make scale-mcast
EIGRP_SCALE_ROUTES=1000 make scale-linkdown
EIGRP_SCALE_TLV_ROUTES=48 make scale-mixed-tlv
```

## Parameters and limits

* `EIGRP_SCALE_ROUTES=100000`: route total in 100K and source-RIB tests.
* `EIGRP_SCALE_PEERS=2048`: peer-route and multicast tests.
* `EIGRP_SCALE_ROUTES_PER_PEER=50`: peer-route test.
* `EIGRP_SCALE_BATCH=24`: route TLVs per injected RTP packet.
* `EIGRP_SCALE_WATCHDOG_SEC=1200`: per-test deadlock watchdog.
* `EIGRP_SCALE_CHURN_ROUTES=10000`: prefixes changed per churn cycle (must be below base route count).
* `EIGRP_SCALE_CHURN_CYCLES=3`: number of churn cycles.
* `EIGRP_SCALE_MCAST_ROUTES=256`: multicast stress route count.
* `EIGRP_SCALE_MCAST_SLOW_PEERS=300`: peers whose ACKs are held until release.
* `EIGRP_SCALE_TLV_ROUTES=2048`: mixed-TLV route count.

The default 100K route tests can use significant RAM and may run for many
minutes. Monitor host memory and choose smaller sizes for an initial smoke.
The watchdog is a liveness requirement, **not** a subsecond convergence target.
Elapsed times and RSS are printed as information only. The tests count actual
UUT topology/RIB records with streaming reads, check that the required
adjacencies remain UP, and require every involved UUT process to survive.

The heavy scenarios must be run on a full checkout of `peer-scale`. Generator
unit tests alone cannot qualify protocol behavior or performance.
