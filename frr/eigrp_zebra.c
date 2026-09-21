// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Zebra connect library for EIGRP.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
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

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_debug.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_vty.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_frr.h"
#include "eigrpd/eigrp_frr_memory.h"

/* Zebra structure to hold current status. */
struct zclient *eigrp_zclient = NULL;

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_ZEBRA_INSTANCE,
		    "EIGRP Zebra instance state");

struct eigrp_zebra_instance_state {
	eigrp_instance_t *eigrp;
	eigrp_metrics_t dmetric[ZEBRA_ROUTE_MAX];
	unsigned int redistribute_count;
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

void eigrp_zebra_instance_delete(eigrp_instance_t *eigrp)
{
	struct eigrp_zebra_instance_state **cursor;
	struct eigrp_zebra_instance_state *state;

	for (cursor = &eigrp_zebra_instances; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->eigrp != eigrp)
			continue;
		state = *cursor;
		*cursor = state->next;
		XFREE(MTYPE_EIGRP_ZEBRA_INSTANCE, state);
		return;
	}
}

static void eigrp_zebra_instance_delete_all(void)
{
	struct eigrp_zebra_instance_state *state;

	while ((state = eigrp_zebra_instances) != NULL) {
		eigrp_zebra_instances = state->next;
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
	eigrp_instance_router_id_refresh_vrf((eigrp_vrf_id_t)vrf_id);
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

static void eigrp_zebra_connected(struct zclient *zclient)
{
	zclient_send_reg_requests(zclient, VRF_DEFAULT);
}

/* Zebra route add and delete treatment. */
static int eigrp_zebra_redistribute_route(ZAPI_CALLBACK_ARGS)
{
	struct zapi_route api;

	if (zapi_route_decode(zclient->ibuf, &api) < 0) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR Zebra redistribute %s decode failed for VRF %u",
			cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD ? "add" : "delete",
			(unsigned)vrf_id);
		return -1;
	}

	/* The external-route receive path is not implemented yet.  Keep this
	 * callback limited to Zebra decode/debug until it can normalize a route
	 * event and hand it to the EIGRP redistribution target.
	 */
	if ((term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_RIB))
		eigrp_log(EIGRP_LOG_DEBUG,
			"Zebra: redistribute %s %s",
			cmd == ZEBRA_REDISTRIBUTE_ROUTE_ADD ? "add" : "delete",
			eigrp_zebra_prefix_string(&api.prefix));

	return 0;
}

static int eigrp_zebra_interface_address_add(ZAPI_CALLBACK_ARGS)
{
	eigrp_interface_runtime_state_t state;
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

	eigrp_network_interface_refresh((eigrp_vrf_id_t)vrf_id, &state);
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

	eigrp_interface_runtime_address_remove((eigrp_vrf_id_t)vrf_id,
					       ifp->ifindex, &removed,
					       EIGRP_INTERFACE_REMOVE_HOST);

	connected_free(&c);
	return 0;
}

eigrp_result_t eigrp_zebra_route_install(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix,
	const eigrp_southbound_nexthop_t *nexthops, size_t nexthop_count,
	uint32_t distance)
{
	struct zapi_route api;
	struct zapi_nexthop *api_nh;
	struct prefix host_prefix;
	size_t i;
	int count = 0;

	if (!eigrp || !prefix || (!nexthops && nexthop_count != 0))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_zclient)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	if (!eigrp_zclient->redist[AFI_IP][ZEBRA_ROUTE_EIGRP])
		return EIGRP_RESULT_SUCCESS;
	if (eigrp_frr_prefix_export(prefix, &host_prefix) != EIGRP_RESULT_SUCCESS)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	zapi_route_init(&api);
	api.vrf_id = eigrp->vrf_id;
	api.type = ZEBRA_ROUTE_EIGRP;
	api.safi = SAFI_UNICAST;
	api.metric = distance;
	api.prefix = host_prefix;

	SET_FLAG(api.message, ZAPI_MESSAGE_NEXTHOP);
	SET_FLAG(api.message, ZAPI_MESSAGE_METRIC);

	for (i = 0; i < nexthop_count && count < MULTIPATH_NUM; i++) {
		const eigrp_southbound_nexthop_t *nexthop = &nexthops[i];

		api_nh = &api.nexthops[count];
		zapi_nexthop_init(api_nh);
		api_nh->vrf_id = eigrp->vrf_id;
		api_nh->ifindex = nexthop->ifindex;
		if (nexthop->gateway_present
		    && nexthop->gateway.afi == EIGRP_ADDRESS_FAMILY_IPV4) {
			memcpy(&api_nh->gate.ipv4, nexthop->gateway.bytes,
			       sizeof(api_nh->gate.ipv4));
			api_nh->type = NEXTHOP_TYPE_IPV4_IFINDEX;
		} else {
			api_nh->type = NEXTHOP_TYPE_IFINDEX;
		}
		count++;
	}
	api.nexthop_num = count;

	if ((term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_RIB)
	    || eigrp_debug_address_family_enabled(
		    eigrp, EIGRP_DEBUG_AF_NOTIFICATIONS, NULL))
		eigrp_log(EIGRP_LOG_DEBUG, "Zebra: Route add %s",
				eigrp_zebra_prefix_string(&host_prefix));

	zclient_route_send(ZEBRA_ROUTE_ADD, eigrp_zclient, &api);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_zebra_route_remove(eigrp_instance_t *eigrp,
				       const eigrp_prefix_t *prefix)
{
	struct zapi_route api;
	struct prefix host_prefix;

	if (!eigrp || !prefix)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_zclient)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	if (!eigrp_zclient->redist[AFI_IP][ZEBRA_ROUTE_EIGRP])
		return EIGRP_RESULT_SUCCESS;
	if (eigrp_frr_prefix_export(prefix, &host_prefix) != EIGRP_RESULT_SUCCESS)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	zapi_route_init(&api);
	api.vrf_id = eigrp->vrf_id;
	api.type = ZEBRA_ROUTE_EIGRP;
	api.safi = SAFI_UNICAST;
	api.prefix = host_prefix;
	zclient_route_send(ZEBRA_ROUTE_DELETE, eigrp_zclient, &api);

	if ((term_debug_eigrp_notifications & EIGRP_DEBUG_NOTIFICATION_RIB)
	    || eigrp_debug_address_family_enabled(
		    eigrp, EIGRP_DEBUG_AF_NOTIFICATIONS, NULL))
		eigrp_log(EIGRP_LOG_DEBUG, "Zebra: Route del %s",
				eigrp_zebra_prefix_string(&host_prefix));
	return EIGRP_RESULT_SUCCESS;
}

