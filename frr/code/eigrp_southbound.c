// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP host southbound abstraction.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <zebra.h>
#include <lib/version.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp.h"
#include "eigrp_log.h"
#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_instance.h"
#include "eigrp_metric.h"
#include "eigrp_network.h"
#include "eigrp_packet.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
#include "eigrp_frr_rib.h"
#include "eigrp_frr.h"
#include "eigrp_frr_policy.h"
#include "eigrp_southbound.h"

#include "vrf.h"
#include "frrevent.h"
#include "workqueue.h"
#include "lib/sockopt.h"
#include "keychain.h"
#include "lib/libfrr.h"

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_WORK_QUEUE, "EIGRP work queue");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_WORK_QUEUE_NAME, "EIGRP work queue name");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_EVENT, "EIGRP host event");

struct eigrp_frr_socket {
	eigrp_instance_t *eigrp;
	int fd;
	uint32_t maxsndbuflen;
	struct eigrp_frr_socket *next;
};

static struct eigrp_frr_socket *eigrp_frr_sockets;

static struct eigrp_frr_socket *eigrp_frr_socket_find(
	const eigrp_instance_t *eigrp)
{
	struct eigrp_frr_socket *socket;

	for (socket = eigrp_frr_sockets; socket; socket = socket->next)
		if (socket->eigrp == eigrp)
			return socket;
	return NULL;
}

int eigrp_southbound_socket_fd_get(const eigrp_instance_t *eigrp)
{
	struct eigrp_frr_socket *socket = eigrp_frr_socket_find(eigrp);

	return socket ? socket->fd : -1;
}

static void eigrp_frr_socket_forget(eigrp_instance_t *eigrp)
{
	struct eigrp_frr_socket **cursor;
	struct eigrp_frr_socket *socket;

	for (cursor = &eigrp_frr_sockets; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->eigrp != eigrp)
			continue;
		socket = *cursor;
		*cursor = socket->next;
		if (socket->fd >= 0)
			close(socket->fd);
		free(socket);
		return;
	}
}

void eigrp_rib_init(void)
{
	eigrp_zebra_init();
}

void eigrp_rib_finish(void)
{
	eigrp_zebra_stop();
}

void eigrp_rib_instance_delete(eigrp_instance_t *eigrp)
{
	eigrp_zebra_instance_delete(eigrp);
}

eigrp_result_t eigrp_rib_route_add(eigrp_instance_t *eigrp,
					const eigrp_rib_route_t *route)
{
	if (!route)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	return eigrp_zebra_route_add(eigrp, route);
}

eigrp_result_t eigrp_rib_route_del(
	eigrp_instance_t *eigrp, const eigrp_prefix_t *prefix)
{
	return eigrp_zebra_route_del(eigrp, prefix);
}

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

	if (!event) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR event callback has no EIGRP event context");
		return;
	}
	if (!event->callback) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR event callback has no bound EIGRP callback");
		if (event->owner && *event->owner == event)
			*event->owner = NULL;
		event->host_event = NULL;
		XFREE(MTYPE_EIGRP_EVENT, event);
		return;
	}

	callback = event->callback;
	arg = event->arg;
	if (event->owner && *event->owner == event)
		*event->owner = NULL;
	event->host_event = NULL;
	XFREE(MTYPE_EIGRP_EVENT, event);

	callback(arg);
}

static eigrp_event_t *eigrp_southbound_event_prepare(
	eigrp_event_t **owner, eigrp_event_callback_t callback, void *arg)
{
	eigrp_event_t *event;

	if (!owner) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR event registration has no EIGRP event owner");
		return NULL;
	}
	if (!callback) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR event registration has no EIGRP callback");
		return NULL;
	}

	eigrp_sys_event_cancel(owner);
	event = XCALLOC(MTYPE_EIGRP_EVENT, sizeof(*event));
	event->callback = callback;
	event->arg = arg;
	event->owner = owner;
	*owner = event;
	return event;
}

void eigrp_sys_event_cancel(eigrp_event_t **owner)
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

void eigrp_sys_event_add(eigrp_event_t **owner,
				eigrp_event_callback_t callback, void *arg)
{
	eigrp_event_t *event = eigrp_southbound_event_prepare(owner, callback, arg);

	if (event)
		event_add_event(eigrpd_event, eigrp_southbound_event_run, event, 0,
				&event->host_event);
}

void eigrp_sys_timer_add(eigrp_event_t **owner,
				eigrp_event_callback_t callback, void *arg,
				uint32_t delay_msec)
{
	eigrp_event_t *event = eigrp_southbound_event_prepare(owner, callback, arg);

	if (event)
		event_add_timer_msec(eigrpd_event, eigrp_southbound_event_run, event,
				     delay_msec, &event->host_event);
}

