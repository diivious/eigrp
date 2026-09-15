// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP host southbound abstraction.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_southbound.h"

#include "vrf.h"
#include "workqueue.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_WORK_QUEUE, "EIGRP work queue");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_WORK_QUEUE_NAME, "EIGRP work queue name");

struct eigrp_work_queue {
	eigrp_instance_t *eigrp;
	struct work_queue *host_queue;
	char *name;
	eigrp_work_queue_func_t workfunc;
	eigrp_work_queue_delete_func_t deletefunc;
};

static wq_item_status eigrp_work_queue_host_run(struct work_queue *host_queue,
						void *data)
{
	eigrp_work_queue_t *queue = host_queue->spec.data;
	eigrp_work_queue_result_t result;

	if (!queue || !queue->workfunc)
		return WQ_SUCCESS;

	result = queue->workfunc(queue, data);
	switch (result) {
	case EIGRP_WORK_QUEUE_REQUEUE:
		return WQ_REQUEUE;
	case EIGRP_WORK_QUEUE_BLOCKED:
		return WQ_QUEUE_BLOCKED;
	case EIGRP_WORK_QUEUE_SUCCESS:
	default:
		return WQ_SUCCESS;
	}
}

static void eigrp_work_queue_host_delete(struct work_queue *host_queue,
						 void *data)
{
	eigrp_work_queue_t *queue = host_queue->spec.data;

	if (queue && queue->deletefunc)
		queue->deletefunc(queue, data);
}

static void eigrp_work_queue_host_create(eigrp_work_queue_t *queue)
{
	queue->host_queue = work_queue_new(eigrpd_event, queue->name);
	queue->host_queue->spec.data = queue;
	queue->host_queue->spec.workfunc = eigrp_work_queue_host_run;
	queue->host_queue->spec.del_item_data = eigrp_work_queue_host_delete;
}

eigrp_work_queue_t *eigrp_work_queue_new(eigrp_instance_t *eigrp,
						 const char *name,
						 eigrp_work_queue_func_t workfunc,
						 eigrp_work_queue_delete_func_t deletefunc)
{
	eigrp_work_queue_t *queue;

	queue = XCALLOC(MTYPE_EIGRP_WORK_QUEUE, sizeof(*queue));
	queue->eigrp = eigrp;
	queue->name = XSTRDUP(MTYPE_EIGRP_WORK_QUEUE_NAME, name);
	queue->workfunc = workfunc;
	queue->deletefunc = deletefunc;

	eigrp_work_queue_host_create(queue);
	return queue;
}

void eigrp_work_queue_free(eigrp_work_queue_t *queue)
{
	if (!queue)
		return;

	if (queue->host_queue)
		work_queue_free_and_null(&queue->host_queue);
	XFREE(MTYPE_EIGRP_WORK_QUEUE_NAME, queue->name);
	XFREE(MTYPE_EIGRP_WORK_QUEUE, queue);
}

void eigrp_work_queue_reset(eigrp_work_queue_t *queue)
{
	if (!queue)
		return;

	if (queue->host_queue)
		work_queue_free_and_null(&queue->host_queue);
	eigrp_work_queue_host_create(queue);
}

void eigrp_work_queue_enqueue(eigrp_work_queue_t *queue, void *data)
{
	if (!queue || !queue->host_queue || !data)
		return;

	work_queue_add(queue->host_queue, data);
}

eigrp_instance_t *eigrp_work_queue_eigrp(eigrp_work_queue_t *queue)
{
	return queue ? queue->eigrp : NULL;
}

static bool eigrp_southbound_prefix_from_host(const struct prefix *host,
				       eigrp_prefix_t *prefix)
{
	if (!host || !prefix || host->family != AF_INET || host->prefixlen > 32)
		return false;

	memset(prefix, 0, sizeof(*prefix));
	prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	prefix->prefix_length = host->prefixlen;
	memcpy(prefix->address.bytes, &host->u.prefix4, sizeof(host->u.prefix4));
	return true;
}

static bool eigrp_southbound_prefix_to_host(const eigrp_prefix_t *prefix,
				     struct prefix *host)
{
	if (!prefix || !host
	    || prefix->address.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || prefix->prefix_length > 32)
		return false;

	memset(host, 0, sizeof(*host));
	host->family = AF_INET;
	host->prefixlen = prefix->prefix_length;
	memcpy(&host->u.prefix4, prefix->address.bytes, sizeof(host->u.prefix4));
	return true;
}

static void eigrp_southbound_network_run_interface(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network,
	struct interface *ifp)
{
	eigrp_prefix_t connected_prefix;
	eigrp_interface_t *ei;
	struct connected *co;

	if (!eigrp || !network || !ifp)
		return;

