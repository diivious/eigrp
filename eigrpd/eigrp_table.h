// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP portable prefix table.
 * Copyright (C) 2026 Donnie V. Savage
 */
#ifndef _EIGRP_TABLE_H_
#define _EIGRP_TABLE_H_

#include "eigrpd/eigrp_types.h"

typedef struct eigrp_table_node {
	struct eigrp_table_node *next;
	eigrp_prefix_t prefix;
	void *info;
} eigrp_table_node_t;

typedef struct eigrp_table {
	eigrp_table_node_t *head;
} eigrp_table_t;

eigrp_table_t *eigrp_table_new(void);
void eigrp_table_free(eigrp_table_t *table);
eigrp_table_node_t *eigrp_table_node_get(eigrp_table_t *table,
					 const eigrp_prefix_t *prefix);
eigrp_table_node_t *eigrp_table_node_lookup(eigrp_table_t *table,
					    const eigrp_prefix_t *prefix);
eigrp_table_node_t *eigrp_table_node_match(eigrp_table_t *table,
					   const eigrp_prefix_t *prefix);
static inline eigrp_table_node_t *eigrp_table_first(eigrp_table_t *table)
{
	return table ? table->head : NULL;
}
static inline eigrp_table_node_t *eigrp_table_next(eigrp_table_node_t *node)
{
	return node ? node->next : NULL;
}
static inline void eigrp_table_node_release(eigrp_table_node_t *node)
{
	(void)node;
}

#endif /* _EIGRP_TABLE_H_ */
