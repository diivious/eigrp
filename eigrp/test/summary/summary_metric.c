// SPDX-License-Identifier: GPL-2.0-or-later
/* Copyright (C) 2026 Donnie V. Savage
 * Real eigrp_summary.c implementation with isolated topology/config adapters.
 */
#include <assert.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "../../code/eigrp_summary.c"
#include "eigrp_neighbor.h"

static eigrp_af_config_t af;
static bool leak_permit;

/* Isolate host configuration and topology storage; exercise the real summary
 * selection, metric, provenance, and query-suppression implementations. */
eigrp_af_config_t *eigrp_af_config_runtime_read(eigrp_instance_t *eigrp)
{
	return af.runtime == eigrp ? &af : NULL;
}

eigrp_intf_config_t *eigrp_intf_config_read(eigrp_af_config_t *af_config,
					   const char *name)
{
	eigrp_intf_config_t *item;

	for (item = af_config->interfaces; item; item = item->next)
		if (strcmp(item->interface_name, name) == 0)
			return item;
	return NULL;
}

eigrp_route_descriptor_t *eigrp_topology_route_read(eigrp_prefix_descriptor_t *prefix)
{
	return (eigrp_route_descriptor_t *)prefix->internal_routes;
}

bool eigrp_prefix_address_match(const eigrp_prefix_t *prefix,
				const eigrp_address_t *address)
{
	size_t i, bytes = prefix->prefix_length / 8;
	int remaining = prefix->prefix_length % 8;

	if (prefix->address.afi != address->afi
	    || memcmp(prefix->address.bytes, address->bytes, bytes))
		return false;
	i = bytes;
	return !remaining || ((prefix->address.bytes[i] ^ address->bytes[i])
		& (0xff << (8 - remaining))) == 0;
}

