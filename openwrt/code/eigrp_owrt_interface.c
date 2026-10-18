// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux rtnetlink interface inventory and event observation for OpenWrt.
 * No OpenWrt implementation types cross the public EIGRP host API.
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if_arp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "eigrp_owrt_interface.h"
#include "eigrp_owrt_runtime.h"

#define EIGRP_OWRT_NL_BUFFER (128U * 1024U)
#define EIGRP_OWRT_BANDWIDTH_DEFAULT 100000U

static struct {
	pthread_mutex_t lock;
	pthread_mutex_t scan_lock;
	pthread_t thread;
	bool started;
	bool running;
	int watcher;
	int wake_read;
	int wake_write;
	eigrp_owrt_link_t *snapshot;
} inventory = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.scan_lock = PTHREAD_MUTEX_INITIALIZER,
	.watcher = -1,
	.wake_read = -1,
	.wake_write = -1,
};

static uint32_t eigrp_owrt_link_speed(const char *name)
{
	char path[160], buffer[48], *end;
	FILE *file;
	unsigned long long mbps;

	/* Speed from the Linux netdev sysfs ABI; absent on tunnels/bridges. */
	if (strchr(name, '/') ||
	    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", name) >=
		(int)sizeof(path))
		return EIGRP_OWRT_BANDWIDTH_DEFAULT;
	file = fopen(path, "r");
	if (!file)
		return EIGRP_OWRT_BANDWIDTH_DEFAULT;
	if (!fgets(buffer, sizeof(buffer), file)) {
		fclose(file);
		return EIGRP_OWRT_BANDWIDTH_DEFAULT;
	}
	fclose(file);
	errno = 0;
	mbps = strtoull(buffer, &end, 10);
	if (errno || end == buffer || (*end && *end != '\n') ||
	    mbps == 0 || mbps > UINT32_MAX / 1000U)
		return EIGRP_OWRT_BANDWIDTH_DEFAULT;
	return (uint32_t)mbps * 1000U;
}

