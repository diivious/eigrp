// SPDX-License-Identifier: ISC
// Copyright (C) 2026 Donnie V. Savage
/* Production neighbor lifecycle exercised with real portable/Unix modules. */
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

static void run_lifecycle(int afi)
{
    eigrp_instance_t eigrp = {0};
    eigrp_intf_t ei = {0};
    eigrp_addr_t src;
    eigrp_nbr_t *nbr;
    eigrp_packet_queue_t *queue;
    uint64_t generation;
    unsigned i;

    assert(pthread_mutex_init(&eigrp.peer_lock, NULL) == 0);
    assert(pthread_mutex_init(&eigrp.work_lock, NULL) == 0);
    assert(pthread_cond_init(&eigrp.work_cond, NULL) == 0);
    eigrp.tlv2_codec.encoder = eigrp_packet_encoder_safe;
    eigrp.eiflist = eigrp_list_create();
    eigrp.topology_table = eigrp_topology_table_create();
    eigrp.topology_changes = eigrp_list_create();
    assert(eigrp.eiflist && eigrp.topology_table && eigrp.topology_changes);
    ei.eigrp = &eigrp;
    ei.nbrs = eigrp_list_create();
    assert(ei.nbrs && eigrp_list_add(eigrp.eiflist, &ei));
    address_init(&src, afi, 1);

    nbr = eigrp_nbr_create(&ei, &src);
    assert(nbr && nbr->published && eigrp_nbr_is_eligible(nbr));
    assert(nbr->state == EIGRP_NEIGHBOR_DOWN);
    queue = nbr->retrans_queue;
    generation = nbr->generation;

    /* Repeated penalty/release must preserve identity and reusable RTP queue,
     * without allowing an adjacency state jump while suspended. */
    for (i = 0; i < 8; ++i) {
        eigrp_nbr_state_update(EIGRP_SET, nbr, EIGRP_NEIGHBOR_PENDING);
        assert(nbr->state == EIGRP_NEIGHBOR_PENDING);
        nbr->tlv_version = EIGRP_TLV_64B_VERSION;
        eigrp_nbr_state_update(EIGRP_SET, nbr, EIGRP_NEIGHBOR_UP);
        assert(nbr->state == EIGRP_NEIGHBOR_UP);
        assert(ei.tlv2_peer_count == 1);
        eigrp_packet_enqueue(queue, eigrp_packet_create(128, nbr));
        assert(queue->count == 1);
        assert(eigrp_nbr_limit_state_update(nbr, EIGRP_NBR_LIMIT_SUSPENDED));
        assert(!eigrp_nbr_is_eligible(nbr) && nbr->published);
        assert(nbr->state == EIGRP_NEIGHBOR_DOWN);
        assert(nbr->t_holddown == NULL && nbr->t_nbr_send_gr == NULL);
        assert(nbr->t_sync == NULL && !nbr->sync_active);
        assert(ei.tlv2_peer_count == 0);
        assert(nbr->retrans_queue == queue && queue->count == 0);
        assert(nbr->generation != generation);
        generation = nbr->generation;
        assert(eigrp_nbr_lookup(&ei, NULL, &src) == nbr);
        assert(ei.nbrs->count == 1);
        eigrp_nbr_state_update(EIGRP_SET, nbr, EIGRP_NEIGHBOR_UP);
        assert(nbr->state == EIGRP_NEIGHBOR_DOWN);
        eigrp_nbr_holddown_update(nbr);
        assert(nbr->t_holddown == NULL);
        eigrp_nbr_limit_release(nbr);
        assert(eigrp_nbr_is_eligible(nbr) && nbr->state == EIGRP_NEIGHBOR_DOWN);
        assert(nbr->generation != generation);
        generation = nbr->generation;
        assert(nbr->retrans_queue == queue);
    }

    assert(eigrp_nbr_limit_state_update(nbr, EIGRP_NBR_LIMIT_SUSPENDED));
    assert(eigrp_nbr_limit_state_update(nbr, EIGRP_NBR_LIMIT_LOCKED));
    assert(nbr->limit_state == EIGRP_NBR_LIMIT_LOCKED);
    assert(!eigrp_nbr_limit_state_update(nbr, EIGRP_NBR_LIMIT_SUSPENDED));
    eigrp_nbr_limit_release(nbr);
    assert(nbr->limit_state == EIGRP_NBR_LIMIT_LOCKED);
    eigrp_nbr_delete(nbr);
    assert(ei.nbrs->count == 0);

    /* An explicitly deleted locked peer can be discovered as a new object. */
    nbr = eigrp_nbr_create(&ei, &src);
    assert(nbr && eigrp_nbr_is_eligible(nbr));
    assert(nbr->state == EIGRP_NEIGHBOR_DOWN);
    eigrp_nbr_delete(nbr);
    assert(ei.nbrs->count == 0);

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
    run_lifecycle(AF_INET);
    run_lifecycle(AF_INET6);
    return 0;
}
