// SPDX-License-Identifier: ISC
/* Verify optional pointer indexing and safe deletion during list iteration. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "eigrp_list.h"

int main(void)
{
    enum { N = 10000 };
    eigrp_list_t *list = eigrp_list_create();
    eigrp_list_item_t *node, *next;
    uintptr_t id;
    size_t seen = 0;
    assert(list && eigrp_list_index_enable(list));
    for (id = 1; id <= N; id++)
        assert(eigrp_list_add(list, (void *)(id << 4)));
    assert(list->count == N);
    for (id = 1; id <= N; id++)
        assert(eigrp_list_lookup(list, (void *)(id << 4)));
    /* DUAL may append an already-listed destination from an FSM callback.
     * Removing by the exact iterator node must preserve cached next. */
    for (id = 1; id <= 100; id++)
        assert(eigrp_list_add(list, (void *)(id << 4)));
    for (node = list->head; node; node = next) {
        next = node->next;
        eigrp_list_remove(list, node);
        seen++;
    }
    assert(seen == N + 100 && list->count == 0);
    for (id = 1; id <= N; id++)
        assert(eigrp_list_lookup(list, (void *)(id << 4)) == NULL);
    eigrp_list_delete(&list);
    assert(!list);
    puts("portable list index passed");
    return 0;
}
