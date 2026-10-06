// SPDX-License-Identifier: ISC
/* Copyright (C) 2026 Donnie V. Savage */
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "eigrp_unix_rib.h"

static unsigned source_adds;
static unsigned source_removes;
static eigrp_rib_route_t last_source;

eigrp_afi_t eigrp_instance_afi(const eigrp_instance_t *eigrp)
{
	return eigrp == (const eigrp_instance_t *)1 ? EIGRP_AFI_IPV4
		: EIGRP_AFI_IPV6;
}

eigrp_result_t eigrp_rib_redist_add(eigrp_instance_t *eigrp,
	const eigrp_rib_route_t *route)
{
	assert(eigrp != NULL);
	last_source = *route;
	source_adds++;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_redist_del(eigrp_instance_t *eigrp,
	const eigrp_rib_route_t *route)
{
	assert(eigrp != NULL);
	last_source = *route;
	source_removes++;
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

static eigrp_address_t address(const char *text)
{
	eigrp_address_t value = {0};
	int af = strchr(text, ':') ? AF_INET6 : AF_INET;

	value.afi = af == AF_INET ? EIGRP_AFI_IPV4 : EIGRP_AFI_IPV6;
	assert(inet_pton(af, text, value.bytes) == 1);
	return value;
}

static void learned_check(eigrp_instance_t *eigrp,
	const eigrp_rib_route_t *route, void *arg)
{
	unsigned *seen = arg;

	assert(eigrp == (eigrp_instance_t *)1);
	assert(route->prefix.address.afi == EIGRP_AFI_IPV4);
	assert(route->metric == 222);
	assert(route->nexthop_count == 1);
	assert(route->nexthops[0].ifindex == 9);
	(*seen)++;
}

int main(void)
{
	eigrp_rib_nexthop_t nh = {0};
	eigrp_rib_route_t learned = {0};
	eigrp_rib_route_t source = {0};
	eigrp_rib_nexthop_t source_nh = {0};
	eigrp_redist_source_t connected = {
		.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED,
	};
	unsigned seen = 0;

	nh.ifindex = 9;
	nh.gateway_present = true;
	nh.gateway = address("10.0.0.2");
	learned.prefix = prefix("10.2.0.0", 24);
	learned.nexthops = &nh;
	learned.nexthop_count = 1;
	learned.metric = 111;
	learned.install.admin_dist = 90;
	learned.install.type = EIGRP_RIB_ROUTE_INTERNAL;
	assert(eigrp_rib_route_add((eigrp_instance_t *)1, &learned)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_rib_route_count() == 1);

	/* ADD/install is replace semantics for an existing instance+prefix. */
	learned.metric = 222;
	assert(eigrp_rib_route_add((eigrp_instance_t *)1, &learned)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_rib_route_count() == 1);
	eigrp_unix_rib_route_walk(learned_check, &seen);
	assert(seen == 1);

	/* IPv6 is stored by the same AF-aware host RIB contract. */
	learned.prefix = prefix("2001:db8:2::", 64);
	nh.gateway = address("2001:db8:12::2");
	assert(eigrp_rib_route_add((eigrp_instance_t *)2, &learned)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_rib_route_count() == 2);
	assert(eigrp_rib_route_del((eigrp_instance_t *)2, &learned.prefix)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_rib_route_count() == 1);

	/* Host source routes are retained even before EIGRP subscribes. */
	source.prefix = prefix("192.0.2.0", 24);
	source_nh.ifindex = 7;
	source.nexthops = &source_nh;
	source.nexthop_count = 1;
	source.redist.source = connected;
	assert(eigrp_unix_rib_source_route_update(&source)
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_rib_source_count() == 1);
	assert(source_adds == 0);
	assert(eigrp_rib_redistribute_add((eigrp_instance_t *)1, &connected)
	       == EIGRP_RESULT_SUCCESS);
	assert(source_adds == 1);
	assert(last_source.nexthop_count == 1);
	assert(last_source.nexthops[0].ifindex == 7);

	/* Changed source snapshots use add/update semantics. */
	source.metric = 55;
	assert(eigrp_unix_rib_source_route_update(&source)
	       == EIGRP_RESULT_SUCCESS);
	assert(source_adds == 2);
	assert(last_source.metric == 55);
	assert(eigrp_unix_rib_source_route_remove(&source)
	       == EIGRP_RESULT_SUCCESS);
	assert(source_removes == 1);
	assert(eigrp_unix_rib_source_count() == 0);

	assert(eigrp_rib_route_del((eigrp_instance_t *)1,
				      &(eigrp_prefix_t){.address = {.afi = EIGRP_AFI_IPV4,
									.bytes = {10, 2, 0, 0}},
							.prefix_length = 24})
	       == EIGRP_RESULT_SUCCESS);
	assert(eigrp_unix_rib_route_count() == 0);
	eigrp_rib_finish();
	puts("unix rib tests: PASS");
	return 0;
}
