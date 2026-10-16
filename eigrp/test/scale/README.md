# Portable EIGRP scale qualification

`make scale` runs the complete `make uut` gate first, then the portable scale
scenarios. `make scale-tests` runs only the scale scenarios for faster iteration.
Both targets are available in `make help`.

## neighbor_2048.py

Runs **one real R1 Unix UUT process** and injects RFC-format, checksummed
IPv4 EIGRP Hello packets from 2,048 independent simulated source addresses
on a single /16-connected interface. No 2,048-process requirement or root
privileges. The synthetic peers send ACKs for R1's reliable INIT/EOT UPDATEs
so all adjacencies can reach UP and queues can drain. The test verifies:

* R1 stays alive, with 2,048 distinct neighbors reported by its management
  state enumeration (the same portable neighbor state exposed to `show neighbors`).
* All 2,048 are UP, not merely allocated as PENDING neighbors.
* The interface has no pending output or reliable neighbor queues in two
  successive snapshots. This defines **quiescence** for this first scenario.
* The elapsed wall-clock time from first injected Hello to quiescence is
  printed as `INFORMATIONAL time_to_quiescence=...s` and is **never compared
  against a throughput or timing performance threshold**.

A progress watchdog defaults to 300 seconds only to prevent a deadlocked test
from hanging indefinitely. Override with `EIGRP_SCALE_WATCHDOG_SEC=<seconds>`
for a slower developer platform. A watchdog failure is a functional failure to
reach quiescence, not a performance benchmark assertion.

This scenario exercises real neighbor discovery, RTP handshaking, wire framing,
packet pacing, and management enumeration. It **does not** simulate a 2,048-node
DUAL topology or a 100,000-route redistribution storm. Those are future scale
scenarios, not claims about this first test.
