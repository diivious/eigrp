// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR IPv4 southbound integration.
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

static bool eigrp_southbound_ipv4_interface_address(
	const eigrp_intf_t *ei, struct in_addr *address)
{
	eigrp_prefix_t prefix;

	if (!ei || !address
	    || eigrp_intf_address_read(ei, &prefix) != EIGRP_RESULT_SUCCESS
	    || prefix.address.afi != EIGRP_AFI_IPV4)
		return false;
	memcpy(address, prefix.address.bytes, sizeof(*address));
	return true;
}

eigrp_result_t eigrp_southbound_ipv4_socket_configure(int fd)
{
	int ret;
#ifdef IP_HDRINCL
	int hincl = 1;

	ret = setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &hincl, sizeof(hincl));
	if (ret < 0)
		zlog_warn("Can't set IP_HDRINCL option for fd %d: %s", fd,
			  safe_strerror(errno));
#elif defined(IPTOS_PREC_INTERNETCONTROL)
	ret = setsockopt_ipv4_tos(fd, IPTOS_PREC_INTERNETCONTROL);
	if (ret < 0) {
		zlog_warn("can't set EIGRP IP_TOS on socket %d: %s", fd,
			  safe_strerror(errno));
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
#else
	zlog_warn("IP_HDRINCL option not available");
#endif
	ret = setsockopt_ifindex(AF_INET, fd, 1);
	if (ret < 0)
		zlog_warn("Can't set pktinfo option for fd %d", fd);
	return EIGRP_RESULT_SUCCESS;
}

