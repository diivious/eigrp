// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Filter Functions.
 * Copyright (C) 2013-2015
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
 */
#include <stdlib.h>
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_const.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_southbound.h"

static char *eigrp_filter_string_duplicate(const char *value)
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

static bool eigrp_filter_string_equal(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	return strcmp(a, b) == 0;
}

void eigrp_filter_runtime_state_clear(eigrp_filter_runtime_state_t *state)
{
	int direction;

	if (!state)
		return;

	for (direction = 0; direction < EIGRP_FILTER_MAX; direction++) {
		free(state->access_list[direction]);
		free(state->prefix_list[direction]);
		state->access_list[direction] = NULL;
		state->prefix_list[direction] = NULL;
	}
}

static bool eigrp_filter_runtime_state_equal(
	const eigrp_filter_runtime_state_t *state,
	const eigrp_filter_runtime_snapshot_t *snapshot)
{
	int direction;

	for (direction = 0; direction < EIGRP_FILTER_MAX; direction++) {
		if (!eigrp_filter_string_equal(state->access_list[direction],
					       snapshot->access_list[direction])
		    || !eigrp_filter_string_equal(state->prefix_list[direction],
						 snapshot->prefix_list[direction]))
			return false;
	}
	return true;
}

static eigrp_result_t eigrp_filter_runtime_state_copy(
	eigrp_filter_runtime_state_t *state,
	const eigrp_filter_runtime_snapshot_t *snapshot)
{
	int direction;

	memset(state, 0, sizeof(*state));
	for (direction = 0; direction < EIGRP_FILTER_MAX; direction++) {
		if (snapshot->access_list[direction]) {
			state->access_list[direction] = eigrp_filter_string_duplicate(
				snapshot->access_list[direction]);
			if (!state->access_list[direction])
				goto failure;
		}
		if (snapshot->prefix_list[direction]) {
			state->prefix_list[direction] = eigrp_filter_string_duplicate(
				snapshot->prefix_list[direction]);
			if (!state->prefix_list[direction])
				goto failure;
		}
	}
	return EIGRP_RESULT_SUCCESS;

failure:
	eigrp_filter_runtime_state_clear(state);
	return EIGRP_RESULT_INTERNAL_FAILURE;
}

static bool eigrp_filter_runtime_state_active(
	const eigrp_filter_runtime_state_t *state)
{
	int direction;

	if (!state)
		return false;
	for (direction = 0; direction < EIGRP_FILTER_MAX; direction++)
		if (state->access_list[direction] || state->prefix_list[direction])
			return true;
	return false;
}

static void eigrp_filter_schedule_process(eigrp_instance_t *eigrp)
{
	if (!eigrp)
		return;
	eigrp_southbound_timer_add(&eigrp->t_distribute,
				   eigrp_distribute_timer_process, eigrp, 10);
}

static void eigrp_filter_schedule_interface(eigrp_interface_t *ei)
{
	if (!ei)
		return;
	eigrp_southbound_timer_add(&ei->t_distribute,
				   eigrp_distribute_timer_interface, ei, 10);
}

eigrp_result_t eigrp_filter_runtime_replace(
	eigrp_instance_t *eigrp, const char *interface_name,
	const eigrp_filter_runtime_snapshot_t *snapshot)
{
	eigrp_filter_runtime_state_t replacement;
	eigrp_filter_runtime_state_t *state;
	eigrp_interface_t *ei = NULL;
	eigrp_result_t result;

	if (!eigrp || !snapshot)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (interface_name && !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (interface_name) {
		ei = eigrp_intf_lookup_by_name(eigrp, interface_name);
		if (!ei)
			return EIGRP_RESULT_NOT_FOUND;
		state = &ei->filter;
	} else {
		state = &eigrp->filter;
	}

	if (eigrp_filter_runtime_state_equal(state, snapshot))
		return EIGRP_RESULT_SUCCESS;

	result = eigrp_filter_runtime_state_copy(&replacement, snapshot);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	eigrp_filter_runtime_state_clear(state);
	*state = replacement;
	if (ei)
		eigrp_filter_schedule_interface(ei);
	else
		eigrp_filter_schedule_process(eigrp);
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_filter_runtime_refresh_all(void)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	struct listnode *instance_node;
	struct listnode *interface_node;

	if (!eigrp_om || !eigrp_om->eigrp)
		return;

	for (ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp, instance_node, eigrp)) {
		if (eigrp_filter_runtime_state_active(&eigrp->filter))
			eigrp_filter_schedule_process(eigrp);
		for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, interface_node, ei))
			if (eigrp_filter_runtime_state_active(&ei->filter))
				eigrp_filter_schedule_interface(ei);
	}
}