void eigrp_sys_read_add(eigrp_event_t **owner, eigrp_instance_t *eigrp,
			eigrp_event_callback_t callback, void *arg)
{
	struct eigrp_frr_socket *socket = eigrp_frr_socket_find(eigrp);
	eigrp_event_t *event;

	if (!socket || socket->fd < 0)
		return;
	event = eigrp_southbound_event_prepare(owner, callback, arg);
	if (event)
		event_add_read(eigrpd_event, eigrp_southbound_event_run, event, socket->fd,
			       &event->host_event);
}

void eigrp_sys_write_add(eigrp_event_t **owner, eigrp_instance_t *eigrp,
			 eigrp_event_callback_t callback, void *arg)
{
	struct eigrp_frr_socket *socket = eigrp_frr_socket_find(eigrp);
	eigrp_event_t *event;

	if (!socket || socket->fd < 0)
		return;
	event = eigrp_southbound_event_prepare(owner, callback, arg);
	if (event)
		event_add_write(eigrpd_event, eigrp_southbound_event_run, event, socket->fd,
				&event->host_event);
}

uint32_t eigrp_sys_timer_remaining_seconds(const eigrp_event_t *event)
{
	if (!event || !event->host_event)
		return 0;
	return event_timer_remain_second(event->host_event);
}

uint64_t eigrp_sys_monotime_msec(void)
{
	struct timeval now;

	monotime(&now);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_usec / 1000U;
}

uint64_t eigrp_sys_wallclock_msec(void)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_usec / 1000U;
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
	eigrp_work_queue_t *queue;
	eigrp_work_queue_result_t result;

	if (!host_queue) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work-queue callback has no host queue");
		return WQ_SUCCESS;
	}
	queue = host_queue->spec.data;
	if (!queue) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work-queue callback has no EIGRP queue context");
		return WQ_SUCCESS;
	}
	if (!queue->workfunc) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work queue %s has no bound EIGRP worker",
				queue->name ? queue->name : "<unnamed>");
		return WQ_SUCCESS;
	}

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

eigrp_work_queue_t *eigrp_sys_work_queue_create(eigrp_instance_t *eigrp,
					 const char *name,
					 eigrp_work_queue_func_t workfunc,
					 eigrp_work_queue_delete_func_t deletefunc)
{
	eigrp_work_queue_t *queue;

	if (!eigrp) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work-queue registration has no EIGRP instance");
		return NULL;
	}
	if (!name || !name[0]) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work-queue registration has no queue name");
		return NULL;
	}
	if (!workfunc) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work-queue registration %s has no worker", name);
		return NULL;
	}

	queue = XCALLOC(MTYPE_EIGRP_WORK_QUEUE, sizeof(*queue));
	queue->eigrp = eigrp;
	queue->name = XSTRDUP(MTYPE_EIGRP_WORK_QUEUE_NAME, name);
	queue->workfunc = workfunc;
	queue->deletefunc = deletefunc;

	eigrp_work_queue_host_create(queue);
	return queue;
}

void eigrp_sys_work_queue_free(eigrp_work_queue_t *queue)
{
	if (!queue)
		return;

	if (queue->host_queue)
		work_queue_free_and_null(&queue->host_queue);
	XFREE(MTYPE_EIGRP_WORK_QUEUE_NAME, queue->name);
	XFREE(MTYPE_EIGRP_WORK_QUEUE, queue);
}

void eigrp_sys_work_queue_clear(eigrp_work_queue_t *queue)
{
	if (!queue)
		return;

	if (queue->host_queue)
		work_queue_free_and_null(&queue->host_queue);
	eigrp_work_queue_host_create(queue);
}

void eigrp_sys_work_queue_enqueue(eigrp_work_queue_t *queue, void *data)
{
	if (!queue) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work-queue enqueue has no EIGRP queue");
		return;
	}
	if (!queue->host_queue) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work-queue enqueue %s has no host queue",
				queue->name ? queue->name : "<unnamed>");
		return;
	}
	if (!data) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR work-queue enqueue %s has no work item",
				queue->name ? queue->name : "<unnamed>");
		return;
	}

	work_queue_add(queue->host_queue, data);
}

eigrp_instance_t *eigrp_sys_work_queue_instance(eigrp_work_queue_t *queue)
{
	return queue ? queue->eigrp : NULL;
}


