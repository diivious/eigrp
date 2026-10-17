// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable doubly-linked list.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdint.h>
#include <stdlib.h>

#include "eigrp_list.h"

#define EIGRP_LIST_INDEX_INITIAL_BUCKETS 1024U

static size_t eigrp_list_pointer_hash(const void *data)
{
	uintptr_t key = (uintptr_t)data >> 3;
	key ^= key >> 17;
	key ^= key >> 9;
	return (size_t)key;
}

static bool eigrp_list_index_resize(eigrp_list_t *list, size_t size)
{
	eigrp_list_item_t **buckets, *node;

	buckets = calloc(size, sizeof(*buckets));
	if (!buckets)
		return false;
	for (node = list->head; node; node = node->next) {
		size_t bucket = eigrp_list_pointer_hash(node->data) % size;
		node->hash_next = buckets[bucket];
		buckets[bucket] = node;
	}
	free(list->buckets);
	list->buckets = buckets;
	list->bucket_count = size;
	return true;
}

/* Opt-in: some lists reorder node->data in place while sorting. Their hash
 * entries would go stale, so only stable-data lists use this index. */
bool eigrp_list_index_enable(eigrp_list_t *list)
{
	if (!list)
		return false;
	if (list->buckets)
		return true;
	return eigrp_list_index_resize(list, EIGRP_LIST_INDEX_INITIAL_BUCKETS);
}

eigrp_list_t *eigrp_list_create(void)
{
	return calloc(1, sizeof(eigrp_list_t));
}

static eigrp_list_item_t *eigrp_list_item_create(void *data)
{
	eigrp_list_item_t *node = calloc(1, sizeof(*node));
	if (node)
		node->data = data;
	return node;
}

static void eigrp_list_index_add(eigrp_list_t *list, eigrp_list_item_t *node)
{
	size_t bucket;

	if (!list->buckets)
		return;
	if (list->count >= list->bucket_count * 2
	    && list->bucket_count <= SIZE_MAX / 2
	    && eigrp_list_index_resize(list, list->bucket_count * 2))
		return; /* Resize already indexed the newly linked node. */
	bucket = eigrp_list_pointer_hash(node->data) % list->bucket_count;
	node->hash_next = list->buckets[bucket];
	list->buckets[bucket] = node;
}

static void eigrp_list_index_remove(eigrp_list_t *list, eigrp_list_item_t *node)
{
	eigrp_list_item_t **cursor;
	size_t bucket;

	if (!list->buckets)
		return;
	bucket = eigrp_list_pointer_hash(node->data) % list->bucket_count;
	for (cursor = &list->buckets[bucket]; *cursor;
	     cursor = &(*cursor)->hash_next) {
		if (*cursor == node) {
			*cursor = node->hash_next;
			return;
		}
	}
}

void eigrp_list_remove(eigrp_list_t *list, eigrp_list_item_t *node)
{
	if (!list || !node)
		return;
	eigrp_list_index_remove(list, node);
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
	eigrp_list_item_t *node, *next;

	if (!listp || !*listp)
		return;
	list = *listp;
	for (node = list->head; node; node = next) {
		next = node->next;
		if (list->del && node->data)
			list->del(node->data);
		free(node);
	}
	free(list->buckets);
	free(list);
	*listp = NULL;
}

eigrp_list_item_t *eigrp_list_add(eigrp_list_t *list, void *data)
{
	eigrp_list_item_t *node;
	if (!list)
		return NULL;
	node = eigrp_list_item_create(data);
	if (!node)
		return NULL;
	node->prev = list->tail;
	if (list->tail)
		list->tail->next = node;
	else
		list->head = node;
	list->tail = node;
	list->count++;
	eigrp_list_index_add(list, node);
	return node;
}

eigrp_list_item_t *eigrp_list_insert(eigrp_list_t *list, void *data)
{
	eigrp_list_item_t *node, *new_node;
	if (!list || !list->cmp)
		return eigrp_list_add(list, data);
	for (node = list->head; node; node = node->next) {
		if (list->cmp(data, node->data) < 0)
			break;
	}
	if (!node)
		return eigrp_list_add(list, data);
	new_node = eigrp_list_item_create(data);
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
	eigrp_list_index_add(list, new_node);
	return new_node;
}

eigrp_list_item_t *eigrp_list_lookup(eigrp_list_t *list, const void *data)
{
	eigrp_list_item_t *node;
	if (!list)
		return NULL;
	if (list->buckets) {
		size_t bucket = eigrp_list_pointer_hash(data) % list->bucket_count;
		for (node = list->buckets[bucket]; node; node = node->hash_next)
			if (node->data == data)
				return node;
		return NULL;
	}
	for (node = list->head; node; node = node->next)
		if (node->data == data)
			return node;
	return NULL;
}

void eigrp_list_delete_data(eigrp_list_t *list, const void *data)
{
	eigrp_list_item_t *node = eigrp_list_lookup(list, data);
	if (node)
		eigrp_list_remove(list, node);
}
