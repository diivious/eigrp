// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Shared control-plane segment model for the Unix EIGRP host shim.
 *
 * A segment is deliberately only a shared medium membership set.  It has no
 * MAC table, ARP/ND state, VLAN behavior, or point-to-point endpoint limit.
 * IPv4/IPv6 and unicast/multicast packet delivery can use the same membership
 * model without changing its topology semantics.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "eigrp_unix_segment.h"

typedef struct eigrp_unix_segment_endpoint {
	char *uut_name;
	char *interface_name;
	eigrp_unix_interface_t *interface;
	struct eigrp_unix_segment_endpoint *next;
} eigrp_unix_segment_endpoint_t;

struct eigrp_unix_segment {
	char *name;
	eigrp_unix_segment_endpoint_t *endpoints;
	struct eigrp_unix_segment *next;
};

static eigrp_unix_segment_t *segments;

eigrp_unix_segment_t *eigrp_unix_segment_find(const char *name)
{
	eigrp_unix_segment_t *segment;

	if (!name)
		return NULL;
	for (segment = segments; segment; segment = segment->next)
		if (strcmp(segment->name, name) == 0)
			return segment;
	return NULL;
}

eigrp_result_t eigrp_unix_segment_create(const char *name,
	eigrp_unix_segment_t **created)
{
	eigrp_unix_segment_t *segment;

	if (created)
		*created = NULL;
	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_unix_segment_find(name))
		return EIGRP_RESULT_CONFLICT;
	segment = calloc(1, sizeof(*segment));
	if (!segment)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	segment->name = strdup(name);
	if (!segment->name) {
		free(segment);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	segment->next = segments;
	segments = segment;
	if (created)
		*created = segment;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_unix_segment_endpoint_attach(
	eigrp_unix_segment_t *segment, const char *uut_name,
	const char *interface_name)
{
	eigrp_unix_segment_endpoint_t *endpoint;

	if (!segment || !uut_name || !uut_name[0] || !interface_name
	    || !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (endpoint = segment->endpoints; endpoint; endpoint = endpoint->next)
		if (strcmp(endpoint->uut_name, uut_name) == 0
		    && strcmp(endpoint->interface_name, interface_name) == 0)
			return EIGRP_RESULT_SUCCESS;
	endpoint = calloc(1, sizeof(*endpoint));
	if (!endpoint)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	endpoint->uut_name = strdup(uut_name);
	endpoint->interface_name = strdup(interface_name);
	if (!endpoint->uut_name || !endpoint->interface_name) {
		free(endpoint->uut_name);
		free(endpoint->interface_name);
		free(endpoint);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	endpoint->next = segment->endpoints;
	segment->endpoints = endpoint;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_unix_segment_interface_attach(
	eigrp_unix_segment_t *segment, const char *uut_name,
	eigrp_unix_interface_t *interface)
{
	eigrp_unix_segment_t *other;
	eigrp_unix_segment_endpoint_t *endpoint;
	eigrp_result_t result;

	if (!segment || !uut_name || !uut_name[0] || !interface)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (other = segments; other; other = other->next)
		for (endpoint = other->endpoints; endpoint; endpoint = endpoint->next)
			if (endpoint->interface == interface)
				return other == segment
				       && strcmp(endpoint->uut_name, uut_name) == 0
				       ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_CONFLICT;
	result = eigrp_unix_segment_endpoint_attach(segment, uut_name,
		eigrp_unix_interface_name(interface));
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	for (endpoint = segment->endpoints; endpoint; endpoint = endpoint->next)
		if (strcmp(endpoint->uut_name, uut_name) == 0
		    && strcmp(endpoint->interface_name,
			      eigrp_unix_interface_name(interface)) == 0) {
			if (endpoint->interface && endpoint->interface != interface)
				return EIGRP_RESULT_CONFLICT;
			endpoint->interface = interface;
			return EIGRP_RESULT_SUCCESS;
		}
	return EIGRP_RESULT_INTERNAL_FAILURE;
}

eigrp_result_t eigrp_unix_segment_interface_detach(
	eigrp_unix_segment_t *segment, const char *uut_name,
	const char *interface_name)
{
	eigrp_unix_segment_endpoint_t **cursor;
	eigrp_unix_segment_endpoint_t *endpoint;

	if (!segment || !uut_name || !uut_name[0] || !interface_name
	    || !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (cursor = &segment->endpoints; *cursor; cursor = &(*cursor)->next) {
		endpoint = *cursor;
		if (strcmp(endpoint->uut_name, uut_name) != 0
		    || strcmp(endpoint->interface_name, interface_name) != 0)
			continue;
		*cursor = endpoint->next;
		free(endpoint->uut_name);
		free(endpoint->interface_name);
		free(endpoint);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

void eigrp_unix_segment_interface_detach_all(eigrp_unix_interface_t *interface)
{
	eigrp_unix_segment_t *segment;
	eigrp_unix_segment_endpoint_t **cursor;

	if (!interface)
		return;
	for (segment = segments; segment; segment = segment->next) {
		for (cursor = &segment->endpoints; *cursor;) {
			eigrp_unix_segment_endpoint_t *endpoint = *cursor;
			if (endpoint->interface != interface) {
				cursor = &endpoint->next;
				continue;
			}
			*cursor = endpoint->next;
			free(endpoint->uut_name);
			free(endpoint->interface_name);
			free(endpoint);
		}
	}
}

eigrp_result_t eigrp_unix_segment_delete(const char *name)
{
	eigrp_unix_segment_t **cursor;
	eigrp_unix_segment_t *segment;
	eigrp_unix_segment_endpoint_t *endpoint;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (cursor = &segments; *cursor; cursor = &(*cursor)->next) {
		if (strcmp((*cursor)->name, name) != 0)
			continue;
		segment = *cursor;
		*cursor = segment->next;
		while ((endpoint = segment->endpoints) != NULL) {
			segment->endpoints = endpoint->next;
			free(endpoint->uut_name);
			free(endpoint->interface_name);
			free(endpoint);
		}
		free(segment->name);
		free(segment);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

const char *eigrp_unix_segment_name(const eigrp_unix_segment_t *segment)
{
	return segment ? segment->name : NULL;
}

eigrp_unix_segment_t *eigrp_unix_segment_interface_find(
	const eigrp_unix_interface_t *interface)
{
	eigrp_unix_segment_t *segment;
	eigrp_unix_segment_endpoint_t *endpoint;

	if (!interface)
		return NULL;
	for (segment = segments; segment; segment = segment->next)
		for (endpoint = segment->endpoints; endpoint; endpoint = endpoint->next)
			if (endpoint->interface == interface)
				return segment;
	return NULL;
}

size_t eigrp_unix_segment_endpoint_count(const eigrp_unix_segment_t *segment)
{
	const eigrp_unix_segment_endpoint_t *endpoint;
	size_t count = 0;

	if (!segment)
		return 0;
	for (endpoint = segment->endpoints; endpoint; endpoint = endpoint->next)
		count++;
	return count;
}

void eigrp_unix_segment_endpoint_walk(const eigrp_unix_segment_t *segment,
	eigrp_unix_segment_endpoint_walk_cb callback, void *arg)
{
	const eigrp_unix_segment_endpoint_t *endpoint;

	if (!segment || !callback)
		return;
	for (endpoint = segment->endpoints; endpoint; endpoint = endpoint->next)
		callback(endpoint->uut_name, endpoint->interface_name,
		 endpoint->interface, arg);
}

void eigrp_unix_segment_model_reset(void)
{
	while (segments)
		(void)eigrp_unix_segment_delete(segments->name);
}