static int eigrp_owrt_netlink_open(uint32_t groups)
{
	struct sockaddr_nl local = { .nl_family = AF_NETLINK,
		.nl_groups = groups };
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static eigrp_owrt_link_t *eigrp_owrt_link_find(eigrp_owrt_link_t *links,
	eigrp_ifindex_t ifindex)
{
	for (; links; links = links->next)
		if (links->ifindex == ifindex)
			return links;
	return NULL;
}

static bool eigrp_owrt_link_decode(eigrp_owrt_link_t **links,
	const struct nlmsghdr *msg)
{
	const struct ifinfomsg *ifi;
	struct rtattr *attr;
	int length;
	eigrp_owrt_link_t *link;
	char name[64] = "";
	uint32_t mtu = 0;

	if (NLMSG_PAYLOAD(msg, 0) < sizeof(*ifi))
		return false;
	ifi = NLMSG_DATA(msg);
	if (ifi->ifi_index <= 0)
		return true;
	length = IFLA_PAYLOAD(msg);
	for (attr = IFLA_RTA(ifi); RTA_OK(attr, length);
	     attr = RTA_NEXT(attr, length)) {
		if (attr->rta_type == IFLA_IFNAME) {
			size_t count = RTA_PAYLOAD(attr);
			const char *value = RTA_DATA(attr);
			if (!count || count > sizeof(name) ||
			    !memchr(value, '\0', count))
				return false;
			memcpy(name, value, count);
		} else if (attr->rta_type == IFLA_MTU &&
			   RTA_PAYLOAD(attr) >= sizeof(mtu)) {
			memcpy(&mtu, RTA_DATA(attr), sizeof(mtu));
		}
	}
	if (!name[0] || !mtu)
		return false;
	link = eigrp_owrt_link_find(*links, (uint32_t)ifi->ifi_index);
	if (!link) {
		link = calloc(1, sizeof(*link));
		if (!link)
			return false;
		link->next = *links;
		*links = link;
	}
	link->ifindex = (uint32_t)ifi->ifi_index;
	memcpy(link->name, name, sizeof(link->name));
	link->mtu = mtu;
	link->multicast = !!(ifi->ifi_flags & IFF_MULTICAST);
	link->operative = (ifi->ifi_flags & (IFF_UP | IFF_RUNNING)) ==
		(IFF_UP | IFF_RUNNING);
	link->bandwidth = eigrp_owrt_link_speed(name);
	return true;
}

static bool eigrp_owrt_address_decode(eigrp_owrt_link_t *links,
	const struct nlmsghdr *msg)
{
	const struct ifaddrmsg *ifa;
	struct rtattr *attr;
	const void *local = NULL, *peer = NULL;
	uint32_t flags;
	eigrp_owrt_address_t *address;
	eigrp_owrt_link_t *link;
	int length;
	size_t bytes;

	if (NLMSG_PAYLOAD(msg, 0) < sizeof(*ifa))
		return false;
	ifa = NLMSG_DATA(msg);
	if (ifa->ifa_family != AF_INET && ifa->ifa_family != AF_INET6)
		return true;
	bytes = ifa->ifa_family == AF_INET ? 4 : 16;
	if (ifa->ifa_prefixlen > bytes * 8)
		return false;
	link = eigrp_owrt_link_find(links, ifa->ifa_index);
	if (!link)
		return true;
	flags = ifa->ifa_flags;
	length = IFA_PAYLOAD(msg);
	for (attr = IFA_RTA(ifa); RTA_OK(attr, length);
	     attr = RTA_NEXT(attr, length)) {
		if (attr->rta_type == IFA_LOCAL && RTA_PAYLOAD(attr) >= bytes)
			local = RTA_DATA(attr);
		else if (attr->rta_type == IFA_ADDRESS &&
			 RTA_PAYLOAD(attr) >= bytes)
			peer = RTA_DATA(attr);
		else if (attr->rta_type == IFA_FLAGS &&
			 RTA_PAYLOAD(attr) >= sizeof(flags))
			memcpy(&flags, RTA_DATA(attr), sizeof(flags));
	}
	if (!local)
		local = peer;
	if (!local)
		return true;
	for (address = link->addresses; address; address = address->next) {
		if (address->prefix.address.afi !=
		    (ifa->ifa_family == AF_INET ? EIGRP_AFI_IPV4 : EIGRP_AFI_IPV6) ||
		    address->prefix.prefix_length != ifa->ifa_prefixlen ||
		    memcmp(address->prefix.address.bytes, local, bytes))
			continue;
		address->secondary = !!(flags & IFA_F_SECONDARY);
		return true;
	}
	address = calloc(1, sizeof(*address));
	if (!address)
		return false;
	address->prefix.address.afi = ifa->ifa_family == AF_INET ?
		EIGRP_AFI_IPV4 : EIGRP_AFI_IPV6;
	memcpy(address->prefix.address.bytes, local, bytes);
	address->prefix.prefix_length = ifa->ifa_prefixlen;
	address->secondary = !!(flags & IFA_F_SECONDARY);
	address->next = link->addresses;
	link->addresses = address;
	return true;
}

static bool eigrp_owrt_netlink_dump(int fd, uint16_t type,
	eigrp_owrt_link_t **links)
{
	static atomic_uint sequence;
	char *buffer;
	struct {
		struct nlmsghdr hdr;
		struct rtgenmsg request;
	} req = {0};
	struct sockaddr_nl kernel = { .nl_family = AF_NETLINK };
	uint32_t seq = atomic_fetch_add_explicit(&sequence, 1U,
		memory_order_relaxed) + 1U;
	bool done = false, ok = true;
	int received;

	buffer = malloc(EIGRP_OWRT_NL_BUFFER);
	if (!buffer)
		return false;
	req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(req.request));
	req.hdr.nlmsg_type = type;
	req.hdr.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.hdr.nlmsg_seq = seq;
	req.request.rtgen_family = AF_UNSPEC;
	if (sendto(fd, &req, req.hdr.nlmsg_len, 0,
	    (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
		free(buffer);
		return false;
	}
	while (!done && ok) {
		struct sockaddr_nl from;
		struct iovec iov = { .iov_base = buffer,
			.iov_len = EIGRP_OWRT_NL_BUFFER };
		struct msghdr msgh = { .msg_name = &from,
			.msg_namelen = sizeof(from), .msg_iov = &iov,
			.msg_iovlen = 1 };
		struct nlmsghdr *msg;
		int remaining;
		received = recvmsg(fd, &msgh, 0);
		if (received < 0 && errno == EINTR)
			continue;
		if (received <= 0 || msgh.msg_flags & MSG_TRUNC ||
		    msgh.msg_namelen < sizeof(from) || from.nl_pid != 0) {
			ok = false;
			break;
		}
		remaining = received;
		for (msg = (struct nlmsghdr *)buffer; NLMSG_OK(msg, remaining);
		     msg = NLMSG_NEXT(msg, remaining)) {
			if (msg->nlmsg_seq != seq)
				continue;
			if (msg->nlmsg_type == NLMSG_DONE) {
				/* Never publish a snapshot from an interrupted dump. */
				if (msg->nlmsg_flags & NLM_F_DUMP_INTR)
					ok = false;
				done = true;
				break;
			}
			if (msg->nlmsg_type == NLMSG_ERROR) {
				ok = false;
				break;
			}
			if (type == RTM_GETLINK && msg->nlmsg_type == RTM_NEWLINK)
				ok = eigrp_owrt_link_decode(links, msg);
			else if (type == RTM_GETADDR && msg->nlmsg_type == RTM_NEWADDR)
				ok = eigrp_owrt_address_decode(*links, msg);
			if (!ok)
				break;
		}
		if (remaining != 0 && !done)
			ok = false;
	}
	free(buffer);
	return ok && done;
}

/* New dumps use a separate socket, so multicast notifications on the watcher
 * cannot interrupt or corrupt the dump transaction. */
static eigrp_owrt_link_t *eigrp_owrt_snapshot_read(bool *valid)
{
	eigrp_owrt_link_t *links = NULL;
	int fd = eigrp_owrt_netlink_open(0);
	*valid = false;
	if (fd < 0)
		return NULL;
	if (eigrp_owrt_netlink_dump(fd, RTM_GETLINK, &links) &&
	    eigrp_owrt_netlink_dump(fd, RTM_GETADDR, &links))
		*valid = true;
	close(fd);
	if (!*valid) {
		eigrp_owrt_interface_snapshot_free(links);
		return NULL;
	}
	return links;
}

bool eigrp_owrt_interface_refresh(void)
{
	eigrp_owrt_link_t *snapshot, *old;
	bool ok, active;

	pthread_mutex_lock(&inventory.scan_lock);
	pthread_mutex_lock(&inventory.lock);
	active = inventory.started && inventory.running;
	pthread_mutex_unlock(&inventory.lock);
	if (!active) {
		pthread_mutex_unlock(&inventory.scan_lock);
		return false;
	}
	snapshot = eigrp_owrt_snapshot_read(&ok);
	if (!ok) {
		pthread_mutex_unlock(&inventory.scan_lock);
		return false;
	}
	/* Hold the protocol lock through notifications to prevent shutdown races.
	 * Startup/shutdown must be ordered relative to eigrp_init/terminate by the
	 * owning daemon. scan_lock serializes independent watcher/refresh callers. */
	eigrp_owrt_runtime_enter();
	pthread_mutex_lock(&inventory.lock);
	active = inventory.running;
	old = inventory.snapshot;
	if (active)
		inventory.snapshot = snapshot;
	pthread_mutex_unlock(&inventory.lock);
	if (active)
		eigrp_owrt_interface_snapshot_emit(old, snapshot);
	eigrp_owrt_runtime_leave();
	if (active)
		eigrp_owrt_interface_snapshot_free(old);
	else
		eigrp_owrt_interface_snapshot_free(snapshot);
	pthread_mutex_unlock(&inventory.scan_lock);
	return active;
}

static void *eigrp_owrt_interface_monitor(void *arg)
{
	struct pollfd pollfds[2];
	char buffer[65536];
	(void)arg;
	for (;;) {
		int count;
		pthread_mutex_lock(&inventory.lock);
		if (!inventory.running) {
			pthread_mutex_unlock(&inventory.lock);
			break;
		}
		pollfds[0].fd = inventory.watcher;
		pollfds[1].fd = inventory.wake_read;
		pthread_mutex_unlock(&inventory.lock);
		pollfds[0].events = POLLIN;
		pollfds[1].events = POLLIN;
		pollfds[0].revents = pollfds[1].revents = 0;
		count = poll(pollfds, 2, -1);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0 || pollfds[1].revents) {
			/* Stop/wakeup.  Fail closed if kernel watch becomes unusable. */
			break;
		}
		if (pollfds[0].revents) {
			ssize_t received = recv(pollfds[0].fd, buffer, sizeof(buffer), 0);
			if (received < 0 && errno == EINTR)
				continue;
			if (received <= 0 && errno != ENOBUFS)
				break;
			/* Link/address event or notification overflow: a full dump
			 * rebuilds the authoritative snapshot without losing deltas. */
			if (!eigrp_owrt_interface_refresh())
				break;
		}
	}
	pthread_mutex_lock(&inventory.lock);
	if (inventory.running) {
		inventory.running = false;
		fprintf(stderr, "OpenEIGRP OpenWrt: interface watcher stopped; restart required\n");
	}
	pthread_mutex_unlock(&inventory.lock);
	return NULL;
}

bool eigrp_owrt_interface_start(void)
{
	int watcher, pipefd[2];
	watcher = eigrp_owrt_netlink_open(RTMGRP_LINK |
		RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR);
	if (watcher < 0)
		return false;
	if (pipe(pipefd) != 0) {
		close(watcher);
		return false;
	}
	pthread_mutex_lock(&inventory.lock);
	if (inventory.started) {
		bool healthy = inventory.running;
		pthread_mutex_unlock(&inventory.lock);
		close(watcher); close(pipefd[0]); close(pipefd[1]);
		/* A failed watcher requires stop/join and a fresh start. */
		return healthy;
	}
	inventory.started = true;
	inventory.running = true;
	inventory.watcher = watcher;
	inventory.wake_read = pipefd[0];
	inventory.wake_write = pipefd[1];
	/* Subscribe before initial dump, so no change can fall between the
	 * first inventory and the multicast watcher. Initial notifications are
	 * synchronous; the caller may safely configure after start returns. */
	pthread_mutex_unlock(&inventory.lock);
	if (!eigrp_owrt_interface_refresh()) {
		pthread_mutex_lock(&inventory.lock);
		inventory.started = inventory.running = false;
		inventory.watcher = inventory.wake_read = inventory.wake_write = -1;
		pthread_mutex_unlock(&inventory.lock);
		close(watcher); close(pipefd[0]); close(pipefd[1]);
		return false;
	}
	pthread_mutex_lock(&inventory.lock);
	if (pthread_create(&inventory.thread, NULL,
	    eigrp_owrt_interface_monitor, NULL) != 0) {
		eigrp_owrt_link_t *failed = inventory.snapshot;
		inventory.snapshot = NULL;
		inventory.started = inventory.running = false;
		inventory.watcher = inventory.wake_read = inventory.wake_write = -1;
		pthread_mutex_unlock(&inventory.lock);
		eigrp_owrt_interface_snapshot_free(failed);
		close(watcher); close(pipefd[0]); close(pipefd[1]);
		return false;
	}
	pthread_mutex_unlock(&inventory.lock);
	return true;
}

void eigrp_owrt_interface_stop(void)
{
	int watcher, readfd, writefd;
	pthread_t thread;
	eigrp_owrt_link_t *snapshot;
	pthread_mutex_lock(&inventory.lock);
	if (!inventory.started) {
		pthread_mutex_unlock(&inventory.lock);
		return;
	}
	inventory.running = false;
	thread = inventory.thread;
	writefd = inventory.wake_write;
	pthread_mutex_unlock(&inventory.lock);
	(void)write(writefd, "x", 1);
	pthread_join(thread, NULL);
	pthread_mutex_lock(&inventory.scan_lock);
	pthread_mutex_lock(&inventory.lock);
	watcher = inventory.watcher;
	readfd = inventory.wake_read;
	writefd = inventory.wake_write;
	snapshot = inventory.snapshot;
	inventory.started = false;
	inventory.watcher = inventory.wake_read = inventory.wake_write = -1;
	inventory.snapshot = NULL;
	pthread_mutex_unlock(&inventory.lock);
	pthread_mutex_unlock(&inventory.scan_lock);
	close(watcher); close(readfd); close(writefd);
	eigrp_owrt_interface_snapshot_free(snapshot);
}

eigrp_result_t eigrp_sys_interface_walk(eigrp_instance_t *instance,
	eigrp_sys_interface_walk_cb callback, void *arg)
{
	eigrp_owrt_link_t *links, *link;
	eigrp_owrt_address_t *address;
	eigrp_afi_t afi;
	bool valid;

	if (!instance || !callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	afi = eigrp_instance_afi(instance);
	links = eigrp_owrt_snapshot_read(&valid);
	if (!valid)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	for (link = links; link; link = link->next) {
		for (address = link->addresses; address; address = address->next) {
			eigrp_intf_runtime_state_t state;
			if (address->prefix.address.afi != afi)
				continue;
			state.interface_name = link->name;
			state.ifindex = link->ifindex;
			state.address = address->prefix;
			state.type = 0;
			state.secondary = address->secondary;
			state.operative = link->operative && link->multicast;
			state.bandwidth = link->bandwidth;
			state.mtu = link->mtu;
			callback(&state, arg);
		}
	}
	eigrp_owrt_interface_snapshot_free(links);
	return EIGRP_RESULT_SUCCESS;
}
