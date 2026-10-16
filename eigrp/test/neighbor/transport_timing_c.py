# SPDX-License-Identifier: ISC
# Copyright (C) 2026 Donnie V. Savage
"""Execute the production portable neighbor RTT/RTO implementation with a fake clock.

The expected SRTT and RTO values are calculated independently in this test.
No fixed-time sleep, socket mock, or duplicate Python implementation is used.
"""

from pathlib import Path
import os
import subprocess
import textwrap
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p / "eigrp" / "code").is_dir())


def test_neighbor_rtp_adaptive_timers_and_karn_rule(tmp_path):
    source = tmp_path / "rtp_timing.c"
    executable = tmp_path / "rtp_timing"
    source.write_text(textwrap.dedent(r'''
        #include <assert.h>
        #include <stdint.h>
        #include <stdbool.h>
        #include "eigrp/code/eigrp.h"
        #include "eigrp/code/eigrp_structs.h"
        #include "eigrp/code/eigrp_neighbor.h"
        #include "eigrp/code/eigrp_packet.h"

        static uint64_t clock_msec = 100000;
        uint64_t eigrp_sys_monotime_msec(void) { return clock_msec; }

        static void sample(eigrp_nbr_t *nbr, eigrp_packet_t *pkt,
                           uint32_t delay, bool retransmitted)
        {
            pkt->sent_msec = clock_msec;
            pkt->retrans_counter = retransmitted ? 1 : 0;
            clock_msec += delay;
            eigrp_nbr_srtt_update(nbr, pkt);
            clock_msec += 20;
        }
        static uint32_t clamp6(uint32_t value)
        {
            uint64_t rto = 6ULL * value;
            return rto < 200 ? 200 : rto > 5000 ? 5000 : rto;
        }
        int main(void)
        {
            eigrp_nbr_t a = {0}, b = {0};
            eigrp_packet_t pkt = {0};
            uint32_t srtt;
            eigrp_nbr_rtt_clear(&a);
            eigrp_nbr_rtt_clear(&b);
            assert(eigrp_nbr_rto(&a) == 2000);
            assert(!a.srtt_valid);

            /* First 400 ms sample, then high 1200 ms sample. */
            sample(&a, &pkt, 400, false);
            srtt = 400;
            assert(a.srtt_valid && a.srtt_msec == srtt);
            assert(eigrp_nbr_rto(&a) == clamp6(srtt));
            sample(&a, &pkt, 1200, false);
            srtt += (1200 - srtt) / 8;
            assert(a.srtt_msec == srtt);
            assert(eigrp_nbr_rto(&a) == clamp6(srtt));

            /* A later low RTT must lower SRTT, not stay pinned. */
            sample(&a, &pkt, 100, false);
            srtt = srtt - (srtt - 100) / 8;
            assert(a.srtt_msec == srtt);
            assert(eigrp_nbr_rto(&a) == clamp6(srtt));

            /* Retransmitted ACK is ambiguous: Karn's algorithm forbids sampling. */
            sample(&a, &pkt, 4500, true);
            assert(a.srtt_msec == srtt);
            assert(eigrp_nbr_rto(&a) == clamp6(srtt));

            /* Invalid send timestamp cannot update RTT state. */
            pkt.retrans_counter = 0;
            pkt.sent_msec = 0;
            eigrp_nbr_srtt_update(&a, &pkt);
            assert(a.srtt_msec == srtt);

            /* ACK from another neighbor cannot change this neighbor's timing. */
            sample(&b, &pkt, 700, false);
            assert(b.srtt_valid && b.srtt_msec == 700);
            assert(a.srtt_msec == srtt);

            /* Backoff doubles, then saturates, and a clean sample restores RTT RTO. */
            eigrp_nbr_rto_backoff(&a);
            assert(eigrp_nbr_rto(&a) == (clamp6(srtt)*2 > 5000 ? 5000 : clamp6(srtt)*2));
            for (unsigned i = 0; i < 10; ++i) eigrp_nbr_rto_backoff(&a);
            assert(eigrp_nbr_rto(&a) == 5000);
            sample(&a, &pkt, 100, false);
            srtt -= (srtt - 100) / 8;
            assert(a.srtt_msec == srtt);
            assert(eigrp_nbr_rto(&a) == clamp6(srtt));

            /* Many tiny samples must never exceed the configured lower bound. */
            eigrp_nbr_rtt_clear(&b);
            sample(&b, &pkt, 1, false);
            assert(eigrp_nbr_rto(&b) == 200);
            return 0;
        }
    '''), encoding="utf-8")
    command = [os.environ.get("CC", "cc"), "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O0", "-ffunction-sections",
               "-fdata-sections", "-I", str(ROOT),
               str(source), str(ROOT / "eigrp/code/eigrp_neighbor.c"),
               *(["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections"]),
               "-o", str(executable)]
    build = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(executable)], cwd=ROOT, text=True, capture_output=True)
    assert run.returncode == 0, run.stderr
