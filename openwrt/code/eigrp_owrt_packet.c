// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Linux IP protocol 88 transport for the native OpenWrt EIGRP host.
 * Portable EIGRP owns checksum, cryptography, RTP, DUAL and packet content.
 * This file owns only Linux raw sockets, control messages and multicast.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "eigrp_owrt_packet.h"
#include "eigrp_owrt_runtime.h"

#define EIGRP_OWRT_HEADER_MIN 20U
#define EIGRP_OWRT_HOP_LIMIT 2

typedef struct eigrp_owrt_socket {
	eigrp_instance_t *instance;  /* identity only; never dereferenced */
	eigrp_afi_t afi;
	int fd;
	struct eigrp_owrt_socket *next;
} eigrp_owrt_socket_t;

static pthread_mutex_t eigrp_owrt_socket_lock = PTHREAD_MUTEX_INITIALIZER;
static eigrp_owrt_socket_t *eigrp_owrt_sockets;

static eigrp_owrt_socket_t *eigrp_owrt_socket_find(eigrp_instance_t *instance)
{
	eigrp_owrt_socket_t *entry;
	for (entry = eigrp_owrt_sockets; entry; entry = entry->next)
		if (entry->instance == instance)
			return entry;
	return NULL;
}

/* Dup under the table lock so a concurrent socket_close cannot invalidate
 * the descriptor while sendmsg/recvmsg/setsockopt is running. The calling
 * host serializes protocol mutation with the OW-04 protocol lock. */
static int eigrp_owrt_socket_dup(eigrp_instance_t *instance, eigrp_afi_t *afi)
{
	eigrp_owrt_socket_t *entry;
	int fd = -1;
	pthread_mutex_lock(&eigrp_owrt_socket_lock);
	entry = eigrp_owrt_socket_find(instance);
	if (entry) {
		if (afi)
			*afi = entry->afi;
		fd = dup(entry->fd);
	}
	pthread_mutex_unlock(&eigrp_owrt_socket_lock);
	return fd;
}

static int eigrp_owrt_socket_setopt(int fd, int level, int option,
	const void *value, socklen_t size)
{
	return setsockopt(fd, level, option, value, size);
}

static int eigrp_owrt_socket_create(eigrp_afi_t afi)
{
	int family = afi == EIGRP_AFI_IPV4 ? AF_INET : AF_INET6;
	int fd, on = 1, off = 0, hops = EIGRP_OWRT_HOP_LIMIT;
	unsigned char loop = 0, v4_hops = EIGRP_OWRT_HOP_LIMIT;

	fd = socket(family, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC,
		EIGRP_OWRT_IP_PROTOCOL);
	if (fd < 0)
		return -1;
	if (eigrp_owrt_socket_setopt(fd, SOL_SOCKET, SO_REUSEADDR,
		&on, sizeof(on)) < 0)
		goto fail;
	if (afi == EIGRP_AFI_IPV4) {
		if (eigrp_owrt_socket_setopt(fd, IPPROTO_IP, IP_PKTINFO,
			&on, sizeof(on)) < 0 ||
		    eigrp_owrt_socket_setopt(fd, IPPROTO_IP, IP_TTL,
			&hops, sizeof(hops)) < 0 ||
		    eigrp_owrt_socket_setopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
			&v4_hops, sizeof(v4_hops)) < 0 ||
		    eigrp_owrt_socket_setopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
			&loop, sizeof(loop)) < 0)
			goto fail;
	} else {
		/* Raw AF_INET6 protocol sockets do not accept IPv4 packets.
		 * IPV6_V6ONLY is a TCP/UDP option, not required here. */
		if (eigrp_owrt_socket_setopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO,
			&on, sizeof(on)) < 0 ||
		    eigrp_owrt_socket_setopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS,
			&hops, sizeof(hops)) < 0 ||
		    eigrp_owrt_socket_setopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
			&hops, sizeof(hops)) < 0 ||
		    eigrp_owrt_socket_setopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
			&off, sizeof(off)) < 0)
			goto fail;
	}
	return fd;
fail:
	close(fd);
	return -1;
}

