// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP timer configuration and state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>


#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_timer.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
#include "eigrp_instance.h"

struct eigrp_timer_config {
	bool active_time_configured;
	uint16_t active_time_seconds;
};

static void eigrp_timer_neighbor_address(const eigrp_nbr_t *nbr,
					 eigrp_address_t *address)
{
	memset(address, 0, sizeof(*address));
	if (nbr->src.afi == AF_INET6) {
		address->afi = EIGRP_AFI_IPV6;
		memcpy(address->bytes, &nbr->src.ip.v6, 16);
		return;
	}
	address->afi = EIGRP_AFI_IPV4;
	memcpy(address->bytes, &nbr->src.ip.v4, 4);
}

typedef struct eigrp_timer_message_args {
	eigrp_operation_t operation;
	eigrp_instance_context_t *context;
	uint16_t seconds;
} eigrp_timer_message_args_t;

static eigrp_result_t eigrp_timer_message_process(eigrp_instance_t *eigrp, void *arg);

/*
 * Syntax:
 *   Classic: `timers active-time <SECONDS|disabled>` / `no timers active-time`
 *   Named: `timers active-time <SECONDS|disabled>` / `no timers active-time`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: topology base mode
 * Description:
 * Sets or resets the ACTIVE/SIA timer configuration.
 * The retained value is consumed by portable DUAL ACTIVE/SIA timer enforcement.
 */
