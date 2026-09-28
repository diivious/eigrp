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
#include <stdlib.h>
#include <string.h>

#include "eigrp_unix_rib.h"

typedef struct eigrp_unix_rib_route_entry {
	eigrp_instance_t *eigrp;
	eigrp_rib_route_t route;
	eigrp_rib_nexthop_t *nexthops;
	struct eigrp_unix_rib_route_entry *next;
} eigrp_unix_rib_route_entry_t;

typedef struct eigrp_unix_rib_source_entry {
	eigrp_rib_source_route_t route;
	struct eigrp_unix_rib_source_entry *next;
} eigrp_unix_rib_source_entry_t;

typedef struct eigrp_unix_rib_subscription {
	eigrp_instance_t *eigrp;
	eigrp_redist_source_t source;
	struct eigrp_unix_rib_subscription *next;
} eigrp_unix_rib_subscription_t;

static eigrp_unix_rib_route_entry_t *learned_routes;
static eigrp_unix_rib_source_entry_t *source_routes;
static eigrp_unix_rib_subscription_t *subscriptions;

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
	const eigrp_rib_source_route_t *left,
	const eigrp_rib_source_route_t *right)
{
	return eigrp_unix_rib_prefix_equal(&left->prefix, &right->prefix)
	       && eigrp_unix_rib_source_equal(&left->source, &right->source);
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

static void eigrp_unix_rib_source_notify(
	const eigrp_rib_source_route_t *route, bool present)
{
	eigrp_unix_rib_subscription_t *subscription;

	for (subscription = subscriptions; subscription;
	     subscription = subscription->next) {
		if (!eigrp_unix_rib_source_equal(&subscription->source,
						 &route->source))
			continue;
		if (eigrp_instance_afi(subscription->eigrp)
		    != route->prefix.address.afi)
			continue;
		if (present)
			(void)eigrp_rib_source_route_add(subscription->eigrp, route);
		else
			(void)eigrp_rib_source_route_remove(subscription->eigrp, route);
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
		learned_routes = route->next;
		eigrp_unix_rib_route_entry_free(route);
	}
	while ((source = source_routes) != NULL) {
		source_routes = source->next;
		free(source);
	}
	while ((subscription = subscriptions) != NULL) {
		subscriptions = subscription->next;
		free(subscription);
	}
}

void eigrp_rib_instance_delete(eigrp_instance_t *eigrp)
{
	eigrp_unix_rib_route_entry_t **route_cursor;
	eigrp_unix_rib_route_entry_t *route;
	eigrp_unix_rib_subscription_t **sub_cursor;
	eigrp_unix_rib_subscription_t *subscription;

	for (route_cursor = &learned_routes; *route_cursor;) {
		if ((*route_cursor)->eigrp != eigrp) {
			route_cursor = &(*route_cursor)->next;
			continue;
		}
		route = *route_cursor;
		*route_cursor = route->next;
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

eigrp_result_t eigrp_rib_route_install(eigrp_instance_t *eigrp,
	const eigrp_rib_route_t *route)
{
	eigrp_unix_rib_route_entry_t *entry;

	if (!eigrp || !route || !eigrp_unix_rib_prefix_valid(&route->prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (entry = learned_routes; entry; entry = entry->next)
		if (entry->eigrp == eigrp
		    && eigrp_unix_rib_prefix_equal(&entry->route.prefix,
						 &route->prefix))
			return eigrp_unix_rib_route_copy(entry, route);
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	entry->eigrp = eigrp;
	if (eigrp_unix_rib_route_copy(entry, route) != EIGRP_RESULT_SUCCESS) {
		free(entry);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	entry->next = learned_routes;
	learned_routes = entry;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_route_remove(eigrp_instance_t *eigrp,
	const eigrp_prefix_t *prefix)
{
	eigrp_unix_rib_route_entry_t **cursor;
	eigrp_unix_rib_route_entry_t *entry;

	if (!eigrp || !eigrp_unix_rib_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (cursor = &learned_routes; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->eigrp != eigrp
		    || !eigrp_unix_rib_prefix_equal(&(*cursor)->route.prefix, prefix))
			continue;
		entry = *cursor;
		*cursor = entry->next;
		eigrp_unix_rib_route_entry_free(entry);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
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
		if (eigrp_unix_rib_source_equal(&entry->route.source, source)
		    && eigrp_instance_afi(eigrp) == entry->route.prefix.address.afi)
			(void)eigrp_rib_source_route_add(eigrp, &entry->route);
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
			if (eigrp_unix_rib_source_equal(&entry->route.source, source)
			    && eigrp_instance_afi(eigrp)
				       == entry->route.prefix.address.afi)
				(void)eigrp_rib_source_route_remove(eigrp,
							    &entry->route);
		subscription = *cursor;
		*cursor = subscription->next;
		free(subscription);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_unix_rib_source_route_update(
	const eigrp_rib_source_route_t *route)
{
	eigrp_unix_rib_source_entry_t *entry;

	if (!route || !eigrp_unix_rib_prefix_valid(&route->prefix)
	    || route->source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (entry = source_routes; entry; entry = entry->next) {
		if (!eigrp_unix_rib_source_key_equal(&entry->route, route))
			continue;
		entry->route = *route;
		eigrp_unix_rib_source_notify(route, true);
		return EIGRP_RESULT_SUCCESS;
	}
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	entry->route = *route;
	entry->next = source_routes;
	source_routes = entry;
	eigrp_unix_rib_source_notify(route, true);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_unix_rib_source_route_remove(
	const eigrp_rib_source_route_t *route)
{
	eigrp_unix_rib_source_entry_t **cursor;
	eigrp_unix_rib_source_entry_t *entry;

	if (!route || !eigrp_unix_rib_prefix_valid(&route->prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (cursor = &source_routes; *cursor; cursor = &(*cursor)->next) {
		if (!eigrp_unix_rib_source_key_equal(&(*cursor)->route, route))
			continue;
		entry = *cursor;
		*cursor = entry->next;
		eigrp_unix_rib_source_notify(&entry->route, false);
		{
			eigrp_unix_rib_source_entry_t *replacement;

			for (replacement = source_routes; replacement;
			     replacement = replacement->next) {
				if (eigrp_unix_rib_prefix_equal(
					    &replacement->route.prefix, &entry->route.prefix)
				    && eigrp_unix_rib_source_equal(
					    &replacement->route.source, &entry->route.source)) {
					eigrp_unix_rib_source_notify(&replacement->route, true);
					break;
				}
			}
		}
		free(entry);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
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