int eigrp_southbound_ipv4_multicast_interface_update(eigrp_operation_t operation,
                                                   eigrp_instance_t *eigrp,
                                                   eigrp_intf_t *ei)
{
	if (operation != EIGRP_SET)
		return -1;
	struct in_addr address;
	eigrp_ifindex_t ifindex;
	const char *name;
	uint8_t val = 0;
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	int ret;

	if (fd < 0)
		return -1;
	ifindex = eigrp_intf_ifindex(ei);
	if (!ifindex || !eigrp_southbound_ipv4_interface_address(ei, &address))
		return -1;
	name = eigrp_intf_name(ei);
	ret = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val));
	if (ret < 0)
		zlog_warn("can't disable IP_MULTICAST_LOOP for fd %d: %s", fd,
			  safe_strerror(errno));
	val = 1;
	ret = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val));
	if (ret < 0)
		zlog_warn("can't set IP_MULTICAST_TTL for fd %d: %s", fd,
			  safe_strerror(errno));
	ret = setsockopt_ipv4_multicast_if(fd, address, ifindex);
	if (ret < 0)
		zlog_warn("can't set multicast interface %s[%u]: %s",
			  name ? name : "?", ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_southbound_ipv4_multicast_join(eigrp_instance_t *eigrp,
                                         eigrp_intf_t *ei)
{
	struct in_addr address;
	eigrp_ifindex_t ifindex = eigrp_intf_ifindex(ei);
	const char *name = eigrp_intf_name(ei);
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	int ret;

	if (fd < 0 || !eigrp_southbound_ipv4_interface_address(ei, &address))
		return -1;
	ret = setsockopt_ipv4_multicast(fd, IP_ADD_MEMBERSHIP, address,
					htonl(EIGRP_MULTICAST_ADDRESS), ifindex);
	if (ret < 0)
		zlog_warn("can't join EIGRP multicast group on %s[%u]: %s",
			  name ? name : "?", ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_southbound_ipv4_multicast_leave(eigrp_instance_t *eigrp,
                                          eigrp_intf_t *ei)
{
	struct in_addr address;
	eigrp_ifindex_t ifindex = eigrp_intf_ifindex(ei);
	const char *name = eigrp_intf_name(ei);
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	int ret;

	if (fd < 0 || !eigrp_southbound_ipv4_interface_address(ei, &address))
		return -1;
	ret = setsockopt_ipv4_multicast(fd, IP_DROP_MEMBERSHIP, address,
					htonl(EIGRP_MULTICAST_ADDRESS), ifindex);
	if (ret < 0)
		zlog_warn("can't leave EIGRP multicast group on %s[%u]: %s",
			  name ? name : "?", ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_sys_ipv4_packet_send(eigrp_instance_t *eigrp,
			       eigrp_intf_t *ei,
			       const eigrp_address_t *destination,
			       const uint8_t *payload, size_t length)
{
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	eigrp_prefix_t local;
	struct sockaddr_in sa_dst;
	struct in_addr dst;
	struct ip iph;
	struct msghdr msg;
	struct iovec iov[2];
	struct cmsghdr *cmsg;
	struct in_pktinfo *pktinfo;
	eigrp_ifindex_t ifindex;
	char control[CMSG_SPACE(sizeof(struct in_pktinfo))];
	int flags = 0;
	int ret;

	if (fd < 0 || !ei || !destination || !payload
	    || destination->afi != EIGRP_AFI_IPV4
	    || eigrp_intf_address_read(ei, &local) != EIGRP_RESULT_SUCCESS)
		return -1;
	ifindex = eigrp_intf_ifindex(ei);
	if (!ifindex)
		return -1;
	memcpy(&dst, destination->bytes, sizeof(dst));
	if (dst.s_addr == htonl(EIGRP_MULTICAST_ADDRESS)
	    && eigrp_sys_multicast_interface_update(EIGRP_SET, eigrp, ei) < 0)
		return -1;
	memset(&iph, 0, sizeof(iph));
	memset(&sa_dst, 0, sizeof(sa_dst));
	memset(&msg, 0, sizeof(msg));
	memset(control, 0, sizeof(control));
	sa_dst.sin_family = AF_INET;
	sa_dst.sin_addr = dst;
	if (!IN_MULTICAST(ntohl(dst.s_addr)))
		flags = MSG_DONTROUTE;
	iph.ip_hl = sizeof(struct ip) / 4;
	iph.ip_v = IPVERSION;
	iph.ip_tos = IPTOS_PREC_INTERNETCONTROL;
	iph.ip_len = (uint16_t)(sizeof(struct ip) + length);
	iph.ip_ttl = EIGRP_IP_TTL;
	iph.ip_p = IPPROTO_EIGRPIGP;
	memcpy(&iph.ip_src, local.address.bytes, sizeof(iph.ip_src));
	iph.ip_dst = dst;
	msg.msg_name = &sa_dst;
	msg.msg_namelen = sizeof(sa_dst);
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = IPPROTO_IP;
	cmsg->cmsg_type = IP_PKTINFO;
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
	pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
	pktinfo->ipi_ifindex = ifindex;
	memcpy(&pktinfo->ipi_spec_dst, local.address.bytes,
	       sizeof(pktinfo->ipi_spec_dst));
	msg.msg_controllen = CMSG_SPACE(sizeof(struct in_pktinfo));
	iov[0].iov_base = &iph;
	iov[0].iov_len = sizeof(iph);
	iov[1].iov_base = (void *)payload;
	iov[1].iov_len = length;
	sockopt_iphdrincl_swab_htosys(&iph);
	ret = sendmsg(fd, &msg, flags);
	sockopt_iphdrincl_swab_systoh(&iph);
	return ret;
}

bool eigrp_sys_ipv4_packet_receive(eigrp_instance_t *eigrp,
				   uint8_t *buffer, size_t capacity,
				   size_t *received_length,
				   eigrp_ifindex_t *ifindex,
				   eigrp_address_t *source,
				   eigrp_address_t *destination,
				   eigrp_packet_rx_meta_t *meta)
{
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	struct ip *iph;
	struct iovec iov;
	struct msghdr msgh;
	uint16_t header_length, ip_length;
	ssize_t ret;
	char control[CMSG_SPACE(SOPT_SIZE_CMSG_IFINDEX_IPV4())];

	if (fd < 0 || !buffer || capacity == 0 || !received_length || !ifindex
	    || !source || !destination || !meta)
		return false;
	memset(&msgh, 0, sizeof(msgh));
	iov.iov_base = buffer;
	iov.iov_len = capacity;
	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;
	msgh.msg_control = control;
	msgh.msg_controllen = sizeof(control);
	ret = recvmsg(fd, &msgh, 0);
	if (ret < (ssize_t)sizeof(struct ip) || (size_t)ret > capacity)
		return false;
	iph = (struct ip *)buffer;
	sockopt_iphdrincl_swab_systoh(iph);
	if (iph->ip_v != 4 || iph->ip_hl < 5)
		return false;
	header_length = (uint16_t)iph->ip_hl * 4U;
	if (header_length > (uint16_t)ret)
		return false;
	ip_length = iph->ip_len;
	if (ip_length != (uint16_t)ret
	    || ip_length < header_length + EIGRP_HEADER_LEN)
		return false;
	memset(source, 0, sizeof(*source));
	memset(destination, 0, sizeof(*destination));
	source->afi = EIGRP_AFI_IPV4;
	destination->afi = EIGRP_AFI_IPV4;
	memcpy(source->bytes, &iph->ip_src, sizeof(iph->ip_src));
	memcpy(destination->bytes, &iph->ip_dst, sizeof(iph->ip_dst));
	*ifindex = (eigrp_ifindex_t)getsockopt_ifindex(AF_INET, &msgh);
	{
		struct interface *ifp = if_lookup_by_index((ifindex_t)*ifindex,
			(vrf_id_t)eigrp_instance_vrf_id(eigrp));
		if (!ifp || !ifp->vrf)
			return false;
		meta->ingress_vrf_id = (eigrp_vrf_id_t)ifp->vrf->vrf_id;
	}
	*received_length = (size_t)ret;
	meta->network_header_length = header_length;
	meta->eigrp_length = ip_length - header_length;
	meta->destination_multicast =
		iph->ip_dst.s_addr == htonl(EIGRP_MULTICAST_ADDRESS);
	return true;
}