eigrp_result_t eigrp_timer_active_time_update(eigrp_operation_t operation, eigrp_instance_context_t *context, uint16_t seconds)
{
	eigrp_timer_message_args_t message = { operation, context, seconds };

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_timer_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->timer_config) {
		context->config->timer_config->active_time_configured = false;
		context->config->timer_config->active_time_seconds = 0;
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		if (!context->config->timer_config) {
			context->config->timer_config =
				calloc(1, sizeof(*context->config->timer_config));
			if (!context->config->timer_config)
				return EIGRP_RESULT_INTERNAL_FAILURE;
		}
		context->config->timer_config->active_time_configured = true;
		context->config->timer_config->active_time_seconds = seconds;
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `timers active-time <SECONDS|disabled>` / `no timers active-time`
 *   Named: `timers active-time <SECONDS|disabled>` / `no timers active-time`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: topology base mode
 * Description:
 * Sets or resets the ACTIVE/SIA timer configuration.
 * The retained value is consumed by portable DUAL ACTIVE/SIA timer enforcement.
 */



static eigrp_result_t eigrp_timer_message_process(eigrp_instance_t *eigrp, void *arg)
{
	eigrp_timer_message_args_t *message = arg;
	(void)eigrp;
	return eigrp_timer_active_time_update(message->operation, message->context,
		message->seconds);
}

uint16_t eigrp_timer_active_time_seconds(const eigrp_instance_t *runtime)
{
	eigrp_af_config_t *af;

	if (!runtime)
		return 180;
	af = eigrp_af_config_runtime_read((eigrp_instance_t *)runtime);
	if (!af || !af->timer_config || !af->timer_config->active_time_configured)
		return 180;
	return af->timer_config->active_time_seconds;
}

void eigrp_timer_config_delete_all(eigrp_af_config_t *af)
{
	if (!af)
		return;
	free(af->timer_config);
	af->timer_config = NULL;
}

/*
 * Syntax:
 *   EXEC: `show eigrp address-family <ipv4|ipv6> ... timers`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Exports active EIGRP timer state through portable callbacks.
 * FRR only formats the returned state.
 */
eigrp_result_t eigrp_timer_state_iterate(const eigrp_instance_context_t *context,
				eigrp_timer_state_cb callback, void *arg)
{
	eigrp_intf_t *interface;
	eigrp_nbr_t *neighbor;
	eigrp_list_item_t *interface_node;
	eigrp_list_item_t *neighbor_node;
	eigrp_result_t result;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (!callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context->runtime)
		return EIGRP_RESULT_SUCCESS;

	for (EIGRP_LIST_ITERATE_RO(context->runtime->eiflist, interface_node,
				  interface)) {
		if (interface->t_hello) {
			eigrp_timer_state_t state = {
				.type = EIGRP_TIMER_STATE_HELLO,
				.interface_name = eigrp_intf_name_string(interface),
				.expiration_seconds =
					eigrp_timer_remaining_seconds(context->runtime,
						interface->t_hello),
			};

			result = callback(&state, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}

		for (EIGRP_LIST_ITERATE_RO(interface->nbrs, neighbor_node, neighbor)) {
			eigrp_timer_state_t state = {0};

			if (neighbor->state == EIGRP_NEIGHBOR_DOWN
			    || !neighbor->t_holddown)
				continue;
			state.type = EIGRP_TIMER_STATE_PEER_HOLD;
			state.interface_name = eigrp_intf_name_string(interface);
			state.neighbor_present = true;
			eigrp_timer_neighbor_address(neighbor, &state.neighbor_address);
			state.expiration_seconds =
				eigrp_timer_remaining_seconds(context->runtime,
					neighbor->t_holddown);
			result = callback(&state, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
	}

	return EIGRP_RESULT_SUCCESS;
}


static void eigrp_timer_unlink_locked(eigrp_instance_t *eigrp,
				      eigrp_timer_event_t *event)
{
	eigrp_timer_event_t **link;

	if (!eigrp || !event)
		return;
	for (link = &eigrp->timer_head; *link; link = &(*link)->next) {
		if (*link == event) {
			*link = event->next;
			event->next = NULL;
			return;
		}
	}
}

void eigrp_timer_add(eigrp_instance_t *eigrp, eigrp_timer_event_t **owner,
		     eigrp_event_callback_t callback, void *arg,
		     uint32_t delay_msec)
{
	eigrp_timer_event_t *event;
	eigrp_timer_event_t **link;

	if (!eigrp || !owner || !callback)
		return;

	eigrp_timer_cancel(eigrp, owner);
	event = calloc(1, sizeof(*event));
	if (!event)
		return;
	event->eigrp = eigrp;
	event->owner = owner;
	event->callback = callback;
	event->arg = arg;
	event->due_msec = eigrp_sys_monotime_msec() + delay_msec;

	pthread_mutex_lock(&eigrp->work_lock);
	if (eigrp->shutdown || !eigrp->thread_running) {
		pthread_mutex_unlock(&eigrp->work_lock);
		free(event);
		return;
	}
	for (link = &eigrp->timer_head; *link; link = &(*link)->next) {
		if ((*link)->due_msec > event->due_msec)
			break;
	}
	event->next = *link;
	*link = event;
	*owner = event;
	pthread_cond_signal(&eigrp->work_cond);
	pthread_mutex_unlock(&eigrp->work_lock);
}

void eigrp_timer_cancel(eigrp_instance_t *eigrp, eigrp_timer_event_t **owner)
{
	eigrp_timer_event_t *event;

	if (!eigrp || !owner)
		return;
	pthread_mutex_lock(&eigrp->work_lock);
	event = *owner;
	if (event) {
		*owner = NULL;
		eigrp_timer_unlink_locked(eigrp, event);
	}
	pthread_cond_signal(&eigrp->work_cond);
	pthread_mutex_unlock(&eigrp->work_lock);
	free(event);
}

uint32_t eigrp_timer_remaining_seconds(eigrp_instance_t *eigrp,
				       const eigrp_timer_event_t *event)
{
	uint64_t now;
	uint64_t remaining;
	uint32_t seconds = 0;

	if (!eigrp || !event)
		return 0;
	pthread_mutex_lock(&eigrp->work_lock);
	now = eigrp_sys_monotime_msec();
	if (event->due_msec > now) {
		remaining = event->due_msec - now;
		seconds = (uint32_t)((remaining + 999U) / 1000U);
	}
	pthread_mutex_unlock(&eigrp->work_lock);
	return seconds;
}

void eigrp_timer_cancel_all(eigrp_instance_t *eigrp)
{
	eigrp_timer_event_t *event;
	eigrp_timer_event_t *next;

	if (!eigrp)
		return;
	pthread_mutex_lock(&eigrp->work_lock);
	event = eigrp->timer_head;
	eigrp->timer_head = NULL;
	while (event) {
		next = event->next;
		if (event->owner && *event->owner == event)
			*event->owner = NULL;
		free(event);
		event = next;
	}
	pthread_cond_signal(&eigrp->work_cond);
	pthread_mutex_unlock(&eigrp->work_lock);
}
