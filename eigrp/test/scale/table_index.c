// SPDX-License-Identifier: ISC
/* Portable prefix table: hash index, normalization, longest match, deletion. */
#include <assert.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eigrp_table.h"

static eigrp_prefix_t ipv4(uint32_t value, uint8_t bits)
{
    eigrp_prefix_t p = {0};
    p.address.afi = EIGRP_AFI_IPV4;
    p.prefix_length = bits;
    p.address.bytes[0] = (uint8_t)(value >> 24);
    p.address.bytes[1] = (uint8_t)(value >> 16);
    p.address.bytes[2] = (uint8_t)(value >> 8);
    p.address.bytes[3] = (uint8_t)value;
    return p;
}

int main(void)
{
    enum { N = 12000 };
    eigrp_table_t *table = eigrp_table_create();
    eigrp_table_node_t *entries[N];
    eigrp_table_node_t *node;
    eigrp_prefix_t key, query;
    size_t count = 0;
    int i;

    assert(table);
    for (i = 0; i < N; i++) {
        key = ipv4(0x64000000U + (uint32_t)i, 32);
        entries[i] = eigrp_table_node_lookup_or_create(table, &key);
        assert(entries[i]);
        entries[i]->info = entries[i];
        assert(eigrp_table_node_lookup_or_create(table, &key) == entries[i]);
    }
    assert(table->size == N);
    for (i = 0; i < N; i++) {
        key = ipv4(0x64000000U + (uint32_t)i, 32);
        assert(eigrp_table_node_lookup(table, &key) == entries[i]);
    }
    for (node = eigrp_table_first(table); node; node = eigrp_table_next(node))
        count++;
    assert(count == N);
    /* Overlapping less-specific prefixes must match by longest prefix. */
    key = ipv4(0x640000FDU, 24);
    node = eigrp_table_node_lookup_or_create(table, &key);
    assert(node);
    node->info = node;
    query = ipv4(0x640000FEU, 32);
    assert(eigrp_table_node_match(table, &query) == entries[254]);
    query = ipv4(0x640000FFU, 32);
    assert(eigrp_table_node_match(table, &query) == entries[255]);
    query = ipv4(0x64000000U + N, 32);
    assert(eigrp_table_node_match(table, &query) == NULL);
    key = ipv4(0x64000080U, 25);
    assert(eigrp_table_node_lookup_or_create(table, &key));
    query = ipv4(0x640000FFU, 25);
    assert(eigrp_table_node_lookup(table, &query) != NULL);
    for (i = 0; i < N; i += 2) {
        key = ipv4(0x64000000U + (uint32_t)i, 32);
        eigrp_table_node_remove(table, entries[i]);
        assert(!eigrp_table_node_lookup(table, &key));
    }
    for (i = 1; i < N; i += 2) {
        key = ipv4(0x64000000U + (uint32_t)i, 32);
        assert(eigrp_table_node_lookup(table, &key) == entries[i]);
    }
    /* Recreate deleted destinations without disturbing existing index/list. */
    for (i = 0; i < N; i += 2) {
        key = ipv4(0x64000000U + (uint32_t)i, 32);
        node = eigrp_table_node_lookup_or_create(table, &key);
        assert(node);
        node->info = node;
    }
    assert(table->size == N + 2);
    eigrp_table_free(table);
    puts("portable table index passed");
    return 0;
}
