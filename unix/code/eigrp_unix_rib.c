// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * In-memory Unix host RIB for standalone EIGRP development.
 *
 * This is deliberately not a kernel routing-table adapter.  It models the
 * routing-stack side of eigrp_rib.h so unprivileged Unix tests can exercise
 * connected/source-route introduction and learned-route lifecycle.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp_unix_rib.h"

typedef struct eigrp_unix_rib_route_entry {
	eigrp_instance_t *eigrp;
	eigrp_rib_route_t route;
	eigrp_rib_nexthop_t *nexthops;
	struct eigrp_unix_rib_route_entry *next;
	struct eigrp_unix_rib_route_entry *prev;
	struct eigrp_unix_rib_route_entry *hash_next;
} eigrp_unix_rib_route_entry_t;

typedef struct eigrp_unix_rib_source_entry {
	eigrp_rib_route_t route;
	eigrp_rib_nexthop_t *nexthops;
	struct eigrp_unix_rib_source_entry *next;
	struct eigrp_unix_rib_source_entry *prev;
	struct eigrp_unix_rib_source_entry *hash_next;
} eigrp_unix_rib_source_entry_t;

typedef struct eigrp_unix_rib_subscription {
	eigrp_instance_t *eigrp;
	eigrp_redist_source_t source;
	struct eigrp_unix_rib_subscription *next;
} eigrp_unix_rib_subscription_t;

static eigrp_unix_rib_route_entry_t *learned_routes;
static eigrp_unix_rib_source_entry_t *source_routes;
static eigrp_unix_rib_subscription_t *subscriptions;

/* An in-memory Unix RIB must not turn every source notification or RIB
 * installation into a full 100K-route scan. These indexes are host-only;
 * neither EIGRP DUAL nor the portable southbound contract depends on them. */
#define EIGRP_UNIX_RIB_BUCKETS 65536U
static eigrp_unix_rib_route_entry_t *learned_buckets[EIGRP_UNIX_RIB_BUCKETS];
static eigrp_unix_rib_source_entry_t *source_buckets[EIGRP_UNIX_RIB_BUCKETS];

static size_t eigrp_unix_rib_prefix_hash(const eigrp_prefix_t *prefix)
{
    uint32_t hash = 2166136261U;
    size_t len = prefix->address.afi == EIGRP_AFI_IPV4 ? 4 : 16;
    size_t i;
    hash = (hash ^ prefix->address.afi) * 16777619U;
    hash = (hash ^ prefix->prefix_length) * 16777619U;
    for (i = 0; i < len; i++)
        hash = (hash ^ prefix->address.bytes[i]) * 16777619U;
    return hash;
}

static size_t eigrp_unix_rib_learned_bucket(eigrp_instance_t *eigrp,
                                            const eigrp_prefix_t *prefix)
{
    uintptr_t owner = (uintptr_t)eigrp;
    return (eigrp_unix_rib_prefix_hash(prefix) ^ (owner >> 4))
           % EIGRP_UNIX_RIB_BUCKETS;
}

static size_t eigrp_unix_rib_source_bucket(const eigrp_rib_route_t *route)
{
    uint32_t hash = (uint32_t)eigrp_unix_rib_prefix_hash(&route->prefix);
    hash = (hash ^ route->redist.source.protocol) * 16777619U;
    hash = (hash ^ route->redist.source.route_instance) * 16777619U;
    return hash % EIGRP_UNIX_RIB_BUCKETS;
}

static bool eigrp_unix_rib_prefix_equal(const eigrp_prefix_t *left,
	const eigrp_prefix_t *right)
{
	return left && right && left->address.afi == right->address.afi
	       && left->prefix_length == right->prefix_length
	       && memcmp(left->address.bytes, right->address.bytes,
			 sizeof(left->address.bytes)) == 0;
}

static bool eigrp_unix_rib_prefix_valid(const eigrp_prefix_t *prefix)
{
	if (!prefix)
		return false;
	if (prefix->address.afi == EIGRP_AFI_IPV4)
		return prefix->prefix_length <= 32;
	if (prefix->address.afi == EIGRP_AFI_IPV6)
		return prefix->prefix_length <= 128;
	return false;
}

static bool eigrp_unix_rib_source_equal(const eigrp_redist_source_t *left,
	const eigrp_redist_source_t *right)
{
	return left && right && left->protocol == right->protocol
	       && left->route_instance == right->route_instance;
}

