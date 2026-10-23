// SPDX-License-Identifier: ISC
// Copyright (C) 2026 Donnie V. Savage
/* Execute the portable neighbor/accounting/timer state APIs, not CLI text. */
#include <assert.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_neighbor.h"
#include "eigrp_list.h"
#include "eigrp_packet.h"
#include "eigrp_packetizer.h"
#include "eigrp_topology.h"
#include "eigrp_statistics.h"
#include "eigrp_timer.h"
#include "eigrp_sys.h"

bool eigrp_sys_auth_key_lookup(const char *name, uint32_t *id,
                               char *key, size_t size)
{
    (void)name; (void)id; (void)key; (void)size;
    return false;
}

struct nbr_limit_view {
    unsigned rows;
    char status;
    uint16_t restart_cnt;
    uint32_t restart_seconds;
    uint32_t reset_seconds;
    eigrp_afi_t afi;
};
static eigrp_result_t accounting_cb(const eigrp_stat_nbr_limit_t *s,
                                 void *arg)
{
    struct nbr_limit_view *out = arg;
    out->rows++;
    out->status = s->status;
    out->restart_cnt = s->restart_cnt;
    out->restart_seconds = s->restart_seconds;
    out->reset_seconds = s->reset_seconds;
    out->afi = s->neighbor_address.afi;
    assert(s->prefix_count == 0);
    return EIGRP_RESULT_SUCCESS;
}
struct timer_counts { unsigned hold_cnt, restart_cnt, reset_cnt; };
static eigrp_result_t timer_cb(const eigrp_timer_state_t *s, void *arg)
{
    struct timer_counts *out = arg;
    assert(s->neighbor_present && s->interface_name);
    if (s->type == EIGRP_TIMER_STATE_LIMIT_RESTART) {
        out->restart_cnt++;
        assert(s->expiration_seconds > 0);
    } else if (s->type == EIGRP_TIMER_STATE_LIMIT_RESET) {
        out->reset_cnt++;
        assert(s->expiration_seconds > 0);
    } else if (s->type == EIGRP_TIMER_STATE_PEER_HOLD) {
        out->hold_cnt++;
    }
    return EIGRP_RESULT_SUCCESS;
}
static eigrp_result_t neighbor_cb(const eigrp_nbr_state_t *s, void *arg)
{
    struct nbr_limit_view *out = arg;
    out->rows++;
    out->status = s->limit_status;
    out->restart_cnt = s->limit_restart_cnt;
    out->restart_seconds = s->limit_restart_seconds;
    out->reset_seconds = s->limit_reset_seconds;
    return EIGRP_RESULT_SUCCESS;
}

static void check_nbr_limit_state(eigrp_instance_t *runtime, eigrp_nbr_t *nbr,
                     char expected, uint16_t count, bool active)
{
    struct nbr_limit_view a = {0}, n = {0};
    struct timer_counts timers = {0};
    uint32_t total = UINT32_MAX;
    eigrp_instance_context_t context = {.runtime=runtime};
    eigrp_result_t result;

    result = eigrp_stat_accounting_iterate(&context, &total,
                                                   accounting_cb, &a);
    assert(result == EIGRP_RESULT_SUCCESS && total == 0);
    assert(a.rows == (expected ? 1U : 0U));
    if (expected) {
        assert(a.status == expected && a.restart_cnt == count);
        assert(a.afi == (nbr->src.afi == AF_INET6 ? EIGRP_AFI_IPV6 : EIGRP_AFI_IPV4));
    }
    result = eigrp_nbr_state_iterate(NULL, runtime, NULL, false,
                                     neighbor_cb, &n);
    if (expected) {
        assert(result == EIGRP_RESULT_SUCCESS);
        assert(n.rows == 1 && n.status == expected && n.restart_cnt == count);
        assert(a.restart_seconds == n.restart_seconds && a.reset_seconds == n.reset_seconds);
    }
    result = eigrp_timer_state_iterate(&context, timer_cb, &timers);
    assert(result == EIGRP_RESULT_SUCCESS);
    assert(timers.restart_cnt == (unsigned)active && timers.reset_cnt == (unsigned)active);
    if (active) {
        assert(a.restart_seconds > 0 && a.reset_seconds > 0);
    } else if (expected == 'D') {
        assert(a.restart_seconds == 0 && a.reset_seconds == 0);
    }
}

