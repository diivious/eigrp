// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR policy adaptation.
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdlib.h>
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrp_frr.h"
#include "eigrp_policy.h"

#include "filter.h"
#include "plist.h"
#include "routemap.h"
#include "vrf.h"

struct eigrp_policy_instance {
	eigrp_instance_t *eigrp;
	struct distribute_ctx *distribute_ctx;
	struct eigrp_policy_instance *next;
};

static struct eigrp_policy_instance *eigrp_policy_instances;

static struct eigrp_policy_instance *eigrp_policy_instance_find(
	eigrp_instance_t *eigrp)
{
	struct eigrp_policy_instance *state;

	for (state = eigrp_policy_instances; state; state = state->next)
		if (state->eigrp == eigrp)
			return state;
	return NULL;
}

static struct eigrp_policy_instance *eigrp_policy_context_find(
	struct distribute_ctx *ctx)
{
	struct eigrp_policy_instance *state;

	for (state = eigrp_policy_instances; state; state = state->next)
		if (state->distribute_ctx == ctx)
			return state;
	return NULL;
}

static void eigrp_policy_distribute_update(struct distribute_ctx *ctx,
					   struct distribute *dist)
{
	struct eigrp_policy_instance *state;
	eigrp_filter_runtime_snapshot_t snapshot;

	if (!ctx || !dist)
		return;
	state = eigrp_policy_context_find(ctx);
	if (!state || !state->eigrp)
		return;

	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.access_list[EIGRP_FILTER_IN] = dist->list[DISTRIBUTE_V4_IN];
	snapshot.access_list[EIGRP_FILTER_OUT] = dist->list[DISTRIBUTE_V4_OUT];
	snapshot.prefix_list[EIGRP_FILTER_IN] = dist->prefix[DISTRIBUTE_V4_IN];
	snapshot.prefix_list[EIGRP_FILTER_OUT] = dist->prefix[DISTRIBUTE_V4_OUT];
	(void)eigrp_filter_runtime_replace(state->eigrp, dist->ifname, &snapshot);
}

static void eigrp_policy_access_list_changed(struct access_list *access)
{
	(void)access;
	eigrp_filter_runtime_refresh_all();
}

static void eigrp_policy_prefix_list_changed(struct prefix_list *prefix)
{
	(void)prefix;
	eigrp_filter_runtime_refresh_all();
}

void eigrp_policy_init(void)
{
	access_list_init();
	access_list_add_hook(eigrp_policy_access_list_changed);
	access_list_delete_hook(eigrp_policy_access_list_changed);

	prefix_list_init();
	prefix_list_add_hook(eigrp_policy_prefix_list_changed);
	prefix_list_delete_hook(eigrp_policy_prefix_list_changed);

	/* Route-map CLI/YANG ownership is FRR-side even though EIGRP route-map
	 * runtime application remains incomplete.
	 */
	route_map_init();
}

void eigrp_policy_finish(void)
{
	struct eigrp_policy_instance *state;
	struct eigrp_policy_instance *next;

	for (state = eigrp_policy_instances; state; state = next) {
		next = state->next;
		if (state->distribute_ctx)
			distribute_list_delete(&state->distribute_ctx);
		free(state);
	}
	eigrp_policy_instances = NULL;

	route_map_finish();
	prefix_list_reset();
}

eigrp_result_t eigrp_policy_instance_create(eigrp_instance_t *eigrp)
{
	struct eigrp_policy_instance *state;
	struct vrf *vrf;

	if (!eigrp)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_policy_instance_find(eigrp))
		return EIGRP_RESULT_SUCCESS;

	state = calloc(1, sizeof(*state));
	if (!state)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	vrf = vrf_lookup_by_id(eigrp->vrf_id);
	state->distribute_ctx = distribute_list_ctx_create(vrf);
	if (!state->distribute_ctx) {
		free(state);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	state->eigrp = eigrp;
	distribute_list_add_hook(state->distribute_ctx,
				 eigrp_policy_distribute_update);
	distribute_list_delete_hook(state->distribute_ctx,
				    eigrp_policy_distribute_update);
	state->next = eigrp_policy_instances;
	eigrp_policy_instances = state;
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_policy_instance_delete(eigrp_instance_t *eigrp)
{
	struct eigrp_policy_instance **cursor;
	struct eigrp_policy_instance *state;

	if (!eigrp)
		return;
	for (cursor = &eigrp_policy_instances; *cursor;
	     cursor = &(*cursor)->next) {
		if ((*cursor)->eigrp != eigrp)
			continue;
		state = *cursor;
		*cursor = state->next;
		if (state->distribute_ctx)
			distribute_list_delete(&state->distribute_ctx);
		free(state);
		return;
	}
}

struct distribute_ctx *eigrp_policy_distribute_context(eigrp_instance_t *eigrp)
{
	struct eigrp_policy_instance *state = eigrp_policy_instance_find(eigrp);

	return state ? state->distribute_ctx : NULL;
}

eigrp_result_t eigrp_policy_filter_evaluate(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, const eigrp_prefix_t *prefix,
	eigrp_filter_decision_t *decision)
{
	struct access_list *access;
	struct prefix_list *plist;
	struct prefix host_prefix;
	afi_t afi;

	(void)eigrp;
	if (!name || !name[0] || !prefix || !decision)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	*decision = EIGRP_FILTER_DECISION_PERMIT;

	if (eigrp_frr_prefix_export(prefix, &host_prefix)
	    != EIGRP_RESULT_SUCCESS)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (prefix->address.afi == EIGRP_ADDRESS_FAMILY_IPV4)
		afi = AFI_IP;
	else if (prefix->address.afi == EIGRP_ADDRESS_FAMILY_IPV6)
		afi = AFI_IP6;
	else
		return EIGRP_RESULT_UNSUPPORTED;

	if (type == EIGRP_DISTRIBUTE_ACCESS_LIST) {
		access = access_list_lookup(afi, name);
		if (!access)
			return EIGRP_RESULT_NOT_FOUND;
		if (access_list_apply(access, &host_prefix) == FILTER_DENY)
			*decision = EIGRP_FILTER_DECISION_DENY;
		return EIGRP_RESULT_SUCCESS;
	}
	if (type == EIGRP_DISTRIBUTE_PREFIX_LIST) {
		plist = prefix_list_lookup(afi, name);
		if (!plist)
			return EIGRP_RESULT_NOT_FOUND;
		if (prefix_list_apply(plist, &host_prefix) == PREFIX_DENY)
			*decision = EIGRP_FILTER_DECISION_DENY;
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_INVALID_ARGUMENT;
}