eigrp_result_t eigrp_sys_socket_open(eigrp_instance_t *instance)
{
	eigrp_owrt_socket_t *entry;
	eigrp_afi_t afi;
	int fd;

	if (!instance)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	/* A socket in the default namespace must never impersonate another VRF. */
	if (eigrp_instance_vrf_id(instance) != EIGRP_VRF_DEFAULT)
		return EIGRP_RESULT_UNSUPPORTED;
	afi = eigrp_instance_afi(instance);
	if (afi != EIGRP_AFI_IPV4 && afi != EIGRP_AFI_IPV6)
		return EIGRP_RESULT_UNSUPPORTED;
	pthread_mutex_lock(&eigrp_owrt_socket_lock);
	if (eigrp_owrt_socket_find(instance)) {
		pthread_mutex_unlock(&eigrp_owrt_socket_lock);
		return EIGRP_RESULT_SUCCESS;
	}
	pthread_mutex_unlock(&eigrp_owrt_socket_lock);

	fd = eigrp_owrt_socket_create(afi);
	if (fd < 0)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	entry = calloc(1, sizeof(*entry));
	if (!entry) {
		close(fd);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	entry->instance = instance;
	entry->afi = afi;
	entry->fd = fd;
	pthread_mutex_lock(&eigrp_owrt_socket_lock);
	if (eigrp_owrt_socket_find(instance)) {
		pthread_mutex_unlock(&eigrp_owrt_socket_lock);
		free(entry);
		close(fd);
		return EIGRP_RESULT_SUCCESS;
	}
	/* Register the descriptor with OW-04 before making it visible. */
	if (!eigrp_owrt_runtime_fd_set(instance, fd)) {
		pthread_mutex_unlock(&eigrp_owrt_socket_lock);
		free(entry);
		close(fd);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	entry->next = eigrp_owrt_sockets;
	eigrp_owrt_sockets = entry;
	pthread_mutex_unlock(&eigrp_owrt_socket_lock);
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_sys_socket_close(eigrp_instance_t *instance)
{
	eigrp_owrt_socket_t **cursor, *entry = NULL;
	if (!instance)
		return;
	pthread_mutex_lock(&eigrp_owrt_socket_lock);
	for (cursor = &eigrp_owrt_sockets; *cursor; cursor = &(*cursor)->next) {
		if ((*cursor)->instance == instance) {
			entry = *cursor;
			*cursor = entry->next;
			break;
		}
	}
	/* FD readiness must be cancelled before kernel descriptor close. */
	if (entry)
		eigrp_owrt_runtime_fd_clear(instance);
	pthread_mutex_unlock(&eigrp_owrt_socket_lock);
	if (entry) {
		close(entry->fd);
		free(entry);
	}
}

void eigrp_sys_socket_send_buffer_ensure(eigrp_instance_t *instance,
	uint32_t minimum)
{
	int fd, current = 0;
	socklen_t length = sizeof(current);
	if (!instance || !minimum)
		return;
	fd = eigrp_owrt_socket_dup(instance, NULL);
	if (fd < 0)
		return;
	if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &current, &length) == 0 &&
	    current > 0 && (uint32_t)current < minimum) {
		int desired = minimum > INT32_MAX ? INT32_MAX : (int)minimum;
		(void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &desired, sizeof(desired));
	}
	close(fd);
}

static int eigrp_owrt_packet_send_v4(int fd, eigrp_intf_t *interface,
	const eigrp_address_t *destination, const uint8_t *data, size_t size)
{
	struct sockaddr_in peer = { .sin_family = AF_INET };
	struct in_pktinfo *info;
	struct msghdr msg = {0};
	struct iovec iov = { .iov_base = (void *)data, .iov_len = size };
	union { struct cmsghdr align; unsigned char buffer[CMSG_SPACE(sizeof(struct in_pktinfo))]; } control;
	struct cmsghdr *cmsg;
	eigrp_prefix_t local;
	ssize_t sent;

	memcpy(&peer.sin_addr, destination->bytes, sizeof(peer.sin_addr));
	memset(&control, 0, sizeof(control));
	msg.msg_name = &peer;
	msg.msg_namelen = sizeof(peer);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.buffer;
	msg.msg_controllen = sizeof(control.buffer);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = IPPROTO_IP;
	cmsg->cmsg_type = IP_PKTINFO;
	cmsg->cmsg_len = CMSG_LEN(sizeof(*info));
	info = (struct in_pktinfo *)CMSG_DATA(cmsg);
	info->ipi_ifindex = (int)eigrp_intf_ifindex(interface);
	if (eigrp_intf_address_read(interface, &local) == EIGRP_RESULT_SUCCESS &&
	    local.address.afi == EIGRP_AFI_IPV4)
		memcpy(&info->ipi_spec_dst, local.address.bytes, 4);
	sent = sendmsg(fd, &msg, MSG_DONTWAIT);
	return sent < 0 ? -errno : (int)sent;
}

static int eigrp_owrt_packet_send_v6(int fd, eigrp_intf_t *interface,
	const eigrp_address_t *destination, const uint8_t *data, size_t size)
{
	struct sockaddr_in6 peer = { .sin6_family = AF_INET6 };
	struct in6_pktinfo *info;
	struct msghdr msg = {0};
	struct iovec iov = { .iov_base = (void *)data, .iov_len = size };
	union { struct cmsghdr align; unsigned char buffer[CMSG_SPACE(sizeof(struct in6_pktinfo))]; } control;
	struct cmsghdr *cmsg;
	eigrp_prefix_t local;
	eigrp_ifindex_t ifindex = eigrp_intf_ifindex(interface);
	ssize_t sent;

	memcpy(&peer.sin6_addr, destination->bytes, sizeof(peer.sin6_addr));
	/* Scope ID is essential for ff02::a and link-local unicast. */
	if (IN6_IS_ADDR_LINKLOCAL(&peer.sin6_addr) ||
	    IN6_IS_ADDR_MC_LINKLOCAL(&peer.sin6_addr))
		peer.sin6_scope_id = ifindex;
	memset(&control, 0, sizeof(control));
	msg.msg_name = &peer;
	msg.msg_namelen = sizeof(peer);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.buffer;
	msg.msg_controllen = sizeof(control.buffer);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = IPPROTO_IPV6;
	cmsg->cmsg_type = IPV6_PKTINFO;
	cmsg->cmsg_len = CMSG_LEN(sizeof(*info));
	info = (struct in6_pktinfo *)CMSG_DATA(cmsg);
	info->ipi6_ifindex = ifindex;
	if (eigrp_intf_address_read(interface, &local) == EIGRP_RESULT_SUCCESS &&
	    local.address.afi == EIGRP_AFI_IPV6 &&
	    IN6_IS_ADDR_LINKLOCAL((const struct in6_addr *)local.address.bytes))
		memcpy(&info->ipi6_addr, local.address.bytes, 16);
	sent = sendmsg(fd, &msg, MSG_DONTWAIT);
	return sent < 0 ? -errno : (int)sent;
}

int eigrp_sys_packet_send(eigrp_instance_t *instance, eigrp_intf_t *interface,
	const eigrp_address_t *destination, const uint8_t *data, size_t size)
{
	eigrp_afi_t afi;
	int fd, result;
	if (!instance || !interface || !destination || !data ||
	    size < EIGRP_OWRT_HEADER_MIN || size > UINT16_MAX ||
	    !eigrp_intf_ifindex(interface))
		return -EINVAL;
	fd = eigrp_owrt_socket_dup(instance, &afi);
	if (fd < 0)
		return -ENOTCONN;
	if (destination->afi != afi)
		result = -EAFNOSUPPORT;
	else if (afi == EIGRP_AFI_IPV4)
		result = eigrp_owrt_packet_send_v4(fd, interface, destination, data, size);
	else
		result = eigrp_owrt_packet_send_v6(fd, interface, destination, data, size);
	close(fd);
	return result;
}

/* Linux IPv4 raw socket frames include the IP header; the portable process
 * requires its exact byte offset. Validate IHL, length and protocol before
 * presenting metadata to the common packet parser. */
bool eigrp_owrt_packet_ipv4_decode(const uint8_t *data, size_t size,
	uint32_t ingress_ifindex, eigrp_address_t *source,
	eigrp_address_t *destination, eigrp_packet_rx_meta_t *meta)
{
	size_t header, payload;
	uint16_t total, fragment;
	uint32_t target;
	if (!data || !source || !destination || !meta || !ingress_ifindex ||
	    size < 20 || size > UINT16_MAX || (data[0] >> 4) != 4)
		return false;
	header = (data[0] & 0x0fU) * 4U;
	if (header < 20 || header > size || data[9] != EIGRP_OWRT_IP_PROTOCOL)
		return false;
	memcpy(&total, data + 2, sizeof(total));
	total = ntohs(total);
	memcpy(&fragment, data + 6, sizeof(fragment));
	fragment = ntohs(fragment);
	if (total != size || total < header || (fragment & 0x3fffU))
		return false;
	payload = total - header;
	if (payload < EIGRP_OWRT_HEADER_MIN)
		return false;
	memset(source, 0, sizeof(*source));
	memset(destination, 0, sizeof(*destination));
	memset(meta, 0, sizeof(*meta));
	source->afi = destination->afi = EIGRP_AFI_IPV4;
	memcpy(source->bytes, data + 12, 4);
	memcpy(destination->bytes, data + 16, 4);
	memcpy(&target, data + 16, 4);
	meta->ingress_vrf_id = EIGRP_VRF_DEFAULT;
	meta->network_header_length = (uint16_t)header;
	meta->eigrp_length = (uint16_t)payload;
	meta->destination_multicast = IN_MULTICAST(ntohl(target));
	return true;
}

bool eigrp_owrt_packet_ipv6_decode(const uint8_t *data, size_t size,
	uint32_t ingress_ifindex, const uint8_t source_bytes[16],
	const uint8_t destination_bytes[16], eigrp_address_t *source,
	eigrp_address_t *destination, eigrp_packet_rx_meta_t *meta)
{
	if (!data || !source_bytes || !destination_bytes || !source ||
	    !destination || !meta || !ingress_ifindex ||
	    size < EIGRP_OWRT_HEADER_MIN || size > UINT16_MAX)
		return false;
	memset(source, 0, sizeof(*source));
	memset(destination, 0, sizeof(*destination));
	memset(meta, 0, sizeof(*meta));
	source->afi = destination->afi = EIGRP_AFI_IPV6;
	memcpy(source->bytes, source_bytes, 16);
	memcpy(destination->bytes, destination_bytes, 16);
	meta->ingress_vrf_id = EIGRP_VRF_DEFAULT;
	meta->network_header_length = 0;
	meta->eigrp_length = (uint16_t)size;
	meta->destination_multicast =
		IN6_IS_ADDR_MULTICAST((const struct in6_addr *)destination_bytes);
	return true;
}

bool eigrp_sys_packet_receive(eigrp_instance_t *instance,
	uint8_t *buffer, size_t capacity, size_t *received_length,
	eigrp_ifindex_t *ifindex, eigrp_address_t *source,
	eigrp_address_t *destination, eigrp_packet_rx_meta_t *meta)
{
	union {
		struct sockaddr_in v4;
		struct sockaddr_in6 v6;
	} peer;
	union { struct cmsghdr align;
		unsigned char buffer[CMSG_SPACE(sizeof(struct in_pktinfo)) +
			CMSG_SPACE(sizeof(struct in6_pktinfo))]; } control;
	struct msghdr msg = {0};
	struct iovec iov;
	struct cmsghdr *cmsg;
	eigrp_afi_t afi;
	uint32_t ingress = 0;
	struct in6_addr dest6 = IN6ADDR_ANY_INIT;
	int fd;
	ssize_t n;
	bool ok = false;

	if (!instance || !buffer || !received_length || !ifindex || !source ||
	    !destination || !meta || capacity < EIGRP_OWRT_HEADER_MIN)
		return false;
	*received_length = 0;
	*ifindex = 0;
	fd = eigrp_owrt_socket_dup(instance, &afi);
	if (fd < 0)
		return false;
	memset(&peer, 0, sizeof(peer));
	memset(&control, 0, sizeof(control));
	iov.iov_base = buffer;
	iov.iov_len = capacity;
	msg.msg_name = &peer;
	msg.msg_namelen = sizeof(peer);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control.buffer;
	msg.msg_controllen = sizeof(control.buffer);
	n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_TRUNC);
	close(fd);
	if (n < (ssize_t)EIGRP_OWRT_HEADER_MIN || (size_t)n > capacity ||
	    (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
		return false;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (afi == EIGRP_AFI_IPV4 && cmsg->cmsg_level == IPPROTO_IP &&
		    cmsg->cmsg_type == IP_PKTINFO &&
		    cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo))) {
			struct in_pktinfo info;
			memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
			ingress = (uint32_t)info.ipi_ifindex;
		} else if (afi == EIGRP_AFI_IPV6 &&
			   cmsg->cmsg_level == IPPROTO_IPV6 &&
			   cmsg->cmsg_type == IPV6_PKTINFO &&
			   cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
			struct in6_pktinfo info;
			memcpy(&info, CMSG_DATA(cmsg), sizeof(info));
			ingress = info.ipi6_ifindex;
			dest6 = info.ipi6_addr;
		}
	}
	if (!ingress)
		return false;
	if (afi == EIGRP_AFI_IPV4)
		ok = eigrp_owrt_packet_ipv4_decode(buffer, (size_t)n, ingress,
			source, destination, meta);
	else if (afi == EIGRP_AFI_IPV6 && peer.v6.sin6_family == AF_INET6)
		ok = eigrp_owrt_packet_ipv6_decode(buffer, (size_t)n, ingress,
			(const uint8_t *)&peer.v6.sin6_addr,
			(const uint8_t *)&dest6, source, destination, meta);
	if (ok) {
		*ifindex = ingress;
		*received_length = (size_t)n;
	}
	return ok;
}

