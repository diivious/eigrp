// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP instance and address-family configuration ownership.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdlib.h>
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_redistribute.h"
#include "eigrpd/eigrp_southbound.h"

/* AF modules intentionally expose only their vector bind entry points. */

static eigrp_instance_parent_config_t *eigrp_instance_parents;

static char *eigrp_instance_string_duplicate(const char *value)
{
	size_t len;
	char *copy;

	if (!value)
		return NULL;

	len = strlen(value) + 1;
	copy = malloc(len);
	if (!copy)
		return NULL;
	memcpy(copy, value, len);
	return copy;
}

static bool eigrp_instance_afi_valid(eigrp_address_family_t afi)
{
	return afi == EIGRP_ADDRESS_FAMILY_IPV4
	       || afi == EIGRP_ADDRESS_FAMILY_IPV6;
}

static eigrp_result_t eigrp_instance_address_family_runtime_create(
	const char *name, eigrp_address_family_config_t *af)
{
	eigrp_instance_t *runtime = NULL;
	eigrp_result_t result;

	if (!name || !af)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	/*
	 * IPv6 named-mode configuration is required now, but its runtime/data
	 * path is intentionally not created until IPv6 protocol support exists.
	 */
	if (af->afi == EIGRP_ADDRESS_FAMILY_IPV6)
		return EIGRP_RESULT_SUCCESS;

	result = eigrp_southbound_instance_create(
		name, af->afi, af->vrf_name, af->asn, &runtime);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	/* Runtime gets the same immutable AF dispatch selected by configuration. */
	runtime->af_vectors = af->af_vectors;
	af->runtime = runtime;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_instance_address_family_runtime_delete(
	const char *name, eigrp_address_family_config_t *af)
{
	eigrp_result_t result;

	if (!name || !af)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af->runtime)
		return EIGRP_RESULT_SUCCESS;

	result = eigrp_southbound_instance_delete(name, af->runtime);
	if (result == EIGRP_RESULT_NOT_FOUND)
		result = EIGRP_RESULT_SUCCESS;
	if (result == EIGRP_RESULT_SUCCESS)
		af->runtime = NULL;
	return result;
}

eigrp_instance_parent_config_t *eigrp_instance_parent_read(const char *name)
{
	eigrp_instance_parent_config_t *parent;

	if (!name || !name[0])
		return NULL;

	for (parent = eigrp_instance_parents; parent; parent = parent->next) {
		if (strcmp(parent->name, name) == 0)
			return parent;
	}
	return NULL;
}