	/* A network statement enables EIGRP on matching primary addresses. */
	frr_each (if_connected, ifp->connected, co) {
		if (!co->address || CHECK_FLAG(co->flags, ZEBRA_IFA_SECONDARY))
			continue;
		if (ifp->info)
			continue;
		if (!eigrp_southbound_prefix_from_host(co->address,
						      &connected_prefix)
		    || !eigrp_network_prefix_match(network, &connected_prefix))
			continue;

		ei = eigrp_intf_new(eigrp, ifp, co->address);
		if (!ei)
			continue;

		ei->eigrp = eigrp;

		/* eigrp_router_id_update() calls eigrp_intf_update() when a
		 * router ID becomes available, so an operative interface can be
		 * started here.
		 */
		if (if_is_operative(ifp))
			eigrp_intf_up(eigrp, ei);
	}
}

eigrp_result_t eigrp_southbound_network_create(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network, bool *changed)
{
	struct prefix host_network;
	struct prefix *stored;
	struct route_node *rn;
	struct interface *ifp;
	struct vrf *vrf;

	if (changed)
		*changed = false;
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (!eigrp_southbound_prefix_to_host(network, &host_network))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	rn = route_node_get(eigrp->networks, &host_network);
	if (rn->info) {
		/* The runtime already owns this network statement. */
		route_unlock_node(rn);
		return EIGRP_RESULT_SUCCESS;
	}

	stored = prefix_new();
	if (!stored) {
		route_unlock_node(rn);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	prefix_copy(stored, &host_network);
	rn->info = stored;

	if (eigrp->router_id.s_addr == INADDR_ANY)
		eigrp_router_id_update(eigrp);

	vrf = vrf_lookup_by_id(eigrp->vrf_id);
	if (vrf) {
		FOR_ALL_INTERFACES (vrf, ifp) {
			zlog_debug("Setting up %s", ifp->name);
			eigrp_southbound_network_run_interface(eigrp, network, ifp);
		}
	}

	if (changed)
		*changed = true;
	return EIGRP_RESULT_SUCCESS;
}

/* Legacy FRR interface event entry point.  Keep the host object in the FRR
 * southbound adapter while matching configured networks with portable prefixes.
 */
void eigrp_intf_update(eigrp_instance_t *eigrp, struct interface *ifp)
{
	eigrp_prefix_t network;
	struct route_node *rn;

	if (!eigrp || !ifp)
		return;

	if (ifp->vrf) {
		if (ifp->vrf->vrf_id != eigrp->vrf_id)
			return;
	} else if (eigrp->vrf_id != VRF_DEFAULT) {
		return;
	}

	if (eigrp->router_id.s_addr == INADDR_ANY)
		return;

	for (rn = route_top(eigrp->networks); rn; rn = route_next(rn)) {
		if (!rn->info)
			continue;
		if (!eigrp_southbound_prefix_from_host(&rn->p, &network))
			continue;
		eigrp_southbound_network_run_interface(eigrp, &network, ifp);
	}
}

eigrp_result_t eigrp_southbound_network_delete(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network, bool *changed)
{
	eigrp_prefix_t connected_prefix;
	eigrp_prefix_t configured_network;
	struct prefix host_network;
	struct prefix *stored;
	struct route_node *rn;
	struct listnode *node;
	struct listnode *nnode;
	eigrp_interface_t *ei;

	if (changed)
		*changed = false;
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (!eigrp_southbound_prefix_to_host(network, &host_network))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	rn = route_node_lookup(eigrp->networks, &host_network);
	if (!rn || !rn->info) {
		if (rn)
			route_unlock_node(rn);
		return EIGRP_RESULT_NOT_FOUND;
	}

	stored = rn->info;
	rn->info = NULL;
	prefix_free(&stored);
	/* route_node_lookup() added one lock and route_node_get() retained the
	 * original lock while the network marker existed.
	 */
	route_unlock_node(rn);
	route_unlock_node(rn);

	/* Disable EIGRP only on interfaces that no remaining network statement
	 * still covers.
	 */
	for (ALL_LIST_ELEMENTS(eigrp->eiflist, node, nnode, ei)) {
		bool found = false;

		if (!eigrp_southbound_prefix_from_host(&ei->address,
						      &connected_prefix))
			continue;

		for (rn = route_top(eigrp->networks); rn; rn = route_next(rn)) {
			if (!rn->info)
				continue;
			if (!eigrp_southbound_prefix_from_host(
				    &rn->p, &configured_network))
				continue;
			if (!eigrp_network_prefix_match(&configured_network,
							&connected_prefix))
				continue;
			found = true;
			route_unlock_node(rn);
			break;
		}

		if (!found)
			eigrp_intf_free(eigrp, ei, INTERFACE_DOWN_BY_VTY);
	}

	if (changed)
		*changed = true;
	return EIGRP_RESULT_SUCCESS;
}
