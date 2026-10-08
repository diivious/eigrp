// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zebra connect library for EIGRP.
 * Copyright (C) 2013-2014, 2026
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include <stdio.h>
#include <zebra.h>

#include "frrevent.h"
#include "command.h"
#include "network.h"
#include "prefix.h"
#include "routemap.h"
#include "table.h"
#include "stream.h"
#include "memory.h"
#include "zclient.h"
#include "filter.h"
#include "plist.h"
#include "log.h"
#include "nexthop.h"

#include "eigrp.h"
#include "eigrp_log.h"
#include "eigrp_structs.h"
#include "eigrp_debug.h"
#include "eigrp_interface.h"
#include "eigrp_instance.h"
#include "eigrp_neighbor.h"
#include "eigrp_frr_rib.h"
#include "eigrp_vty.h"
#include "eigrp_network.h"
#include "eigrp_metric.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
#include "eigrp_frr.h"

/* Zebra structure to hold current status. */
struct zclient *eigrp_zclient = NULL;

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_ZEBRA_INSTANCE,
		    "EIGRP Zebra instance state");

struct eigrp_zebra_redistribute {
	int type;
	eigrp_route_instance_t route_instance;
	struct eigrp_zebra_redistribute *next;
};

struct eigrp_zebra_instance_state {
	eigrp_instance_t *eigrp;
	eigrp_metrics_t dmetric[ZEBRA_ROUTE_MAX];
	bool classic_redistribute[ZEBRA_ROUTE_MAX];
	unsigned int redistribute_count;
	struct eigrp_zebra_redistribute *redistributions;
	struct eigrp_zebra_instance_state *next;
};

static struct eigrp_zebra_instance_state *eigrp_zebra_instances;

static struct eigrp_zebra_instance_state *eigrp_zebra_instance_state_get(
	eigrp_instance_t *eigrp, bool create)
{
	struct eigrp_zebra_instance_state *state;

	if (!eigrp)
		return NULL;
	for (state = eigrp_zebra_instances; state; state = state->next) {
		if (state->eigrp == eigrp)
			return state;
	}
	if (!create)
		return NULL;

	state = XCALLOC(MTYPE_EIGRP_ZEBRA_INSTANCE, sizeof(*state));
	state->eigrp = eigrp;
	state->next = eigrp_zebra_instances;
	eigrp_zebra_instances = state;
	return state;
}

static struct eigrp_zebra_redistribute *eigrp_zebra_redistribute_find(
	struct eigrp_zebra_instance_state *state, int type,
	eigrp_route_instance_t route_instance)
{
	struct eigrp_zebra_redistribute *redist;

	if (!state)
		return NULL;
	for (redist = state->redistributions; redist; redist = redist->next) {
		if (redist->type == type
		    && redist->route_instance == route_instance)
			return redist;
	}
	return NULL;
}

static afi_t eigrp_zebra_instance_afi(const eigrp_instance_t *eigrp)
{
	if (!eigrp)
		return AFI_UNSPEC;
	switch (eigrp_instance_afi(eigrp)) {
	case EIGRP_AFI_IPV4:
		return AFI_IP;
	case EIGRP_AFI_IPV6:
		return AFI_IP6;
	default:
		return AFI_UNSPEC;
	}
}

static bool eigrp_zebra_state_source_in_use(
	const struct eigrp_zebra_instance_state *state, afi_t afi, int type,
	eigrp_route_instance_t route_instance)
{
	const struct eigrp_zebra_redistribute *redist;

	if (!state || !state->eigrp || type <= 0 || type >= ZEBRA_ROUTE_MAX)
		return false;
	if (eigrp_zebra_instance_afi(state->eigrp) != afi)
		return false;
	if (route_instance == 0 && state->classic_redistribute[type])
		return true;
	for (redist = state->redistributions; redist; redist = redist->next) {
		if (redist->type == type
		    && redist->route_instance == route_instance)
			return true;
	}
	return false;
}

static bool eigrp_zebra_source_in_use(afi_t afi, int type,
				       eigrp_route_instance_t route_instance,
				       vrf_id_t vrf_id)
{
	const struct eigrp_zebra_instance_state *state;

	for (state = eigrp_zebra_instances; state; state = state->next) {
		if (!state->eigrp)
			continue;
		/* FRR keys instance-zero subscriptions by VRF bitmap.  Nonzero
		 * route instances are tracked by {AFI, route-type, instance} in
		 * zclient/Zebra and the route event itself carries the VRF.
		 */
		if (route_instance == 0
		    && eigrp_instance_vrf_id(state->eigrp) != vrf_id)
			continue;
		if (eigrp_zebra_state_source_in_use(state, afi, type,
					    route_instance))
			return true;
	}
	return false;
}