eigrp_result_t eigrp_instance_parent_create(const char *name)
{
	eigrp_instance_parent_config_t *parent;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_instance_parent_read(name))
		return EIGRP_RESULT_SUCCESS;

	parent = calloc(1, sizeof(*parent));
	if (!parent)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	parent->name = eigrp_instance_string_duplicate(name);
	if (!parent->name) {
		free(parent);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	parent->next = eigrp_instance_parents;
	eigrp_instance_parents = parent;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_address_family_config_t *eigrp_instance_address_family_read(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_instance_parent_config_t *parent;
	eigrp_address_family_config_t *af;

	parent = eigrp_instance_parent_read(name);
	if (!parent || !vrf_name)
		return NULL;

	for (af = parent->address_families; af; af = af->next) {
		if (af->afi == afi && af->asn == asn
		    && strcmp(af->vrf_name, vrf_name) == 0)
			return af;
	}
	return NULL;
}

eigrp_result_t eigrp_instance_address_family_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_instance_parent_config_t *parent;
	eigrp_address_family_config_t *af;
	eigrp_result_t result;
	bool parent_created = false;

	if (!name || !name[0] || !vrf_name || !vrf_name[0] || asn == 0
	    || !eigrp_instance_afi_valid(afi))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	parent = eigrp_instance_parent_read(name);
	if (!parent) {
		result = eigrp_instance_parent_create(name);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
		parent_created = true;
	}
	if (eigrp_instance_address_family_read(name, afi, vrf_name, asn))
		return EIGRP_RESULT_SUCCESS;

	parent = eigrp_instance_parent_read(name);
	if (!parent) {
		if (parent_created)
			(void)eigrp_instance_parent_delete(name);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	af = calloc(1, sizeof(*af));
	if (!af) {
		if (parent_created)
			(void)eigrp_instance_parent_delete(name);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	af->vrf_name = eigrp_instance_string_duplicate(vrf_name);
	if (!af->vrf_name) {
		free(af);
		if (parent_created)
			(void)eigrp_instance_parent_delete(name);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	af->afi = afi;
	af->asn = asn;

	/* Bind AF behavior once; common feature code calls through this vector. */
	switch (afi) {
	case EIGRP_ADDRESS_FAMILY_IPV4:
		eigrp_ipv4_init(&af->af_vectors);
		break;
	case EIGRP_ADDRESS_FAMILY_IPV6:
		eigrp_ipv6_init(&af->af_vectors);
		break;
	}

	result = eigrp_instance_address_family_runtime_create(name, af);
	if (result != EIGRP_RESULT_SUCCESS) {
		free(af->vrf_name);
		free(af);
		if (parent_created)
			(void)eigrp_instance_parent_delete(name);
		return result;
	}
	af->next = parent->address_families;
	parent->address_families = af;
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_instance_address_family_free(eigrp_address_family_config_t *af)
{
	if (!af)
		return;

	eigrp_network_config_delete_all(af);
	eigrp_neighbor_static_delete_all(af);
	eigrp_interface_config_delete_all(af);
	eigrp_redistribute_config_delete_all(af);
	eigrp_distribute_list_config_delete_all(af);
	free(af->vrf_name);
	free(af);
}

eigrp_result_t eigrp_instance_address_family_delete(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_instance_parent_config_t *parent;
	eigrp_address_family_config_t **cursor;
	eigrp_address_family_config_t *af;

	if (!name || !name[0] || !vrf_name || !vrf_name[0] || asn == 0
	    || !eigrp_instance_afi_valid(afi))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	parent = eigrp_instance_parent_read(name);
	if (!parent)
		return EIGRP_RESULT_NOT_FOUND;

	for (cursor = &parent->address_families; *cursor;
	     cursor = &(*cursor)->next) {
		af = *cursor;
		if (af->afi != afi || af->asn != asn
		    || strcmp(af->vrf_name, vrf_name) != 0)
			continue;
		{
			eigrp_result_t result =
				eigrp_instance_address_family_runtime_delete(
					name, af);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
		*cursor = af->next;
		eigrp_instance_address_family_free(af);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_instance_address_family_walk(
	const eigrp_state_request_t *request,
	eigrp_instance_address_family_walk_cb callback, void *arg)
{
	eigrp_instance_parent_config_t *parent;
	eigrp_address_family_config_t *af;
	eigrp_result_t result;
	const char *vrf_name;
	bool matched = false;

	if (!request || !callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (request->afi != EIGRP_ADDRESS_FAMILY_IPV4
	    && request->afi != EIGRP_ADDRESS_FAMILY_IPV6)
		return EIGRP_RESULT_UNSUPPORTED;

	/* A normal show request without an explicit VRF is scoped to default. */
	vrf_name = request->vrf_name ? request->vrf_name : "default";

	for (parent = eigrp_instance_parents; parent; parent = parent->next) {
		for (af = parent->address_families; af; af = af->next) {
			if (af->afi != request->afi)
				continue;
			if (request->asn && af->asn != request->asn)
				continue;
			if (!request->all_vrfs
			    && strcmp(af->vrf_name, vrf_name) != 0)
				continue;

			matched = true;
			result = callback(parent->name, af, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
	}

	return matched ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_instance_parent_delete(const char *name)
{
	eigrp_instance_parent_config_t **cursor;
	eigrp_instance_parent_config_t *parent;
	eigrp_address_family_config_t *af;
	eigrp_address_family_config_t *next;
	eigrp_result_t result;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;

	for (cursor = &eigrp_instance_parents; *cursor;
	     cursor = &(*cursor)->next) {
		parent = *cursor;
		if (strcmp(parent->name, name) != 0)
			continue;

		/* Stop every bound runtime before discarding configuration ownership. */
		for (af = parent->address_families; af; af = af->next) {
			result = eigrp_instance_address_family_runtime_delete(
				parent->name, af);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}

		*cursor = parent->next;
		for (af = parent->address_families; af; af = next) {
			next = af->next;
			eigrp_instance_address_family_free(af);
		}
		free(parent->name);
		free(parent);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

void eigrp_instance_runtime_unbind(eigrp_instance_t *runtime)
{
	eigrp_instance_parent_config_t *parent;
	eigrp_address_family_config_t *af;

	if (!runtime)
		return;

	for (parent = eigrp_instance_parents; parent; parent = parent->next) {
		for (af = parent->address_families; af; af = af->next) {
			if (af->runtime == runtime)
				af->runtime = NULL;
		}
	}
}


eigrp_address_family_config_t *eigrp_instance_runtime_config(eigrp_instance_t *runtime)
{
	eigrp_instance_parent_config_t *parent;
	eigrp_address_family_config_t *af;

	if (!runtime)
		return NULL;

	for (parent = eigrp_instance_parents; parent; parent = parent->next)
		for (af = parent->address_families; af; af = af->next)
			if (af->runtime == runtime)
				return af;
	return NULL;
}

eigrp_result_t eigrp_instance_router_id_update(eigrp_instance_context_t *context,
					       uint32_t router_id)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (router_id == 0 || router_id == UINT32_MAX)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (context->config) {
		context->config->router_id = router_id;
		context->config->router_id_configured = true;
	}
	if (context->runtime) {
		context->runtime->router_id_static.s_addr = htonl(router_id);
		eigrp_southbound_router_id_refresh(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_router_id_delete(eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->router_id = 0;
		context->config->router_id_configured = false;
	}
	if (context->runtime) {
		context->runtime->router_id_static.s_addr = INADDR_ANY;
		eigrp_southbound_router_id_refresh(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_address_family_shutdown_update(
	eigrp_address_family_config_t *af, bool shutdown)
{
	eigrp_result_t result;

	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	if (af->shutdown == shutdown)
		return EIGRP_RESULT_SUCCESS;

	/* Commit retained configuration first, then change runtime state. */
	af->shutdown = shutdown;
	if (!af->runtime)
		return af->afi == EIGRP_ADDRESS_FAMILY_IPV6
		       ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;

	result = shutdown
		 ? eigrp_southbound_address_family_stop(af->runtime)
		 : eigrp_southbound_address_family_start(af->runtime);
	if (result != EIGRP_RESULT_SUCCESS)
		af->shutdown = !shutdown;
	return result;
}

eigrp_result_t eigrp_instance_parent_shutdown_update(
	eigrp_instance_parent_config_t *parent, bool shutdown)
{
	(void)shutdown;
	if (!parent)
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_instance_distance_update(eigrp_address_family_config_t *af,
					      uint8_t internal_distance,
					      uint8_t external_distance)
{
	if (!internal_distance || !external_distance)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_instance_distance_delete(eigrp_address_family_config_t *af)
{
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

void eigrp_instance_config_finish(void)
{
	eigrp_instance_parent_config_t *parent;
	eigrp_instance_parent_config_t *next;
	eigrp_address_family_config_t *af;
	eigrp_address_family_config_t *af_next;

	for (parent = eigrp_instance_parents; parent; parent = next) {
		next = parent->next;
		for (af = parent->address_families; af; af = af_next) {
			af_next = af->next;
			(void)eigrp_instance_address_family_runtime_delete(
				parent->name, af);
			eigrp_instance_address_family_free(af);
		}
		free(parent->name);
		free(parent);
	}
	eigrp_instance_parents = NULL;
}
