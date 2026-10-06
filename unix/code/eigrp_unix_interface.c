// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unix host interface inventory for standalone EIGRP development.
 *
 * This is host state, not EIGRP protocol state.  Mutations are normalized into
 * the public system contract so portable EIGRP owns protocol consequences.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "eigrp_unix.h"
#include "eigrp_unix_interface.h"
#include "eigrp_unix_rib.h"
#include "eigrp_unix_segment.h"
#include "eigrp_metric.h"
#include "eigrp_const.h"

typedef struct eigrp_unix_interface_address {
	eigrp_prefix_t prefix;
	bool secondary;
	struct eigrp_unix_interface_address *next;
} eigrp_unix_interface_address_t;

struct eigrp_unix_interface {
	char *name;
	eigrp_ifindex_t ifindex;
	bool operative;
	bool multicast_capable;
	uint32_t bandwidth;
	uint32_t mtu;
	eigrp_unix_interface_address_t *addresses;
	struct eigrp_unix_interface *next;
};

static eigrp_unix_interface_t *interfaces;

static bool eigrp_unix_prefix_equal(const eigrp_prefix_t *left,
	const eigrp_prefix_t *right)
{
	return left && right && left->address.afi == right->address.afi
	       && left->prefix_length == right->prefix_length
	       && memcmp(left->address.bytes, right->address.bytes,
			 sizeof(left->address.bytes)) == 0;
}

static bool eigrp_unix_prefix_valid(const eigrp_prefix_t *prefix)
{
	if (!prefix)
		return false;
	if (prefix->address.afi == EIGRP_AFI_IPV4)
		return prefix->prefix_length <= 32;
	if (prefix->address.afi == EIGRP_AFI_IPV6)
		return prefix->prefix_length <= 128;
	return false;
}

static eigrp_prefix_t eigrp_unix_connected_prefix(
	const eigrp_prefix_t *address)
{
	eigrp_prefix_t prefix = *address;
	size_t bytes = address->address.afi == EIGRP_AFI_IPV4 ? 4U : 16U;
	size_t full = address->prefix_length / 8U;
	unsigned remainder = address->prefix_length % 8U;
	size_t index;

	if (remainder && full < bytes) {
		prefix.address.bytes[full] &= (uint8_t)(0xffU << (8U - remainder));
		full++;
	}
	for (index = full; index < bytes; index++)
		prefix.address.bytes[index] = 0;
	return prefix;
}

static void eigrp_unix_connected_route(
	const eigrp_unix_interface_t *interface, const eigrp_prefix_t *address,
	eigrp_rib_route_t *route, eigrp_rib_nexthop_t *nexthop)
{
	eigrp_metric_values_t values = {0};

	memset(route, 0, sizeof(*route));
	memset(nexthop, 0, sizeof(*nexthop));
	route->prefix = eigrp_unix_connected_prefix(address);
	nexthop->ifindex = interface->ifindex;
	route->nexthops = nexthop;
	route->nexthop_count = 1;
	route->redist.source.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED;
	values.bandwidth = interface->bandwidth ? interface->bandwidth
						: EIGRP_BANDWIDTH_DEFAULT;
	values.delay = EIGRP_DELAY_DEFAULT;
	values.reliability = EIGRP_RELIABILITY_DEFAULT;
	values.load = EIGRP_LOAD_DEFAULT;
	values.mtu = interface->mtu > UINT16_MAX ? UINT16_MAX
						       : (uint16_t)interface->mtu;
	eigrp_metric_values_convert(&values, &route->redist.vecmetric);
}

static bool eigrp_unix_interface_connected_prefix_present(
	const eigrp_unix_interface_t *interface, const eigrp_prefix_t *address)
{
	const eigrp_unix_interface_address_t *candidate;
	eigrp_prefix_t prefix = eigrp_unix_connected_prefix(address);

	for (candidate = interface->addresses; candidate; candidate = candidate->next) {
		eigrp_prefix_t other = eigrp_unix_connected_prefix(&candidate->prefix);

		if (eigrp_unix_prefix_equal(&prefix, &other))
			return true;
	}
	return false;
}

static void eigrp_unix_interface_connected_update(
	const eigrp_unix_interface_t *interface, bool present)
{
	const eigrp_unix_interface_address_t *address;
	eigrp_rib_route_t route;
	eigrp_rib_nexthop_t nexthop;

	for (address = interface->addresses; address; address = address->next) {
		eigrp_unix_connected_route(interface, &address->prefix, &route, &nexthop);
		if (present)
			(void)eigrp_unix_rib_source_route_update(&route);
		else
			(void)eigrp_unix_rib_source_route_remove(&route);
	}
}