static void eigrp_zebra_redistribute_list_free(
	struct eigrp_zebra_instance_state *state)
{
	struct eigrp_zebra_redistribute *redist;

	if (!state)
		return;
	while ((redist = state->redistributions) != NULL) {
		state->redistributions = redist->next;
		XFREE(MTYPE_EIGRP_ZEBRA_INSTANCE, redist);
	}
}

void eigrp_zebra_instance_delete(eigrp_instance_t *eigrp)
{
	struct eigrp_zebra_instance_state **cursor;
	struct eigrp_zebra_instance_state *state;
	struct eigrp_zebra_redistribute *redist;
	afi_t afi;
	vrf_id_t vrf_id;
	int type;

	for (cursor = &eigrp_zebra_instances; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->eigrp != eigrp)
			continue;
		state = *cursor;
		afi = eigrp_zebra_instance_afi(state->eigrp);
		vrf_id = state->eigrp
			 ? (vrf_id_t)eigrp_instance_vrf_id(state->eigrp)
			 : VRF_DEFAULT;
		*cursor = state->next;

		if (eigrp_zclient && afi != AFI_UNSPEC) {
			for (type = 1; type < ZEBRA_ROUTE_MAX; type++) {
				if (!state->classic_redistribute[type])
					continue;
				if (!eigrp_zebra_source_in_use(afi, type, 0, vrf_id))
					zclient_redistribute(
						ZEBRA_REDISTRIBUTE_DELETE,
						eigrp_zclient, afi, type, 0, vrf_id);
			}
			for (redist = state->redistributions; redist;
			     redist = redist->next) {
				if (!eigrp_zebra_source_in_use(
					    afi, redist->type,
					    redist->route_instance, vrf_id))
					zclient_redistribute(
						ZEBRA_REDISTRIBUTE_DELETE,
						eigrp_zclient, afi, redist->type,
						(unsigned short)redist->route_instance,
						vrf_id);
			}
		}

		eigrp_zebra_redistribute_list_free(state);
		XFREE(MTYPE_EIGRP_ZEBRA_INSTANCE, state);
		return;
	}
}

static void eigrp_zebra_instance_delete_all(void)
{
	struct eigrp_zebra_instance_state *state;

	while ((state = eigrp_zebra_instances) != NULL) {
		eigrp_zebra_instances = state->next;
		eigrp_zebra_redistribute_list_free(state);
		XFREE(MTYPE_EIGRP_ZEBRA_INSTANCE, state);
	}
}

/* For registering events. */
extern struct event_loop *master;
struct in_addr router_id_zebra;

static const char *eigrp_zebra_prefix_string(const struct prefix *prefix)
{
	static char buffer[PREFIX_STRLEN];

	return prefix2str(prefix, buffer, sizeof(buffer));
}

/* Router-id update message from zebra. */
static int eigrp_zebra_router_id_update(ZAPI_CALLBACK_ARGS)
{
	struct prefix router_id;
	zebra_router_id_update_read(zclient->ibuf, &router_id);

	router_id_zebra = router_id.u.prefix4;
	eigrp_process_routerid_cb((eigrp_vrf_id_t)vrf_id);
	return 0;
}

static int eigrp_zebra_route_notify_owner(ZAPI_CALLBACK_ARGS)
{
	struct prefix p;
	enum zapi_route_notify_owner note;
	uint32_t table;

	if (!zapi_route_notify_decode(zclient->ibuf, &p, &table, &note, NULL,
				      NULL))
		return -1;

	return 0;
}

