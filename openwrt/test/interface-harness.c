// SPDX-License-Identifier: GPL-2.0-or-later
/* Linux interface inventory UUT, with synthetic event sequence and real
 * read-only rtnetlink enumeration. Does not need CAP_NET_ADMIN. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "eigrp_owrt_interface.h"

static atomic_int updated, downed, removed, addr_removed, secondary_seen;
static atomic_int walks;
static atomic_int failures;
static eigrp_afi_t current_afi = EIGRP_AFI_IPV4;

eigrp_afi_t eigrp_instance_afi(const eigrp_instance_t *instance)
{
	(void)instance;
	return current_afi;
}
void eigrp_sys_intf_update(eigrp_vrf_id_t vrf,
	const eigrp_intf_runtime_state_t *state)
{
	if (vrf != EIGRP_VRF_DEFAULT || !state->ifindex ||
	    !state->interface_name || !state->mtu || !state->bandwidth)
		atomic_fetch_add(&failures, 1);
	if (state->secondary)
		atomic_fetch_add(&secondary_seen, 1);
	atomic_fetch_add(&updated, 1);
}
void eigrp_sys_intf_down(eigrp_vrf_id_t vrf, eigrp_ifindex_t ifindex,
	const char *name, uint8_t type, uint32_t bandwidth, uint32_t mtu)
{
	(void)type; (void)bandwidth; (void)mtu;
	if (vrf != EIGRP_VRF_DEFAULT || !ifindex || !name)
		atomic_fetch_add(&failures, 1);
	atomic_fetch_add(&downed, 1);
}
void eigrp_sys_intf_remove(eigrp_vrf_id_t vrf, eigrp_ifindex_t ifindex,
	eigrp_intf_remove_reason_t reason)
{
	if (vrf != EIGRP_VRF_DEFAULT || !ifindex ||
	    reason != EIGRP_INTERFACE_REMOVE_HOST)
		atomic_fetch_add(&failures, 1);
	atomic_fetch_add(&removed, 1);
}
void eigrp_sys_intf_addr_update(eigrp_vrf_id_t vrf,
	eigrp_ifindex_t ifindex, const eigrp_prefix_t *prefix,
	eigrp_intf_remove_reason_t reason)
{
	if (vrf != EIGRP_VRF_DEFAULT || !ifindex || !prefix ||
	    reason != EIGRP_INTERFACE_REMOVE_HOST)
		atomic_fetch_add(&failures, 1);
	atomic_fetch_add(&addr_removed, 1);
}
void eigrp_owrt_runtime_enter(void) {}
void eigrp_owrt_runtime_leave(void) {}

static void seen(const eigrp_intf_runtime_state_t *state, void *arg)
{
	(void)arg;
	assert(state->ifindex > 0);
	assert(state->address.address.afi == current_afi);
	assert(state->interface_name && state->interface_name[0]);
	atomic_fetch_add(&walks, 1);
}

static eigrp_owrt_address_t ipv4(uint8_t host, bool secondary)
{
	eigrp_owrt_address_t a = {0};
	a.prefix.address.afi = EIGRP_AFI_IPV4;
	a.prefix.address.bytes[0] = 192;
	a.prefix.address.bytes[1] = 0;
	a.prefix.address.bytes[2] = 2;
	a.prefix.address.bytes[3] = host;
	a.prefix.prefix_length = 24;
	a.secondary = secondary;
	return a;
}
static eigrp_owrt_link_t link_make(const char *name,
	const eigrp_owrt_address_t *address, bool up)
{
	eigrp_owrt_link_t link = {0};
	link.ifindex = 9001;
	strncpy(link.name, name, sizeof(link.name) - 1);
	link.operative = up;
	link.multicast = true;
	link.mtu = 1500;
	link.bandwidth = 100000;
	link.addresses = (eigrp_owrt_address_t *)address;
	return link;
}
int main(void)
{
	char instance;
	eigrp_owrt_address_t first = ipv4(1, false), extra = ipv4(2, true);
	eigrp_owrt_link_t old = link_make("owrt-ut0", &first, true);
	eigrp_owrt_link_t changed, lowered, renamed;

	eigrp_owrt_interface_snapshot_emit(NULL, &old);
	assert(atomic_load(&updated) == 1);
	eigrp_owrt_interface_snapshot_emit(&old, &old);
	assert(atomic_load(&updated) == 1); /* no duplicate events */
	first.next = &extra;
	changed = link_make("owrt-ut0", &first, true);
	old.addresses = &extra; /* synthetic previous inventory contained secondary only */
	eigrp_owrt_interface_snapshot_emit(&old, &changed);
	assert(atomic_load(&updated) == 2);
	old = changed;
	lowered = link_make("owrt-ut0", &first, false);
	eigrp_owrt_interface_snapshot_emit(&old, &lowered);
	assert(atomic_load(&downed) == 1);
	assert(atomic_load(&updated) == 4); /* down updated both addresses */
	eigrp_owrt_interface_snapshot_emit(&lowered, &old);
	assert(atomic_load(&updated) == 6); /* up updated both addresses */
	/* Repeated rapid down/up transitions must never miss a state edge. */
	for (int i = 0; i < 24; ++i) {
		eigrp_owrt_interface_snapshot_emit(&old, &lowered);
		eigrp_owrt_interface_snapshot_emit(&lowered, &old);
	}
	assert(atomic_load(&downed) == 25);
	assert(atomic_load(&updated) == 6 + 24 * 4);
	first.next = NULL;
	changed = link_make("owrt-ut0", &first, true);
	old.addresses = &extra;
	eigrp_owrt_interface_snapshot_emit(&old, &changed);
	assert(atomic_load(&addr_removed) == 1);
	renamed = link_make("owrt-ut1", &first, true);
	eigrp_owrt_interface_snapshot_emit(&changed, &renamed);
	assert(atomic_load(&removed) == 1);
	eigrp_owrt_interface_snapshot_emit(&renamed, NULL);
	assert(atomic_load(&removed) == 2);
	assert(atomic_load(&failures) == 0);

	assert(eigrp_sys_interface_walk(NULL, seen, NULL) ==
	       EIGRP_RESULT_INVALID_ARGUMENT);
	assert(eigrp_sys_interface_walk((eigrp_instance_t *)&instance,
		seen, NULL) == EIGRP_RESULT_SUCCESS);
	/* Read-only inventory includes lo on a conventional Linux environment. */
	assert(atomic_load(&walks) > 0);
	current_afi = EIGRP_AFI_IPV6;
	assert(eigrp_sys_interface_walk((eigrp_instance_t *)&instance,
		seen, NULL) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_owrt_interface_start());
	assert(eigrp_owrt_interface_start());
	assert(eigrp_owrt_interface_refresh());
	eigrp_owrt_interface_stop();
	eigrp_owrt_interface_stop();
	assert(eigrp_owrt_interface_start());
	eigrp_owrt_interface_stop();
	puts("OpenWrt Linux interface netlink, diff and lifecycle smoke: PASS");
	return 0;
}
