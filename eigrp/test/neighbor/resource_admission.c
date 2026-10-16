// SPDX-License-Identifier: ISC
// Copyright (C) 2026 Donnie V. Savage
/* Portable neighbor admission and allocation-failure regression executable. */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_neighbor.h"
#include "eigrp_list.h"
#include "eigrp_packet.h"

/* UUT host normally supplies the key-chain adapter.  The resource test uses
 * auth-off wire Hellos; keep the host boundary self-contained here. */
bool eigrp_sys_auth_key_lookup(const char *name, uint32_t *id,
                               char *key, size_t size)
{
    (void)name; (void)id; (void)key; (void)size;
    return false;
}

/* Other translation units are compiled with -Dcalloc=eigrp_test_calloc.
 * This harness is not: its allocator forwards to the actual C runtime. */
static unsigned allocations;
static unsigned failure_at;
void *eigrp_test_calloc(size_t count, size_t length)
{
    ++allocations;
    if (failure_at && allocations == failure_at)
        return NULL;
    return calloc(count, length);
}
static void fail_after(unsigned offset)
{
    failure_at = allocations + offset;
}

static void peer_address(eigrp_addr_t *address, int family, unsigned num)
{
    memset(address, 0, sizeof(*address));
    address->afi = family;
    if (family == AF_INET) {
        uint32_t value = htonl(0x0a000000U | (num & 0x00ffffffU));
        memcpy(&address->ip.v4, &value, 4);
    } else {
        address->ip.v6.s6_addr[0] = 0xfe;
        address->ip.v6.s6_addr[1] = 0x80;
        address->ip.v6.s6_addr[12] = (uint8_t)(num >> 24);
        address->ip.v6.s6_addr[13] = (uint8_t)(num >> 16);
        address->ip.v6.s6_addr[14] = (uint8_t)(num >> 8);
        address->ip.v6.s6_addr[15] = (uint8_t)num;
    }
}

static void instance_init(eigrp_instance_t *runtime)
{
    memset(runtime, 0, sizeof(*runtime));
    assert(pthread_mutex_init(&runtime->peer_lock, NULL) == 0);
    assert(pthread_mutex_init(&runtime->work_lock, NULL) == 0);
    assert(pthread_cond_init(&runtime->work_cond, NULL) == 0);
    runtime->thread_running = true;
    runtime->eiflist = eigrp_list_create();
    assert(runtime->eiflist);
}
static void interface_init(eigrp_instance_t *runtime, eigrp_intf_t *ei)
{
    memset(ei, 0, sizeof(*ei));
    ei->eigrp = runtime;
    ei->nbrs = eigrp_list_create();
    assert(ei->nbrs);
    assert(eigrp_list_add(runtime->eiflist, ei));
}
static void peer_destroy(eigrp_nbr_t *nbr)
{
    /* Exercise production unpublication, timer/queue cleanup. */
    eigrp_nbr_delete(nbr);
}
static void instance_finish(eigrp_instance_t *runtime)
{
    eigrp_intf_t *ei;
    eigrp_list_item_t *node;

    for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, node, ei)) {
        while (ei->nbrs->head)
            peer_destroy(ei->nbrs->head->data);
        eigrp_list_delete(&ei->nbrs);
    }
    eigrp_list_delete(&runtime->eiflist);
    eigrp_nbr_warning_state_clear(runtime);
    pthread_cond_destroy(&runtime->work_cond);
    pthread_mutex_destroy(&runtime->work_lock);
    pthread_mutex_destroy(&runtime->peer_lock);
}

static void allocation_failures(void)
{
    eigrp_instance_t runtime;
    eigrp_intf_t iface;
    eigrp_addr_t address;
    eigrp_nbr_t *nbr;
    unsigned i;

    instance_init(&runtime);
    interface_init(&runtime, &iface);
    peer_address(&address, AF_INET, 1);
    /* Internal self-neighbor creation also needs both allocations. */
    for (i = 1; i <= 2; ++i) {
        fail_after(i);
        assert(eigrp_nbr_create(NULL, &address) == NULL);
        failure_at = 0;
    }
    nbr = eigrp_nbr_create(NULL, &address);
    assert(nbr && nbr->retrans_queue && !nbr->published);
    peer_destroy(nbr);
    for (i = 1; i <= 3; ++i) {
        /* Neighbor, RTP queue, interface-list node allocations. */
        fail_after(i);
        nbr = eigrp_nbr_create(&iface, &address);
        assert(nbr == NULL);
        assert(iface.nbrs->count == 0);
        failure_at = 0;
    }
    nbr = eigrp_nbr_create(&iface, &address);
    assert(nbr && nbr->retrans_queue && nbr->published);
    assert(iface.nbrs->count == 1);
    peer_destroy(nbr);
    assert(iface.nbrs->count == 0);
    instance_finish(&runtime);
}