static eigrp_result_t eigrp_zebra_vrf_register(eigrp_vrf_id_t vrf_id,
	void *arg)
{
	struct zclient *zclient = arg;

	if (vrf_id != EIGRP_VRF_DEFAULT)
		zclient_send_reg_requests(zclient, (vrf_id_t)vrf_id);
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_zebra_connected(struct zclient *zclient)
{
	zclient_send_reg_requests(zclient, VRF_DEFAULT);
	(void)eigrp_instance_vrf_iterate(eigrp_zebra_vrf_register, zclient);
	/* Zebra removes client-owned routes when the client disconnects.
	 * Replay EIGRP's currently installed topology after reconnect without
	 * exposing DUAL descriptors to the FRR adapter.
	 */
	(void)eigrp_rib_routes_replay();
}

static eigrp_redist_protocol_t eigrp_zebra_redistribute_protocol(int type)
{
	switch (type) {
	case ZEBRA_ROUTE_CONNECT:
		return EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED;
	case ZEBRA_ROUTE_STATIC:
		return EIGRP_REDISTRIBUTE_PROTOCOL_STATIC;
	case ZEBRA_ROUTE_RIP:
		return EIGRP_REDISTRIBUTE_PROTOCOL_RIP;
	case ZEBRA_ROUTE_OSPF:
		return EIGRP_REDISTRIBUTE_PROTOCOL_OSPF;
	case ZEBRA_ROUTE_ISIS:
		return EIGRP_REDISTRIBUTE_PROTOCOL_ISIS;
	case ZEBRA_ROUTE_BGP:
		return EIGRP_REDISTRIBUTE_PROTOCOL_BGP;
	case ZEBRA_ROUTE_EIGRP:
		return EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
	default:
		return EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED;
	}
}

static const char *eigrp_zebra_redistribute_protocol_name(
	eigrp_redist_protocol_t protocol)
{
	switch (protocol) {
	case EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED:
		return "connected";
	case EIGRP_REDISTRIBUTE_PROTOCOL_STATIC:
		return "static";
	case EIGRP_REDISTRIBUTE_PROTOCOL_RIP:
		return "rip";
	case EIGRP_REDISTRIBUTE_PROTOCOL_OSPF:
		return "ospf";
	case EIGRP_REDISTRIBUTE_PROTOCOL_ISIS:
		return "isis";
	case EIGRP_REDISTRIBUTE_PROTOCOL_BGP:
		return "bgp";
	case EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP:
		return "eigrp";
	case EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED:
	default:
		return "unknown";
	}
}

static bool eigrp_zebra_redistribute_accepts(
	const struct eigrp_zebra_instance_state *state, int type,
	eigrp_route_instance_t route_instance)
{
	const struct eigrp_zebra_redistribute *redist;

	if (!state)
		return false;
	for (redist = state->redistributions; redist; redist = redist->next) {
		if (redist->type == type
		    && redist->route_instance == route_instance)
			return true;
	}
	return false;
}

static bool eigrp_zebra_redist_nexthop_import(
	const struct zapi_route *api, eigrp_rib_route_t *route,
	eigrp_rib_nexthop_t *storage)
{
	const struct zapi_nexthop *nexthop;
	int index;

	if (!api || !route || !storage)
		return false;
	for (index = 0; index < api->nexthop_num; index++) {
		nexthop = &api->nexthops[index];
		memset(storage, 0, sizeof(*storage));
		switch (nexthop->type) {
		case NEXTHOP_TYPE_IFINDEX:
			storage->ifindex = nexthop->ifindex;
			route->nexthops = storage;
			route->nexthop_count = 1;
			return true;
		case NEXTHOP_TYPE_IPV4:
		case NEXTHOP_TYPE_IPV4_IFINDEX:
			storage->ifindex = nexthop->ifindex;
			storage->gateway_present = true;
			storage->gateway.afi = EIGRP_AFI_IPV4;
			memcpy(storage->gateway.bytes, &nexthop->gate.ipv4,
			       sizeof(nexthop->gate.ipv4));
			route->nexthops = storage;
			route->nexthop_count = 1;
			return false;
		case NEXTHOP_TYPE_IPV6:
		case NEXTHOP_TYPE_IPV6_IFINDEX:
			storage->ifindex = nexthop->ifindex;
			storage->gateway_present = true;
			storage->gateway.afi = EIGRP_AFI_IPV6;
			memcpy(storage->gateway.bytes, &nexthop->gate.ipv6,
			       sizeof(nexthop->gate.ipv6));
			route->nexthops = storage;
			route->nexthop_count = 1;
			return false;
		case NEXTHOP_TYPE_BLACKHOLE:
			break;
		default:
			break;
		}
	}
	return false;
}

static void eigrp_zebra_redist_vecmetric_import(
	eigrp_rib_route_t *route, vrf_id_t vrf_id, bool interface_static)
{
	const eigrp_rib_nexthop_t *nexthop;
	eigrp_metric_values_t values = {0};
	struct interface *ifp;

	if (!route || !route->nexthops || !route->nexthop_count)
		return;
	if (route->redist.source.protocol != EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED
	    && (route->redist.source.protocol != EIGRP_REDISTRIBUTE_PROTOCOL_STATIC
		|| !interface_static))
		return;

	nexthop = &route->nexthops[0];
	if (!nexthop->ifindex)
		return;
	ifp = if_lookup_by_index(nexthop->ifindex, vrf_id);
	if (!ifp)
		return;

	/* FRR exposes interface bandwidth at the route-notification boundary.
	 * Delay has no equivalent generic RIB/interface attribute, so use the
	 * same portable default that initializes an EIGRP interface.  Any future
	 * host-specific delay source belongs in this parent-side normalization.
	 */
	values.bandwidth = ifp->bandwidth ? ifp->bandwidth
					  : EIGRP_BANDWIDTH_DEFAULT;
	values.delay = EIGRP_DELAY_DEFAULT;
	values.reliability = EIGRP_RELIABILITY_DEFAULT;
	values.load = EIGRP_LOAD_DEFAULT;
	values.mtu = ifp->mtu > UINT16_MAX ? UINT16_MAX : (uint16_t)ifp->mtu;
	eigrp_metric_values_convert(&values, &route->redist.vecmetric);
}

static eigrp_result_t eigrp_zebra_redist_route_import(
	const struct zapi_route *api, vrf_id_t vrf_id, eigrp_rib_route_t *route,
	eigrp_rib_nexthop_t *nexthop)
{
	eigrp_redist_protocol_t protocol;
	eigrp_result_t result;
	bool interface_static;

	if (!api || !route || !nexthop || api->safi != SAFI_UNICAST)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	protocol = eigrp_zebra_redistribute_protocol(api->type);
	if (protocol == EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED)
		return EIGRP_RESULT_UNSUPPORTED;

	memset(route, 0, sizeof(*route));
	result = eigrp_frr_prefix_import(&api->prefix, &route->prefix);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	route->redist.source.protocol = protocol;
	route->redist.source.route_instance = api->instance;
	route->metric = api->metric;
	route->tag = api->tag;
	interface_static = eigrp_zebra_redist_nexthop_import(api, route, nexthop);
	eigrp_zebra_redist_vecmetric_import(route, vrf_id, interface_static);
	return EIGRP_RESULT_SUCCESS;
}

/* Normalize Zebra redistribution events and feed subscribed EIGRP runtimes. */
static int eigrp_zebra_redistribute_route(ZAPI_CALLBACK_ARGS)
{
	struct eigrp_zebra_instance_state *state;
	eigrp_rib_route_t route;
	eigrp_rib_nexthop_t nexthop;
	eigrp_result_t result;
	const char *protocol_name;
	struct zapi_route api;

	if (zapi_route_decode(zclient->ibuf, &api) < 0) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra redistribute %s decode failed for VRF %u",
			cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD ? "add" : "delete",
			(unsigned)vrf_id);
		return -1;
	}

	result = eigrp_zebra_redist_route_import(&api, vrf_id, &route, &nexthop);
	if (result == EIGRP_RESULT_UNSUPPORTED)
		return 0;
	if (result != EIGRP_RESULT_SUCCESS) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra redistribute %s normalization failed for VRF %u (result %u)",
			cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD ? "add" : "delete",
			(unsigned)vrf_id, (unsigned)result);
		return 0;
	}
	protocol_name = eigrp_zebra_redistribute_protocol_name(
		route.redist.source.protocol);

	for (state = eigrp_zebra_instances; state; state = state->next) {
		if (!state->eigrp
		    || eigrp_instance_vrf_id(state->eigrp) != (eigrp_vrf_id_t)vrf_id)
			continue;
		if (eigrp_instance_afi(state->eigrp)
		    != route.prefix.address.afi)
			continue;
		/* An EIGRP route originated by this exact runtime must never be
		 * reflected back through the generic redistribution receive path.
		 */
		if (route.redist.source.protocol == EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP
		    && route.redist.source.route_instance
		       == eigrp_instance_asn(state->eigrp))
			continue;
		if (!eigrp_zebra_redistribute_accepts(
			    state, api.type, route.redist.source.route_instance))
			continue;

		/* Zebra reports a changed redistributed route as ADD with a new
		 * snapshot; DEL is the withdrawal lifecycle event.
		 */
		result = cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD
				 ? eigrp_rib_redist_add(state->eigrp, &route)
				 : eigrp_rib_redist_del(state->eigrp, &route);
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND)
			eigrp_log(EIGRP_LOG_ERROR,
				  "FRR Zebra redistribute %s %s source %s instance %u failed (result %u)",
				  cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD ? "add"
							       : "delete",
				  eigrp_zebra_prefix_string(&api.prefix),
				  protocol_name,
				  (unsigned)route.redist.source.route_instance,
				  (unsigned)result);
	}

	if ((term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_RIB))
		eigrp_log(EIGRP_LOG_DEBUG,
			"Zebra: redistribute %s %s source %s instance %u",
			cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD ? "add" : "delete",
			eigrp_zebra_prefix_string(&api.prefix), protocol_name,
			(unsigned)route.redist.source.route_instance);

	return 0;
}

