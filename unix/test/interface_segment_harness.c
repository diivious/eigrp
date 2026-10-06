// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 Donnie V. Savage */
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "eigrp_unix_interface.h"
#include "eigrp_unix_rib.h"
#include "eigrp_unix_segment.h"

static unsigned state_updates;
static unsigned link_downs;
static unsigned link_removes;
static unsigned address_removes;
static eigrp_intf_runtime_state_t last_state;

void eigrp_sys_intf_update(eigrp_vrf_id_t vrf_id,
	const eigrp_intf_runtime_state_t *state)
{
	assert(vrf_id == EIGRP_VRF_DEFAULT);
	last_state = *state;
	state_updates++;
}

void eigrp_sys_intf_down(eigrp_vrf_id_t vrf_id,
	eigrp_ifindex_t ifindex, const char *name, uint8_t type,
	uint32_t bandwidth, uint32_t mtu)
{
	(void)type; (void)bandwidth; (void)mtu;
	assert(vrf_id == EIGRP_VRF_DEFAULT);
	assert(ifindex != 0);
	assert(name != NULL);
	link_downs++;
}

void eigrp_sys_intf_remove(eigrp_vrf_id_t vrf_id,
	eigrp_ifindex_t ifindex, eigrp_intf_remove_reason_t reason)
{
	assert(vrf_id == EIGRP_VRF_DEFAULT);
	assert(ifindex != 0);
	assert(reason == EIGRP_INTERFACE_REMOVE_HOST);
	link_removes++;
}

void eigrp_sys_intf_addr_update(eigrp_vrf_id_t vrf_id,
	eigrp_ifindex_t ifindex, const eigrp_prefix_t *address,
	eigrp_intf_remove_reason_t reason)
{
	assert(vrf_id == EIGRP_VRF_DEFAULT);
	assert(ifindex != 0);
	assert(address != NULL);
	assert(reason == EIGRP_INTERFACE_REMOVE_HOST);
	address_removes++;
}

/* eigrp_sys_interface_walk filters by the runtime AF through this public
 * accessor.  The harness uses opaque sentinel identities for each AF. */
eigrp_afi_t eigrp_instance_afi(const eigrp_instance_t *eigrp)
{
	return eigrp == (const eigrp_instance_t *)1 ? EIGRP_AFI_IPV4
		: EIGRP_AFI_IPV6;
}

eigrp_result_t eigrp_rib_redist_add(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *route)
{
	(void)eigrp;
	(void)route;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_redist_del(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *route)
{
	(void)eigrp;
	(void)route;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_prefix_t prefix(const char *text, unsigned length)
{
	eigrp_prefix_t value = {0};
	int af = strchr(text, ':') ? AF_INET6 : AF_INET;

	value.address.afi = af == AF_INET ? EIGRP_AFI_IPV4 : EIGRP_AFI_IPV6;
	value.prefix_length = (uint8_t)length;
	assert(inet_pton(af, text, value.address.bytes) == 1);
	return value;
}

static void count_state(const eigrp_intf_runtime_state_t *state, void *arg)
{
	unsigned *count = arg;
	assert(state->interface_name != NULL);
	(*count)++;
}

static void count_endpoint(const char *uut_name, const char *interface_name,
	const eigrp_unix_interface_t *interface, void *arg)
{
	unsigned *count = arg;
	assert(uut_name && uut_name[0]);
	assert(interface_name && interface_name[0]);
	assert(interface != NULL);
	(*count)++;
}

int main(void)
{
	eigrp_unix_interface_t *r1e0, *r1e1, *r2e0, *r3e0;
	eigrp_unix_segment_t *lan12;
	eigrp_prefix_t v4a = prefix("10.12.0.1", 24);
	eigrp_prefix_t v4b = prefix("10.12.0.101", 24);
	eigrp_prefix_t v6a = prefix("2001:db8:12::1", 64);
	unsigned count = 0;

	assert(eigrp_unix_interface_create("eth0", 1, true, 1000000, 1500, &r1e0)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_interface_create("eth1", 2, true, 1000000, 1500, &r1e1)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_interface_create("r2eth0", 3, true, 1000000, 1500, &r2e0)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_interface_create("r3eth0", 4, true, 1000000, 1500, &r3e0)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_interface_address_add("eth0", &v4a, false)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_interface_address_add("eth0", &v4b, true)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_interface_address_add("eth0", &v6a, false)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_interface_address_count(r1e0) == 3);
	assert(state_updates == 3);
	assert(last_state.address.address.afi == EIGRP_AFI_IPV6);

	assert(eigrp_unix_interface_up("eth0") == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_rib_source_count() == 2);
	assert(eigrp_unix_interface_is_up(r1e0));
	assert(state_updates == 6);
	assert(eigrp_unix_interface_down("eth0") == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_rib_source_count() == 0);
	assert(link_downs == 1);
	assert(eigrp_unix_interface_up("eth0") == EIGRP_RESULT_SUCCESS);

	assert(eigrp_sys_interface_walk((eigrp_instance_t *)1, count_state, &count)
	       == EIGRP_RESULT_SUCCESS);
	assert(count == 2); /* both IPv4 addresses */
	count = 0;
	assert(eigrp_sys_interface_walk((eigrp_instance_t *)2, count_state, &count)
	       == EIGRP_RESULT_SUCCESS);
	assert(count == 1); /* IPv6 address on the same interface */

	assert(eigrp_unix_segment_create("lan12", &lan12) == EIGRP_RESULT_SUCCESS);
	/* Topology membership is keyed by UUT + interface name, so a shared
	 * medium naturally represents R1:eth0, R2:eth0, R3:eth0. */
	assert(eigrp_unix_segment_endpoint_attach(lan12, "R1", "eth0")
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_segment_endpoint_attach(lan12, "R2", "eth0")
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_segment_endpoint_attach(lan12, "R3", "eth0")
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_segment_endpoint_count(lan12) == 3);
	assert(eigrp_unix_segment_delete("lan12") == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_segment_create("lan12", &lan12) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_segment_interface_attach(lan12, "R1", r1e0)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_segment_interface_attach(lan12, "R2", r2e0)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_segment_interface_attach(lan12, "R3", r3e0)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_segment_endpoint_count(lan12) == 3);
	count = 0;
	eigrp_unix_segment_endpoint_walk(lan12, count_endpoint, &count);
	assert(count == 3);

	assert(eigrp_unix_interface_address_remove("eth0", &v4b)
	       == EIGRP_RESULT_SUCCESS);
	assert(address_removes == 1);
	assert(eigrp_unix_interface_address_count(r1e0) == 2);
	assert(eigrp_unix_interface_delete("eth0") == EIGRP_RESULT_SUCCESS);
	assert(link_removes == 1);
	assert(eigrp_unix_segment_endpoint_count(lan12) == 2);

	(void)r1e1;
	eigrp_unix_segment_model_reset();
	eigrp_unix_interface_model_reset();
	eigrp_rib_finish();
	puts("unix interface/segment tests: PASS");
	return 0;
}