eigrp_result_t eigrp_eventlog_msg_add(eigrp_instance_t *eigrp,
				     uint16_t opcode, const eigrp_prefix_t *addr,
				     eventmsg_arg_t a, eventmsg_arg_t b,
				     eventmsg_arg_t c, eventmsg_arg_t d)
{
	(void)eigrp; (void)opcode; (void)addr;
	(void)a; (void)b; (void)c; (void)d;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_sys_summary_leak_map_evaluate(eigrp_instance_t *eigrp,
					       const char *name,
					       const eigrp_prefix_t *destination,
					       eigrp_filter_decision_t *decision)
{
	(void)eigrp; (void)name; (void)destination;
	*decision = leak_permit ? EIGRP_FILTER_DECISION_PERMIT : EIGRP_FILTER_DECISION_DENY;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_prefix_t pfx(const char *ip, uint8_t length)
{
	eigrp_prefix_t p = {0};
	p.address.afi = EIGRP_AFI_IPV4;
	p.prefix_length = length;
	assert(inet_pton(AF_INET, ip, p.address.bytes) == 1);
	return p;
}

static void component(eigrp_prefix_descriptor_t *prefix,
			      eigrp_route_descriptor_t *route,
			      const char *ip, uint32_t local, uint32_t predecessor,
			      eigrp_intf_t *ingress, eigrp_nbr_t *origin,
			      uint16_t type)
{
	memset(prefix, 0, sizeof(*prefix));
	memset(route, 0, sizeof(*route));
	prefix->destination = pfx(ip, 24);
	prefix->distance = local;
	prefix->state = EIGRP_FSM_STATE_PASSIVE;
	prefix->internal_routes = (eigrp_list_t *)route;
	route->prefix = prefix;
	route->dest = prefix->destination;
	route->distance = local;
	route->reported_distance = predecessor;
	route->type = type;
	route->ei = ingress;
	route->adv_router = origin;
	route->metric.delay = predecessor;
	route->reported_metric.delay = predecessor;
	route->total_metric.delay = local;
	route->extdata.tag = 1234;
}

int main(void)
{
	eigrp_instance_t eigrp = {0};
	eigrp_intf_t out = {0}, ingress = {0};
	eigrp_nbr_t self = {0}, neighbor = {0};
	eigrp_intf_config_t ic = {0};
	eigrp_summary_config_t sc = {0};
	struct eigrp_summary_state state = {0};
	struct eigrp_summary_metric_entry override = {0};
	eigrp_table_t topology = {0};
	eigrp_table_node_t n1 = {0}, n2 = {0};
	eigrp_prefix_descriptor_t p1 = {0}, p2 = {0}, sp = {0};
	eigrp_route_descriptor_t r1 = {0}, r2 = {0}, sr = {0};
	eigrp_prefix_t specific = pfx("10.30.1.0", 24);
	uint32_t remote_metric, local_metric;
	int tests = 0;

	out.name = "eth12";
	ingress.name = "eth23";
	out.eigrp = ingress.eigrp = &eigrp;
	eigrp.af_vectors.classic_internal_tlv_type = EIGRP_TLV_IPv4_INT;
	eigrp.af_vectors.classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
	eigrp.neighbor_self = &self;
	eigrp.k_values[0] = eigrp.k_values[2] = 1;
	eigrp.topology_table = &topology;
	af.runtime = &eigrp;
	af.interfaces = &ic;
	ic.interface_name = "eth12";
	ic.summaries = &sc;
	sc.prefix = pfx("10.30.0.0", 16);
	component(&p1, &r1, "10.30.1.0", 7864320, 7208960,
		  &ingress, &neighbor, EIGRP_TLV_IPv4_INT);
	component(&p2, &r2, "10.30.2.0", 13762560, 13107200,
		  &ingress, &neighbor, EIGRP_TLV_IPv4_INT);
	topology.head = &n1; n1.info = &p1; n1.next = &n2; n2.info = &p2;

	assert(eigrp_summary_route_build(&eigrp, &out, &p1, &r1, &sp, &sr));
	assert(sr.metric.delay == r1.total_metric.delay);
	assert(sr.metric.delay != r1.metric.delay);
	assert(sr.ei == NULL && sr.adv_router == &self);
	assert(sr.type == EIGRP_TLV_IPv4_INT && sr.extdata.tag == 0);
	assert(sr.nexthop.afi == 0);
	assert(sp.distance == sr.distance && sp.fdistance == sr.distance);
	assert(sr.reported_metric.delay == sr.metric.delay);
	assert(r1.metric.delay == 7208960 && r1.ei == &ingress);
	++tests;

	/* Withdrawal must reselect another finite component, not replay stale
	 * predecessor metrics or poison a reachable aggregate. */
	p1.distance = r1.distance = EIGRP_MAX_METRIC;
	r1.metric.delay = EIGRP_MAX_METRIC;
	assert(eigrp_summary_route_build(&eigrp, &out, &p1, &r1, &sp, &sr));
	assert(sr.metric.delay == r2.total_metric.delay);
	assert(sr.distance != EIGRP_MAX_METRIC);
	++tests;

	/* External best component must be announced as an internal summary. */
	r2.type = EIGRP_TLV_IPv4_EXT;
	assert(eigrp_summary_route_build(&eigrp, &out, &p2, &r2, &sp, &sr));
	assert(sr.type == EIGRP_TLV_IPv4_INT && sr.extdata.tag == 0);
	++tests;

	/* Local connected subnet under outgoing summary is not split-horizon
	 * poisoned, even though its component interface is the outgoing one. */
	r2.ei = &out; r2.adv_router = &self;
	assert(eigrp_summary_route_build(&eigrp, &out, &p2, &r2, &sp, &sr));
	assert(sr.metric.delay != EIGRP_MAX_METRIC && sr.ei == NULL);
	++tests;

	/* Current connected DNDBs may retain infinite `distance` despite a
	 * finite FD and a live local RDB.  The aggregate must use the RDB. */
	p2.nt = EIGRP_TOPOLOGY_TYPE_CONNECTED;
	p2.fdistance = r2.distance;
	p2.distance = EIGRP_MAX_METRIC;
	assert(eigrp_summary_route_build(&eigrp, &out, &p2, &r2, &sp, &sr));
	assert(sr.metric.delay == r2.total_metric.delay);
	assert(eigrp_summary_query_route_build(&eigrp, &out, &sc.prefix, &sp, &sr));
	assert(sr.metric.delay == r2.total_metric.delay);
	p2.distance = r2.distance;
	p2.nt = EIGRP_TOPOLOGY_TYPE_REMOTE;
	++tests;

	/* A route learned over that interface must not support an aggregate
	 * back to the same neighbor; choose an independent source if present. */
	r2.adv_router = &neighbor;
	assert(eigrp_summary_route_build(&eigrp, &out, &p2, &r2, &sp, &sr));
	assert(sr.metric.delay == EIGRP_MAX_METRIC);
	p1.distance = r1.distance = 15000000;
	r1.metric.delay = 14000000;
	r1.total_metric.delay = 15000000;
	assert(eigrp_summary_route_build(&eigrp, &out, &p2, &r2, &sp, &sr));
	assert(sr.metric.delay == r1.total_metric.delay);
	++tests;

	/* Metric override takes precedence only while a live component exists. */
	af.summary_state = &state;
	state.metrics = &override;
	override.prefix = sc.prefix;
	override.config.metric_configured = true;
	override.config.metric.bandwidth = 100000;
	override.config.metric.delay = 100;
	override.config.metric.reliability = 255;
	override.config.metric.load = 1;
	override.config.metric.mtu = 1500;
	assert(eigrp_summary_route_build(&eigrp, &out, &p1, &r1, &sp, &sr));
	assert(sr.metric.delay == 100);
	assert(sr.total_metric.delay == 100 && sp.reported_metric.delay == 100);
	local_metric = eigrp_metric_calculate(&eigrp, sr.metric);
	assert(sr.distance == local_metric);
	p1.distance = r1.distance = EIGRP_MAX_METRIC;
	r1.metric.delay = EIGRP_MAX_METRIC;
	assert(eigrp_summary_route_build(&eigrp, &out, &p1, &r1, &sp, &sr));
	assert(sr.metric.delay == EIGRP_MAX_METRIC);
	++tests;

	/* A QUERY for a suppressed component must receive an unreachable reply;
	 * an exact summary QUERY is answered from reachable components. */
	r2.ei = &ingress; r2.adv_router = &neighbor;
	assert(eigrp_summary_query_route_build(&eigrp, &out, &sc.prefix, &sp, &sr));
	assert(sr.metric.delay == r2.total_metric.delay || sr.metric.delay == 100);
	assert(eigrp_summary_query_route_build(&eigrp, &out, &specific, &sp, &sr));
	assert(sr.metric.delay == EIGRP_MAX_METRIC && sr.type == EIGRP_TLV_IPv4_INT);
	assert(sr.distance == EIGRP_MAX_METRIC && sr.adv_router == &self);
	assert(!eigrp_summary_query_route_build(&eigrp, &out,
						  &(eigrp_prefix_t){0}, &sp, &sr));
	++tests;

	/* Deliberate leak-map permits normal DUAL processing; deny suppresses. */
	sc.leak_map = "test-leak";
	leak_permit = true;
	assert(!eigrp_summary_query_route_build(&eigrp, &out, &specific, &sp, &sr));
	leak_permit = false;
	assert(eigrp_summary_query_route_build(&eigrp, &out, &specific, &sp, &sr));
	assert(sr.metric.delay == EIGRP_MAX_METRIC);
	++tests;

	/* Infinity with no fallback even without an explicit metric override. */
	af.summary_state = NULL;
	remote_metric = sr.distance;
	assert(remote_metric == EIGRP_MAX_METRIC);
	assert(eigrp_summary_route_build(&eigrp, &out, &p1, &r1, &sp, &sr));
	assert(sr.metric.delay == r2.total_metric.delay);
	r2.distance = p2.distance = EIGRP_MAX_METRIC;
	assert(eigrp_summary_route_build(&eigrp, &out, &p1, &r1, &sp, &sr));
	assert(sr.metric.delay == EIGRP_MAX_METRIC && sr.distance == EIGRP_MAX_METRIC);
	++tests;

	printf("PASS: %d summary implementation cases\\n", tests);
	return 0;
}
