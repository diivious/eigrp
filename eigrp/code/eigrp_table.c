// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable prefix table.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdlib.h>
#include <string.h>

#include "eigrp_prefix.h"
#include "eigrp_table.h"

#define EIGRP_TABLE_INITIAL_BUCKETS 1024U

static size_t eigrp_table_prefix_hash(const eigrp_prefix_t *prefix)
{
	const uint8_t *bytes = prefix->address.bytes;
	size_t length = prefix->address.afi == EIGRP_AFI_IPV4 ? 4 : 16;
	size_t hash = 2166136261U;
	size_t i;

	hash = (hash ^ (unsigned int)prefix->address.afi) * 16777619U;
	hash = (hash ^ prefix->prefix_length) * 16777619U;
	for (i = 0; i < length; i++)
		hash = (hash ^ bytes[i]) * 16777619U;
	return hash;
}

static bool eigrp_table_prefix_match(const eigrp_prefix_t *a,
				     const eigrp_prefix_t *b)
{
	return a->address.afi == b->address.afi
	    && a->prefix_length == b->prefix_length
	    && !memcmp(a->address.bytes, b->address.bytes,
		       a->address.afi == EIGRP_AFI_IPV4 ? 4 : 16);
}

/* Rehashing leaves the stable insertion-order list and all node pointers
 * untouched. If allocation fails, the existing buckets remain usable. */
static bool eigrp_table_resize(eigrp_table_t *table, size_t count)
{
	eigrp_table_node_t **buckets, *node;

	buckets = calloc(count, sizeof(*buckets));
	if (!buckets)
		return false;
	for (node = table->head; node; node = node->next) {
		size_t bucket = eigrp_table_prefix_hash(&node->prefix) % count;
		node->hash_next = buckets[bucket];
		buckets[bucket] = node;
	}
	free(table->buckets);
	table->buckets = buckets;
	table->bucket_count = count;
	return true;
}

eigrp_table_t *eigrp_table_create(void)
{
	eigrp_table_t *table = calloc(1, sizeof(*table));

	if (!table)
		return NULL;
	if (!eigrp_table_resize(table, EIGRP_TABLE_INITIAL_BUCKETS)) {
		free(table);
		return NULL;
	}
	return table;
}

void eigrp_table_free(eigrp_table_t *table)
{
	eigrp_table_node_t *node, *next;
	if (!table)
		return;
	for (node = table->head; node; node = next) {
		next = node->next;
		free(node);
	}
	free(table->buckets);
	free(table);
}

eigrp_table_node_t *eigrp_table_node_lookup(eigrp_table_t *table,
					    const eigrp_prefix_t *prefix)
{
	eigrp_table_node_t *node;
	eigrp_prefix_t key;
	if (!table || !prefix || !eigrp_prefix_valid(prefix)
	    || !table->bucket_count)
		return NULL;
	key = *prefix;
	eigrp_prefix_normalize(&key);
	for (node = table->buckets[eigrp_table_prefix_hash(&key)
				      % table->bucket_count];
	     node; node = node->hash_next)
		if (eigrp_table_prefix_match(&node->prefix, &key))
			return node;
	return NULL;
}

eigrp_table_node_t *eigrp_table_node_lookup_or_create(eigrp_table_t *table,
					 const eigrp_prefix_t *prefix)
{
	eigrp_table_node_t *node;
	size_t bucket;
	if (!table || !prefix || !eigrp_prefix_valid(prefix))
		return NULL;
	node = eigrp_table_node_lookup(table, prefix);
	if (node)
		return node;
	if (table->size >= table->bucket_count * 2
	    && table->bucket_count <= SIZE_MAX / 2)
		(void)eigrp_table_resize(table, table->bucket_count * 2);
	node = calloc(1, sizeof(*node));
	if (!node)
		return NULL;
	node->prefix = *prefix;
	eigrp_prefix_normalize(&node->prefix);
	bucket = eigrp_table_prefix_hash(&node->prefix) % table->bucket_count;
	node->hash_next = table->buckets[bucket];
	table->buckets[bucket] = node;
	node->previous = table->tail;
	if (table->tail)
		table->tail->next = node;
	else
		table->head = node;
	table->tail = node;
	table->size++;
	return node;
}

void eigrp_table_node_remove(eigrp_table_t *table, eigrp_table_node_t *node)
{
	eigrp_table_node_t **cursor;
	size_t bucket;
	bool found = false;
	if (!table || !node || !table->bucket_count)
		return;
	bucket = eigrp_table_prefix_hash(&node->prefix) % table->bucket_count;
	for (cursor = &table->buckets[bucket]; *cursor;
	     cursor = &(*cursor)->hash_next) {
		if (*cursor == node) {
			*cursor = node->hash_next;
			found = true;
			break;
		}
	}
	if (!found)
		return;
	if (node->previous)
		node->previous->next = node->next;
	else
		table->head = node->next;
	if (node->next)
		node->next->previous = node->previous;
	else
		table->tail = node->previous;
	if (table->size)
		table->size--;
	free(node);
}

eigrp_table_node_t *eigrp_table_node_match(eigrp_table_t *table,
					   const eigrp_prefix_t *prefix)
{
	eigrp_prefix_t candidate;
	eigrp_table_node_t *node;
	int bitlen;
	if (!table || !eigrp_prefix_valid(prefix))
		return NULL;
	/* Longest-prefix match via exact hash probes instead of a table scan. */
	candidate = *prefix;
	for (bitlen = prefix->prefix_length; bitlen >= 0; bitlen--) {
		candidate.prefix_length = (uint8_t)bitlen;
		node = eigrp_table_node_lookup(table, &candidate);
		if (node && node->info)
			return node;
	}
	return NULL;
}
