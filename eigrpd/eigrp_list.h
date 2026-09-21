// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable doubly-linked list.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _EIGRP_LIST_H_
#define _EIGRP_LIST_H_

#include <stddef.h>

typedef struct eigrp_list_node {
	struct eigrp_list_node *next;
	struct eigrp_list_node *prev;
	void *data;
} eigrp_list_node_t;

typedef struct eigrp_list {
	eigrp_list_node_t *head;
	eigrp_list_node_t *tail;
	unsigned int count;
	int (*cmp)(void *, void *);
	void (*del)(void *);
} eigrp_list_t;

eigrp_list_t *eigrp_list_new(void);
void eigrp_list_delete(eigrp_list_t **list);
eigrp_list_node_t *eigrp_list_add(eigrp_list_t *list, void *data);
eigrp_list_node_t *eigrp_list_add_sort(eigrp_list_t *list, void *data);
void eigrp_list_delete_data(eigrp_list_t *list, const void *data);
void eigrp_list_delete_node(eigrp_list_t *list, eigrp_list_node_t *node);
eigrp_list_node_t *eigrp_list_lookup(eigrp_list_t *list, const void *data);

static inline eigrp_list_node_t *eigrp_list_head(const eigrp_list_t *list)
{
	return list ? list->head : NULL;
}

static inline eigrp_list_node_t *eigrp_list_tail(const eigrp_list_t *list)
{
	return list ? list->tail : NULL;
}

static inline void *eigrp_list_node_data(const eigrp_list_node_t *node)
{
	return node ? node->data : NULL;
}

static inline int eigrp_list_isempty(const eigrp_list_t *list)
{
	return !list || list->count == 0;
}

#define EIGRP_LIST_ELEMENTS(L, N, NN, D) \
	(N) = ((L) ? (L)->head : NULL), \
	(NN) = ((N) ? (N)->next : NULL), \
	(D) = ((N) ? (N)->data : NULL); \
	(N) != NULL; \
	(N) = (NN), (NN) = ((N) ? (N)->next : NULL), \
	(D) = ((N) ? (N)->data : NULL)

#define EIGRP_LIST_ELEMENTS_RO(L, N, D) \
	(N) = ((L) ? (L)->head : NULL), \
	(D) = ((N) ? (N)->data : NULL); \
	(N) != NULL; \
	(N) = (N)->next, (D) = ((N) ? (N)->data : NULL)

#endif /* _EIGRP_LIST_H_ */