static bool eigrp_unix_rib_source_key_equal(
	const eigrp_rib_route_t *left,
	const eigrp_rib_route_t *right)
{
	return eigrp_unix_rib_prefix_equal(&left->prefix, &right->prefix)
	       && eigrp_unix_rib_source_equal(&left->redist.source, &right->redist.source);
}

static eigrp_unix_rib_route_entry_t *eigrp_unix_rib_learned_lookup(
    eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix)
{
    eigrp_unix_rib_route_entry_t *entry;
    for (entry = learned_buckets[eigrp_unix_rib_learned_bucket(eigrp, prefix)];
         entry; entry = entry->hash_next)
        if (entry->eigrp == eigrp
            && eigrp_unix_rib_prefix_equal(&entry->route.prefix, prefix))
            return entry;
    return NULL;
}

static eigrp_unix_rib_source_entry_t *eigrp_unix_rib_source_lookup(
    const eigrp_rib_route_t *route)
{
    eigrp_unix_rib_source_entry_t *entry;
    for (entry = source_buckets[eigrp_unix_rib_source_bucket(route)];
         entry; entry = entry->hash_next)
        if (eigrp_unix_rib_source_key_equal(&entry->route, route))
            return entry;
    return NULL;
}

static void eigrp_unix_rib_learned_unlink(eigrp_unix_rib_route_entry_t *entry)
{
    eigrp_unix_rib_route_entry_t **cursor;
    size_t bucket = eigrp_unix_rib_learned_bucket(entry->eigrp, &entry->route.prefix);
    for (cursor = &learned_buckets[bucket]; *cursor; cursor = &(*cursor)->hash_next)
        if (*cursor == entry) {
            *cursor = entry->hash_next;
            break;
        }
    if (entry->prev) entry->prev->next = entry->next;
    else learned_routes = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
}

static void eigrp_unix_rib_source_unlink(eigrp_unix_rib_source_entry_t *entry)
{
    eigrp_unix_rib_source_entry_t **cursor;
    size_t bucket = eigrp_unix_rib_source_bucket(&entry->route);
    for (cursor = &source_buckets[bucket]; *cursor; cursor = &(*cursor)->hash_next)
        if (*cursor == entry) {
            *cursor = entry->hash_next;
            break;
        }
    if (entry->prev) entry->prev->next = entry->next;
    else source_routes = entry->next;
    if (entry->next) entry->next->prev = entry->prev;
}

static void eigrp_unix_rib_route_entry_free(
	eigrp_unix_rib_route_entry_t *entry)
{
	if (!entry)
		return;
	free(entry->nexthops);
	free(entry);
}

static eigrp_result_t eigrp_unix_rib_route_copy(
	eigrp_unix_rib_route_entry_t *entry, const eigrp_rib_route_t *route)
{
	eigrp_rib_nexthop_t *nexthops = NULL;

	if (route->nexthop_count && !route->nexthops)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (route->nexthop_count) {
		nexthops = calloc(route->nexthop_count, sizeof(*nexthops));
		if (!nexthops)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		memcpy(nexthops, route->nexthops,
		       route->nexthop_count * sizeof(*nexthops));
	}
	free(entry->nexthops);
	entry->route = *route;
	entry->nexthops = nexthops;
	entry->route.nexthops = nexthops;
	return EIGRP_RESULT_SUCCESS;
}


static void eigrp_unix_rib_source_entry_free(
	eigrp_unix_rib_source_entry_t *entry)
{
	if (!entry)
		return;
	free(entry->nexthops);
	free(entry);
}