int eigrp_sys_ipv4_packet_send(eigrp_instance_t *instance, eigrp_intf_t *interface,
	const eigrp_address_t *destination, const uint8_t *buffer, size_t size)
{
	if (!instance || eigrp_instance_afi(instance) != EIGRP_AFI_IPV4)
		return -EAFNOSUPPORT;
	return eigrp_sys_packet_send(instance, interface, destination, buffer, size);
}

int eigrp_sys_ipv6_packet_send(eigrp_instance_t *instance, eigrp_intf_t *interface,
	const eigrp_address_t *destination, const uint8_t *buffer, size_t size)
{
	if (!instance || eigrp_instance_afi(instance) != EIGRP_AFI_IPV6)
		return -EAFNOSUPPORT;
	return eigrp_sys_packet_send(instance, interface, destination, buffer, size);
}

bool eigrp_sys_ipv4_packet_receive(eigrp_instance_t *instance,
	uint8_t *buffer, size_t capacity, size_t *received_length,
	eigrp_ifindex_t *ifindex, eigrp_address_t *source,
	eigrp_address_t *destination, eigrp_packet_rx_meta_t *meta)
{
	return instance && eigrp_instance_afi(instance) == EIGRP_AFI_IPV4 &&
		eigrp_sys_packet_receive(instance, buffer, capacity, received_length,
			ifindex, source, destination, meta);
}

