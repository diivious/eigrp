// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable doubly-linked list.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdlib.h>

#include "eigrpd/eigrp_list.h"

eigrp_list_t *eigrp_list_new(void)
{
	return calloc(1, sizeof(eigrp_list_t));
}

static eigrp_list_node_t *eigrp_list_node_new(void *data)
{
	eigrp_list_node_t *node = calloc(1, sizeof(*node));
	if (node)
		node->data = data;
	return node;
}

void eigrp_list_delete_node(eigrp_list_t *list, eigrp_list_node_t *node)
{
	if (!list || !node)
		return;
	if (node->prev)
		node->prev->next = node->next;
	else
		list->head = node->next;
	if (node->next)
		node->next->prev = node->prev;
	else
		list->tail = node->prev;
	if (list->count)
		list->count--;
	if (list->del && node->data)
		list->del(node->data);
	free(node);
}

void eigrp_list_delete(eigrp_list_t **listp)
{
	eigrp_list_t *list;
	eigrp_list_node_t *node, *next;

	if (!listp || !*listp)
		return;
	list = *listp;
	for (node = list->head; node; node = next) {
		next = node->next;
		if (list->del && node->data)
			list->del(node->data);
		free(node);
	}
	free(list);
	*listp = NULL;
}

eigrp_list_node_t *eigrp_list_add(eigrp_list_t *list, void *data)
{
	eigrp_list_node_t *node;
	if (!list)
		return NULL;
	node = eigrp_list_node_new(data);
	if (!node)
		return NULL;
	node->prev = list->tail;
	if (list->tail)
		list->tail->next = node;
	else
		list->head = node;
	list->tail = node;
	list->count++;
	return node;
}

eigrp_list_node_t *eigrp_list_add_sort(eigrp_list_t *list, void *data)
{
	eigrp_list_node_t *node, *new_node;
	if (!list || !list->cmp)
		return eigrp_list_add(list, data);
	for (node = list->head; node; node = node->next) {
		if (list->cmp(data, node->data) < 0)
			break;
	}
	if (!node)
		return eigrp_list_add(list, data);
	new_node = eigrp_list_node_new(data);
	if (!new_node)
		return NULL;
	new_node->next = node;
	new_node->prev = node->prev;
	if (node->prev)
		node->prev->next = new_node;
	else
		list->head = new_node;
	node->prev = new_node;
	list->count++;
	return new_node;
}

eigrp_list_node_t *eigrp_list_lookup(eigrp_list_t *list, const void *data)
{
	eigrp_list_node_t *node;
	if (!list)
		return NULL;
	for (node = list->head; node; node = node->next)
		if (node->data == data)
			return node;
	return NULL;
}

void eigrp_list_delete_data(eigrp_list_t *list, const void *data)
{
	eigrp_list_node_t *node = eigrp_list_lookup(list, data);
	if (node)
		eigrp_list_delete_node(list, node);
}
