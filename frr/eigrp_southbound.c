// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP host southbound abstraction.
 * Copyright (C) 2026 Donnie V. Savage
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrp_zebra.h"
#include "eigrp_frr.h"

#include "plist.h"
#include "table.h"
#include "vrf.h"
#include "frrevent.h"
#include "workqueue.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_WORK_QUEUE, "EIGRP work queue");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_WORK_QUEUE_NAME, "EIGRP work queue name");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_EVENT, "EIGRP host event");

extern struct event_loop *eigrpd_event;
extern struct in_addr router_id_zebra;

struct eigrp_event {
	struct event *host_event;
	eigrp_event_callback_t callback;
	void *arg;
	eigrp_event_t **owner;
};

static void eigrp_southbound_event_run(struct event *host_event)
{
	eigrp_event_t *event = EVENT_ARG(host_event);
	eigrp_event_callback_t callback;
	void *arg;

	if (!event)
		return;

	callback = event->callback;
	arg = event->arg;
	if (event->owner && *event->owner == event)
		*event->owner = NULL;
	event->host_event = NULL;
	XFREE(MTYPE_EIGRP_EVENT, event);

	if (callback)
		callback(arg);
}

static eigrp_event_t *eigrp_southbound_event_prepare(
	eigrp_event_t **owner, eigrp_event_callback_t callback, void *arg)
{
	eigrp_event_t *event;

	if (!owner || !callback)
		return NULL;

	eigrp_southbound_event_cancel(owner);
	event = XCALLOC(MTYPE_EIGRP_EVENT, sizeof(*event));
	event->callback = callback;
	event->arg = arg;
	event->owner = owner;
	*owner = event;
	return event;
}

void eigrp_southbound_event_cancel(eigrp_event_t **owner)
{
	eigrp_event_t *event;

	if (!owner || !*owner)
		return;

	event = *owner;
	*owner = NULL;
	if (event->host_event)
		event_cancel(&event->host_event);
	XFREE(MTYPE_EIGRP_EVENT, event);
}

void eigrp_southbound_event_add(eigrp_event_t **owner,
				eigrp_event_callback_t callback, void *arg)
{
	eigrp_event_t *event = eigrp_southbound_event_prepare(owner, callback, arg);

	if (event)
		event_add_event(eigrpd_event, eigrp_southbound_event_run, event, 0,
				&event->host_event);
}

void eigrp_southbound_timer_add(eigrp_event_t **owner,
				eigrp_event_callback_t callback, void *arg,
				uint32_t seconds)
{
	eigrp_event_t *event = eigrp_southbound_event_prepare(owner, callback, arg);

	if (event)
		event_add_timer(eigrpd_event, eigrp_southbound_event_run, event, seconds,
				&event->host_event);
}

void eigrp_southbound_timer_msec_add(eigrp_event_t **owner,
				     eigrp_event_callback_t callback, void *arg,
				     uint32_t milliseconds)
{
	eigrp_event_t *event = eigrp_southbound_event_prepare(owner, callback, arg);

	if (event)
		event_add_timer_msec(eigrpd_event, eigrp_southbound_event_run, event,
				     milliseconds, &event->host_event);
}

void eigrp_southbound_read_add(eigrp_event_t **owner, int fd,
			       eigrp_event_callback_t callback, void *arg)
{
	eigrp_event_t *event = eigrp_southbound_event_prepare(owner, callback, arg);

	if (event)
		event_add_read(eigrpd_event, eigrp_southbound_event_run, event, fd,
			       &event->host_event);
}

void eigrp_southbound_write_add(eigrp_event_t **owner, int fd,
				eigrp_event_callback_t callback, void *arg)
{
	eigrp_event_t *event = eigrp_southbound_event_prepare(owner, callback, arg);

	if (event)
		event_add_write(eigrpd_event, eigrp_southbound_event_run, event, fd,
				&event->host_event);
}

uint32_t eigrp_southbound_timer_remaining_seconds(const eigrp_event_t *event)
{
	if (!event || !event->host_event)
		return 0;
	return event_timer_remain_second(event->host_event);
}

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