static int eigrp_zebra_interface_address_add(ZAPI_CALLBACK_ARGS)
{
	eigrp_intf_runtime_state_t state;
	eigrp_result_t result;
	struct connected *c;
	struct interface *ifp;

	c = zebra_interface_address_read(cmd, zclient->ibuf, vrf_id);
	if (!c) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra interface-address add decode failed for VRF %u",
			(unsigned)vrf_id);
		return 0;
	}
	ifp = c->ifp;
	if (!ifp) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra interface-address add has no interface for VRF %u",
			(unsigned)vrf_id);
		return 0;
	}
	if (!c->address) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra interface %s[%u] address add has no address",
			ifp->name, ifp->ifindex);
		return 0;
	}

	result = eigrp_frr_interface_state_import(
		ifp, c->address, CHECK_FLAG(c->flags, ZEBRA_IFA_SECONDARY), &state);
	if (result != EIGRP_RESULT_SUCCESS) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra interface %s[%u] address add could not normalize family %u/%u (result %u)",
			ifp->name, ifp->ifindex, (unsigned)c->address->family,
			(unsigned)c->address->prefixlen, (unsigned)result);
		return 0;
	}

	if ((term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_INTERFACE))
		eigrp_log(EIGRP_LOG_DEBUG, "Zebra: interface %s address add %s", ifp->name,
				eigrp_zebra_prefix_string(c->address));

	eigrp_sys_intf_update((eigrp_vrf_id_t)vrf_id, &state);
	return 0;
}