static bool eigrp_filter_reference_denies(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, const eigrp_prefix_t *prefix)
{
	eigrp_filter_decision_t decision = EIGRP_FILTER_DECISION_PERMIT;
	eigrp_result_t result;

	if (!name)
		return false;
	result = eigrp_southbound_filter_evaluate(eigrp, type, name, prefix,
						 &decision);
	return result == EIGRP_RESULT_SUCCESS
	       && decision == EIGRP_FILTER_DECISION_DENY;
}

static bool eigrp_filter_runtime_state_denies(
	eigrp_instance_t *eigrp, const eigrp_filter_runtime_state_t *state,
	int direction, const eigrp_prefix_t *prefix)
{
	if (eigrp_filter_reference_denies(
		    eigrp, EIGRP_DISTRIBUTE_ACCESS_LIST,
		    state->access_list[direction], prefix))
		return true;
	if (eigrp_filter_reference_denies(
		    eigrp, EIGRP_DISTRIBUTE_PREFIX_LIST,
		    state->prefix_list[direction], prefix))
		return true;
	return false;
}

bool eigrp_filter_prefix_apply(eigrp_instance_t *eigrp,
			       eigrp_interface_t *ei, int direction,
			       const eigrp_prefix_t *prefix)
{
	if (!eigrp || !ei || !prefix || direction < 0
	    || direction >= EIGRP_FILTER_MAX)
		return false;

	if (eigrp_filter_runtime_state_denies(eigrp, &eigrp->filter, direction,
					       prefix))
		return true;
	return eigrp_filter_runtime_state_denies(eigrp, &ei->filter, direction,
						 prefix);
}

void eigrp_distribute_timer_process(void *arg)
{
	eigrp_instance_t *eigrp = arg;

	if (!eigrp)
		return;
	eigrp->t_distribute = NULL;
	eigrp_update_send_process_GR(eigrp, EIGRP_GR_FILTER, NULL);
}

void eigrp_distribute_timer_interface(void *arg)
{
	eigrp_interface_t *ei = arg;

	if (!ei)
		return;
	ei->t_distribute = NULL;
	eigrp_update_send_interface_GR(ei, EIGRP_GR_FILTER, NULL);
}

