// SPDX-License-Identifier: GPL-2.0-or-later
/* OpenWrt Linux kernel FIB installation and route-source observation.
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/rtnetlink.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "eigrp_owrt_rib.h"
#include "eigrp_owrt_runtime.h"

#ifndef RTPROT_EIGRP
#define RTPROT_EIGRP 192
#endif
#define EIGRP_OWRT_RIB_MESSAGE_SIZE 8192U
#define EIGRP_OWRT_RIB_DUMP_SIZE (256U * 1024U)
#define EIGRP_OWRT_RIB_MAX_HOPS 32U

typedef struct eigrp_owrt_rib_entry {
	eigrp_instance_t *owner; /* nonnull for EIGRP-installed routes */
	bool present; /* kernel installation confirmed */
	bool observed; /* temporary during authoritative FIB dump */
	eigrp_rib_route_t route;
	eigrp_rib_nexthop_t *hops;
	struct eigrp_owrt_rib_entry *next;
} eigrp_owrt_rib_entry_t;

typedef struct eigrp_owrt_rib_subscription {
	eigrp_instance_t *instance;
	eigrp_redist_source_t source;
	struct eigrp_owrt_rib_subscription *next;
} eigrp_owrt_rib_subscription_t;

static struct {
	pthread_mutex_t lock;
	pthread_mutex_t scan_lock;
	pthread_t watcher_thread;
	bool started;
	bool running;
	int watcher_fd;
	int wake_read;
	int wake_write;
	eigrp_owrt_rib_entry_t *installed;
	eigrp_owrt_rib_entry_t *sources;
	eigrp_owrt_rib_subscription_t *subscriptions;
} rib = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.scan_lock = PTHREAD_MUTEX_INITIALIZER,
	.watcher_fd = -1,
	.wake_read = -1,
	.wake_write = -1,
};

static bool eigrp_owrt_rib_prefix_equal(const eigrp_prefix_t *a,
	const eigrp_prefix_t *b)
{
	size_t width;
	if (a->address.afi != b->address.afi ||
	    a->prefix_length != b->prefix_length)
		return false;
	width = a->address.afi == EIGRP_AFI_IPV4 ? 4 : 16;
	return memcmp(a->address.bytes, b->address.bytes, width) == 0;
}

static bool eigrp_owrt_rib_source_equal(const eigrp_redist_source_t *a,
	const eigrp_redist_source_t *b)
{
	return a->protocol == b->protocol &&
		a->route_instance == b->route_instance;
}

static void eigrp_owrt_rib_entry_free(eigrp_owrt_rib_entry_t *entry)
{
	if (entry) {
		free(entry->hops);
		free(entry);
	}
}

static void eigrp_owrt_rib_list_free(eigrp_owrt_rib_entry_t *list)
{
	while (list) {
		eigrp_owrt_rib_entry_t *next = list->next;
		eigrp_owrt_rib_entry_free(list);
		list = next;
	}
}

static eigrp_owrt_rib_entry_t *eigrp_owrt_rib_entry_new(
	eigrp_instance_t *owner, const eigrp_rib_route_t *route)
{
	eigrp_owrt_rib_entry_t *entry;
	if (!route || route->nexthop_count > EIGRP_OWRT_RIB_MAX_HOPS ||
	    (route->nexthop_count && !route->nexthops))
		return NULL;
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->owner = owner;
	entry->present = false;
	entry->route = *route;
	if (route->nexthop_count) {
		entry->hops = calloc(route->nexthop_count, sizeof(*entry->hops));
		if (!entry->hops) {
			free(entry);
			return NULL;
		}
		memcpy(entry->hops, route->nexthops,
			 sizeof(*entry->hops) * route->nexthop_count);
	}
	entry->route.nexthops = entry->hops;
	return entry;
}

