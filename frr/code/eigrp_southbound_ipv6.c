// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR IPv6 southbound integration.
 *
 * Code migrated from eigrp_southbound.c during the address-family
 * integration split.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */
#include <zebra.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eigrpd.h"
#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_instance.h"
#include "eigrp_packet.h"
#include "eigrp_sys.h"
#include "eigrp_southbound_internal.h"
#include "lib/sockopt.h"
#include "vrf.h"

static bool eigrp_southbound_ipv6_interface_linklocal(
	eigrp_instance_t *eigrp, const eigrp_intf_t *ei,
	struct in6_addr *address)
{
	struct connected *co;
	struct interface *ifp;
	struct vrf *vrf;
	eigrp_ifindex_t ifindex;

	if (!eigrp || !ei || !address)
		return false;
	ifindex = eigrp_intf_ifindex(ei);
	if (!ifindex)
		return false;
	vrf = vrf_lookup_by_id((vrf_id_t)eigrp_instance_vrf_id(eigrp));
	if (!vrf)
		return false;
	FOR_ALL_INTERFACES (vrf, ifp) {
		if ((eigrp_ifindex_t)ifp->ifindex != ifindex)
			continue;
		frr_each (if_connected, ifp->connected, co) {
			if (!co->address || co->address->family != AF_INET6
			    || !IN6_IS_ADDR_LINKLOCAL(&co->address->u.prefix6))
				continue;
			*address = co->address->u.prefix6;
			return true;
		}
		return false;
	}
	return false;
}

eigrp_result_t eigrp_southbound_ipv6_socket_configure(int fd)
{
	int one = EIGRP_IP_TTL;
	int zero = 0;

	if (setsockopt_ifindex(AF_INET6, fd, 1) < 0) {
		zlog_warn("Can't set pktinfo option for fd %d", fd);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &one, sizeof(one)) < 0
	    || setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &one, sizeof(one)) < 0
	    || setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &zero, sizeof(zero)) < 0)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	return EIGRP_RESULT_SUCCESS;
}

int eigrp_southbound_ipv6_multicast_interface_update(eigrp_operation_t operation,
                                                   eigrp_instance_t *eigrp,
                                                   eigrp_intf_t *ei)
{
	if (operation != EIGRP_SET)
		return -1;
	eigrp_ifindex_t ifindex = eigrp_intf_ifindex(ei);
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	int ret;

	if (fd < 0 || !ifindex)
		return -1;
	ret = setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
	if (ret < 0)
		zlog_warn("can't set IPv6 multicast interface %s[%u]: %s",
			  eigrp_intf_name(ei) ? eigrp_intf_name(ei) : "?",
			  ifindex, safe_strerror(errno));
	return ret;
}

