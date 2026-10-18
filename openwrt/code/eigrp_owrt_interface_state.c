// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Diff Linux interface snapshots into EIGRP-owned lifecycle notifications.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <string.h>
#include <stdlib.h>

#include "eigrp_owrt_interface.h"

void eigrp_owrt_interface_snapshot_free(eigrp_owrt_link_t *links)
{
	while (links) {
		eigrp_owrt_link_t *next = links->next;
		eigrp_owrt_address_t *address = links->addresses;
		while (address) {
			eigrp_owrt_address_t *following = address->next;
			free(address);
			address = following;
		}
		free(links);
		links = next;
	}
}

static const eigrp_owrt_link_t *eigrp_owrt_link_lookup(
	const eigrp_owrt_link_t *links, eigrp_ifindex_t ifindex)
{
	for (; links; links = links->next)
		if (links->ifindex == ifindex)
			return links;
	return NULL;
}

static bool eigrp_owrt_address_equal(const eigrp_prefix_t *a,
	const eigrp_prefix_t *b)
{
	return a->address.afi == b->address.afi &&
		a->prefix_length == b->prefix_length &&
		memcmp(a->address.bytes, b->address.bytes,
			a->address.afi == EIGRP_AFI_IPV4 ? 4 : 16) == 0;
}

static const eigrp_owrt_address_t *eigrp_owrt_address_lookup(
	const eigrp_owrt_address_t *addresses, const eigrp_prefix_t *prefix)
{
	for (; addresses; addresses = addresses->next)
		if (eigrp_owrt_address_equal(&addresses->prefix, prefix))
			return addresses;
	return NULL;
}

static void eigrp_owrt_address_emit(const eigrp_owrt_link_t *link,
	const eigrp_owrt_address_t *address)
{
	eigrp_intf_runtime_state_t state = {
		.interface_name = link->name,
		.ifindex = link->ifindex,
		.address = address->prefix,
		.type = 0,
		.secondary = address->secondary,
		.operative = link->operative && link->multicast,
		.bandwidth = link->bandwidth,
		.mtu = link->mtu,
	};
	eigrp_sys_intf_update(EIGRP_VRF_DEFAULT, &state);
}

void eigrp_owrt_interface_snapshot_emit(const eigrp_owrt_link_t *before,
	const eigrp_owrt_link_t *after)
{
	const eigrp_owrt_link_t *old, *current;
	const eigrp_owrt_address_t *addr, *previous;

	/* First invalidate removed links and removed/changed addresses. */
	for (old = before; old; old = old->next) {
		current = eigrp_owrt_link_lookup(after, old->ifindex);
		if (!current) {
			eigrp_sys_intf_remove(EIGRP_VRF_DEFAULT, old->ifindex,
				EIGRP_INTERFACE_REMOVE_HOST);
			continue;
		}
		if (strcmp(old->name, current->name) != 0) {
			/* Same ifindex with a new name is a new EIGRP interface. */
			eigrp_sys_intf_remove(EIGRP_VRF_DEFAULT, old->ifindex,
				EIGRP_INTERFACE_REMOVE_HOST);
			continue;
		}
		if (old->operative && old->multicast &&
		    !(current->operative && current->multicast))
			eigrp_sys_intf_down(EIGRP_VRF_DEFAULT, old->ifindex,
				old->name, 0, old->bandwidth, old->mtu);
		for (addr = old->addresses; addr; addr = addr->next) {
			previous = eigrp_owrt_address_lookup(current->addresses,
				&addr->prefix);
			if (!previous)
				eigrp_sys_intf_addr_update(EIGRP_VRF_DEFAULT,
					old->ifindex, &addr->prefix,
					EIGRP_INTERFACE_REMOVE_HOST);
		}
	}
	/* Then emit additions and changed attributes, once per address. */
	for (current = after; current; current = current->next) {
		old = eigrp_owrt_link_lookup(before, current->ifindex);
		if (old && strcmp(old->name, current->name) != 0)
			old = NULL;
		for (addr = current->addresses; addr; addr = addr->next) {
			previous = old ? eigrp_owrt_address_lookup(old->addresses,
				&addr->prefix) : NULL;
			if (!previous || previous->secondary != addr->secondary ||
			    old->operative != current->operative ||
			    old->multicast != current->multicast ||
			    old->bandwidth != current->bandwidth ||
			    old->mtu != current->mtu)
				eigrp_owrt_address_emit(current, addr);
		}
	}
}