static bool eigrp_owrt_rib_route_equal(const eigrp_rib_route_t *a,
	const eigrp_rib_route_t *b)
{
	size_t i;
	if (!eigrp_owrt_rib_prefix_equal(&a->prefix, &b->prefix) ||
	    a->metric != b->metric || a->tag != b->tag ||
	    a->nexthop_count != b->nexthop_count)
		return false;
	for (i = 0; i < a->nexthop_count; i++) {
		const eigrp_rib_nexthop_t *x = &a->nexthops[i], *y = &b->nexthops[i];
		if (x->ifindex != y->ifindex || x->gateway_present != y->gateway_present ||
		    x->weight != y->weight ||
		    (x->gateway_present &&
		     (x->gateway.afi != y->gateway.afi ||
		      memcmp(x->gateway.bytes, y->gateway.bytes,
		         x->gateway.afi == EIGRP_AFI_IPV4 ? 4 : 16) != 0)))
			return false;
	}
	return true;
}

static eigrp_owrt_rib_entry_t *eigrp_owrt_rib_source_find(
	eigrp_owrt_rib_entry_t *entries, const eigrp_rib_route_t *route)
{
	for (; entries; entries = entries->next)
		if (eigrp_owrt_rib_prefix_equal(&entries->route.prefix, &route->prefix) &&
		    eigrp_owrt_rib_source_equal(&entries->route.redist.source,
			&route->redist.source) && entries->route.metric == route->metric)
			return entries;
	return NULL;
}