static int eigrp_zebra_interface_address_delete(ZAPI_CALLBACK_ARGS)
{
	eigrp_prefix_t removed;
	eigrp_result_t result;
	struct connected *c;
	struct interface *ifp;

	c = zebra_interface_address_read(cmd, zclient->ibuf, vrf_id);
	if (!c) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra interface-address delete decode failed for VRF %u",
			(unsigned)vrf_id);
		return 0;
	}

	ifp = c->ifp;
	if (!ifp) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra interface-address delete has no interface for VRF %u",
			(unsigned)vrf_id);
		connected_free(&c);
		return 0;
	}
	if (!c->address) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra interface %s[%u] address delete has no address",
			ifp->name, ifp->ifindex);
		connected_free(&c);
		return 0;
	}

	result = eigrp_frr_prefix_import(c->address, &removed);
	if (result != EIGRP_RESULT_SUCCESS) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra interface %s[%u] address delete could not normalize family %u/%u (result %u)",
			ifp->name, ifp->ifindex, (unsigned)c->address->family,
			(unsigned)c->address->prefixlen, (unsigned)result);
		connected_free(&c);
		return 0;
	}

	if ((term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_INTERFACE))
		eigrp_log(EIGRP_LOG_DEBUG, "Zebra: interface %s address delete %s", ifp->name,
				eigrp_zebra_prefix_string(c->address));

	eigrp_sys_intf_addr_update((eigrp_vrf_id_t)vrf_id,
					       ifp->ifindex, &removed,
					       EIGRP_INTERFACE_REMOVE_HOST);

	connected_free(&c);
	return 0;
}

/* Convert wide EIGRP metrics to the 32-bit Zebra RIB scale.  Infinity
 * is mapped to RIB infinity before scaling; classic metrics pass through.
 */
static uint32_t eigrp_zebra_rib_metric(const eigrp_instance_t *eigrp,
                                      uint64_t metric)
{
	if (eigrp->metric_version == EIGRP_TLV_64B_VERSION) {
		if (metric >= EIGRP_64BIT_METRIC_INFINITY)
			return EIGRP_RIB_METRIC_INFINITY;
		return (uint32_t)(metric / 128U);
	}

	return (uint32_t)metric;
}