static void eigrp_unix_interface_state_fill(
	const eigrp_unix_interface_t *interface,
	const eigrp_unix_interface_address_t *address,
	eigrp_intf_runtime_state_t *state)
{
	memset(state, 0, sizeof(*state));
	state->interface_name = interface->name;
	state->ifindex = interface->ifindex;
	state->address = address->prefix;
	state->type = 0;
	state->secondary = address->secondary;
	state->operative = interface->operative;
	state->bandwidth = interface->bandwidth;
	state->mtu = interface->mtu;
}

static void eigrp_unix_interface_notify(const eigrp_unix_interface_t *interface)
{
	const eigrp_unix_interface_address_t *address;
	eigrp_intf_runtime_state_t state;

	for (address = interface->addresses; address; address = address->next) {
		eigrp_unix_interface_state_fill(interface, address, &state);
		eigrp_sys_intf_update(EIGRP_VRF_DEFAULT, &state);
	}
}

eigrp_unix_interface_t *eigrp_unix_interface_find(const char *name)
{
	eigrp_unix_interface_t *interface;

	if (!name)
		return NULL;
	for (interface = interfaces; interface; interface = interface->next)
		if (strcmp(interface->name, name) == 0)
			return interface;
	return NULL;
}

static eigrp_unix_interface_t *eigrp_unix_interface_find_ifindex(
	eigrp_ifindex_t ifindex)
{
	eigrp_unix_interface_t *interface;

	for (interface = interfaces; interface; interface = interface->next)
		if (interface->ifindex == ifindex)
			return interface;
	return NULL;
}

