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
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_redistribute.h"
#include "eigrpd/eigrp_summary.h"
#include "eigrpd/eigrp_timer.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"

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
	eigrp_instance_t *runtime;
	eigrp_vrf_id_t vrf_id;
	eigrp_result_t result;

	if (!name || !af)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	result = eigrp_sys_vrf_resolve(af->vrf_name, &vrf_id);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	/* {AF, VRF, AS} is the protocol runtime identity.  The named parent is
	 * local configuration ownership and is never part of wire identity.
	 */
	runtime = eigrp_lookup_by_af_as_vrf(af->afi, af->asn, vrf_id);
	if (runtime) {
		if (!runtime->name || strcmp(runtime->name, name) != 0)
			return EIGRP_RESULT_CONFLICT;
	} else {
		runtime = eigrp_get_by_af(
			af->afi, af->asn, vrf_id,
			af->afi == EIGRP_ADDRESS_FAMILY_IPV4);
		if (!runtime)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		eigrp_name_set(runtime, name);
	}

	/* Runtime gets the same immutable AF dispatch selected by configuration. */
	runtime->af_vectors = af->af_vectors;
	af->runtime = runtime;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_instance_address_family_runtime_delete(
	const char *name, eigrp_address_family_config_t *af)
{
	if (!name || !af)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af->runtime)
		return EIGRP_RESULT_SUCCESS;
	if (!af->runtime->name || strcmp(af->runtime->name, name) != 0)
		return EIGRP_RESULT_CONFLICT;

	eigrp_finish_final(af->runtime);
	af->runtime = NULL;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_classic_validate(
	uint16_t asn, eigrp_vrf_id_t vrf_id, const char **owner_name)
{
	eigrp_instance_t *runtime;

	if (owner_name)
		*owner_name = NULL;
	if (!asn)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	runtime = eigrp_lookup_by_as_vrf(asn, vrf_id);
	if (!runtime || !runtime->name)
		return EIGRP_RESULT_SUCCESS;
	if (owner_name)
		*owner_name = runtime->name;
	return EIGRP_RESULT_CONFLICT;
}

eigrp_result_t eigrp_instance_classic_create(
	uint16_t asn, eigrp_vrf_id_t vrf_id, eigrp_instance_t **runtime)
{
	eigrp_result_t result;

	if (!runtime)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	*runtime = NULL;
	result = eigrp_instance_classic_validate(asn, vrf_id, NULL);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	*runtime = eigrp_get(asn, vrf_id);
	return *runtime ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_INTERNAL_FAILURE;
}

eigrp_instance_t *eigrp_instance_classic_read(uint16_t asn,
					eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *runtime = eigrp_lookup_by_as_vrf(asn, vrf_id);

	return runtime && !runtime->name ? runtime : NULL;
}

eigrp_result_t eigrp_instance_classic_delete(eigrp_instance_t *runtime)
{
	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (runtime->name)
		return EIGRP_RESULT_CONFLICT;
	eigrp_finish_final(runtime);
	return EIGRP_RESULT_SUCCESS;
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

/*
 * Syntax:
 *   Named: `router eigrp NAME` / `no router eigrp NAME`
 * Supported: Named
 * Placement:
 *   Named: global configuration
 * Description:
 * Creates or removes the named EIGRP parent configuration object.
 * The parent is a configuration container; address-family creation owns AS/family runtime creation.
 */
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

/*
 * Syntax:
 *   Named: `address-family <ipv4|ipv6> [unicast] [vrf NAME] autonomous-system AS` / `no address-family ...`
 * Supported: Named
 * Placement:
 *   Named: router EIGRP parent mode
 * Description:
 * Creates or removes one named EIGRP address-family and its EIGRP-owned runtime context.
 * IPv4 and IPv6 retain separate AF state while sharing the named parent.
 */
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
	eigrp_redistribute_policy_delete_all(af);
	eigrp_distribute_list_config_delete_all(af);
	eigrp_offset_config_delete_all(af);
	eigrp_metric_config_delete_all(af);
	eigrp_summary_state_delete_all(af);
	eigrp_timer_config_delete_all(af);
	eigrp_neighbor_policy_delete_all(af);
	free(af->vrf_name);
	free(af);
}