static eigrp_result_t eigrp_unix_rib_source_copy(
	eigrp_unix_rib_source_entry_t *entry, const eigrp_rib_route_t *route)
{
	eigrp_rib_nexthop_t *nexthops = NULL;

	if (!entry || !route || (route->nexthop_count && !route->nexthops))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (route->nexthop_count) {
		nexthops = calloc(route->nexthop_count, sizeof(*nexthops));
		if (!nexthops)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		memcpy(nexthops, route->nexthops,
		       route->nexthop_count * sizeof(*nexthops));
	}
	free(entry->nexthops);
	entry->route = *route;
	entry->nexthops = nexthops;
	entry->route.nexthops = nexthops;
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_unix_rib_source_notify(
	const eigrp_rib_route_t *route, bool present)
{
	eigrp_unix_rib_subscription_t *subscription;

	for (subscription = subscriptions; subscription;
	     subscription = subscription->next) {
		if (!eigrp_unix_rib_source_equal(&subscription->source,
						 &route->redist.source))
			continue;
		if (eigrp_instance_afi(subscription->eigrp)
		    != route->prefix.address.afi)
			continue;
		if (present)
			(void)eigrp_rib_redist_add(subscription->eigrp, route);
		else
			(void)eigrp_rib_redist_del(subscription->eigrp, route);
	}
}

void eigrp_rib_init(void)
{
}

void eigrp_rib_finish(void)
{
	eigrp_unix_rib_route_entry_t *route;
	eigrp_unix_rib_source_entry_t *source;
	eigrp_unix_rib_subscription_t *subscription;

	while ((route = learned_routes) != NULL) {
		eigrp_unix_rib_learned_unlink(route);
		eigrp_unix_rib_route_entry_free(route);
	}
	while ((source = source_routes) != NULL) {
		eigrp_unix_rib_source_unlink(source);
		eigrp_unix_rib_source_entry_free(source);
	}
	while ((subscription = subscriptions) != NULL) {
		subscriptions = subscription->next;
		free(subscription);
	}
}

void eigrp_rib_instance_delete(eigrp_instance_t *eigrp)
{
    eigrp_unix_rib_route_entry_t *route, *next;
    eigrp_unix_rib_subscription_t **sub_cursor;
    eigrp_unix_rib_subscription_t *subscription;

    for (route = learned_routes; route; route = next) {
        next = route->next;
        if (route->eigrp != eigrp)
            continue;
        eigrp_unix_rib_learned_unlink(route);
        eigrp_unix_rib_route_entry_free(route);
    }
	for (sub_cursor = &subscriptions; *sub_cursor;) {
		if ((*sub_cursor)->eigrp != eigrp) {
			sub_cursor = &(*sub_cursor)->next;
			continue;
		}
		subscription = *sub_cursor;
		*sub_cursor = subscription->next;
		free(subscription);
	}
}

eigrp_result_t eigrp_rib_route_add(eigrp_instance_t *eigrp,
    const eigrp_rib_route_t *route)
{
    eigrp_unix_rib_route_entry_t *entry;
    size_t bucket;
    if (!eigrp || !route || !eigrp_unix_rib_prefix_valid(&route->prefix))
        return EIGRP_RESULT_INVALID_ARGUMENT;
    entry = eigrp_unix_rib_learned_lookup(eigrp, &route->prefix);
    if (entry)
        return eigrp_unix_rib_route_copy(entry, route);
    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return EIGRP_RESULT_INTERNAL_FAILURE;
    entry->eigrp = eigrp;
    if (eigrp_unix_rib_route_copy(entry, route) != EIGRP_RESULT_SUCCESS) {
        free(entry);
        return EIGRP_RESULT_INTERNAL_FAILURE;
    }
    bucket = eigrp_unix_rib_learned_bucket(eigrp, &route->prefix);
    entry->hash_next = learned_buckets[bucket];
    learned_buckets[bucket] = entry;
    entry->next = learned_routes;
    if (learned_routes) learned_routes->prev = entry;
    learned_routes = entry;
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_route_del(eigrp_instance_t *eigrp,
    const eigrp_prefix_t *prefix)
{
    eigrp_unix_rib_route_entry_t *entry;
    if (!eigrp || !eigrp_unix_rib_prefix_valid(prefix))
        return EIGRP_RESULT_INVALID_ARGUMENT;
    entry = eigrp_unix_rib_learned_lookup(eigrp, prefix);
    if (!entry)
        return EIGRP_RESULT_NOT_FOUND;
    eigrp_unix_rib_learned_unlink(entry);
    eigrp_unix_rib_route_entry_free(entry);
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_redistribute_add(eigrp_instance_t *eigrp,
	const eigrp_redist_source_t *source)
{
	eigrp_unix_rib_subscription_t *subscription;
	eigrp_unix_rib_source_entry_t *entry;

	if (!eigrp || !source
	    || source->protocol == EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (subscription = subscriptions; subscription;
	     subscription = subscription->next)
		if (subscription->eigrp == eigrp
		    && eigrp_unix_rib_source_equal(&subscription->source, source))
			return EIGRP_RESULT_SUCCESS;
	subscription = calloc(1, sizeof(*subscription));
	if (!subscription)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	subscription->eigrp = eigrp;
	subscription->source = *source;
	subscription->next = subscriptions;
	subscriptions = subscription;
	for (entry = source_routes; entry; entry = entry->next)
		if (eigrp_unix_rib_source_equal(&entry->route.redist.source, source)
		    && eigrp_instance_afi(eigrp) == entry->route.prefix.address.afi)
			(void)eigrp_rib_redist_add(eigrp, &entry->route);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_redistribute_remove(eigrp_instance_t *eigrp,
	const eigrp_redist_source_t *source)
{
	eigrp_unix_rib_subscription_t **cursor;
	eigrp_unix_rib_subscription_t *subscription;
	eigrp_unix_rib_source_entry_t *entry;

	if (!eigrp || !source)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (cursor = &subscriptions; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->eigrp != eigrp
		    || !eigrp_unix_rib_source_equal(&(*cursor)->source, source))
			continue;
		for (entry = source_routes; entry; entry = entry->next)
			if (eigrp_unix_rib_source_equal(&entry->route.redist.source, source)
			    && eigrp_instance_afi(eigrp)
				       == entry->route.prefix.address.afi)
				(void)eigrp_rib_redist_del(eigrp,
							    &entry->route);
		subscription = *cursor;
		*cursor = subscription->next;
		free(subscription);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_unix_rib_source_route_update(
    const eigrp_rib_route_t *route)
{
    eigrp_unix_rib_source_entry_t *entry;
    size_t bucket;
    if (!route || !eigrp_unix_rib_prefix_valid(&route->prefix)
        || route->redist.source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED)
        return EIGRP_RESULT_INVALID_ARGUMENT;
    entry = eigrp_unix_rib_source_lookup(route);
    if (entry) {
        if (eigrp_unix_rib_source_copy(entry, route) != EIGRP_RESULT_SUCCESS)
            return EIGRP_RESULT_INTERNAL_FAILURE;
        eigrp_unix_rib_source_notify(&entry->route, true);
        return EIGRP_RESULT_SUCCESS;
    }
    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return EIGRP_RESULT_INTERNAL_FAILURE;
    if (eigrp_unix_rib_source_copy(entry, route) != EIGRP_RESULT_SUCCESS) {
        free(entry);
        return EIGRP_RESULT_INTERNAL_FAILURE;
    }
    bucket = eigrp_unix_rib_source_bucket(route);
    entry->hash_next = source_buckets[bucket];
    source_buckets[bucket] = entry;
    entry->next = source_routes;
    if (source_routes) source_routes->prev = entry;
    source_routes = entry;
    eigrp_unix_rib_source_notify(&entry->route, true);
    return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_unix_rib_source_route_remove(
    const eigrp_rib_route_t *route)
{
    eigrp_unix_rib_source_entry_t *entry;
    if (!route || !eigrp_unix_rib_prefix_valid(&route->prefix))
        return EIGRP_RESULT_INVALID_ARGUMENT;
    entry = eigrp_unix_rib_source_lookup(route);
    if (!entry)
        return EIGRP_RESULT_NOT_FOUND;
    eigrp_unix_rib_source_unlink(entry);
    eigrp_unix_rib_source_notify(&entry->route, false);
    /* Source identity includes prefix, protocol and route_instance.  The
     * keyed source store cannot contain another entry of this identity. */
    eigrp_unix_rib_source_entry_free(entry);
    return EIGRP_RESULT_SUCCESS;
}

void eigrp_unix_rib_route_walk(eigrp_unix_rib_route_walk_cb callback,
	void *arg)
{
	eigrp_unix_rib_route_entry_t *entry;

	if (!callback)
		return;
	for (entry = learned_routes; entry; entry = entry->next)
		callback(entry->eigrp, &entry->route, arg);
}

void eigrp_unix_rib_source_walk(eigrp_unix_rib_source_walk_cb callback,
	void *arg)
{
	eigrp_unix_rib_source_entry_t *entry;

	if (!callback)
		return;
	for (entry = source_routes; entry; entry = entry->next)
		callback(&entry->route, arg);
}

size_t eigrp_unix_rib_route_count(void)
{
	eigrp_unix_rib_route_entry_t *entry;
	size_t count = 0;

	for (entry = learned_routes; entry; entry = entry->next)
		count++;
	return count;
}

size_t eigrp_unix_rib_source_count(void)
{
	eigrp_unix_rib_source_entry_t *entry;
	size_t count = 0;

	for (entry = source_routes; entry; entry = entry->next)
		count++;
	return count;
}