static void flood_interface(int family)
{
    eigrp_instance_t runtime;
    eigrp_intf_t iface;
    eigrp_addr_t address;
    eigrp_nbr_t *first, *nbr;
    unsigned i;
    unsigned prior_allocations;

    instance_init(&runtime);
    interface_init(&runtime, &iface);
    peer_address(&address, family, 1);
    first = eigrp_nbr_create(&iface, &address);
    assert(first);
    /* An established neighbor must survive rejected discovery traffic. */
    first->state = EIGRP_NEIGHBOR_UP;
    for (i = 2; i <= EIGRP_NEIGHBOR_MAX_PER_INTERFACE; ++i) {
        peer_address(&address, family, i);
        nbr = eigrp_nbr_create(&iface, &address);
        assert(nbr);
    }
    assert(iface.nbrs->count == EIGRP_NEIGHBOR_MAX_PER_INTERFACE);
    prior_allocations = allocations;
    for (i = EIGRP_NEIGHBOR_MAX_PER_INTERFACE + 1; i <= 10000; ++i) {
        peer_address(&address, family, i);
        assert(eigrp_nbr_create(&iface, &address) == NULL);
    }
    assert(allocations == prior_allocations);
    assert(iface.nbrs->count == EIGRP_NEIGHBOR_MAX_PER_INTERFACE);
    assert(first->published && first->state == EIGRP_NEIGHBOR_UP);
    peer_address(&address, family, 1);
    assert(eigrp_nbr_lookup(&iface, NULL, &address) == first);
    first->state = EIGRP_NEIGHBOR_DOWN;
    peer_destroy(first);
    assert(iface.nbrs->count == EIGRP_NEIGHBOR_MAX_PER_INTERFACE - 1);
    peer_address(&address, family, 10001);
    nbr = eigrp_nbr_create(&iface, &address);
    assert(nbr && iface.nbrs->count == EIGRP_NEIGHBOR_MAX_PER_INTERFACE);
    instance_finish(&runtime);
}

static void flood_process(void)
{
    eigrp_instance_t runtime;
    eigrp_intf_t ifaces[5];
    eigrp_addr_t address;
    eigrp_nbr_t *victim, *recovered;
    unsigned i, j;

    instance_init(&runtime);
    for (i = 0; i < 5; i++)
        interface_init(&runtime, &ifaces[i]);
    for (i = 0; i < 4; i++)
        for (j = 0; j < EIGRP_NEIGHBOR_MAX_PER_INTERFACE; j++) {
            peer_address(&address, AF_INET, 1 + (i * EIGRP_NEIGHBOR_MAX_PER_INTERFACE) + j);
            assert(eigrp_nbr_create(&ifaces[i], &address));
        }
    assert(ifaces[4].nbrs->count == 0);
    peer_address(&address, AF_INET6, 50000);
    assert(eigrp_nbr_create(&ifaces[4], &address) == NULL);
    victim = ifaces[3].nbrs->head->data;
    peer_destroy(victim);
    assert(ifaces[3].nbrs->count == EIGRP_NEIGHBOR_MAX_PER_INTERFACE - 1);
    recovered = eigrp_nbr_create(&ifaces[4], &address);
    assert(recovered && ifaces[4].nbrs->count == 1);
    instance_finish(&runtime);
}

static void down_queue_reuse(void)
{
    eigrp_instance_t runtime;
    eigrp_intf_t iface;
    eigrp_addr_t address;
    eigrp_nbr_t *nbr;
    eigrp_packet_queue_t *queue;

    instance_init(&runtime);
    interface_init(&runtime, &iface);
    peer_address(&address, AF_INET, 1);
    nbr = eigrp_nbr_create(&iface, &address);
    assert(nbr);
    queue = nbr->retrans_queue;
    /* No more queue allocations when resetting a peer, even under OOM. */
    fail_after(1);
    eigrp_nbr_state_update(EIGRP_SET, nbr, EIGRP_NEIGHBOR_DOWN);
    assert(nbr->retrans_queue == queue);
    failure_at = 0;
    peer_destroy(nbr);
    instance_finish(&runtime);
}


/* A discovery burst must never monopolize the AF queue.  Real established
 * peer messages can still enter while 256 new-identity Hellos are pending. */