bool eigrp_sys_ipv6_packet_receive(eigrp_instance_t *instance,
	uint8_t *buffer, size_t capacity, size_t *received_length,
	eigrp_ifindex_t *ifindex, eigrp_address_t *source,
	eigrp_address_t *destination, eigrp_packet_rx_meta_t *meta)
{
	return instance && eigrp_instance_afi(instance) == EIGRP_AFI_IPV6 &&
		eigrp_sys_packet_receive(instance, buffer, capacity, received_length,
			ifindex, source, destination, meta);
}

/* IP multicast membership is socket-local, and EIGRP owns join state on
 * each runtime interface. On Linux IPv4 the interface index avoids ambiguity
 * when addresses change. IPv6 uses its scoped group and interface index. */
static int eigrp_owrt_multicast(eigrp_instance_t *instance,
	eigrp_intf_t *interface, bool join)
{
	eigrp_afi_t afi;
	int fd, rc, saved;
	eigrp_ifindex_t index;
	if (!instance || !interface || !(index = eigrp_intf_ifindex(interface)))
		return -EINVAL;
	fd = eigrp_owrt_socket_dup(instance, &afi);
	if (fd < 0)
		return -ENOTCONN;
	if (afi == EIGRP_AFI_IPV4) {
		struct ip_mreqn membership = {0};
		membership.imr_multiaddr.s_addr = htonl(EIGRP_OWRT_IPV4_ALL_ROUTERS);
		membership.imr_ifindex = (int)index;
		rc = setsockopt(fd, IPPROTO_IP,
			join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
			&membership, sizeof(membership));
	} else {
		struct ipv6_mreq membership = {0};
		(void)inet_pton(AF_INET6, "ff02::a", &membership.ipv6mr_multiaddr);
		membership.ipv6mr_interface = index;
		rc = setsockopt(fd, IPPROTO_IPV6,
			join ? IPV6_JOIN_GROUP : IPV6_LEAVE_GROUP,
			&membership, sizeof(membership));
	}
	saved = errno;
	close(fd);
	return rc == 0 ? 0 : -saved;
}