static int eigrp_is_type_redistributed(int type, vrf_id_t vrf_id)
{
	if (!eigrp_zclient || type <= 0 || type >= ZEBRA_ROUTE_MAX)
		return 0;
	return vrf_bitmap_check(&eigrp_zclient->redist[AFI_IP][type], vrf_id);
}

static int eigrp_zebra_redistribute_type(const char *protocol)
{
	int type;

	if (!protocol || !protocol[0])
		return -1;
	type = proto_redistnum(AFI_IP, protocol);
	if (type == 0 || type >= ZEBRA_ROUTE_MAX)
		return -1;
	return type;
}

eigrp_result_t eigrp_zebra_redistribute_update(
	eigrp_instance_t *eigrp, const char *protocol,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	struct eigrp_zebra_instance_state *state;
	eigrp_metrics_t runtime_metric;
	int type;

	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (!eigrp_zclient)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	type = eigrp_zebra_redistribute_type(protocol);
	if (type < 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	state = eigrp_zebra_instance_state_get(eigrp, true);
	if (!state)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	eigrp_metric_values_convert(metric, &runtime_metric);
	if (eigrp_is_type_redistributed(type, eigrp->vrf_id)) {
		state->dmetric[type] = runtime_metric;
	} else {
		state->dmetric[type] = runtime_metric;
		zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD, eigrp_zclient, AFI_IP,
				     type, 0, eigrp->vrf_id);
		++state->redistribute_count;
	}

	/* Zebra subscription is real, but the current external-route receive path
	 * does not yet install redistributed routes into EIGRP topology state or
	 * apply route-map policy.  Keep the named configuration and report the
	 * runtime feature as incomplete until that data path is implemented.
	 */
	(void)route_map;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_zebra_redistribute_delete(eigrp_instance_t *eigrp,
						const char *protocol)
{
	struct eigrp_zebra_instance_state *state;
	int type;

	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (!eigrp_zclient)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	type = eigrp_zebra_redistribute_type(protocol);
	if (type < 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_is_type_redistributed(type, eigrp->vrf_id))
		return EIGRP_RESULT_NOT_FOUND;

	state = eigrp_zebra_instance_state_get(eigrp, false);
	if (state) {
		memset(&state->dmetric[type], 0, sizeof(state->dmetric[type]));
		if (state->redistribute_count > 0)
			--state->redistribute_count;
	}
	zclient_redistribute(ZEBRA_REDISTRIBUTE_DELETE, eigrp_zclient, AFI_IP,
			     type, 0, eigrp->vrf_id);
	return EIGRP_RESULT_SUCCESS;
}

int eigrp_redistribute_set(eigrp_instance_t *eigrp, int type,
			   struct eigrp_metrics metric)
{
	struct eigrp_zebra_instance_state *state;

	if (!eigrp || type <= 0 || type >= ZEBRA_ROUTE_MAX)
		return CMD_WARNING_CONFIG_FAILED;
	state = eigrp_zebra_instance_state_get(eigrp, true);
	if (!state)
		return CMD_WARNING_CONFIG_FAILED;

	state->dmetric[type] = metric;
	if (eigrp_is_type_redistributed(type, eigrp->vrf_id))
		return CMD_SUCCESS;

	zclient_redistribute(ZEBRA_REDISTRIBUTE_ADD, eigrp_zclient, AFI_IP, type, 0,
			     eigrp->vrf_id);
	++state->redistribute_count;
	return CMD_SUCCESS;
}

int eigrp_redistribute_unset(eigrp_instance_t *eigrp, int type)
{
	struct eigrp_zebra_instance_state *state;

	if (!eigrp || type <= 0 || type >= ZEBRA_ROUTE_MAX)
		return CMD_WARNING_CONFIG_FAILED;
	state = eigrp_zebra_instance_state_get(eigrp, false);

	if (eigrp_is_type_redistributed(type, eigrp->vrf_id)) {
		if (state) {
			memset(&state->dmetric[type], 0, sizeof(state->dmetric[type]));
			if (state->redistribute_count > 0)
				--state->redistribute_count;
		}
		zclient_redistribute(ZEBRA_REDISTRIBUTE_DELETE, eigrp_zclient,
				     AFI_IP, type, 0, eigrp->vrf_id);
	}

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