eigrp_result_t eigrp_southbound_socket_open(eigrp_instance_t *eigrp)
{
	struct vrf *vrf;
	eigrp_result_t result = EIGRP_RESULT_SUCCESS;
	int fd = -1;
	int ret;
#ifdef IP_HDRINCL
	int hincl = 1;
#endif

	if (!eigrp)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	vrf = vrf_lookup_by_id((vrf_id_t)eigrp->vrf_id);
	if (!vrf)
		return EIGRP_RESULT_NOT_FOUND;

	frr_with_privs (&eigrpd_privs) {
		fd = vrf_socket(AF_INET, SOCK_RAW, IPPROTO_EIGRPIGP, vrf->vrf_id,
				vrf->vrf_id != VRF_DEFAULT ? vrf->name : NULL);
		if (fd < 0) {
			zlog_err("EIGRP socket: %s", safe_strerror(errno));
			result = EIGRP_RESULT_INTERNAL_FAILURE;
		} else {
#ifdef IP_HDRINCL
			ret = setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &hincl,
					 sizeof(hincl));
			if (ret < 0)
				zlog_warn("Can't set IP_HDRINCL option for fd %d: %s", fd,
					  safe_strerror(errno));
#elif defined(IPTOS_PREC_INTERNETCONTROL)
			ret = setsockopt_ipv4_tos(fd, IPTOS_PREC_INTERNETCONTROL);
			if (ret < 0) {
				zlog_warn("can't set EIGRP IP_TOS on socket %d: %s", fd,
					  safe_strerror(errno));
				close(fd);
				fd = -1;
				result = EIGRP_RESULT_INTERNAL_FAILURE;
			}
#else
			zlog_warn("IP_HDRINCL option not available");
#endif

			if (result == EIGRP_RESULT_SUCCESS) {
				ret = setsockopt_ifindex(AF_INET, fd, 1);
				if (ret < 0)
					zlog_warn("Can't set pktinfo option for fd %d", fd);
			}
		}
	}

	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	eigrp->fd = fd;
	eigrp->maxsndbuflen = getsockopt_so_sendbuf(fd);
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_southbound_socket_close(eigrp_instance_t *eigrp)
{
	if (!eigrp || eigrp->fd < 0)
		return;
	close(eigrp->fd);
	eigrp->fd = -1;
}

void eigrp_southbound_socket_send_buffer_ensure(eigrp_instance_t *eigrp,
						uint32_t minimum)
{
	int new_size;

	if (!eigrp || eigrp->fd < 0 || eigrp->maxsndbuflen >= minimum)
		return;

	setsockopt_so_sendbuf(eigrp->fd, minimum);
	new_size = getsockopt_so_sendbuf(eigrp->fd);
	if (new_size < 0 || new_size < (int)minimum)
		zlog_warn("%s: tried to set SO_SNDBUF to %u, but got %d",
			  __func__, minimum, new_size);
	if (new_size >= 0)
		eigrp->maxsndbuflen = (uint32_t)new_size;
	else
		zlog_warn("%s: failed to get SO_SNDBUF", __func__);
}

bool eigrp_southbound_router_id_get(eigrp_instance_t *eigrp,
				    struct in_addr *router_id)
{
	(void)eigrp;
	if (!router_id)
		return false;

	*router_id = router_id_zebra;
	return router_id->s_addr != INADDR_ANY;
}

