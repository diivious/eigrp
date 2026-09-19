#ifndef EIGRP_TEST_TABLE_H
#define EIGRP_TEST_TABLE_H

#include <zebra.h>

struct route_table { struct route_node *top; };
struct route_node { struct route_node *next; struct route_node *parent; struct route_table *table; struct prefix p; void *info; unsigned int lock; };

static inline struct route_table *route_table_init(void) { return calloc(1, sizeof(struct route_table)); }
static inline void route_table_finish(struct route_table *t) { free(t); }
static inline struct route_node *route_node_get(struct route_table *t, const struct prefix *p) { (void)p; struct route_node *n=calloc(1,sizeof(*n)); n->table=t; if (p) n->p=*p; return n; }
static inline struct route_node *route_node_match(struct route_table *t, const struct prefix *p) { (void)t; (void)p; return NULL; }
static inline struct route_node *route_node_lookup(struct route_table *t, const struct prefix *p) { (void)t; (void)p; return NULL; }
static inline struct route_node *route_top(struct route_table *t) { return t ? t->top : NULL; }
static inline struct route_node *route_next(struct route_node *n) { return n ? n->next : NULL; }
static inline void route_unlock_node(struct route_node *n) { (void)n; }
static inline void route_lock_node(struct route_node *n) { (void)n; }

#endif