/*
 * Syntax:
 *   Named: `address-family <ipv4|ipv6> [unicast] [vrf NAME] autonomous-system AS` / `no address-family ...`
 * Supported: Named
 * Placement:
 *   Named: router EIGRP parent mode
 * Description:
 * Creates or removes one named EIGRP address-family and its EIGRP-owned runtime context.
 * IPv4 and IPv6 retain separate AF state while sharing the named parent.
 */
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

/*
 * Syntax:
 *   EXEC: named address-family show commands and `show eigrp protocols`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Walks EIGRP-owned address-family state using an EIGRP request and callback.
 * FRR VTY and YANG objects remain outside the common API.
 */
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
	/*
	 * The CLI token selects EIGRP's Multicast Address Family (VRID 0x0001),
	 * not normal multicast packet transport.  The MAF config/runtime model
	 * is intentionally deferred, so keep the grammar but terminate the real
	 * state target truthfully here.
	 */
	if (request->multicast)
		return EIGRP_RESULT_NOT_IMPLEMENTED;

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

/*
 * Syntax:
 *   Named: `router eigrp NAME` / `no router eigrp NAME`
 * Supported: Named
 * Placement:
 *   Named: global configuration
 * Description:
 * Creates or removes the named EIGRP parent configuration object.
 * The parent is a configuration container; address-family creation owns AS/family runtime creation.
 */
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

static void eigrp_instance_router_id_refresh(eigrp_instance_t *runtime)
{
	if (!runtime)
		return;
	if (!runtime->data_path_ready) {
		runtime->router_id = runtime->router_id_static;
		return;
	}
	eigrp_router_id_update(runtime);
}

bool eigrp_instance_data_path_ready(const eigrp_instance_t *runtime)
{
	return runtime && runtime->data_path_ready;
}

void eigrp_sys_router_id_refresh(eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *runtime;
	eigrp_list_node_t *node;

	if (!eigrp_om)
		return;
	for (EIGRP_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, runtime)) {
		if (runtime->vrf_id == vrf_id)
			eigrp_instance_router_id_refresh(runtime);
	}
}