static bool eigrp_southbound_interface_ipv4_address(
	const eigrp_interface_t *ei, struct in_addr *address)
{
	if (!ei || !address
	    || ei->address.address.afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return false;

	memcpy(address, ei->address.address.bytes, sizeof(*address));
	return true;
}

int eigrp_southbound_multicast_interface_set(eigrp_instance_t *eigrp,
				             eigrp_interface_t *ei)
{
	struct in_addr address;
	uint8_t val = 0;
	int ret;

	if (!eigrp || !eigrp_southbound_interface_ipv4_address(ei, &address))
		return -1;

	ret = setsockopt(eigrp->fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val));
	if (ret < 0)
		zlog_warn("can't disable IP_MULTICAST_LOOP for fd %d: %s",
			  eigrp->fd, safe_strerror(errno));

	val = 1;
	ret = setsockopt(eigrp->fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val));
	if (ret < 0)
		zlog_warn("can't set IP_MULTICAST_TTL for fd %d: %s",
			  eigrp->fd, safe_strerror(errno));

	ret = setsockopt_ipv4_multicast_if(eigrp->fd, address, ei->ifindex);
	if (ret < 0)
		zlog_warn("can't set multicast interface %s[%u]: %s", ei->name,
			  ei->ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_southbound_multicast_join(eigrp_instance_t *eigrp,
				    eigrp_interface_t *ei)
{
	struct in_addr address;
	int ret;

	if (!eigrp || !eigrp_southbound_interface_ipv4_address(ei, &address))
		return -1;

	ret = setsockopt_ipv4_multicast(eigrp->fd, IP_ADD_MEMBERSHIP, address,
					htonl(EIGRP_MULTICAST_ADDRESS), ei->ifindex);
	if (ret < 0)
		zlog_warn("can't join EIGRP multicast group on %s[%u]: %s",
			  ei->name, ei->ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_southbound_multicast_leave(eigrp_instance_t *eigrp,
				     eigrp_interface_t *ei)
{
	struct in_addr address;
	int ret;

	if (!eigrp || !eigrp_southbound_interface_ipv4_address(ei, &address))
		return -1;

	ret = setsockopt_ipv4_multicast(eigrp->fd, IP_DROP_MEMBERSHIP, address,
					htonl(EIGRP_MULTICAST_ADDRESS), ei->ifindex);
	if (ret < 0)
		zlog_warn("can't leave EIGRP multicast group on %s[%u]: %s",
			  ei->name, ei->ifindex, safe_strerror(errno));
	return ret;
}

eigrp_result_t eigrp_southbound_instance_create(
	const char *name, eigrp_address_family_t afi, const char *vrf_name,
	uint16_t asn, eigrp_instance_t **runtime)
{
	eigrp_instance_t *eigrp;
	struct vrf *vrf;

	if (runtime)
		*runtime = NULL;
	if (!name || !name[0] || !vrf_name || !vrf_name[0] || asn == 0
	    || !runtime)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return EIGRP_RESULT_UNSUPPORTED;

	vrf = vrf_lookup_by_name(vrf_name);
	if (!vrf)
		return EIGRP_RESULT_NOT_FOUND;

	/*
	 * {AF, VRF, AS} is the runtime/on-wire identity.  A named parent is
	 * local configuration ownership only, so two different parents (or a
	 * classic instance and a named parent) must not share one runtime.
	 */
	eigrp = eigrp_lookup_by_as_vrf(asn, vrf->vrf_id);
	if (eigrp) {
		if (!eigrp->name || strcmp(eigrp->name, name) != 0)
			return EIGRP_RESULT_CONFLICT;
		*runtime = eigrp;
		return EIGRP_RESULT_SUCCESS;
	}

	eigrp = eigrp_get(asn, vrf->vrf_id);
	if (!eigrp)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	eigrp_name_set(eigrp, name);
	*runtime = eigrp;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_southbound_instance_delete(
	const char *name, eigrp_instance_t *runtime)
{
	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (!runtime->name || strcmp(runtime->name, name) != 0)
		return EIGRP_RESULT_CONFLICT;

	eigrp_finish_final(runtime);
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_southbound_router_id_refresh(eigrp_instance_t *runtime)
{
	if (runtime)
		eigrp_router_id_update(runtime);
}

eigrp_result_t eigrp_southbound_address_family_stop(eigrp_instance_t *runtime)
{
	eigrp_interface_t *ei;
	struct listnode *node;

	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;

	for (ALL_LIST_ELEMENTS_RO(runtime->eiflist, node, ei)) {
		if (!ei->t_hello)
			continue;
		eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);
		eigrp_intf_down(ei);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_southbound_address_family_start(eigrp_instance_t *runtime)
{
	eigrp_address_family_config_t *af;
	eigrp_interface_config_t *config;
	eigrp_interface_t *ei;
	struct listnode *node;

	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (runtime->router_id.s_addr == INADDR_ANY)
		eigrp_router_id_update(runtime);
	if (runtime->router_id.s_addr == INADDR_ANY)
		return EIGRP_RESULT_SUCCESS;

	af = eigrp_instance_runtime_config(runtime);
	for (ALL_LIST_ELEMENTS_RO(runtime->eiflist, node, ei)) {
		config = af ? eigrp_interface_config_read(af, ei->name) : NULL;
		if (config)
			eigrp_interface_runtime_bind(ei, config);
		if ((config && config->shutdown) || !ei->operative || ei->t_hello)
			continue;
		eigrp_intf_up(runtime, ei);
	}
	return EIGRP_RESULT_SUCCESS;
}

static bool eigrp_southbound_interface_vrf_match(
	const eigrp_instance_t *eigrp, const struct interface *ifp)
{
	if (!eigrp || !ifp)
		return false;
	if (ifp->vrf)
		return ifp->vrf->vrf_id == (vrf_id_t)eigrp->vrf_id;
	return eigrp->vrf_id == EIGRP_VRF_DEFAULT;
}

static uint8_t eigrp_southbound_interface_type(const struct interface *ifp)
{
	if (if_is_pointopoint(ifp))
		return EIGRP_IFTYPE_POINTOPOINT;
	if (if_is_loopback(ifp))
		return EIGRP_IFTYPE_LOOPBACK;
	return EIGRP_IFTYPE_BROADCAST;
}

static bool eigrp_southbound_interface_state_get(
	struct interface *ifp, const struct prefix *address,
	eigrp_interface_runtime_state_t *state)
{
	if (!ifp || !address || !state)
		return false;

	memset(state, 0, sizeof(*state));
	if (eigrp_frr_prefix_import(address, &state->address)
	    != EIGRP_RESULT_SUCCESS)
		return false;
	state->interface_name = ifp->name;
	state->ifindex = ifp->ifindex;
	state->type = eigrp_southbound_interface_type(ifp);
	state->operative = if_is_operative(ifp);
	state->bandwidth = ifp->bandwidth;
	state->mtu = ifp->mtu;
	return true;
}

static void eigrp_southbound_network_run_interface(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network,
	struct interface *ifp)
{
	eigrp_interface_runtime_state_t state;
	eigrp_address_family_config_t *af;
	eigrp_interface_config_t *config;
	eigrp_prefix_t connected_prefix;
	eigrp_interface_t *ei;
	struct connected *co;
	bool was_running;
	uint32_t old_mtu;

	if (!eigrp || !network || !ifp
	    || eigrp->router_id.s_addr == INADDR_ANY
	    || !eigrp_southbound_interface_vrf_match(eigrp, ifp))
		return;

	frr_each (if_connected, ifp->connected, co) {
		if (!co->address || co->address->family != AF_INET
		    || CHECK_FLAG(co->flags, ZEBRA_IFA_SECONDARY))
			continue;
		if (eigrp_frr_prefix_import(co->address, &connected_prefix)
			    != EIGRP_RESULT_SUCCESS
		    || !eigrp->af_vectors.network_interface_match
		    || !eigrp->af_vectors.network_interface_match(
			    network, &connected_prefix))
			continue;
		if (!eigrp_southbound_interface_state_get(ifp, co->address, &state))
			continue;

		ei = eigrp_intf_lookup_by_ifindex(eigrp, state.ifindex);
		was_running = ei && ei->t_hello;
		old_mtu = ei ? ei->curr_mtu : state.mtu;
		if (ei)
			eigrp_interface_runtime_update(ei, &state);
		else
			ei = eigrp_interface_runtime_create(eigrp, &state);
		if (!ei)
			return;

		af = eigrp_instance_runtime_config(eigrp);
		config = af ? eigrp_interface_config_read(af, state.interface_name) : NULL;
		if (config)
			eigrp_interface_runtime_bind(ei, config);

		if ((af && af->shutdown) || (config && config->shutdown)
		    || !state.operative) {
			if (was_running)
				eigrp_intf_down(ei);
			return;
		}

		if (was_running && old_mtu != state.mtu)
			eigrp_interface_runtime_reset(ei);
		else if (!was_running)
			eigrp_intf_up(eigrp, ei);
		return;
	}
}

static void eigrp_southbound_interface_refresh_one(eigrp_instance_t *eigrp,
					   struct interface *ifp)
{
	eigrp_prefix_t network;
	struct route_node *rn;

	if (!eigrp || !ifp || eigrp->router_id.s_addr == INADDR_ANY
	    || !eigrp_southbound_interface_vrf_match(eigrp, ifp))
		return;

	for (rn = route_top(eigrp->networks); rn; rn = route_next(rn)) {
		if (!rn->info)
			continue;
		if (eigrp_frr_prefix_import(&rn->p, &network)
		    != EIGRP_RESULT_SUCCESS)
			continue;
		eigrp_southbound_network_run_interface(eigrp, &network, ifp);
	}
}

void eigrp_southbound_interfaces_refresh(eigrp_instance_t *eigrp)
{
	struct interface *ifp;
	struct vrf *vrf;

	if (!eigrp)
		return;
	vrf = vrf_lookup_by_id((vrf_id_t)eigrp->vrf_id);
	if (!vrf)
		return;

	FOR_ALL_INTERFACES (vrf, ifp)
		eigrp_southbound_interface_refresh_one(eigrp, ifp);
}

static int eigrp_southbound_if_real(struct interface *ifp)
{
	eigrp_instance_t *eigrp;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp))
		eigrp_southbound_interface_refresh_one(eigrp, ifp);
	return 0;
}

static int eigrp_southbound_if_up(struct interface *ifp)
{
	return eigrp_southbound_if_real(ifp);
}

static int eigrp_southbound_if_down(struct interface *ifp)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	eigrp_interface_runtime_state_t state;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp)) {
		if (!eigrp_southbound_interface_vrf_match(eigrp, ifp))
			continue;
		ei = eigrp_intf_lookup_by_ifindex(eigrp, ifp->ifindex);
		if (!ei)
			continue;
		memset(&state, 0, sizeof(state));
		state.interface_name = ifp->name;
		state.ifindex = ifp->ifindex;
		state.address = ei->address;
		state.type = eigrp_southbound_interface_type(ifp);
		state.operative = false;
		state.bandwidth = ifp->bandwidth;
		state.mtu = ifp->mtu;
		eigrp_interface_runtime_update(ei, &state);
		eigrp_intf_down(ei);
	}
	return 0;
}

static int eigrp_southbound_if_unreal(struct interface *ifp)
{
	eigrp_instance_t *eigrp;
	eigrp_interface_t *ei;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(eigrp_om->eigrp, node, eigrp)) {
		if (!eigrp_southbound_interface_vrf_match(eigrp, ifp))
			continue;
		ei = eigrp_intf_lookup_by_ifindex(eigrp, ifp->ifindex);
		if (ei)
			eigrp_interface_runtime_delete(ei, INTERFACE_DOWN_BY_ZEBRA);
	}
	return 0;
}