eigrp_result_t eigrp_offset_update(eigrp_instance_context_t *context,
				   const char *access_list,
				   eigrp_offset_direction_t direction,
				   uint32_t offset,
				   const char *interface_name)
{
	(void)offset;
	(void)interface_name;
	if (!access_list || !access_list[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (direction != EIGRP_OFFSET_IN && direction != EIGRP_OFFSET_OUT)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_offset_delete(eigrp_instance_context_t *context,
				   const char *access_list,
				   eigrp_offset_direction_t direction,
				   uint32_t offset,
				   const char *interface_name)
{
	return eigrp_offset_update(context, access_list, direction, offset,
				   interface_name);
}

struct eigrp_distribute_list_config {
	eigrp_distribute_list_type_t type;
	eigrp_offset_direction_t direction;
	char *name;
	char *interface_name;
	eigrp_distribute_list_config_t *next;
};

static char *eigrp_distribute_string_duplicate(const char *value)
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

static bool eigrp_distribute_interface_equal(const char *a, const char *b)
{
	if (!a || !b)
		return a == b;
	return strcmp(a, b) == 0;
}

static eigrp_distribute_list_config_t *eigrp_distribute_list_config_find(
	eigrp_address_family_config_t *af, eigrp_distribute_list_type_t type,
	eigrp_offset_direction_t direction, const char *interface_name)
{
	eigrp_distribute_list_config_t *config;

	if (!af)
		return NULL;
	for (config = af->distribute_lists; config; config = config->next) {
		if (config->type == type && config->direction == direction
		    && eigrp_distribute_interface_equal(config->interface_name,
							 interface_name))
			return config;
	}
	return NULL;
}

static eigrp_result_t eigrp_distribute_list_validate(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name)
{
	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (interface_name && !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (type != EIGRP_DISTRIBUTE_ACCESS_LIST
	    && type != EIGRP_DISTRIBUTE_PREFIX_LIST)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (direction != EIGRP_OFFSET_IN && direction != EIGRP_OFFSET_OUT)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_SUCCESS;
}

static bool eigrp_distribute_runtime_result_committable(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS
	       || result == EIGRP_RESULT_NOT_IMPLEMENTED;
}

static eigrp_result_t eigrp_filter_runtime_reference_update(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name, bool remove)
{
	eigrp_filter_runtime_snapshot_t snapshot;
	eigrp_filter_runtime_state_t *state;
	eigrp_interface_t *ei = NULL;
	int slot = direction == EIGRP_OFFSET_OUT ? EIGRP_FILTER_OUT
						     : EIGRP_FILTER_IN;

	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (interface_name) {
		ei = eigrp_intf_lookup_by_name(eigrp, interface_name);
		if (!ei)
			return remove ? EIGRP_RESULT_NOT_FOUND
				      : EIGRP_RESULT_SUCCESS;
		state = &ei->filter;
	} else {
		state = &eigrp->filter;
	}

	memset(&snapshot, 0, sizeof(snapshot));
	for (int i = 0; i < EIGRP_FILTER_MAX; i++) {
		snapshot.access_list[i] = state->access_list[i];
		snapshot.prefix_list[i] = state->prefix_list[i];
	}

	if (type == EIGRP_DISTRIBUTE_ACCESS_LIST)
		snapshot.access_list[slot] = remove ? NULL : name;
	else
		snapshot.prefix_list[slot] = remove ? NULL : name;

	return eigrp_filter_runtime_replace(eigrp, interface_name, &snapshot);
}

eigrp_result_t eigrp_distribute_list_update(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name)
{
	eigrp_distribute_list_config_t *config = NULL;
	eigrp_distribute_list_config_t *new_config = NULL;
	char *new_name;
	eigrp_result_t result;

	result = eigrp_distribute_list_validate(context, type, name, direction,
						 interface_name);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	new_name = eigrp_distribute_string_duplicate(name);
	if (!new_name)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	if (context->config) {
		config = eigrp_distribute_list_config_find(
			context->config, type, direction, interface_name);
		if (!config) {
			new_config = calloc(1, sizeof(*new_config));
			if (!new_config) {
				free(new_name);
				return EIGRP_RESULT_INTERNAL_FAILURE;
			}
			new_config->type = type;
			new_config->direction = direction;
			new_config->name = new_name;
			new_name = NULL;
			if (interface_name) {
				new_config->interface_name =
					eigrp_distribute_string_duplicate(interface_name);
				if (!new_config->interface_name) {
					free(new_config->name);
					free(new_config);
					return EIGRP_RESULT_INTERNAL_FAILURE;
				}
			}
		}
	}

	result = EIGRP_RESULT_SUCCESS;
	if (context->runtime)
		result = eigrp_filter_runtime_reference_update(
			context->runtime, type, name, direction, interface_name, false);
	if (!eigrp_distribute_runtime_result_committable(result)) {
		free(new_name);
		if (new_config) {
			free(new_config->interface_name);
			free(new_config->name);
			free(new_config);
		}
		return result;
	}

	if (context->config) {
		if (config) {
			free(config->name);
			config->name = new_name;
			new_name = NULL;
		} else if (new_config) {
			new_config->next = context->config->distribute_lists;
			context->config->distribute_lists = new_config;
			new_config = NULL;
		}
	}

	free(new_name);
	return result;
}

eigrp_result_t eigrp_distribute_list_delete(
	eigrp_instance_context_t *context, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name)
{
	eigrp_distribute_list_config_t **cursor = NULL;
	eigrp_distribute_list_config_t *config = NULL;
	eigrp_result_t result;

	result = eigrp_distribute_list_validate(context, type, name, direction,
						 interface_name);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	if (context->config) {
		for (cursor = &context->config->distribute_lists; *cursor;
		     cursor = &(*cursor)->next) {
			if ((*cursor)->type != type
			    || (*cursor)->direction != direction
			    || strcmp((*cursor)->name, name) != 0
			    || !eigrp_distribute_interface_equal(
				    (*cursor)->interface_name, interface_name))
				continue;
			config = *cursor;
			break;
		}
		/* The retained named filter entry is the ownership marker for this
		 * runtime reference.  Do not clear a filter installed by classic
		 * configuration when no named entry exists.
		 */
		if (!config)
			return EIGRP_RESULT_NOT_FOUND;
	}

	result = EIGRP_RESULT_NOT_FOUND;
	if (context->runtime) {
		result = eigrp_filter_runtime_reference_update(
			context->runtime, type, name, direction, interface_name, true);
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND
		    && result != EIGRP_RESULT_NOT_IMPLEMENTED)
			return result;
	}

	if (config) {
		*cursor = config->next;
		free(config->interface_name);
		free(config->name);
		free(config);
		if (result == EIGRP_RESULT_NOT_FOUND || !context->runtime)
			result = EIGRP_RESULT_SUCCESS;
	}
	return result;
}

void eigrp_distribute_list_config_delete_all(eigrp_address_family_config_t *af)
{
	eigrp_distribute_list_config_t *config;
	eigrp_distribute_list_config_t *next;

	if (!af)
		return;
	for (config = af->distribute_lists; config; config = next) {
		next = config->next;
		free(config->interface_name);
		free(config->name);
		free(config);
	}
	af->distribute_lists = NULL;
}