static void test_limit_state_read(eigrp_instance_t *runtime)
{
    eigrp_pfxlimits_summary_t state;
    eigrp_instance_context_t context = {.runtime = runtime};
    uint64_t now = eigrp_sys_monotime_msec();

    assert(eigrp_stat_pfxlimits_summary_read(NULL, &state) == EIGRP_RESULT_NOT_FOUND);
    assert(eigrp_stat_pfxlimits_summary_read(&context, NULL) == EIGRP_RESULT_INVALID_ARGUMENT);
    assert(eigrp_stat_pfxlimits_summary_read(&context, &state) == EIGRP_RESULT_SUCCESS);
    assert(state.process.state == 0 && state.process.prefix_cnt == 0);
    assert(state.redist.state == 0 && state.redist.prefix_cnt == 0);

    runtime->eigrp_pfxlimits_af_state = 1;
    runtime->eigrp_pfxlimits_af_restart_cnt = 2;
    runtime->eigrp_pfxlimits_af_penalty_q16 = 2 * 65536U;
    runtime->eigrp_pfxlimits_af_restart_due_msec = now + 2500;
    runtime->eigrp_pfxlimits_af_reset_due_msec = now + 5500;
    runtime->redist_limit_state = 2;
    runtime->redist_limit_restart_cnt = 3;
    runtime->redist_prefix_count = 7;
    assert(eigrp_stat_pfxlimits_summary_read(&context, &state) == EIGRP_RESULT_SUCCESS);
    assert(state.process.state == 1 && state.process.restart_cnt == 2);
    assert(state.process.penalty_q16 == 2 * 65536U);
    assert(state.process.restart_seconds >= 2 && state.process.restart_seconds <= 3);
    assert(state.process.reset_seconds >= 5 && state.process.reset_seconds <= 6);
    assert(state.redist.state == 2 && state.redist.restart_cnt == 3);
    assert(state.redist.prefix_cnt == 7 && state.redist.restart_seconds == 0);
    assert(state.redist.reset_seconds == 0);

    runtime->eigrp_pfxlimits_af_state = 0;
    runtime->eigrp_pfxlimits_af_restart_due_msec = 0;
    runtime->redist_limit_state = 0;
    runtime->redist_prefix_count = 0;
    assert(eigrp_stat_pfxlimits_summary_read(&context, &state) == EIGRP_RESULT_SUCCESS);
    assert(state.process.state == 0 && state.process.restart_seconds == 0);
    assert(state.process.restart_cnt == 2 && state.process.reset_seconds > 0);
    assert(state.redist.state == 0 && state.redist.prefix_cnt == 0);
    runtime->eigrp_pfxlimits_af_reset_due_msec = 0;
}

static void run(int family)
{
    eigrp_instance_t runtime = {0};
    eigrp_intf_t interface = {0};
    eigrp_addr_t address = {0};
    eigrp_nbr_t *nbr;
    eigrp_prefix_limit_t limit = {.maximum=1, .restart_count=2};
    uint64_t now;

    assert(!pthread_mutex_init(&runtime.peer_lock, NULL));
    assert(!pthread_mutex_init(&runtime.work_lock, NULL));
    assert(!pthread_cond_init(&runtime.work_cond, NULL));
    runtime.thread_running = true;
    runtime.tlv2_codec.encoder = eigrp_packet_encoder_safe;
    runtime.eiflist = eigrp_list_create();
    runtime.topology_table = eigrp_topology_table_create();
    runtime.topology_changes = eigrp_list_create();
    interface.eigrp = &runtime;
    interface.name = "test0";
    interface.nbrs = eigrp_list_create();
    assert(runtime.eiflist && runtime.topology_table
           && runtime.topology_changes && interface.nbrs);
    assert(eigrp_list_add(runtime.eiflist, &interface));
    address.afi = family;
    if (family == AF_INET) {
        address.ip.v4.s_addr = htonl(0x0a000002U);
    } else {
        address.ip.v6.s6_addr[0] = 0xfe;
        address.ip.v6.s6_addr[1] = 0x80;
        address.ip.v6.s6_addr[15] = 2;
    }
    nbr = eigrp_nbr_create(&interface, &address);
    assert(nbr);

    test_limit_state_read(&runtime);
    check_nbr_limit_state(&runtime, nbr, 0, 0, false); /* INIT not shown as penalty P. */
    eigrp_nbr_state_update(EIGRP_SET, nbr, EIGRP_NEIGHBOR_UP);
    check_nbr_limit_state(&runtime, nbr, 'A', 0, false);
    assert(eigrp_nbr_limit_exceeded(nbr, &limit));
    check_nbr_limit_state(&runtime, nbr, 'P', 1, true);

    now = eigrp_sys_monotime_msec();
    assert(eigrp_nbr_limit_remaining(0, now) == 0);
    assert(eigrp_nbr_limit_remaining(now, now) == 0);
    assert(eigrp_nbr_limit_remaining(now+1, now) == 1);
    assert(eigrp_nbr_limit_remaining(now+1000, now) == 1);
    assert(eigrp_nbr_limit_remaining(now+1001, now) == 2);

    eigrp_timer_cancel(&runtime, &nbr->t_limit_restart);
    nbr->limit_restart_due_msec = now - 1;
    eigrp_nbr_limit_restart_expired(nbr);
    assert(eigrp_nbr_is_eligible(nbr));
    assert(eigrp_nbr_limit_exceeded(nbr, &limit));
    check_nbr_limit_state(&runtime, nbr, 'D', 2, false);
    eigrp_nbr_delete(nbr);
    assert(!interface.nbrs->count && !runtime.timer_head);
    check_nbr_limit_state(&runtime, NULL, 0, 0, false);

    eigrp_packetizer_delete(&runtime);
    eigrp_topology_table_delete(&runtime, runtime.topology_table);
    eigrp_list_delete(&runtime.topology_changes);
    eigrp_list_delete(&interface.nbrs);
    eigrp_list_delete(&runtime.eiflist);
    eigrp_nbr_warning_state_clear(&runtime);
    pthread_cond_destroy(&runtime.work_cond);
    pthread_mutex_destroy(&runtime.work_lock);
    pthread_mutex_destroy(&runtime.peer_lock);
}
int main(void)
{
    run(AF_INET);
    run(AF_INET6);
    return 0;
}