int eigrp_sys_multicast_join(eigrp_instance_t *instance, eigrp_intf_t *interface)
{
	return eigrp_owrt_multicast(instance, interface, true);
}

int eigrp_sys_multicast_leave(eigrp_instance_t *instance, eigrp_intf_t *interface)
{
	return eigrp_owrt_multicast(instance, interface, false);
}

int eigrp_sys_multicast_interface_update(eigrp_operation_t operation,
	eigrp_instance_t *instance, eigrp_intf_t *interface)
{
	eigrp_afi_t afi;
	int fd, rc, saved;
	uint32_t index = 0;
	if (!instance || (operation != EIGRP_SET && operation != EIGRP_RESET))
		return -EINVAL;
	if (operation == EIGRP_SET) {
		if (!interface || !(index = eigrp_intf_ifindex(interface)))
			return -EINVAL;
	}
	fd = eigrp_owrt_socket_dup(instance, &afi);
	if (fd < 0)
		return -ENOTCONN;
	if (afi == EIGRP_AFI_IPV4) {
		struct ip_mreqn choice = {0};
		choice.imr_ifindex = (int)index;
		rc = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF,
			&choice, sizeof(choice));
	} else {
		int choice = (int)index;
		rc = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF,
			&choice, sizeof(choice));
	}
	saved = errno;
	close(fd);
	return rc == 0 ? 0 : -saved;
}
