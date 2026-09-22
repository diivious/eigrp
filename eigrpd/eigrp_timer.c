// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP timer configuration and state targets.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>


#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrp_timer.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"

struct eigrp_timer_config {
	bool active_time_configured;
	uint16_t active_time_seconds;
};

static void eigrp_timer_neighbor_address(const eigrp_neighbor_t *nbr,
					 eigrp_address_t *address)
{
	memset(address, 0, sizeof(*address));
	if (nbr->src.afi == AF_INET6) {
		address->afi = EIGRP_ADDRESS_FAMILY_IPV6;
		memcpy(address->bytes, &nbr->src.ip.v6, 16);
		return;
	}
	address->afi = EIGRP_ADDRESS_FAMILY_IPV4;
	memcpy(address->bytes, &nbr->src.ip.v4, 4);
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
 * The FRR reference callback did not implement live ACTIVE-time behavior, so this target retains configuration and returns NOT_IMPLEMENTED when runtime enforcement is requested.
 */
eigrp_result_t eigrp_timer_active_time_set(eigrp_instance_context_t *context,
					      uint16_t seconds)
{
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
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
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
 * The FRR reference callback did not implement live ACTIVE-time behavior, so this target retains configuration and returns NOT_IMPLEMENTED when runtime enforcement is requested.
 */
eigrp_result_t eigrp_timer_active_time_reset(eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->timer_config) {
		context->config->timer_config->active_time_configured = false;
		context->config->timer_config->active_time_seconds = 0;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

void eigrp_timer_config_delete_all(eigrp_address_family_config_t *af)
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
eigrp_result_t eigrp_timer_show(const eigrp_instance_context_t *context,
				eigrp_timer_state_cb callback, void *arg)
{
	eigrp_interface_t *interface;
	eigrp_neighbor_t *neighbor;
	eigrp_list_node_t *interface_node;
	eigrp_list_node_t *neighbor_node;
	eigrp_result_t result;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (!callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context->runtime)
		return EIGRP_RESULT_SUCCESS;
	if (!context->runtime->data_path_ready)
		return EIGRP_RESULT_NOT_IMPLEMENTED;

	for (EIGRP_LIST_ELEMENTS_RO(context->runtime->eiflist, interface_node,
				  interface)) {
		if (interface->t_hello) {
			eigrp_timer_state_t state = {
				.type = EIGRP_TIMER_STATE_HELLO,
				.interface_name = eigrp_intf_name_string(interface),
				.expiration_seconds =
					eigrp_sys_timer_remaining_seconds(
						interface->t_hello),
			};

			result = callback(&state, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}

		for (EIGRP_LIST_ELEMENTS_RO(interface->nbrs, neighbor_node, neighbor)) {
			eigrp_timer_state_t state = {0};

			if (neighbor->state == EIGRP_NEIGHBOR_DOWN
			    || !neighbor->t_holddown)
				continue;
			state.type = EIGRP_TIMER_STATE_PEER_HOLD;
			state.interface_name = eigrp_intf_name_string(interface);
			state.neighbor_present = true;
			eigrp_timer_neighbor_address(neighbor, &state.neighbor_address);
			state.expiration_seconds =
				eigrp_sys_timer_remaining_seconds(
					neighbor->t_holddown);
			result = callback(&state, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
	}

	return EIGRP_RESULT_SUCCESS;
}