eigrp_result_t eigrp_zebra_route_add(
	eigrp_instance_t *eigrp, const eigrp_rib_route_t *route)
{
	struct zapi_route api;
	struct zapi_nexthop *api_nh;
	struct prefix host_prefix;
	size_t i;
	int count = 0;

	if (!eigrp || !route
	    || (!route->nexthops && route->nexthop_count != 0))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_zclient)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	if (eigrp_frr_prefix_export(&route->prefix, &host_prefix)
	    != EIGRP_RESULT_SUCCESS)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	zapi_route_init(&api);
	api.vrf_id = eigrp_instance_vrf_id(eigrp);
	api.type = ZEBRA_ROUTE_EIGRP;
	api.instance = eigrp_instance_asn(eigrp);
	api.safi = SAFI_UNICAST;
	api.metric = eigrp_zebra_rib_metric(eigrp, route->metric);
	api.distance = route->install.admin_dist;
	api.tag = route->tag;
	api.prefix = host_prefix;

	SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);
	SET_FLAG(api.message, ZAPI_MESSAGE_METRIC);
	SET_FLAG(api.message, ZAPI_MESSAGE_DISTANCE);
	if (route->install.type == EIGRP_RIB_ROUTE_EXTERNAL)
		SET_FLAG(api.message, ZAPI_MESSAGE_TAG);

	for (i = 0; i < route->nexthop_count && count < MULTIPATH_NUM; i++) {
		const eigrp_rib_nexthop_t *nexthop = &route->nexthops[i];

		api_nh = &api.nexthops[count];
		zapi_nexthop_init(api_nh);
		api_nh->vrf_id = eigrp_instance_vrf_id(eigrp);
		api_nh->ifindex = nexthop->ifindex;
		api_nh->weight = nexthop->weight;
		if (nexthop->gateway_present
		    && nexthop->gateway.afi == EIGRP_AFI_IPV4) {
			memcpy(&api_nh->gate.ipv4, nexthop->gateway.bytes,
			       sizeof(api_nh->gate.ipv4));
			api_nh->type = NEXTHOP_TYPE_IPV4_IFINDEX;
		} else if (nexthop->gateway_present
			   && nexthop->gateway.afi == EIGRP_AFI_IPV6) {
			memcpy(&api_nh->gate.ipv6, nexthop->gateway.bytes,
			       sizeof(api_nh->gate.ipv6));
			api_nh->type = NEXTHOP_TYPE_IPV6_IFINDEX;
		} else {
			api_nh->type = NEXTHOP_TYPE_IFINDEX;
		}
		count++;
	}
	api.nexthop_num = count;

	if ((term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_RIB)
	    || eigrp_debug_af_enabled(
		    eigrp, EIGRP_DEBUG_AF_NOTIFICATIONS, NULL))
		eigrp_log(EIGRP_LOG_DEBUG, "Zebra: Route add %s",
				eigrp_zebra_prefix_string(&host_prefix));

	zclient_route_send(ZEBRA_ROUTE_ADD, eigrp_zclient, &api);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_zebra_route_del(eigrp_instance_t *eigrp,
				       const eigrp_prefix_t *prefix)
{
	struct zapi_route api;
	struct prefix host_prefix;

	if (!eigrp || !prefix)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_zclient)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	if (eigrp_frr_prefix_export(prefix, &host_prefix)
	    != EIGRP_RESULT_SUCCESS)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	zapi_route_init(&api);
	api.vrf_id = eigrp_instance_vrf_id(eigrp);
	api.type = ZEBRA_ROUTE_EIGRP;
	api.instance = eigrp_instance_asn(eigrp);
	api.safi = SAFI_UNICAST;
	api.prefix = host_prefix;
	zclient_route_send(ZEBRA_ROUTE_DELETE, eigrp_zclient, &api);

	if ((term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_RIB)
	    || eigrp_debug_af_enabled(
		    eigrp, EIGRP_DEBUG_AF_NOTIFICATIONS, NULL))
		eigrp_log(EIGRP_LOG_DEBUG, "Zebra: Route del %s",
				eigrp_zebra_prefix_string(&host_prefix));
	return EIGRP_RESULT_SUCCESS;
}

