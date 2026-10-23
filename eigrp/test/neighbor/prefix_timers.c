// SPDX-License-Identifier: ISC
// Copyright (C) 2026 Donnie V. Savage
/* Exercise the production prefix-limit timer engine and peer lifecycle. */
#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_neighbor.h"
#include "eigrp_list.h"
#include "eigrp_packet.h"
#include "eigrp_topology.h"
#include "eigrp_metric.h"
#include "eigrp_timer.h"
#include "eigrp_packetizer.h"
#include "eigrp_sys.h"

bool eigrp_sys_auth_key_lookup(const char *name, uint32_t *id,
                               char *key, size_t length)
{
    (void)name; (void)id; (void)key; (void)length;
    return false;
}

static void address_init(eigrp_addr_t *address, int afi, unsigned id)
{
    memset(address, 0, sizeof(*address));
    address->afi = afi;
    if (afi == AF_INET) {
        uint32_t addr = htonl(0x0a000000U | id);
        memcpy(&address->ip.v4, &addr, 4);
    } else {
        address->ip.v6.s6_addr[0] = 0xfe;
        address->ip.v6.s6_addr[1] = 0x80;
        address->ip.v6.s6_addr[15] = (uint8_t)id;
    }
}


static void run_timers(int afi)
{
    eigrp_instance_t eigrp = {0};
    eigrp_intf_t ei = {0};
    eigrp_addr_t src;
    eigrp_nbr_t *nbr;
    eigrp_prefix_limit_t limit = {.maximum = 1};
    uint64_t now, first_delay, second_delay;
    unsigned i;

    assert(pthread_mutex_init(&eigrp.peer_lock, NULL) == 0);
    assert(pthread_mutex_init(&eigrp.work_lock, NULL) == 0);
    assert(pthread_cond_init(&eigrp.work_cond, NULL) == 0);
    eigrp.thread_running = true; /* Real timer queue enabled on the test AF. */
    eigrp.tlv2_codec.encoder = eigrp_packet_encoder_safe;
    eigrp.eiflist = eigrp_list_create();
    eigrp.topology_table = eigrp_topology_table_create();
    eigrp.topology_changes = eigrp_list_create();
    ei.eigrp = &eigrp;
    ei.nbrs = eigrp_list_create();
    assert(eigrp.eiflist && ei.nbrs && eigrp.topology_table && eigrp.topology_changes);
    assert(eigrp_list_add(eigrp.eiflist, &ei));
    address_init(&src, afi, 23);
    nbr = eigrp_nbr_create(&ei, &src);
    assert(nbr);

    /* A receive-time violation marks the neighbor DOWN/ineligible but
     * cannot free packet- or DUAL-owned objects until the caller returns. */
    assert(eigrp_nbr_limit_mark_down(nbr, &limit));
    assert(nbr->limit_cleanup_pending);
    assert(nbr->limit_state == EIGRP_NBR_LIMIT_NONE);
    assert(nbr->state == EIGRP_NEIGHBOR_DOWN);
    assert(!eigrp_nbr_is_eligible(nbr));
    assert(!nbr->t_limit_restart && !nbr->t_limit_reset);
    eigrp_nbr_limit_cleanup(nbr);
    assert(!nbr->limit_cleanup_pending);
    assert(nbr->limit_state == EIGRP_NBR_LIMIT_SUSPENDED);
    assert(nbr->t_limit_restart && nbr->t_limit_reset);
    eigrp_nbr_delete(nbr);
    assert(!eigrp.timer_head);
    nbr = eigrp_nbr_create(&ei, &src);
    assert(nbr);

    /* Warning-only never starts a penalty. */
    limit.warning_only = true;
    assert(!eigrp_nbr_limit_exceeded(nbr, &limit));
    assert(nbr->limit_restart_cnt == 0 && eigrp_nbr_is_eligible(nbr));
    limit.warning_only = false;

    for (i = 1; i <= 2; ++i) {
        now = eigrp_sys_monotime_msec();
        assert(eigrp_nbr_limit_exceeded(nbr, &limit));
        assert(nbr->limit_restart_cnt == i && !eigrp_nbr_is_eligible(nbr));
        assert(nbr->limit_state == EIGRP_NBR_LIMIT_SUSPENDED);
        assert(nbr->state == EIGRP_NEIGHBOR_DOWN);
        assert(nbr->t_limit_restart && nbr->t_limit_reset);
        assert(nbr->limit_restart_due_msec >= now + 300000U);
        assert(nbr->limit_restart_due_msec <= now + 300200U);
        assert(nbr->limit_reset_due_msec >= now + 900000U);
        assert(nbr->limit_reset_due_msec <= now + 900200U);
        /* An early timer callback must preserve ineligibility. */
        eigrp_nbr_limit_restart_expired(nbr);
        assert(!eigrp_nbr_is_eligible(nbr) && nbr->t_limit_restart);
        /* Model expiry in the real timer owner; no sleeps of five minutes. */
        eigrp_timer_cancel(&eigrp, &nbr->t_limit_restart);
        nbr->limit_restart_due_msec = now - 1U;
        eigrp_nbr_limit_restart_expired(nbr);
        assert(eigrp_nbr_is_eligible(nbr));
        assert(nbr->state == EIGRP_NEIGHBOR_DOWN);
        assert(!nbr->t_limit_restart && nbr->t_limit_reset);
    }

    /* Config values are snapshotted per violation, including restart count. */
    limit.restart_minutes = 9;
    limit.restart_count = 3;
    assert(eigrp_nbr_limit_exceeded(nbr, &limit));
    assert(nbr->limit_restart_cnt == 3 && nbr->limit_state == EIGRP_NBR_LIMIT_LOCKED);
    assert(!nbr->t_limit_restart && !nbr->t_limit_reset);
    assert(!eigrp_nbr_limit_exceeded(nbr, &limit));
    eigrp_nbr_limit_restart_expired(nbr);
    eigrp_nbr_limit_reset_expired(nbr);
    assert(nbr->limit_state == EIGRP_NBR_LIMIT_LOCKED);
    eigrp_nbr_delete(nbr);
    assert(!ei.nbrs->count && !eigrp.timer_head);

    /* Reset after a quiet interval clears counters without changing
     * operational adjacency.  Dampening increases, then decays, delay. */
    nbr = eigrp_nbr_create(&ei, &src);
    assert(nbr);
    limit = (eigrp_prefix_limit_t){.maximum=1, .dampened=true,
        .restart_minutes=2, .reset_time_minutes=15, .restart_count=5};
    assert(eigrp_nbr_limit_exceeded(nbr, &limit));
    now = eigrp_sys_monotime_msec();
    first_delay = nbr->limit_restart_due_msec - now;
    assert(first_delay > 118000U && first_delay <= 120000U);
    assert(nbr->limit_penalty_q16 == 65536U);
    eigrp_timer_cancel(&eigrp, &nbr->t_limit_restart);
    nbr->limit_restart_due_msec = now - 1U;
    eigrp_nbr_limit_restart_expired(nbr);
    assert(eigrp_nbr_is_eligible(nbr));
    assert(eigrp_nbr_limit_exceeded(nbr, &limit));
    now = eigrp_sys_monotime_msec();
    second_delay = nbr->limit_restart_due_msec - now;
    assert(second_delay > first_delay * 19U / 10U);
    assert(second_delay <= first_delay * 21U / 10U);
    assert(nbr->limit_restart_cnt == 2);
    /* Expire reset first: restart timer continues independently. */
    eigrp_timer_cancel(&eigrp, &nbr->t_limit_reset);
    nbr->limit_reset_due_msec = now - 1U;
    eigrp_nbr_limit_reset_expired(nbr);
    assert(nbr->limit_restart_cnt == 0 && !nbr->limit_penalty_q16);
    assert(nbr->limit_state == EIGRP_NBR_LIMIT_SUSPENDED);
    assert(nbr->t_limit_restart);
    eigrp_timer_cancel(&eigrp, &nbr->t_limit_restart);
    nbr->limit_restart_due_msec = now - 1U;
    eigrp_nbr_limit_restart_expired(nbr);
    assert(eigrp_nbr_is_eligible(nbr));
    /* Half-life decay: earlier penalty 1.0 becomes 0.5 before next addition. */
    assert(eigrp_nbr_limit_exceeded(nbr, &limit));
    assert(nbr->limit_penalty_q16 == 65536U);
    eigrp_timer_cancel(&eigrp, &nbr->t_limit_restart);
    nbr->limit_restart_due_msec = now - 1U;
    eigrp_nbr_limit_restart_expired(nbr);
    nbr->limit_decay_msec = eigrp_sys_monotime_msec() - 180000U;
    assert(eigrp_nbr_limit_exceeded(nbr, &limit));
    assert(nbr->limit_penalty_q16 >= 98000U && nbr->limit_penalty_q16 <= 98500U);
    eigrp_nbr_delete(nbr);
    assert(!ei.nbrs->count && !eigrp.timer_head);

    eigrp_packetizer_delete(&eigrp);
    eigrp_topology_table_delete(&eigrp, eigrp.topology_table);
    eigrp_list_delete(&eigrp.topology_changes);
    eigrp_list_delete(&ei.nbrs);
    eigrp_list_delete(&eigrp.eiflist);
    eigrp_nbr_warning_state_clear(&eigrp);
    pthread_cond_destroy(&eigrp.work_cond);
    pthread_mutex_destroy(&eigrp.work_lock);
    pthread_mutex_destroy(&eigrp.peer_lock);
}

int main(void)
{
    run_timers(AF_INET);
    run_timers(AF_INET6);
    return 0;
}