eigrp_result_t eigrp_unix_interface_create(
	const char *name, eigrp_ifindex_t ifindex, bool multicast_capable,
	uint32_t bandwidth, uint32_t mtu, eigrp_unix_interface_t **created)
{
	eigrp_unix_interface_t *interface;

	if (created)
		*created = NULL;
	if (!name || !name[0] || !ifindex || !mtu)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_unix_interface_find(name) || eigrp_unix_interface_find_ifindex(ifindex))
		return EIGRP_RESULT_CONFLICT;
	interface = calloc(1, sizeof(*interface));
	if (!interface)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	interface->name = strdup(name);
	if (!interface->name) {
		free(interface);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	interface->ifindex = ifindex;
	interface->multicast_capable = multicast_capable;
	interface->bandwidth = bandwidth;
	interface->mtu = mtu;
	interface->next = interfaces;
	interfaces = interface;
	if (created)
		*created = interface;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_unix_interface_delete(const char *name)
{
	eigrp_unix_interface_t **cursor;
	eigrp_unix_interface_t *interface;
	eigrp_unix_interface_address_t *address;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (cursor = &interfaces; *cursor; cursor = &(*cursor)->next) {
		if (strcmp((*cursor)->name, name) != 0)
			continue;
		interface = *cursor;
		*cursor = interface->next;
		eigrp_unix_segment_interface_detach_all(interface);
		eigrp_unix_interface_connected_update(interface, false);
		eigrp_sys_intf_remove(EIGRP_VRF_DEFAULT, interface->ifindex,
			EIGRP_INTERFACE_REMOVE_HOST);
		while ((address = interface->addresses) != NULL) {
			interface->addresses = address->next;
			free(address);
		}
		free(interface->name);
		free(interface);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

eigrp_result_t eigrp_unix_interface_up(const char *name)
{
	eigrp_unix_interface_t *interface;
	eigrp_result_t result = EIGRP_RESULT_SUCCESS;

	eigrp_unix_runtime_enter();
	interface = eigrp_unix_interface_find(name);
	if (!interface) {
		result = EIGRP_RESULT_NOT_FOUND;
		goto out;
	}
	if (interface->operative)
		goto out;
	interface->operative = true;
	eigrp_unix_interface_connected_update(interface, true);
	eigrp_unix_interface_notify(interface);

out:
	eigrp_unix_runtime_leave();
	return result;
}

eigrp_result_t eigrp_unix_interface_down(const char *name)
{
	eigrp_unix_interface_t *interface;
	eigrp_result_t result = EIGRP_RESULT_SUCCESS;

	eigrp_unix_runtime_enter();
	interface = eigrp_unix_interface_find(name);
	if (!interface) {
		result = EIGRP_RESULT_NOT_FOUND;
		goto out;
	}
	if (!interface->operative)
		goto out;
	eigrp_unix_interface_connected_update(interface, false);
	interface->operative = false;
	eigrp_sys_intf_down(EIGRP_VRF_DEFAULT, interface->ifindex,
		interface->name, 0, interface->bandwidth, interface->mtu);

out:
	eigrp_unix_runtime_leave();
	return result;
}

eigrp_result_t eigrp_unix_interface_address_add(
	const char *name, const eigrp_prefix_t *prefix, bool secondary)
{
	eigrp_unix_interface_t *interface = eigrp_unix_interface_find(name);
	eigrp_unix_interface_address_t *address;
	eigrp_intf_runtime_state_t state;

	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	if (!eigrp_unix_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (address = interface->addresses; address; address = address->next)
		if (eigrp_unix_prefix_equal(&address->prefix, prefix))
			return EIGRP_RESULT_CONFLICT;
	address = calloc(1, sizeof(*address));
	if (!address)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	address->prefix = *prefix;
	address->secondary = secondary;
	address->next = interface->addresses;
	interface->addresses = address;
	if (interface->operative) {
		eigrp_rib_route_t route;
		eigrp_rib_nexthop_t nexthop;
		eigrp_result_t result;

		eigrp_unix_connected_route(interface, &address->prefix, &route, &nexthop);
		result = eigrp_unix_rib_source_route_update(&route);

		if (result != EIGRP_RESULT_SUCCESS) {
			interface->addresses = address->next;
			free(address);
			return result;
		}
	}
	eigrp_unix_interface_state_fill(interface, address, &state);
	eigrp_sys_intf_update(EIGRP_VRF_DEFAULT, &state);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_unix_interface_address_remove(
	const char *name, const eigrp_prefix_t *prefix)
{
	eigrp_unix_interface_t *interface = eigrp_unix_interface_find(name);
	eigrp_unix_interface_address_t **cursor;
	eigrp_unix_interface_address_t *address;

	if (!interface)
		return EIGRP_RESULT_NOT_FOUND;
	if (!eigrp_unix_prefix_valid(prefix))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	for (cursor = &interface->addresses; *cursor; cursor = &(*cursor)->next) {
		if (!eigrp_unix_prefix_equal(&(*cursor)->prefix, prefix))
			continue;
		address = *cursor;
		*cursor = address->next;
		if (interface->operative
		    && !eigrp_unix_interface_connected_prefix_present(
			       interface, &address->prefix)) {
			eigrp_rib_route_t route;
			eigrp_rib_nexthop_t nexthop;

			eigrp_unix_connected_route(interface, &address->prefix, &route, &nexthop);
			(void)eigrp_unix_rib_source_route_remove(&route);
		}
		eigrp_sys_intf_addr_update(EIGRP_VRF_DEFAULT,
			interface->ifindex, &address->prefix,
			EIGRP_INTERFACE_REMOVE_HOST);
		free(address);
		/* If portable EIGRP used the removed address, another address on the
		 * same host interface is now eligible to repopulate runtime state. */
		eigrp_unix_interface_notify(interface);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

void eigrp_unix_interface_model_reset(void)
{
	while (interfaces)
		(void)eigrp_unix_interface_delete(interfaces->name);
}

const char *eigrp_unix_interface_name(const eigrp_unix_interface_t *interface)
{
	return interface ? interface->name : NULL;
}

eigrp_ifindex_t eigrp_unix_interface_ifindex(
	const eigrp_unix_interface_t *interface)
{
	return interface ? interface->ifindex : 0;
}

bool eigrp_unix_interface_is_up(const eigrp_unix_interface_t *interface)
{
	return interface && interface->operative;
}

bool eigrp_unix_interface_multicast_capable(
	const eigrp_unix_interface_t *interface)
{
	return interface && interface->multicast_capable;
}

size_t eigrp_unix_interface_address_count(
	const eigrp_unix_interface_t *interface)
{
	const eigrp_unix_interface_address_t *address;
	size_t count = 0;

	if (!interface)
		return 0;
	for (address = interface->addresses; address; address = address->next)
		count++;
	return count;
}

void eigrp_unix_interface_walk(eigrp_unix_interface_walk_cb callback, void *arg)
{
	eigrp_unix_interface_t *interface;

	if (!callback)
		return;
	for (interface = interfaces; interface; interface = interface->next)
		callback(interface, arg);
}

void eigrp_unix_interface_address_walk(
	const eigrp_unix_interface_t *interface,
	eigrp_unix_interface_address_walk_cb callback, void *arg)
{
	const eigrp_unix_interface_address_t *address;

	if (!interface || !callback)
		return;
	for (address = interface->addresses; address; address = address->next)
		callback(&address->prefix, address->secondary, arg);
}

/* Public host service: enumerate every address belonging to every interface.
 * Portable EIGRP decides which addresses participate for the runtime AF. */
eigrp_result_t eigrp_sys_interface_walk(
	eigrp_instance_t *eigrp, eigrp_sys_interface_walk_cb callback, void *arg)
{
	eigrp_unix_interface_t *interface;
	eigrp_unix_interface_address_t *address;
	eigrp_intf_runtime_state_t state;
	eigrp_afi_t afi;

	if (!eigrp || !callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	afi = eigrp_instance_afi(eigrp);
	for (interface = interfaces; interface; interface = interface->next) {
		for (address = interface->addresses; address; address = address->next) {
			if (address->prefix.address.afi != afi)
				continue;
			eigrp_unix_interface_state_fill(interface, address, &state);
			callback(&state, arg);
		}
	}
	return EIGRP_RESULT_SUCCESS;
}
