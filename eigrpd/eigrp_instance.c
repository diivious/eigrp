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

	if (!name || !name[0] || !vrf_name || !vrf_name[0] || asn == 0
	    || !eigrp_instance_afi_valid(afi))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	result = eigrp_instance_parent_create(name);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (eigrp_instance_address_family_read(name, afi, vrf_name, asn))
		return EIGRP_RESULT_SUCCESS;

	parent = eigrp_instance_parent_read(name);
	if (!parent)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	af = calloc(1, sizeof(*af));
	if (!af)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	af->vrf_name = eigrp_instance_string_duplicate(vrf_name);
	if (!af->vrf_name) {
		free(af);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	af->afi = afi;
	af->asn = asn;
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
		*cursor = af->next;
		eigrp_instance_address_family_free(af);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_instance_parent_delete(const char *name)
{
	eigrp_instance_parent_config_t **cursor;
	eigrp_instance_parent_config_t *parent;
	eigrp_address_family_config_t *af;
	eigrp_address_family_config_t *next;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;

	for (cursor = &eigrp_instance_parents; *cursor;
	     cursor = &(*cursor)->next) {
		parent = *cursor;
		if (strcmp(parent->name, name) != 0)
			continue;

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

eigrp_result_t eigrp_instance_router_id_update(eigrp_instance_context_t *context,
					       uint32_t router_id)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->router_id = router_id;
		context->config->router_id_configured = true;
	}
	if (context->runtime)
		context->runtime->router_id_static.s_addr = htonl(router_id);
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
	if (context->runtime)
		context->runtime->router_id_static.s_addr = INADDR_ANY;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_address_family_shutdown_update(
	eigrp_address_family_config_t *af, bool shutdown)
{
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	af->shutdown = shutdown;
	return EIGRP_RESULT_SUCCESS;
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
			eigrp_instance_address_family_free(af);
		}
		free(parent->name);
		free(parent);
	}
	eigrp_instance_parents = NULL;
}