static int eigrp_zebra_redistribute_type(eigrp_redist_protocol_t protocol)
{
	switch (protocol) {
	case EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED:
		return ZEBRA_ROUTE_CONNECT;
	case EIGRP_REDISTRIBUTE_PROTOCOL_STATIC:
		return ZEBRA_ROUTE_STATIC;
	case EIGRP_REDISTRIBUTE_PROTOCOL_RIP:
		return ZEBRA_ROUTE_RIP;
	case EIGRP_REDISTRIBUTE_PROTOCOL_OSPF:
		return ZEBRA_ROUTE_OSPF;
	case EIGRP_REDISTRIBUTE_PROTOCOL_ISIS:
		return ZEBRA_ROUTE_ISIS;
	case EIGRP_REDISTRIBUTE_PROTOCOL_BGP:
		return ZEBRA_ROUTE_BGP;
	case EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP:
		return ZEBRA_ROUTE_EIGRP;
	case EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED:
	default:
		return -1;
	}
}

struct eigrp_zebra_subscription_params {
	afi_t afi;
	int type;
	unsigned short instance;
	vrf_id_t vrf_id;
};

static bool eigrp_zebra_source_instance_valid(
	eigrp_redist_protocol_t protocol,
	eigrp_route_instance_t route_instance)
{
	switch (protocol) {
	case EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP:
		return route_instance != 0 && route_instance <= UINT16_MAX;
	case EIGRP_REDISTRIBUTE_PROTOCOL_OSPF:
		return route_instance <= UINT16_MAX;
	case EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED:
	case EIGRP_REDISTRIBUTE_PROTOCOL_STATIC:
	case EIGRP_REDISTRIBUTE_PROTOCOL_RIP:
	case EIGRP_REDISTRIBUTE_PROTOCOL_ISIS:
	case EIGRP_REDISTRIBUTE_PROTOCOL_BGP:
		/* FRR does not carry a route instance for these source types.
		 * In particular, a BGP ASN is not a Zebra route-instance value.
		 */
		return route_instance == 0;
	case EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED:
	default:
		return false;
	}
}

