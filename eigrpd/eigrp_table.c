// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable prefix table.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdlib.h>
#include <string.h>

#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_table.h"

static bool eigrp_table_prefix_equal(const eigrp_prefix_t *a,
				     const eigrp_prefix_t *b)
{
	eigrp_prefix_t na, nb;
	if (!a || !b || a->address.afi != b->address.afi
	    || a->prefix_length != b->prefix_length)
		return false;
	na = *a;
	nb = *b;
	eigrp_prefix_normalize(&na);
	eigrp_prefix_normalize(&nb);
	return memcmp(na.address.bytes, nb.address.bytes,
		      na.address.afi == EIGRP_ADDRESS_FAMILY_IPV4 ? 4 : 16) == 0;
}

static bool eigrp_table_prefix_contains(const eigrp_prefix_t *network,
					const eigrp_prefix_t *candidate)
{
	eigrp_prefix_t n, c;
	unsigned int bytes, full, rem;
	uint8_t mask;

	if (!network || !candidate || network->address.afi != candidate->address.afi
	    || network->prefix_length > candidate->prefix_length)
		return false;
	n = *network;
	c = *candidate;
	eigrp_prefix_normalize(&n);
	eigrp_prefix_normalize(&c);
	bytes = n.address.afi == EIGRP_ADDRESS_FAMILY_IPV4 ? 4 : 16;
	full = n.prefix_length / 8;
	rem = n.prefix_length % 8;
	if (full && memcmp(n.address.bytes, c.address.bytes, full) != 0)
		return false;
	if (rem) {
		if (full >= bytes)
			return false;
		mask = (uint8_t)(0xffU << (8U - rem));
		if ((n.address.bytes[full] & mask) != (c.address.bytes[full] & mask))
			return false;
	}
	return true;
}

eigrp_table_t *eigrp_table_new(void)
{
	return calloc(1, sizeof(eigrp_table_t));
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
	free(table);
}

eigrp_table_node_t *eigrp_table_node_lookup(eigrp_table_t *table,
					    const eigrp_prefix_t *prefix)
{
	eigrp_table_node_t *node;
	if (!table || !prefix)
		return NULL;
	for (node = table->head; node; node = node->next)
		if (eigrp_table_prefix_equal(&node->prefix, prefix))
			return node;
	return NULL;
}

eigrp_table_node_t *eigrp_table_node_get(eigrp_table_t *table,
					 const eigrp_prefix_t *prefix)
{
	eigrp_table_node_t *node, **tail;
	if (!table || !prefix || !eigrp_prefix_valid(prefix))
		return NULL;
	node = eigrp_table_node_lookup(table, prefix);
	if (node)
		return node;
	node = calloc(1, sizeof(*node));
	if (!node)
		return NULL;
	node->prefix = *prefix;
	eigrp_prefix_normalize(&node->prefix);
	for (tail = &table->head; *tail; tail = &(*tail)->next)
		;
	*tail = node;
	return node;
}

eigrp_table_node_t *eigrp_table_node_match(eigrp_table_t *table,
					   const eigrp_prefix_t *prefix)
{
	eigrp_table_node_t *node, *best = NULL;
	if (!table || !prefix)
		return NULL;
	for (node = table->head; node; node = node->next) {
		if (!node->info || !eigrp_table_prefix_contains(&node->prefix, prefix))
			continue;
		if (!best || node->prefix.prefix_length > best->prefix.prefix_length)
			best = node;
	}
	return best;
}