static void discovery_queue_pressure(void)
{
    eigrp_instance_t runtime;
    eigrp_packet_input_t *input;
    unsigned i;

    instance_init(&runtime);
    for (i = 0; i < 256; ++i) {
        input = calloc(1, sizeof(*input));
        assert(input);
        input->ingress_authenticated = true;
        input->ingress_discovery = true;
        assert(eigrp_packet_input_enqueue(&runtime, input));
    }
    assert(runtime.packet_discovery_count == 256);
    for (i = 0; i < 2048; ++i) {
        input = calloc(1, sizeof(*input));
        assert(input);
        input->ingress_authenticated = true;
        input->ingress_discovery = true;
        assert(!eigrp_packet_input_enqueue(&runtime, input));
        free(input);
    }
    assert(runtime.packet_count == 256);
    for (i = 0; i < 256; ++i) {
        input = calloc(1, sizeof(*input));
        assert(input);
        input->ingress_authenticated = true;
        assert(eigrp_packet_input_enqueue(&runtime, input));
    }
    assert(runtime.packet_count == 512);
    assert(runtime.packet_discovery_count == 256);
    eigrp_packet_input_queue_clear(&runtime);
    assert(runtime.packet_count == 0 && runtime.packet_discovery_count == 0);
    input = calloc(1, sizeof(*input));
    assert(input);
    input->ingress_discovery = true;
    assert(eigrp_packet_input_enqueue(&runtime, input));
    assert(runtime.packet_discovery_count == 1);
    eigrp_packet_input_queue_clear(&runtime);
    instance_finish(&runtime);
}


/* Exercise real portable ingress validation and Hello admission after a
 * 2,048-neighbor fanout has been established.  A known peer is unaffected. */
static bool test_source_on_link(eigrp_intf_t *ei, const eigrp_addr_t *src)
{
    return ei && src && (src->afi == AF_INET || src->afi == AF_INET6);
}

static void hello_ingress_limit(int family)
{
    static const unsigned char parameter_tlv[12] = {
        0x00, 0x01, 0x00, 0x0c, /* Parameter type and length */
        0x01, 0x00, 0x01, 0x00, 0x00, 0x00, /* Default K-values */
        0x00, 0x0f /* Hold time */
    };
    eigrp_instance_t runtime;
    eigrp_intf_t iface;
    eigrp_virt_router_t vr = {0};
    eigrp_packet_input_t input = {0};
    eigrp_addr_t addr;
    eigrp_nbr_t *existing;
    unsigned i;

    instance_init(&runtime);
    interface_init(&runtime, &iface);
    vr.vrid = 1;
    runtime.virt_router = &vr;
    runtime.AS = 4453;
    runtime.af_vectors.packet_address_bytes = family == AF_INET ? 4 : 16;
    runtime.af_vectors.packet_source_on_link = test_source_on_link;
    iface.ifindex = 77;
    iface.name = "admission-test";
    input.ifindex = iface.ifindex;
    input.meta.network_header_length = 0;
    input.meta.eigrp_length = EIGRP_HEADER_LEN + sizeof(parameter_tlv);
    input.stream = eigrp_stream_create(128);
    assert(input.stream);
    eigrp_packet_header_init(EIGRP_OPC_HELLO, &runtime, input.stream, 0, 0, 0);
    assert(eigrp_stream_put(input.stream, parameter_tlv,
                            sizeof(parameter_tlv)) == sizeof(parameter_tlv));
    eigrp_packet_checksum(&iface, input.stream, input.meta.eigrp_length);
    for (i = 1; i <= EIGRP_NEIGHBOR_MAX_PER_INTERFACE; ++i) {
        peer_address(&addr, family, i);
        assert(eigrp_nbr_create(&iface, &addr));
    }
    peer_address(&input.source, family, EIGRP_NEIGHBOR_MAX_PER_INTERFACE + 1);
    assert(!eigrp_packet_ingress_authenticate(&runtime, &input));
    assert(!input.ingress_authenticated);
    assert(iface.nbrs->count == EIGRP_NEIGHBOR_MAX_PER_INTERFACE);

    peer_address(&input.source, family, 1);
    existing = eigrp_nbr_lookup(&iface, NULL, &input.source);
    assert(existing);
    existing->state = EIGRP_NEIGHBOR_UP;
    assert(eigrp_packet_ingress_authenticate(&runtime, &input));
    assert(input.ingress_authenticated);
    assert(!input.ingress_discovery);
    assert(input.ingress_neighbor_generation == existing->generation);
    assert(existing->state == EIGRP_NEIGHBOR_UP);

    /* Expiry or deletion returns a slot; new arrivals can join again. */
    existing->state = EIGRP_NEIGHBOR_DOWN;
    peer_destroy(existing);
    memset(&input.ingress_authenticated, 0, sizeof(input.ingress_authenticated));
    input.ingress_discovery = false;
    input.ingress_neighbor_generation = 0;
    peer_address(&input.source, family, EIGRP_NEIGHBOR_MAX_PER_INTERFACE + 1);
    assert(eigrp_packet_ingress_authenticate(&runtime, &input));
    assert(input.ingress_discovery);
    eigrp_stream_free(input.stream);
    instance_finish(&runtime);
}

int main(void)
{
    allocation_failures();
    flood_interface(AF_INET);
    flood_interface(AF_INET6);
    flood_process();
    down_queue_reuse();
    discovery_queue_pressure();
    hello_ingress_limit(AF_INET);
    hello_ingress_limit(AF_INET6);
    return 0;
}