static eigrp_result_t eigrp_zebra_subscription_params_build(
	const eigrp_instance_t *eigrp,
	const eigrp_redist_source_t *source,
	struct eigrp_zebra_subscription_params *params)
{
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (!source || !params)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_zebra_source_instance_valid(source->protocol,
					       source->route_instance))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	memset(params, 0, sizeof(*params));
	params->afi = eigrp_zebra_instance_afi(eigrp);
	if (params->afi == AFI_UNSPEC)
		return EIGRP_RESULT_UNSUPPORTED;
	params->type = eigrp_zebra_redistribute_type(source->protocol);
	if (params->type < 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	params->instance = (unsigned short)source->route_instance;
	params->vrf_id = (vrf_id_t)eigrp_instance_vrf_id(eigrp);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_zebra_redistribute_update(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source)
{
	struct eigrp_zebra_subscription_params params;
	struct eigrp_zebra_instance_state *state;
	struct eigrp_zebra_redistribute *redist;
	eigrp_result_t result;
	bool was_in_use;
	result = eigrp_zebra_subscription_params_build(eigrp, source, &params);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (!eigrp_zclient)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	state = eigrp_zebra_instance_state_get(eigrp, true);
	if (!state)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	if (eigrp_zebra_redistribute_find(state, params.type,
					 params.instance))
		return EIGRP_RESULT_SUCCESS;
	was_in_use = eigrp_zebra_source_in_use(
		params.afi, params.type, params.instance, params.vrf_id);
	redist = XCALLOC(MTYPE_EIGRP_ZEBRA_INSTANCE, sizeof(*redist));
	if (!redist)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	redist->type = params.type;
	redist->route_instance = params.instance;
	redist->next = state->redistributions;
	state->redistributions = redist;

	/* Preserve the exact FRR source identity.  Nonzero protocol instances
	 * are tracked globally by zclient for one {AFI, type, instance}; route
	 * events still carry VRF and are filtered to the target runtime below.
	 */
	if (!was_in_use) {
		zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD, eigrp_zclient,
				     params.afi, params.type, params.instance,
				     params.vrf_id);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_zebra_redistribute_delete(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source)
{
	struct eigrp_zebra_subscription_params params;
	struct eigrp_zebra_instance_state *state;
	struct eigrp_zebra_redistribute **cursor;
	struct eigrp_zebra_redistribute *redist;
	eigrp_result_t result;

	result = eigrp_zebra_subscription_params_build(eigrp, source, &params);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (!eigrp_zclient)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	state = eigrp_zebra_instance_state_get(eigrp, false);
	if (!state)
		return EIGRP_RESULT_NOT_FOUND;

	for (cursor = &state->redistributions; *cursor;
	     cursor = &(*cursor)->next) {
		if ((*cursor)->type != params.type
		    || (*cursor)->route_instance != params.instance)
			continue;
		redist = *cursor;
		*cursor = redist->next;
		XFREE(MTYPE_EIGRP_ZEBRA_INSTANCE, redist);
		if (!eigrp_zebra_source_in_use(params.afi, params.type,
					params.instance, params.vrf_id))
			zclient_redistribute(ZEBRA_REDISTRIBUTE_DELETE,
					     eigrp_zclient, params.afi,
					     params.type, params.instance,
					     params.vrf_id);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

int eigrp_redistribute_update(eigrp_operation_t operation,
			      eigrp_instance_t *eigrp, int type,
			      struct eigrp_metrics metric)
{
	struct eigrp_zebra_instance_state *state;
	vrf_id_t vrf_id;
	afi_t afi;
	bool was_in_use;

	if (!eigrp || !eigrp_zclient || type <= 0 || type >= ZEBRA_ROUTE_MAX)
		return CMD_WARNING_CONFIG_FAILED;
	afi = eigrp_zebra_instance_afi(eigrp);
	if (afi == AFI_UNSPEC)
		return CMD_WARNING_CONFIG_FAILED;
	vrf_id = (vrf_id_t)eigrp_instance_vrf_id(eigrp);

	if (operation == EIGRP_RESET) {
		state = eigrp_zebra_instance_state_get(eigrp, false);
		if (!state || !state->classic_redistribute[type])
			return CMD_SUCCESS;

		memset(&state->dmetric[type], 0, sizeof(state->dmetric[type]));
		state->classic_redistribute[type] = false;
		if (state->redistribute_count > 0)
			--state->redistribute_count;
		if (!eigrp_zebra_source_in_use(afi, type, 0, vrf_id))
			zclient_redistribute(ZEBRA_REDISTRIBUTE_DELETE,
					     eigrp_zclient, afi, type, 0, vrf_id);
		return CMD_SUCCESS;
	}
	if (operation != EIGRP_SET)
		return CMD_WARNING_CONFIG_FAILED;

	state = eigrp_zebra_instance_state_get(eigrp, true);
	if (!state)
		return CMD_WARNING_CONFIG_FAILED;
	state->dmetric[type] = metric;
	if (state->classic_redistribute[type])
		return CMD_SUCCESS;
	was_in_use = eigrp_zebra_source_in_use(afi, type, 0, vrf_id);
	state->classic_redistribute[type] = true;
	++state->redistribute_count;
	if (!was_in_use)
		zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD, eigrp_zclient, afi,
				     type, 0, vrf_id);
	return CMD_SUCCESS;
}

static zclient_handler *const eigrp_handlers[] = {
	[ZEBRA_ROUTER_ID_UPDATE]	= eigrp_zebra_router_id_update,
	[ZEBRA_INTERFACE_ADDRESS_ADD]	= eigrp_zebra_interface_address_add,
	[ZEBRA_INTERFACE_ADDRESS_DELETE]= eigrp_zebra_interface_address_delete,
	[ZEBRA_REDISTRIBUTE_ROUTE_ADD]	= eigrp_zebra_redistribute_route,
	[ZEBRA_REDISTRIBUTE_ROUTE_DEL]	= eigrp_zebra_redistribute_route,
	[ZEBRA_ROUTE_NOTIFY_OWNER] 	= eigrp_zebra_route_notify_owner,
};

void eigrp_zebra_init(void)
{
	eigrp_zclient = zclient_new(master, &zclient_options_default, eigrp_handlers,
			      array_size(eigrp_handlers));

	zclient_init(eigrp_zclient, ZEBRA_ROUTE_EIGRP, 0, &eigrpd_privs);
	eigrp_zclient->zebra_connected = eigrp_zebra_connected;
}

void eigrp_zebra_stop(void)
{
	if (eigrp_zclient) {
		zclient_stop(eigrp_zclient);
		zclient_free(eigrp_zclient);
		eigrp_zclient = NULL;
	}
	eigrp_zebra_instance_delete_all();
}