eigrp_result_t eigrp_instance_address_family_stop(eigrp_instance_t *runtime)
{
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (!runtime->data_path_ready)
		return EIGRP_RESULT_NOT_IMPLEMENTED;

	for (EIGRP_LIST_ELEMENTS_RO(runtime->eiflist, node, ei)) {
		if (!ei->t_hello)
			continue;
		eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);
		eigrp_intf_down(ei);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_address_family_start(eigrp_instance_t *runtime)
{
	eigrp_address_family_config_t *af;
	eigrp_interface_config_t *config;
	eigrp_interface_t *ei;
	eigrp_list_node_t *node;

	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (!runtime->data_path_ready)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	if (runtime->router_id.s_addr == INADDR_ANY)
		eigrp_router_id_update(runtime);
	if (runtime->router_id.s_addr == INADDR_ANY)
		return EIGRP_RESULT_SUCCESS;

	/* Re-read host interface state before deciding which EIGRP interfaces can
	 * run.  The host adapter only enumerates and normalizes interface state.
	 */
	eigrp_network_interfaces_refresh(runtime);
	af = eigrp_instance_runtime_config(runtime);
	for (EIGRP_LIST_ELEMENTS_RO(runtime->eiflist, node, ei)) {
		config = af ? eigrp_interface_config_read(af, ei->name) : NULL;
		if (config)
			eigrp_interface_runtime_bind(ei, config);
		if ((config && config->shutdown) || !ei->operative || ei->t_hello)
			continue;
		eigrp_intf_up(runtime, ei);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `eigrp router-id A.B.C.D` / `no eigrp router-id [A.B.C.D]`
 *   Named: `eigrp router-id A.B.C.D` / `no eigrp router-id [A.B.C.D]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: address-family mode
 * Description:
 * Sets or resets the 32-bit EIGRP router ID for the selected instance.
 * Named mode reaches the same EIGRP-owned router-ID behavior instead of duplicating protocol state in the FRR CLI.
 */
eigrp_result_t eigrp_instance_router_id_set(eigrp_instance_context_t *context,
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
		eigrp_instance_router_id_refresh(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `eigrp router-id A.B.C.D` / `no eigrp router-id [A.B.C.D]`
 *   Named: `eigrp router-id A.B.C.D` / `no eigrp router-id [A.B.C.D]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: address-family mode
 * Description:
 * Sets or resets the 32-bit EIGRP router ID for the selected instance.
 * Named mode reaches the same EIGRP-owned router-ID behavior instead of duplicating protocol state in the FRR CLI.
 */
eigrp_result_t eigrp_instance_router_id_reset(eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->router_id = 0;
		context->config->router_id_configured = false;
	}
	if (context->runtime) {
		context->runtime->router_id_static.s_addr = INADDR_ANY;
		eigrp_instance_router_id_refresh(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `shutdown` / `no shutdown`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Changes the administrative state of one named address-family.
 * The common target owns retained state and runtime start/stop behavior.
 */
static eigrp_result_t eigrp_instance_address_family_shutdown_apply(
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
		 ? eigrp_instance_address_family_stop(af->runtime)
		 : eigrp_instance_address_family_start(af->runtime);
	/* NOT_IMPLEMENTED is a truthful data-path boundary, not config failure. */
	if (result != EIGRP_RESULT_SUCCESS
	    && result != EIGRP_RESULT_NOT_IMPLEMENTED)
		af->shutdown = !shutdown;
	return result;
}

eigrp_result_t eigrp_instance_address_family_shutdown_set(
	eigrp_address_family_config_t *af)
{
	return eigrp_instance_address_family_shutdown_apply(af, true);
}

eigrp_result_t eigrp_instance_address_family_shutdown_reset(
	eigrp_address_family_config_t *af)
{
	return eigrp_instance_address_family_shutdown_apply(af, false);
}

/*
 * Syntax:
 *   Named: `shutdown` / `no shutdown`
 * Supported: Named
 * Placement:
 *   Named: router EIGRP parent mode
 * Description:
 * Represents administrative shutdown of the named parent rather than one address-family.
 * The real target remains in place and reports NOT_IMPLEMENTED until parent-wide runtime semantics are defined.
 */
static eigrp_result_t eigrp_instance_parent_shutdown_apply(
	eigrp_instance_parent_config_t *parent, bool shutdown)
{
	(void)shutdown;
	if (!parent)
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_instance_parent_shutdown_set(
	eigrp_instance_parent_config_t *parent)
{
	return eigrp_instance_parent_shutdown_apply(parent, true);
}

eigrp_result_t eigrp_instance_parent_shutdown_reset(
	eigrp_instance_parent_config_t *parent)
{
	return eigrp_instance_parent_shutdown_apply(parent, false);
}

/*
 * Syntax:
 *   Named: `distance eigrp INTERNAL EXTERNAL` / `no distance eigrp`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets internal and external EIGRP administrative distance.
 * RIB-side application remains owned by this target and reports NOT_IMPLEMENTED while that runtime path is incomplete.
 */
eigrp_result_t eigrp_instance_distance_set(eigrp_address_family_config_t *af,
					      uint8_t internal_distance,
					      uint8_t external_distance)
{
	if (!internal_distance || !external_distance)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

/*
 * Syntax:
 *   Named: `distance eigrp INTERNAL EXTERNAL` / `no distance eigrp`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets internal and external EIGRP administrative distance.
 * RIB-side application remains owned by this target and reports NOT_IMPLEMENTED while that runtime path is incomplete.
 */
eigrp_result_t eigrp_instance_distance_reset(eigrp_address_family_config_t *af)
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