void eigrp_southbound_runtime_init(void)
{
	hook_register_prio(if_real, 0, eigrp_southbound_if_real);
	hook_register_prio(if_up, 0, eigrp_southbound_if_up);
	hook_register_prio(if_down, 0, eigrp_southbound_if_down);
	hook_register_prio(if_unreal, 0, eigrp_southbound_if_unreal);
}

eigrp_result_t eigrp_southbound_network_exists(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *network, bool *exists)
{
	struct prefix host_network;
	struct route_node *rn;

	if (!exists)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	*exists = false;
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (eigrp_frr_prefix_export(network, &host_network)
	    != EIGRP_RESULT_SUCCESS)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	rn = route_node_lookup(eigrp->networks, &host_network);
	if (!rn)
		return EIGRP_RESULT_SUCCESS;

	*exists = (rn->info != NULL);
	route_unlock_node(rn);
	return EIGRP_RESULT_SUCCESS;
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
	if (eigrp_frr_prefix_export(network, &host_network)
	    != EIGRP_RESULT_SUCCESS)
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
	if (eigrp_frr_prefix_export(network, &host_network)
	    != EIGRP_RESULT_SUCCESS)
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

		connected_prefix = ei->address;

		for (rn = route_top(eigrp->networks); rn; rn = route_next(rn)) {
			if (!rn->info)
				continue;
			if (eigrp_frr_prefix_import(&rn->p, &configured_network)
			    != EIGRP_RESULT_SUCCESS)
				continue;
			if (!eigrp->af_vectors.network_interface_match
			    || !eigrp->af_vectors.network_interface_match(
				    &configured_network, &connected_prefix))
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

eigrp_result_t eigrp_southbound_redistribute_update(
	eigrp_instance_t *eigrp, const char *protocol,
	const eigrp_metric_values_t *metric, const char *route_map)
{
	return eigrp_zebra_redistribute_update(eigrp, protocol, metric, route_map);
}

eigrp_result_t eigrp_southbound_redistribute_delete(
	eigrp_instance_t *eigrp, const char *protocol)
{
	return eigrp_zebra_redistribute_delete(eigrp, protocol);
}

static int eigrp_southbound_filter_slot(eigrp_offset_direction_t direction)
{
	return direction == EIGRP_OFFSET_OUT ? EIGRP_FILTER_OUT : EIGRP_FILTER_IN;
}

static void eigrp_southbound_distribute_timer_process(void *arg)
{
	eigrp_instance_t *eigrp = arg;

	eigrp_update_send_process_GR(eigrp, EIGRP_GR_FILTER, NULL);
}

static void eigrp_southbound_distribute_timer_interface(void *arg)
{
	eigrp_interface_t *ei = arg;

	eigrp_update_send_interface_GR(ei, EIGRP_GR_FILTER, NULL);
}

static void eigrp_southbound_distribute_schedule_process(eigrp_instance_t *eigrp)
{
	eigrp_southbound_timer_add(&eigrp->t_distribute,
				  eigrp_southbound_distribute_timer_process,
				  eigrp, 10);
}

static void eigrp_southbound_distribute_schedule_interface(eigrp_interface_t *ei)
{
	eigrp_southbound_timer_add(&ei->t_distribute,
				  eigrp_southbound_distribute_timer_interface,
				  ei, 10);
}

eigrp_result_t eigrp_southbound_distribute_list_update(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name)
{
	eigrp_interface_t *ei = NULL;
	struct access_list *access = NULL;
	struct prefix_list *prefix = NULL;
	bool changed = false;
	int slot;

	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (!name || !name[0]
	    || (direction != EIGRP_OFFSET_IN && direction != EIGRP_OFFSET_OUT))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (type != EIGRP_DISTRIBUTE_ACCESS_LIST
	    && type != EIGRP_DISTRIBUTE_PREFIX_LIST)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (interface_name) {
		ei = eigrp_intf_lookup_by_name(eigrp, interface_name);
		/* Configuration may precede creation of the runtime interface. */
		if (!ei)
			return EIGRP_RESULT_SUCCESS;
	}

	slot = eigrp_southbound_filter_slot(direction);
	if (type == EIGRP_DISTRIBUTE_ACCESS_LIST) {
		access = access_list_lookup(AFI_IP, name);
		if (ei) {
			changed = ei->list[slot] != access;
			ei->list[slot] = access;
		} else {
			changed = eigrp->list[slot] != access;
			eigrp->list[slot] = access;
		}
	} else {
		prefix = prefix_list_lookup(AFI_IP, name);
		if (ei) {
			changed = ei->prefix[slot] != prefix;
			ei->prefix[slot] = prefix;
		} else {
			changed = eigrp->prefix[slot] != prefix;
			eigrp->prefix[slot] = prefix;
		}
	}

	if (changed) {
		if (ei)
			eigrp_southbound_distribute_schedule_interface(ei);
		else
			eigrp_southbound_distribute_schedule_process(eigrp);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_southbound_distribute_list_delete(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, eigrp_offset_direction_t direction,
	const char *interface_name)
{
	eigrp_interface_t *ei = NULL;
	bool changed = false;
	int slot;

	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	if (!name || !name[0]
	    || (direction != EIGRP_OFFSET_IN && direction != EIGRP_OFFSET_OUT))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (type != EIGRP_DISTRIBUTE_ACCESS_LIST
	    && type != EIGRP_DISTRIBUTE_PREFIX_LIST)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (interface_name) {
		ei = eigrp_intf_lookup_by_name(eigrp, interface_name);
		if (!ei)
			return EIGRP_RESULT_NOT_FOUND;
	}

	slot = eigrp_southbound_filter_slot(direction);
	if (type == EIGRP_DISTRIBUTE_ACCESS_LIST) {
		if (ei) {
			changed = ei->list[slot] != NULL;
			ei->list[slot] = NULL;
		} else {
			changed = eigrp->list[slot] != NULL;
			eigrp->list[slot] = NULL;
		}
	} else {
		if (ei) {
			changed = ei->prefix[slot] != NULL;
			ei->prefix[slot] = NULL;
		} else {
			changed = eigrp->prefix[slot] != NULL;
			eigrp->prefix[slot] = NULL;
		}
	}

	if (!changed)
		return EIGRP_RESULT_NOT_FOUND;
	if (ei)
		eigrp_southbound_distribute_schedule_interface(ei);
	else
		eigrp_southbound_distribute_schedule_process(eigrp);
	return EIGRP_RESULT_SUCCESS;
}