static int eigrp_southbound_ipv6_multicast_membership(eigrp_instance_t *eigrp,
                                                       eigrp_intf_t *ei,
                                                       int option)
{
	struct ipv6_mreq mreq = {0};
	eigrp_ifindex_t ifindex = eigrp_intf_ifindex(ei);
	const char *name = eigrp_intf_name(ei);
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	int ret;

	if (fd < 0 || !ifindex
	    || inet_pton(AF_INET6, "ff02::a", &mreq.ipv6mr_multiaddr) != 1)
		return -1;
	mreq.ipv6mr_interface = ifindex;
	ret = setsockopt(fd, IPPROTO_IPV6, option, &mreq, sizeof(mreq));
	if (ret < 0)
		zlog_warn("can't %s EIGRP IPv6 multicast group on %s[%u]: %s",
			  option == IPV6_JOIN_GROUP ? "join" : "leave", name ? name : "?",
			  ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_southbound_ipv6_multicast_join(eigrp_instance_t *eigrp,
                                         eigrp_intf_t *ei)
{
	return eigrp_southbound_ipv6_multicast_membership(eigrp, ei, IPV6_JOIN_GROUP);
}

int eigrp_southbound_ipv6_multicast_leave(eigrp_instance_t *eigrp,
                                          eigrp_intf_t *ei)
{
	return eigrp_southbound_ipv6_multicast_membership(eigrp, ei, IPV6_LEAVE_GROUP);
}

int eigrp_sys_ipv6_packet_send(eigrp_instance_t *eigrp,
                               eigrp_intf_t *ei,
                               const eigrp_address_t *destination,
                               const uint8_t *payload, size_t length)
{
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	struct sockaddr_in6 sa = {0};
	struct msghdr msg = {0};
	struct iovec iov = {0};
	struct cmsghdr *cmsg;
	struct in6_pktinfo *pktinfo;
	struct in6_addr source;
	char control[CMSG_SPACE(sizeof(struct in6_pktinfo))] = {0};
	eigrp_ifindex_t ifindex;

	if (fd < 0 || !ei || !destination || !payload
	    || destination->afi != EIGRP_AFI_IPV6)
		return -1;
	ifindex = eigrp_intf_ifindex(ei);
	if (!ifindex || !eigrp_southbound_ipv6_interface_linklocal(eigrp, ei, &source))
		return -1;
	sa.sin6_family = AF_INET6;
	sa.sin6_scope_id = ifindex;
	memcpy(&sa.sin6_addr, destination->bytes, sizeof(sa.sin6_addr));
	if (IN6_IS_ADDR_MULTICAST(&sa.sin6_addr)
	    && eigrp_sys_multicast_interface_update(EIGRP_SET, eigrp, ei) < 0)
		return -1;
	iov.iov_base = (void *)payload;
	iov.iov_len = length;
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = IPPROTO_IPV6;
	cmsg->cmsg_type = IPV6_PKTINFO;
	cmsg->cmsg_len = CMSG_LEN(sizeof(*pktinfo));
	pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);
	pktinfo->ipi6_ifindex = ifindex;
	pktinfo->ipi6_addr = source;
	return sendmsg(fd, &msg, 0);
}

bool eigrp_sys_ipv6_packet_receive(eigrp_instance_t *eigrp,
                                   uint8_t *buffer, size_t capacity,
                                   size_t *received_length,
                                   eigrp_ifindex_t *ifindex,
                                   eigrp_address_t *source,
                                   eigrp_address_t *destination,
                                   eigrp_packet_rx_meta_t *meta)
{
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	struct sockaddr_in6 sa = {0};
	struct msghdr msg = {0};
	struct iovec iov = {0};
	struct cmsghdr *cmsg;
	char control[CMSG_SPACE(sizeof(struct in6_pktinfo))] = {0};
	ssize_t ret;

	if (fd < 0 || !buffer || !capacity || !received_length || !ifindex
	    || !source || !destination || !meta)
		return false;
	iov.iov_base = buffer;
	iov.iov_len = capacity;
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	ret = recvmsg(fd, &msg, 0);
	if (ret < EIGRP_HEADER_LEN || (size_t)ret > capacity || ret > UINT16_MAX
	    || sa.sin6_family != AF_INET6
	    || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
		return false;
	memset(source, 0, sizeof(*source));
	memset(destination, 0, sizeof(*destination));
	source->afi = EIGRP_AFI_IPV6;
	destination->afi = EIGRP_AFI_IPV6;
	memcpy(source->bytes, &sa.sin6_addr, sizeof(sa.sin6_addr));
	*ifindex = 0;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO
		    && cmsg->cmsg_len >= CMSG_LEN(sizeof(struct in6_pktinfo))) {
			const struct in6_pktinfo *pktinfo =
				(const struct in6_pktinfo *)CMSG_DATA(cmsg);
			*ifindex = pktinfo->ipi6_ifindex;
			memcpy(destination->bytes, &pktinfo->ipi6_addr,
			       sizeof(pktinfo->ipi6_addr));
			break;
		}
	}
	if (!*ifindex)
		return false;
	{
		struct interface *ifp = if_lookup_by_index((ifindex_t)*ifindex,
			(vrf_id_t)eigrp_instance_vrf_id(eigrp));
		if (!ifp || !ifp->vrf)
			return false;
		meta->ingress_vrf_id = (eigrp_vrf_id_t)ifp->vrf->vrf_id;
	}
	*received_length = (size_t)ret;
	meta->network_header_length = 0;
	meta->eigrp_length = (uint16_t)ret;
	{
		struct in6_addr group;
		meta->destination_multicast =
			inet_pton(AF_INET6, "ff02::a", &group) == 1
			&& memcmp(destination->bytes, &group, sizeof(group)) == 0;
	}
	return true;
}

