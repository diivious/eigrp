// SPDX-License-Identifier: GPL-2.0-or-later
/* OW-07 native unit tests. Uses real read-only netlink enumeration and
 * synthetic routes for deterministic multicast/subscription lifecycle. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eigrp_owrt_rib.h"

static atomic_int adds, deletes, replays;
static pthread_mutex_t protocol = PTHREAD_MUTEX_INITIALIZER;
static eigrp_afi_t afi = EIGRP_AFI_IPV4;

eigrp_afi_t eigrp_instance_afi(const eigrp_instance_t *instance)
{ (void)instance; return afi; }
eigrp_vrf_id_t eigrp_instance_vrf_id(const eigrp_instance_t *instance)
{ (void)instance; return EIGRP_VRF_DEFAULT; }
void eigrp_owrt_runtime_enter(void) { pthread_mutex_lock(&protocol); }
void eigrp_owrt_runtime_leave(void) { pthread_mutex_unlock(&protocol); }
eigrp_result_t eigrp_rib_redist_add(eigrp_instance_t *instance,
	const eigrp_rib_route_t *route)
{
	assert(instance && route && route->nexthops && route->nexthop_count);
	assert(route->redist.source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_STATIC);
	atomic_fetch_add(&adds, 1);
	return EIGRP_RESULT_SUCCESS;
}
eigrp_result_t eigrp_rib_redist_del(eigrp_instance_t *instance,
	const eigrp_rib_route_t *route)
{
	assert(instance && route);
	atomic_fetch_add(&deletes, 1);
	return EIGRP_RESULT_SUCCESS;
}
eigrp_result_t eigrp_rib_routes_replay(void)
{
	atomic_fetch_add(&replays, 1);
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_rib_route_t make_route(eigrp_rib_nexthop_t *hop)
{
	eigrp_rib_route_t route = {0};
	route.prefix.address.afi = EIGRP_AFI_IPV4;
	route.prefix.address.bytes[0] = 198;
	route.prefix.address.bytes[1] = 51;
	route.prefix.address.bytes[2] = 100;
	route.prefix.prefix_length = 24;
	route.metric = 123;
	route.nexthops = hop;
	route.nexthop_count = 1;
	route.redist.source.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_STATIC;
	return route;
}
int main(int argc, char **argv)
{
	char opaque_instance = 0;
	bool live = argc == 2 && strcmp(argv[1], "--kernel") == 0;
	eigrp_instance_t *instance = (void *)&opaque_instance;
	eigrp_rib_nexthop_t hop = {.ifindex = 2, .weight = 1};
	eigrp_rib_route_t route = make_route(&hop), parsed;
	eigrp_rib_nexthop_t out[32];
	char msg[8192];
	struct nlmsghdr *nl = (void *)msg;
	struct rtmsg *rtm;
	int n;
	eigrp_redist_source_t static_source = {
		.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_STATIC, .route_instance = 0,
	};
	pthread_mutexattr_t attr;
	assert(argc == 1 || live);
	assert(pthread_mutexattr_init(&attr) == 0);
	assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) == 0);
	pthread_mutex_destroy(&protocol);
	assert(pthread_mutex_init(&protocol, &attr) == 0);
	pthread_mutexattr_destroy(&attr);

	/* Kernel representation and default-VRF protocol ownership. */
	n = eigrp_owrt_rib_message_build(true, false, &route, msg, sizeof(msg));
	assert(n > 0 && nl->nlmsg_type == RTM_NEWROUTE &&
	       (nl->nlmsg_flags & NLM_F_EXCL));
	rtm = NLMSG_DATA(nl);
	assert(rtm->rtm_protocol == RTPROT_EIGRP);
	assert(rtm->rtm_scope == RT_SCOPE_LINK);
	assert(eigrp_owrt_rib_message_decode(nl, &parsed, out, 32) == 0);
	rtm->rtm_protocol = RTPROT_STATIC;
	assert(eigrp_owrt_rib_message_decode(nl, &parsed, out, 32) == 1);
	assert(parsed.metric == 123 && parsed.nexthop_count == 1 &&
	       parsed.nexthops[0].ifindex == 2);
	assert(parsed.redist.source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_STATIC);
	rtm->rtm_protocol = RTPROT_KERNEL;
	assert(eigrp_owrt_rib_message_decode(nl, &parsed, out, 32) == 1);
	assert(parsed.redist.source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED);
	rtm->rtm_protocol = RTPROT_BGP;
	assert(eigrp_owrt_rib_message_decode(nl, &parsed, out, 32) == 1);
	assert(parsed.redist.source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_BGP);
	rtm->rtm_table = RT_TABLE_LOCAL;
	assert(eigrp_owrt_rib_message_decode(nl, &parsed, out, 32) == 0);
	assert(eigrp_owrt_rib_message_build(false, false, &route,
	       msg, sizeof(msg)) > 0 && nl->nlmsg_type == RTM_DELROUTE);
	assert(eigrp_owrt_rib_message_build(true, true, &route,
	       msg, sizeof(msg)) > 0 && (nl->nlmsg_flags & NLM_F_REPLACE));

	/* IPv6 ECMP round-trip and input validation. */
	{
		eigrp_rib_nexthop_t both[2] = {{.ifindex = 2,.weight = 1},
			{.ifindex = 3,.weight = 3,.gateway_present = true,
			 .gateway = {.afi = EIGRP_AFI_IPV6, .bytes = {0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,2}}}};
		eigrp_rib_route_t v6 = route;
		v6.prefix.address.afi = EIGRP_AFI_IPV6;
		v6.prefix.address.bytes[0] = 0x20;
		v6.prefix.address.bytes[1] = 0x01;
		v6.prefix.prefix_length = 64;
		v6.nexthops = both;
		v6.nexthop_count = 2;
		assert(eigrp_owrt_rib_message_build(true, false, &v6,
			msg, sizeof(msg)) > 0);
		rtm = NLMSG_DATA(nl);
		rtm->rtm_protocol = RTPROT_STATIC;
		assert(rtm->rtm_scope == RT_SCOPE_UNIVERSE);
		assert(eigrp_owrt_rib_message_decode(nl, &parsed, out, 32) == 1);
		assert(parsed.nexthop_count == 2 && parsed.nexthops[1].ifindex == 3 &&
			parsed.nexthops[1].weight == 3 && parsed.nexthops[1].gateway_present);
	}
	{
		eigrp_redist_source_t unsupported = static_source;
		unsupported.route_instance = 4453;
		assert(eigrp_rib_redistribute_add(instance, &unsupported) ==
			EIGRP_RESULT_UNSUPPORTED);
		unsupported.route_instance = 0;
		unsupported.protocol = EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
		assert(eigrp_rib_redistribute_add(instance, &unsupported) ==
			EIGRP_RESULT_UNSUPPORTED);
	}
	assert(eigrp_rib_redistribute_add(instance, &static_source) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_owrt_rib_snapshot_reconcile(&route, 1));
	assert(atomic_load(&adds) == 1 && atomic_load(&deletes) == 0);
	assert(eigrp_owrt_rib_snapshot_reconcile(&route, 1));
	assert(atomic_load(&adds) == 1);
	/* Changed next-hop must withdraw then re-advertise, not duplicate. */
	hop.ifindex = 3;
	assert(eigrp_owrt_rib_snapshot_reconcile(&route, 1));
	assert(atomic_load(&adds) == 2 && atomic_load(&deletes) == 1);
	assert(eigrp_owrt_rib_snapshot_reconcile(NULL, 0));
	assert(atomic_load(&deletes) == 2);
	assert(eigrp_rib_redistribute_remove(instance, &static_source) == EIGRP_RESULT_SUCCESS);
	assert(eigrp_rib_redistribute_remove(instance, &static_source) == EIGRP_RESULT_NOT_FOUND);

	/* Unprivileged Linux rtnetlink route dumps and subscription lifecycle. */
	assert(eigrp_owrt_rib_refresh());
	assert(eigrp_owrt_rib_start());
	assert(eigrp_owrt_rib_start());
	eigrp_owrt_rib_stop();
	eigrp_owrt_rib_stop();
	assert(eigrp_owrt_rib_start());
	eigrp_owrt_rib_stop();
	if (live) {
		eigrp_rib_nexthop_t kernel_hop = { .ifindex = if_nametoindex("rib0") };
		eigrp_rib_route_t kernel_route = make_route(&kernel_hop);
		assert(kernel_hop.ifindex != 0);
		kernel_route.prefix.address.bytes[0] = 203;
		kernel_route.prefix.address.bytes[1] = 0;
		kernel_route.prefix.address.bytes[2] = 113;
		kernel_route.metric = 789;
		assert(eigrp_rib_route_add(instance, &kernel_route) == EIGRP_RESULT_SUCCESS);
		assert(eigrp_rib_route_add(instance, &kernel_route) == EIGRP_RESULT_SUCCESS);
		kernel_route.metric = 790; /* kernel key change and old-key withdrawal */
		assert(eigrp_rib_route_add(instance, &kernel_route) == EIGRP_RESULT_SUCCESS);
		/* Simulate an externally deleted EIGRP route while monitoring was
		 * unavailable. GETROUTE resynchronization must invalidate the cache. */
		assert(system("ip -4 route del 203.0.113.0/24 metric 790") == 0);
		assert(eigrp_owrt_rib_refresh());
		assert(atomic_load(&replays) > 0);
		assert(eigrp_rib_route_add(instance, &kernel_route) == EIGRP_RESULT_SUCCESS);
		assert(eigrp_rib_route_del(instance, &kernel_route.prefix) == EIGRP_RESULT_SUCCESS);
		assert(eigrp_rib_route_del(instance, &kernel_route.prefix) == EIGRP_RESULT_NOT_FOUND);
		puts("OpenWrt isolated kernel route install/replace/withdraw: PASS");
	}
	assert(eigrp_rib_route_del(instance, &route.prefix) == EIGRP_RESULT_NOT_FOUND);
	eigrp_rib_instance_delete(instance);
	eigrp_rib_finish();
	puts("OpenWrt kernel RIB netlink, ECMP and subscription UUT: PASS");
	return 0;
}