static int eigrp_owrt_rib_socket_open(uint32_t groups)
{
	int fd;
	struct sockaddr_nl local = { .nl_family = AF_NETLINK, .nl_groups = groups };
	struct timeval timeout = { .tv_sec = 3 };
	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static eigrp_result_t eigrp_owrt_rib_kernel_apply(bool add, bool replace,
	const eigrp_rib_route_t *route)
{
	char message[EIGRP_OWRT_RIB_MESSAGE_SIZE];
	char response[4096];
	struct nlmsghdr *nl = (void *)message;
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	int fd, length;
	static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
	static uint32_t seq;
	uint32_t request_seq;
	struct sockaddr_nl local = {0};
	socklen_t local_length = sizeof(local);

	length = eigrp_owrt_rib_message_build(add, replace, route,
		message, sizeof(message));
	if (length < 0)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	fd = eigrp_owrt_rib_socket_open(0);
	if (fd < 0)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	if (getsockname(fd, (struct sockaddr *)&local, &local_length) != 0) {
		close(fd);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	pthread_mutex_lock(&seq_lock);
	request_seq = ++seq;
	pthread_mutex_unlock(&seq_lock);
	nl->nlmsg_seq = request_seq;
	if (sendto(fd, message, (size_t)length, 0,
	    (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
		close(fd);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	for (;;) {
		struct nlmsghdr *reply;
		int remaining, received = recv(fd, response, sizeof(response), 0);
		if (received <= 0) {
			close(fd);
			return EIGRP_RESULT_INTERNAL_FAILURE;
		}
		remaining = received;
		for (reply = (void *)response; NLMSG_OK(reply, remaining);
		     reply = NLMSG_NEXT(reply, remaining)) {
			struct nlmsgerr *error;
			int code;
			if (reply->nlmsg_seq != request_seq || reply->nlmsg_pid != local.nl_pid)
				continue;
			if (reply->nlmsg_type != NLMSG_ERROR ||
			    NLMSG_PAYLOAD(reply, 0) < sizeof(*error))
				continue;
			error = NLMSG_DATA(reply);
			code = error->error;
			close(fd);
			if (code == 0)
				return EIGRP_RESULT_SUCCESS;
			fprintf(stderr, "OpenEIGRP OpenWrt: kernel route %s: %s\n",
				add ? "install" : "withdraw", strerror(-code));
			if (code == -EEXIST)
				return EIGRP_RESULT_CONFLICT;
			if (code == -ESRCH || code == -ENOENT)
				return EIGRP_RESULT_NOT_FOUND;
			return EIGRP_RESULT_INTERNAL_FAILURE;
		}
	}
}

static eigrp_owrt_rib_entry_t *eigrp_owrt_rib_install_find(
	eigrp_instance_t *instance, const eigrp_prefix_t *prefix)
{
	eigrp_owrt_rib_entry_t *entry;
	for (entry = rib.installed; entry; entry = entry->next)
		if (entry->owner == instance &&
		    eigrp_owrt_rib_prefix_equal(&entry->route.prefix, prefix))
			return entry;
	return NULL;
}

void eigrp_rib_init(void)
{
	/* The adapter uses process-static locks. The service owner explicitly
	 * starts the asynchronous watcher after core initialization. */
}

eigrp_result_t eigrp_rib_route_add(eigrp_instance_t *instance,
	const eigrp_rib_route_t *route)
{
	eigrp_owrt_rib_entry_t *copy, *old;
	eigrp_result_t result;
	if (!instance || !route ||
	    eigrp_instance_vrf_id(instance) != EIGRP_VRF_DEFAULT ||
	    eigrp_instance_afi(instance) != route->prefix.address.afi)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	copy = eigrp_owrt_rib_entry_new(instance, route);
	if (!copy)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	pthread_mutex_lock(&rib.lock);
	old = eigrp_owrt_rib_install_find(instance, &route->prefix);
	if (old && old->present &&
	    eigrp_owrt_rib_route_equal(&old->route, route)) {
		pthread_mutex_unlock(&rib.lock);
		eigrp_owrt_rib_entry_free(copy);
		return EIGRP_RESULT_SUCCESS;
	}
	/* A change to RTA_PRIORITY changes Linux's route key. Withdraw the
	 * old key first, otherwise REPLACE may leave two installed routes. If
	 * the new add fails, attempt to restore the previously selected route. */
	if (old && old->present &&
	    (old->route.metric > UINT32_MAX ? UINT32_MAX : (uint32_t)old->route.metric) !=
	    (route->metric > UINT32_MAX ? UINT32_MAX : (uint32_t)route->metric)) {
		result = eigrp_owrt_rib_kernel_apply(false, false, &old->route);
		if (result != EIGRP_RESULT_SUCCESS) {
			pthread_mutex_unlock(&rib.lock);
			eigrp_owrt_rib_entry_free(copy);
			return result;
		}
		old->present = false;
		result = eigrp_owrt_rib_kernel_apply(true, false, route);
		if (result != EIGRP_RESULT_SUCCESS) {
			old->present = eigrp_owrt_rib_kernel_apply(true, false,
				&old->route) == EIGRP_RESULT_SUCCESS;
			if (!old->present)
				fprintf(stderr, "OpenEIGRP OpenWrt: route replacement rollback failed\n");
			pthread_mutex_unlock(&rib.lock);
			eigrp_owrt_rib_entry_free(copy);
			return result;
		}
	} else {
		/* An initial add must never overwrite another route with the same
		 * key. Replace only keys this process believes it owns. */
		result = eigrp_owrt_rib_kernel_apply(true, old && old->present, route);
	}
	if (result == EIGRP_RESULT_SUCCESS) {
		if (old) {
			free(old->hops);
			old->hops = copy->hops;
			old->route = copy->route;
			old->route.nexthops = old->hops;
			old->present = true;
			copy->hops = NULL;
		} else {
			copy->next = rib.installed;
			copy->present = true;
			rib.installed = copy;
			copy = NULL;
		}
	}
	pthread_mutex_unlock(&rib.lock);
	eigrp_owrt_rib_entry_free(copy);
	return result;
}

eigrp_result_t eigrp_rib_route_del(eigrp_instance_t *instance,
	const eigrp_prefix_t *prefix)
{
	eigrp_owrt_rib_entry_t **cursor, *old;
	eigrp_result_t result;
	if (!instance || !prefix)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	pthread_mutex_lock(&rib.lock);
	for (cursor = &rib.installed; *cursor; cursor = &(*cursor)->next)
		if ((*cursor)->owner == instance &&
		    eigrp_owrt_rib_prefix_equal(&(*cursor)->route.prefix, prefix))
			break;
	if (!*cursor) {
		pthread_mutex_unlock(&rib.lock);
		return EIGRP_RESULT_NOT_FOUND;
	}
	old = *cursor;
	result = old->present ?
		eigrp_owrt_rib_kernel_apply(false, false, &old->route) :
		EIGRP_RESULT_NOT_FOUND;
	if (result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND) {
		*cursor = old->next;
		eigrp_owrt_rib_entry_free(old);
		result = EIGRP_RESULT_SUCCESS;
	}
	pthread_mutex_unlock(&rib.lock);
	return result;
}

static bool eigrp_owrt_rib_snapshot_add(eigrp_owrt_rib_entry_t **list,
	const eigrp_rib_route_t *route)
{
	eigrp_owrt_rib_entry_t *old, *copy;
	copy = eigrp_owrt_rib_entry_new(NULL, route);
	if (!copy)
		return false;
	old = eigrp_owrt_rib_source_find(*list, route);
	if (old) {
		free(old->hops);
		old->hops = copy->hops;
		old->route = copy->route;
		old->route.nexthops = old->hops;
		copy->hops = NULL;
		eigrp_owrt_rib_entry_free(copy);
	} else {
		copy->next = *list;
		*list = copy;
	}
	return true;
}

static bool eigrp_owrt_rib_snapshot_set(eigrp_owrt_rib_entry_t *updated)
{
	eigrp_owrt_rib_entry_t *previous, *item;
	eigrp_owrt_rib_subscription_t *subscription;

	/* All portable callbacks occur under the protocol lock; instance-delete
	 * and subscription mutation use the same lock ordering. Never hold the
	 * rib lock across core callbacks. */
	eigrp_owrt_runtime_enter();
	pthread_mutex_lock(&rib.lock);
	previous = rib.sources;
	rib.sources = updated;
	for (subscription = rib.subscriptions; subscription;
	     subscription = subscription->next) {
		for (item = previous; item; item = item->next) {
			eigrp_owrt_rib_entry_t *after;
			if (!eigrp_owrt_rib_source_equal(
			    &subscription->source, &item->route.redist.source) ||
			    eigrp_instance_afi(subscription->instance) !=
			    item->route.prefix.address.afi)
				continue;
			after = eigrp_owrt_rib_source_find(updated, &item->route);
			if (!after || !eigrp_owrt_rib_route_equal(&item->route, &after->route)) {
				pthread_mutex_unlock(&rib.lock);
				(void)eigrp_rib_redist_del(subscription->instance, &item->route);
				pthread_mutex_lock(&rib.lock);
			}
		}
		for (item = updated; item; item = item->next) {
			eigrp_owrt_rib_entry_t *before;
			if (!eigrp_owrt_rib_source_equal(
			    &subscription->source, &item->route.redist.source) ||
			    eigrp_instance_afi(subscription->instance) !=
			    item->route.prefix.address.afi)
				continue;
			before = eigrp_owrt_rib_source_find(previous, &item->route);
			if (!before || !eigrp_owrt_rib_route_equal(&before->route, &item->route)) {
				pthread_mutex_unlock(&rib.lock);
				(void)eigrp_rib_redist_add(subscription->instance, &item->route);
				pthread_mutex_lock(&rib.lock);
			}
		}
	}
	pthread_mutex_unlock(&rib.lock);
	eigrp_owrt_runtime_leave();
	eigrp_owrt_rib_list_free(previous);
	return true;
}

bool eigrp_owrt_rib_snapshot_reconcile(const eigrp_rib_route_t *routes,
	size_t count)
{
	eigrp_owrt_rib_entry_t *list = NULL;
	size_t i;
	if (count && !routes)
		return false;
	eigrp_owrt_runtime_enter();
	pthread_mutex_lock(&rib.scan_lock);
	for (i = 0; i < count; i++) {
		if (!eigrp_owrt_rib_snapshot_add(&list, &routes[i])) {
			eigrp_owrt_rib_list_free(list);
			pthread_mutex_unlock(&rib.scan_lock);
			eigrp_owrt_runtime_leave();
			return false;
		}
	}
	(void)eigrp_owrt_rib_snapshot_set(list);
	pthread_mutex_unlock(&rib.scan_lock);
	eigrp_owrt_runtime_leave();
	return true;
}

/* Decode only the identifying key of a kernel-owned EIGRP route.
 * This must never confuse another table or protocol with our own routes. */
static bool eigrp_owrt_rib_kernel_key(const struct nlmsghdr *msg,
    eigrp_prefix_t *prefix, uint32_t *priority)
{
    const struct rtmsg *rtm;
    struct rtattr *attr;
    uint32_t table;
    int len;
    size_t width;
    if ((msg->nlmsg_type != RTM_DELROUTE && msg->nlmsg_type != RTM_NEWROUTE) ||
        NLMSG_PAYLOAD(msg, 0) < sizeof(*rtm))
        return false;
    rtm = NLMSG_DATA(msg);
    if (rtm->rtm_protocol != RTPROT_EIGRP ||
        (rtm->rtm_family != AF_INET && rtm->rtm_family != AF_INET6))
        return false;
    width = rtm->rtm_family == AF_INET ? 4 : 16;
    if (rtm->rtm_dst_len > width * 8)
        return false;
    table = rtm->rtm_table;
    memset(prefix, 0, sizeof(*prefix));
    *priority = 0;
    prefix->address.afi = width == 4 ? EIGRP_AFI_IPV4 : EIGRP_AFI_IPV6;
    prefix->prefix_length = rtm->rtm_dst_len;
    len = RTM_PAYLOAD(msg);
    for (attr = RTM_RTA(rtm); RTA_OK(attr, len); attr = RTA_NEXT(attr, len)) {
        if (attr->rta_type == RTA_DST && RTA_PAYLOAD(attr) >= width)
            memcpy(prefix->address.bytes, RTA_DATA(attr), width);
        if (attr->rta_type == RTA_PRIORITY &&
            RTA_PAYLOAD(attr) >= sizeof(*priority))
            memcpy(priority, RTA_DATA(attr), sizeof(*priority));
        if (attr->rta_type == RTA_TABLE && RTA_PAYLOAD(attr) >= sizeof(table))
            memcpy(&table, RTA_DATA(attr), sizeof(table));
    }
    return len == 0 && table == RT_TABLE_MAIN;
}

static uint32_t eigrp_owrt_rib_metric_priority(uint64_t metric)
{
    return metric > UINT32_MAX ? UINT32_MAX : (uint32_t)metric;
}

/* Caller holds rib.lock. */
static bool eigrp_owrt_rib_mark_kernel_state_locked(
    const struct nlmsghdr *msg, bool present)
{
    eigrp_prefix_t prefix;
    uint32_t priority;
    eigrp_owrt_rib_entry_t *entry;
    bool changed = false;
    if (!eigrp_owrt_rib_kernel_key(msg, &prefix, &priority))
        return false;
    for (entry = rib.installed; entry; entry = entry->next) {
        if (eigrp_owrt_rib_metric_priority(entry->route.metric) != priority ||
            !eigrp_owrt_rib_prefix_equal(&entry->route.prefix, &prefix))
            continue;
        if (present) {
            entry->observed = true;
        } else if (entry->present) {
            entry->present = false;
            changed = true;
        }
    }
    return changed;
}

static bool eigrp_owrt_rib_mark_kernel_deleted(const struct nlmsghdr *msg)
{
    bool changed;
    if (msg->nlmsg_type != RTM_DELROUTE)
        return false;
    pthread_mutex_lock(&rib.lock);
    changed = eigrp_owrt_rib_mark_kernel_state_locked(msg, false);
    pthread_mutex_unlock(&rib.lock);
    return changed;
}

static bool eigrp_owrt_rib_dump_family(int family,
	eigrp_owrt_rib_entry_t **list)
{
	static uint32_t seq;
	struct {
		struct nlmsghdr header;
		struct rtmsg route;
	} request = {0};
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	char *buffer;
	int fd, received;
	bool complete = false;
	uint32_t request_seq;
	struct sockaddr_nl local = {0};
	socklen_t local_length = sizeof(local);
	fd = eigrp_owrt_rib_socket_open(0);
	if (fd < 0)
		return false;
	if (getsockname(fd, (struct sockaddr *)&local, &local_length) != 0) {
		close(fd);
		return false;
	}
	request_seq = ++seq; /* serialized by scan_lock */
	request.header.nlmsg_len = NLMSG_LENGTH(sizeof(request.route));
	request.header.nlmsg_type = RTM_GETROUTE;
	request.header.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
	request.header.nlmsg_seq = request_seq;
	request.route.rtm_family = (unsigned char)family;
	if (sendto(fd, &request, request.header.nlmsg_len, 0,
	    (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
		close(fd);
		return false;
	}
	buffer = malloc(EIGRP_OWRT_RIB_DUMP_SIZE);
	if (!buffer) {
		close(fd);
		return false;
	}
	while (!complete && (received = recv(fd, buffer, EIGRP_OWRT_RIB_DUMP_SIZE,
		0)) > 0) {
		struct nlmsghdr *msg;
		int remaining = received;
		for (msg = (void *)buffer; NLMSG_OK(msg, remaining);
		     msg = NLMSG_NEXT(msg, remaining)) {
			eigrp_rib_route_t route;
			eigrp_rib_nexthop_t hops[EIGRP_OWRT_RIB_MAX_HOPS];
			int result;
			if (msg->nlmsg_seq != request_seq || msg->nlmsg_pid != local.nl_pid)
				continue;
			if (msg->nlmsg_type == NLMSG_DONE) {
				if (msg->nlmsg_flags & NLM_F_DUMP_INTR)
					goto end;
				complete = true;
				break;
			}
			if (msg->nlmsg_type == NLMSG_ERROR)
				goto end;
			if (msg->nlmsg_type == RTM_NEWROUTE)
				(void)eigrp_owrt_rib_mark_kernel_state_locked(msg, true);
			result = eigrp_owrt_rib_message_decode(msg, &route,
				hops, EIGRP_OWRT_RIB_MAX_HOPS);
			if (result < 0 || (result == 1 &&
			    !eigrp_owrt_rib_snapshot_add(list, &route)))
				goto end;
		}
		if (remaining != 0 && !complete)
			goto end;
	}
end:
	free(buffer);
	close(fd);
	return complete;
}

bool eigrp_owrt_rib_refresh(void)
{
	eigrp_owrt_rib_entry_t *list = NULL, *entry;
	bool ok, replay = false;
	/* Host callback ordering must always be protocol -> scan -> rib. In
	 * particular, a core callback may add a route while the watcher scans. */
	eigrp_owrt_runtime_enter();
	pthread_mutex_lock(&rib.scan_lock);
	pthread_mutex_lock(&rib.lock);
	for (entry = rib.installed; entry; entry = entry->next)
		entry->observed = false;
	ok = eigrp_owrt_rib_dump_family(AF_INET, &list) &&
	     eigrp_owrt_rib_dump_family(AF_INET6, &list);
	if (ok) {
		for (entry = rib.installed; entry; entry = entry->next) {
			if (entry->present && !entry->observed) {
				entry->present = false;
				replay = true;
			}
		}
	}
	pthread_mutex_unlock(&rib.lock);
	if (ok)
		(void)eigrp_owrt_rib_snapshot_set(list);
	else
		eigrp_owrt_rib_list_free(list);
	pthread_mutex_unlock(&rib.scan_lock);
	if (ok && replay)
		(void)eigrp_rib_routes_replay();
	eigrp_owrt_runtime_leave();
	return ok;
}

static bool eigrp_owrt_rib_source_supported(const eigrp_redist_source_t *source)
{
	return source && source->route_instance == 0 &&
		source->protocol >= EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED &&
		source->protocol <= EIGRP_REDISTRIBUTE_PROTOCOL_BGP;
}

eigrp_result_t eigrp_rib_redistribute_add(eigrp_instance_t *instance,
	const eigrp_redist_source_t *source)
{
	eigrp_owrt_rib_subscription_t *sub, *entry;
	eigrp_owrt_rib_entry_t *route;
	if (!instance || !source)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_owrt_rib_source_supported(source) ||
	    eigrp_instance_vrf_id(instance) != EIGRP_VRF_DEFAULT)
		return EIGRP_RESULT_UNSUPPORTED;
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	entry->instance = instance;
	entry->source = *source;
	eigrp_owrt_runtime_enter();
	pthread_mutex_lock(&rib.lock);
	for (sub = rib.subscriptions; sub; sub = sub->next)
		if (sub->instance == instance &&
		    eigrp_owrt_rib_source_equal(&sub->source, source)) {
			pthread_mutex_unlock(&rib.lock);
			eigrp_owrt_runtime_leave();
			free(entry);
			return EIGRP_RESULT_SUCCESS;
		}
	entry->next = rib.subscriptions;
	rib.subscriptions = entry;
	for (route = rib.sources; route; route = route->next)
		if (eigrp_owrt_rib_source_equal(&route->route.redist.source, source) &&
		    eigrp_instance_afi(instance) == route->route.prefix.address.afi) {
			pthread_mutex_unlock(&rib.lock);
			(void)eigrp_rib_redist_add(instance, &route->route);
			pthread_mutex_lock(&rib.lock);
		}
	pthread_mutex_unlock(&rib.lock);
	eigrp_owrt_runtime_leave();
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_rib_redistribute_remove(eigrp_instance_t *instance,
	const eigrp_redist_source_t *source)
{
	eigrp_owrt_rib_subscription_t **cursor, *deleted;
	eigrp_owrt_rib_entry_t *route;
	if (!instance || !source)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	eigrp_owrt_runtime_enter();
	pthread_mutex_lock(&rib.lock);
	for (cursor = &rib.subscriptions; *cursor; cursor = &(*cursor)->next)
		if ((*cursor)->instance == instance &&
		    eigrp_owrt_rib_source_equal(&(*cursor)->source, source))
			break;
	if (!*cursor) {
		pthread_mutex_unlock(&rib.lock);
		eigrp_owrt_runtime_leave();
		return EIGRP_RESULT_NOT_FOUND;
	}
	deleted = *cursor;
	*cursor = deleted->next;
	for (route = rib.sources; route; route = route->next)
		if (eigrp_owrt_rib_source_equal(&route->route.redist.source, source) &&
		    eigrp_instance_afi(instance) == route->route.prefix.address.afi) {
			pthread_mutex_unlock(&rib.lock);
			(void)eigrp_rib_redist_del(instance, &route->route);
			pthread_mutex_lock(&rib.lock);
		}
	pthread_mutex_unlock(&rib.lock);
	eigrp_owrt_runtime_leave();
	free(deleted);
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_rib_instance_delete(eigrp_instance_t *instance)
{
	eigrp_owrt_rib_entry_t **route_cursor, *entry;
	eigrp_owrt_rib_subscription_t **sub_cursor, *sub;
	if (!instance)
		return;
	eigrp_owrt_runtime_enter();
	pthread_mutex_lock(&rib.lock);
	for (sub_cursor = &rib.subscriptions; *sub_cursor;) {
		if ((*sub_cursor)->instance != instance) {
			sub_cursor = &(*sub_cursor)->next;
			continue;
		}
		sub = *sub_cursor;
		*sub_cursor = sub->next;
		free(sub);
	}
	for (route_cursor = &rib.installed; *route_cursor;) {
		if ((*route_cursor)->owner != instance) {
			route_cursor = &(*route_cursor)->next;
			continue;
		}
		entry = *route_cursor;
		*route_cursor = entry->next;
		if (eigrp_owrt_rib_kernel_apply(false, false, &entry->route) !=
		    EIGRP_RESULT_SUCCESS)
			fprintf(stderr, "OpenEIGRP OpenWrt: unable to withdraw route during instance deletion\n");
		eigrp_owrt_rib_entry_free(entry);
	}
	pthread_mutex_unlock(&rib.lock);
	eigrp_owrt_runtime_leave();
}

static void *eigrp_owrt_rib_watch(void *arg)
{
	struct pollfd pfd[2];
	char buffer[65536];
	(void)arg;
	for (;;) {
		int fd, wakefd, ready;
		bool active, replay = false;
		pthread_mutex_lock(&rib.lock);
		active = rib.running;
		fd = rib.watcher_fd;
		wakefd = rib.wake_read;
		pthread_mutex_unlock(&rib.lock);
		if (!active)
			break;
		pfd[0] = (struct pollfd){ .fd = fd, .events = POLLIN };
		pfd[1] = (struct pollfd){ .fd = wakefd, .events = POLLIN };
		ready = poll(pfd, 2, -1);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready <= 0 || pfd[1].revents)
			break;
		if (pfd[0].revents) {
			ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
			struct nlmsghdr *msg;
			int remaining = (int)bytes;
			if (bytes <= 0) {
				if (errno == EINTR)
					continue;
				if (errno != ENOBUFS)
					break;
			} else {
				for (msg = (void *)buffer; NLMSG_OK(msg, remaining);
				     msg = NLMSG_NEXT(msg, remaining)) {
					if (eigrp_owrt_rib_mark_kernel_deleted(msg))
						replay = true;
				}
			}
			if (!eigrp_owrt_rib_refresh())
				break;
			if (replay) {
				eigrp_owrt_runtime_enter();
				(void)eigrp_rib_routes_replay();
				eigrp_owrt_runtime_leave();
			}
		}
	}
	pthread_mutex_lock(&rib.lock);
	if (rib.running) {
		rib.running = false;
		fprintf(stderr, "OpenEIGRP OpenWrt: RIB watcher stopped; restart required\n");
	}
	pthread_mutex_unlock(&rib.lock);
	return NULL;
}

bool eigrp_owrt_rib_start(void)
{
	int fd, pipefd[2];
	fd = eigrp_owrt_rib_socket_open(RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE);
	if (fd < 0)
		return false;
	if (pipe(pipefd) != 0) {
		close(fd);
		return false;
	}
	pthread_mutex_lock(&rib.lock);
	if (rib.started) {
		bool active = rib.running;
		pthread_mutex_unlock(&rib.lock);
		close(fd); close(pipefd[0]); close(pipefd[1]);
		return active;
	}
	rib.started = rib.running = true;
	rib.watcher_fd = fd;
	rib.wake_read = pipefd[0];
	rib.wake_write = pipefd[1];
	pthread_mutex_unlock(&rib.lock);
	/* Subscribed before the dump. Changes between registration and the first
	 * dump will be observed by the subsequent watch and full reconciliation. */
	if (!eigrp_owrt_rib_refresh()) {
		pthread_mutex_lock(&rib.lock);
		rib.started = rib.running = false;
		rib.watcher_fd = rib.wake_read = rib.wake_write = -1;
		pthread_mutex_unlock(&rib.lock);
		close(fd); close(pipefd[0]); close(pipefd[1]);
		return false;
	}
	pthread_mutex_lock(&rib.lock);
	if (pthread_create(&rib.watcher_thread, NULL, eigrp_owrt_rib_watch, NULL)) {
		rib.started = rib.running = false;
		rib.watcher_fd = rib.wake_read = rib.wake_write = -1;
		pthread_mutex_unlock(&rib.lock);
		close(fd); close(pipefd[0]); close(pipefd[1]);
		return false;
	}
	pthread_mutex_unlock(&rib.lock);
	return true;
}

void eigrp_owrt_rib_stop(void)
{
	int fd, rfd, wfd;
	pthread_t thread;
	pthread_mutex_lock(&rib.lock);
	if (!rib.started) {
		pthread_mutex_unlock(&rib.lock);
		return;
	}
	rib.running = false;
	thread = rib.watcher_thread;
	wfd = rib.wake_write;
	pthread_mutex_unlock(&rib.lock);
	(void)write(wfd, "x", 1);
	pthread_join(thread, NULL);
	pthread_mutex_lock(&rib.scan_lock);
	pthread_mutex_lock(&rib.lock);
	fd = rib.watcher_fd;
	rfd = rib.wake_read;
	wfd = rib.wake_write;
	rib.watcher_fd = rib.wake_read = rib.wake_write = -1;
	rib.started = false;
	pthread_mutex_unlock(&rib.lock);
	pthread_mutex_unlock(&rib.scan_lock);
	close(fd); close(rfd); close(wfd);
}

void eigrp_rib_finish(void)
{
	eigrp_owrt_rib_entry_t *routes, *sources;
	eigrp_owrt_rib_subscription_t *subs;
	eigrp_owrt_rib_stop();
	pthread_mutex_lock(&rib.scan_lock);
	pthread_mutex_lock(&rib.lock);
	routes = rib.installed;
	sources = rib.sources;
	subs = rib.subscriptions;
	rib.installed = rib.sources = NULL;
	rib.subscriptions = NULL;
	pthread_mutex_unlock(&rib.lock);
	pthread_mutex_unlock(&rib.scan_lock);
	while (routes) {
		eigrp_owrt_rib_entry_t *next = routes->next;
		if (eigrp_owrt_rib_kernel_apply(false, false, &routes->route) !=
		    EIGRP_RESULT_SUCCESS)
			fprintf(stderr, "OpenEIGRP OpenWrt: unable to withdraw route during shutdown\n");
		eigrp_owrt_rib_entry_free(routes);
		routes = next;
	}
	eigrp_owrt_rib_list_free(sources);
	while (subs) {
		eigrp_owrt_rib_subscription_t *next = subs->next;
		free(subs);
		subs = next;
	}
}