eigrp_result_t eigrp_sys_socket_open(eigrp_instance_t *eigrp)
{
	struct eigrp_frr_socket *socket;
	struct vrf *vrf;
	eigrp_afi_t afi;
	eigrp_result_t result = EIGRP_RESULT_SUCCESS;
	int family;
	int fd = -1;

	if (!eigrp)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_frr_socket_find(eigrp))
		return EIGRP_RESULT_SUCCESS;

	vrf = vrf_lookup_by_id((vrf_id_t)eigrp_instance_vrf_id(eigrp));
	if (!vrf)
		return EIGRP_RESULT_NOT_FOUND;

	afi = eigrp_instance_afi(eigrp);
	family = afi == EIGRP_AFI_IPV6 ? AF_INET6 : AF_INET;
	frr_with_privs (&eigrpd_privs) {
		fd = vrf_socket(family, SOCK_RAW, IPPROTO_EIGRPIGP, vrf->vrf_id,
				vrf->vrf_id != VRF_DEFAULT ? vrf->name : NULL);
	}
	if (fd < 0) {
		zlog_err("EIGRP socket: %s", safe_strerror(errno));
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	if (afi == EIGRP_AFI_IPV6)
		result = eigrp_southbound_ipv6_socket_configure(fd);
	else
		result = eigrp_southbound_ipv4_socket_configure(fd);
	if (result != EIGRP_RESULT_SUCCESS) {
		close(fd);
		return result;
	}

	socket = calloc(1, sizeof(*socket));
	if (!socket) {
		close(fd);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	socket->eigrp = eigrp;
	socket->fd = fd;
	socket->maxsndbuflen = (uint32_t)getsockopt_so_sendbuf(fd);
	socket->next = eigrp_frr_sockets;
	eigrp_frr_sockets = socket;
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_sys_socket_close(eigrp_instance_t *eigrp)
{
	if (eigrp)
		eigrp_frr_socket_forget(eigrp);
}

void eigrp_sys_socket_send_buffer_ensure(eigrp_instance_t *eigrp,
					uint32_t minimum)
{
	struct eigrp_frr_socket *socket = eigrp_frr_socket_find(eigrp);
	int new_size;

	if (!socket || socket->fd < 0 || socket->maxsndbuflen >= minimum)
		return;
	setsockopt_so_sendbuf(socket->fd, minimum);
	new_size = getsockopt_so_sendbuf(socket->fd);
	if (new_size < 0 || new_size < (int)minimum)
		zlog_warn("%s: tried to set SO_SNDBUF to %u, but got %d",
			  __func__, minimum, new_size);
	if (new_size >= 0)
		socket->maxsndbuflen = (uint32_t)new_size;
}

bool eigrp_sys_router_id_get(eigrp_instance_t *eigrp, uint32_t *router_id)
{
	(void)eigrp;
	if (!router_id)
		return false;
	*router_id = ntohl(router_id_zebra.s_addr);
	return router_id_zebra.s_addr != INADDR_ANY;
}



int eigrp_sys_multicast_interface_update(eigrp_operation_t operation,
                                      eigrp_instance_t *eigrp,
                                      eigrp_intf_t *ei)
{
	if (operation != EIGRP_SET)
		return -1;
	if (!eigrp || !ei)
		return -1;
	if (eigrp_instance_afi(eigrp) == EIGRP_AFI_IPV6)
		return eigrp_southbound_ipv6_multicast_interface_update(operation, eigrp, ei);
	return eigrp_southbound_ipv4_multicast_interface_update(operation, eigrp, ei);
}

int eigrp_sys_multicast_join(eigrp_instance_t *eigrp, eigrp_intf_t *ei)
{
	if (!eigrp || !ei)
		return -1;
	if (eigrp_instance_afi(eigrp) == EIGRP_AFI_IPV6)
		return eigrp_southbound_ipv6_multicast_join(eigrp, ei);
	return eigrp_southbound_ipv4_multicast_join(eigrp, ei);
}

int eigrp_sys_multicast_leave(eigrp_instance_t *eigrp, eigrp_intf_t *ei)
{
	if (!eigrp || !ei)
		return -1;
	if (eigrp_instance_afi(eigrp) == EIGRP_AFI_IPV6)
		return eigrp_southbound_ipv6_multicast_leave(eigrp, ei);
	return eigrp_southbound_ipv4_multicast_leave(eigrp, ei);
}

eigrp_result_t eigrp_sys_vrf_resolve(const char *vrf_name,
					     eigrp_vrf_id_t *vrf_id)
{
	struct vrf *vrf;

	if (!vrf_name || !vrf_name[0] || !vrf_id)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	vrf = vrf_lookup_by_name(vrf_name);
	if (!vrf)
		return EIGRP_RESULT_NOT_FOUND;
	*vrf_id = (eigrp_vrf_id_t)vrf->vrf_id;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_sys_interface_walk(
	eigrp_instance_t *eigrp, eigrp_sys_interface_walk_cb callback,
	void *arg)
{
	eigrp_intf_runtime_state_t state;
	struct connected *co;
	struct interface *ifp;
	struct vrf *vrf;

	if (!eigrp || !callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	vrf = vrf_lookup_by_id((vrf_id_t)eigrp->vrf_id);
	if (!vrf)
		return EIGRP_RESULT_NOT_FOUND;

	FOR_ALL_INTERFACES (vrf, ifp) {
		frr_each (if_connected, ifp->connected, co) {
			if (!co->address) {
				eigrp_log(EIGRP_LOG_ERROR,
					"FRR interface walk %s[%u] has a connected entry with no address",
					ifp->name, ifp->ifindex);
				continue;
			}
			if (eigrp_frr_interface_state_import(
				    ifp, co->address,
				    CHECK_FLAG(co->flags, ZEBRA_IFA_SECONDARY),
				    &state) != EIGRP_RESULT_SUCCESS) {
				eigrp_log(EIGRP_LOG_ERROR,
					"FRR interface walk could not normalize %s[%u] address family %u/%u",
					ifp->name, ifp->ifindex,
					(unsigned)co->address->family,
					(unsigned)co->address->prefixlen);
				continue;
			}
			callback(&state, arg);
		}
	}
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_southbound_interface_notify(struct interface *ifp)
{
	eigrp_intf_runtime_state_t state;
	eigrp_vrf_id_t vrf_id;
	struct connected *co;

	if (!ifp) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR interface notification has no interface object");
		return;
	}
	vrf_id = ifp->vrf ? (eigrp_vrf_id_t)ifp->vrf->vrf_id
			     : EIGRP_VRF_DEFAULT;

	frr_each (if_connected, ifp->connected, co) {
		if (!co->address) {
			eigrp_log(EIGRP_LOG_ERROR,
				"FRR interface notification %s[%u] has a connected entry with no address",
				ifp->name, ifp->ifindex);
			continue;
		}
		if (eigrp_frr_interface_state_import(
			    ifp, co->address,
			    CHECK_FLAG(co->flags, ZEBRA_IFA_SECONDARY), &state)
		    != EIGRP_RESULT_SUCCESS) {
			eigrp_log(EIGRP_LOG_ERROR,
				"FRR interface notification could not normalize %s[%u] address family %u/%u",
				ifp->name, ifp->ifindex,
				(unsigned)co->address->family,
				(unsigned)co->address->prefixlen);
			continue;
		}
		eigrp_sys_intf_update(vrf_id, &state);
	}
}

static int eigrp_southbound_if_real(struct interface *ifp)
{
	eigrp_southbound_interface_notify(ifp);
	return 0;
}

static int eigrp_southbound_if_up(struct interface *ifp)
{
	eigrp_southbound_interface_notify(ifp);
	return 0;
}

static int eigrp_southbound_if_down(struct interface *ifp)
{
	eigrp_vrf_id_t vrf_id;

	if (!ifp) {
		eigrp_log(EIGRP_LOG_ERROR, "FRR interface-down notification has no interface object");
		return 0;
	}
	vrf_id = ifp->vrf ? (eigrp_vrf_id_t)ifp->vrf->vrf_id
			     : EIGRP_VRF_DEFAULT;
	eigrp_sys_intf_down(
		vrf_id, ifp->ifindex, ifp->name, eigrp_frr_interface_type(ifp),
		ifp->bandwidth, ifp->mtu);
	return 0;
}

static int eigrp_southbound_if_unreal(struct interface *ifp)
{
	eigrp_vrf_id_t vrf_id;

	if (!ifp) {
		eigrp_log(EIGRP_LOG_ERROR,
			"FRR interface-delete notification has no interface object");
		return 0;
	}
	vrf_id = ifp->vrf ? (eigrp_vrf_id_t)ifp->vrf->vrf_id
			     : EIGRP_VRF_DEFAULT;
	eigrp_sys_intf_remove(
		vrf_id, ifp->ifindex, EIGRP_INTERFACE_REMOVE_HOST);
	return 0;
}

void eigrp_sys_runtime_init(void)
{
	hook_register_prio(if_real, 0, eigrp_southbound_if_real);
	hook_register_prio(if_up, 0, eigrp_southbound_if_up);
	hook_register_prio(if_down, 0, eigrp_southbound_if_down);
	hook_register_prio(if_unreal, 0, eigrp_southbound_if_unreal);
}

void eigrp_sys_runtime_finish(void)
{
	vrf_terminate();
	frr_fini();
}

eigrp_result_t eigrp_rib_redistribute_add(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source)
{
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	return eigrp_zebra_redistribute_update(eigrp, source);
}

eigrp_result_t eigrp_rib_redistribute_remove(
	eigrp_instance_t *eigrp, const eigrp_redist_source_t *source)
{
	if (!eigrp)
		return EIGRP_RESULT_NOT_FOUND;
	return eigrp_zebra_redistribute_delete(eigrp, source);
}

void eigrp_sys_policy_init(void)
{
	eigrp_policy_init();
}

void eigrp_sys_policy_finish(void)
{
	eigrp_policy_finish();
}

eigrp_result_t eigrp_sys_policy_instance_create(eigrp_instance_t *eigrp)
{
	return eigrp_policy_instance_create(eigrp);
}

void eigrp_sys_policy_instance_delete(eigrp_instance_t *eigrp)
{
	eigrp_policy_instance_delete(eigrp);
}

eigrp_result_t eigrp_sys_filter_evaluate(
	eigrp_instance_t *eigrp, eigrp_distribute_list_type_t type,
	const char *name, const eigrp_prefix_t *prefix,
	eigrp_filter_decision_t *decision)
{
	return eigrp_policy_filter_evaluate(eigrp, type, name, prefix, decision);
}

eigrp_result_t eigrp_sys_summary_leak_map_evaluate(
	eigrp_instance_t *eigrp, const char *name, const eigrp_prefix_t *prefix,
	eigrp_filter_decision_t *decision)
{
	return eigrp_policy_summary_leak_map_evaluate(eigrp, name, prefix, decision);
}

eigrp_result_t eigrp_sys_redistribute_route_map_evaluate(
	eigrp_instance_t *eigrp, const char *name,
	const eigrp_rib_route_t *route,
	eigrp_filter_decision_t *decision)
{
	return eigrp_policy_redistribute_route_map_evaluate(eigrp, name, route,
						    decision);
}









int eigrp_sys_packet_send(eigrp_instance_t *eigrp,
                          eigrp_intf_t *ei,
                          const eigrp_address_t *destination,
                          const uint8_t *payload, size_t length)
{
	if (!eigrp || !destination)
		return -1;
	switch (destination->afi) {
	case EIGRP_AFI_IPV4:
		return eigrp_sys_ipv4_packet_send(eigrp, ei, destination, payload,
					  length);
	case EIGRP_AFI_IPV6:
		return eigrp_sys_ipv6_packet_send(eigrp, ei, destination, payload,
					  length);
	default:
		return -1;
	}
}

bool eigrp_sys_packet_receive(eigrp_instance_t *eigrp,
                              uint8_t *buffer, size_t capacity,
                              size_t *received_length,
                              eigrp_ifindex_t *ifindex,
                              eigrp_address_t *source,
                              eigrp_address_t *destination,
                              eigrp_packet_rx_meta_t *meta)
{
	if (!eigrp)
		return false;
	if (eigrp_instance_afi(eigrp) == EIGRP_AFI_IPV6)
		return eigrp_sys_ipv6_packet_receive(
			eigrp, buffer, capacity, received_length, ifindex, source,
			destination, meta);
	return eigrp_sys_ipv4_packet_receive(eigrp, buffer, capacity,
					     received_length, ifindex, source,
					     destination, meta);
}

bool eigrp_sys_auth_key_lookup(const char *keychain_name,
                                      uint32_t *key_id, char *key_string,
                                      size_t key_string_size)
{
	struct keychain *keychain;
	struct key *key;

	if (!keychain_name || !keychain_name[0] || !key_id || !key_string
	    || key_string_size == 0)
		return false;
	keychain = keychain_lookup(keychain_name);
	if (!keychain)
		return false;
	key = key_lookup_for_send(keychain);
	if (!key || !key->string)
		return false;
	*key_id = key->index;
	strlcpy(key_string, key->string, key_string_size);
	return true;
}

void eigrp_sys_software_version(uint8_t *major, uint8_t *minor)
{
	unsigned int maj = 0, min = 0;
	if (major)
		*major = 0;
	if (minor)
		*minor = 0;
	if (sscanf(VERSION, "%u.%u", &maj, &min) == 2) {
		if (major)
			*major = (uint8_t)maj;
		if (minor)
			*minor = (uint8_t)min;
	}
}
