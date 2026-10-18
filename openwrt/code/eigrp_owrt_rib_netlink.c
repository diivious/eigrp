// SPDX-License-Identifier: GPL-2.0-or-later
/* Kernel rtnetlink translation for the OpenWrt EIGRP host RIB.
 * Copyright (C) 2026 Donnie V. Savage
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "eigrp_owrt_rib.h"

#ifndef RTPROT_EIGRP
#define RTPROT_EIGRP 192
#endif
#define EIGRP_OWRT_RIB_MAX_HOPS 32U

static int eigrp_owrt_rib_family(eigrp_afi_t afi)
{
	if (afi == EIGRP_AFI_IPV4)
		return AF_INET;
	if (afi == EIGRP_AFI_IPV6)
		return AF_INET6;
	return -1;
}

static bool eigrp_owrt_rib_attr(struct nlmsghdr *msg, size_t capacity,
	unsigned short kind, const void *data, size_t size)
{
	size_t at = NLMSG_ALIGN(msg->nlmsg_len);
	size_t total = RTA_LENGTH(size);
	struct rtattr *attr;

	if (at > capacity || total > capacity - at ||
	    RTA_ALIGN(total) > capacity - at)
		return false;
	attr = (struct rtattr *)((char *)msg + at);
	attr->rta_type = kind;
	attr->rta_len = (unsigned short)total;
	if (size)
		memcpy(RTA_DATA(attr), data, size);
	msg->nlmsg_len = (unsigned int)(at + RTA_ALIGN(total));
	return true;
}

static uint32_t eigrp_owrt_rib_priority(uint64_t metric)
{
	return metric > UINT32_MAX ? UINT32_MAX : (uint32_t)metric;
}

int eigrp_owrt_rib_message_build(bool add, bool replace,
	const eigrp_rib_route_t *route, void *buffer, size_t capacity)
{
	struct nlmsghdr *msg = buffer;
	struct rtmsg *rtm;
	size_t width, i;
	uint32_t priority;
	int family;

	if (!buffer || !route || capacity < NLMSG_SPACE(sizeof(*rtm)) ||
	    route->nexthop_count == 0 ||
	    route->nexthop_count > EIGRP_OWRT_RIB_MAX_HOPS || !route->nexthops)
		return -1;
	family = eigrp_owrt_rib_family(route->prefix.address.afi);
	if (family < 0 || route->prefix.prefix_length >
	    (family == AF_INET ? 32 : 128))
		return -1;
	width = family == AF_INET ? 4U : 16U;
	for (i = 0; i < route->nexthop_count; i++) {
		const eigrp_rib_nexthop_t *nh = &route->nexthops[i];
		if (nh->ifindex == 0 || (nh->gateway_present &&
		    nh->gateway.afi != route->prefix.address.afi))
			return -1;
	}
	memset(buffer, 0, capacity);
	msg->nlmsg_len = NLMSG_LENGTH(sizeof(*rtm));
	msg->nlmsg_type = add ? RTM_NEWROUTE : RTM_DELROUTE;
	msg->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	if (add)
		msg->nlmsg_flags |= NLM_F_CREATE | (replace ? NLM_F_REPLACE : NLM_F_EXCL);
	rtm = NLMSG_DATA(msg);
	rtm->rtm_family = (unsigned char)family;
	rtm->rtm_dst_len = route->prefix.prefix_length;
	rtm->rtm_table = RT_TABLE_MAIN;
	rtm->rtm_protocol = RTPROT_EIGRP;
	/* Device-only paths are link scoped. A gateway makes the route
	 * globally scoped even when the next-hop address is link-local. */
	rtm->rtm_scope = RT_SCOPE_LINK;
	for (i = 0; i < route->nexthop_count; i++)
		if (route->nexthops[i].gateway_present) {
			rtm->rtm_scope = RT_SCOPE_UNIVERSE;
			break;
		}
	rtm->rtm_type = RTN_UNICAST;
	if (!eigrp_owrt_rib_attr(msg, capacity, RTA_DST,
	    route->prefix.address.bytes, width))
		return -1;
	priority = eigrp_owrt_rib_priority(route->metric);
	if (!eigrp_owrt_rib_attr(msg, capacity, RTA_PRIORITY,
	    &priority, sizeof(priority)))
		return -1;
	if (route->nexthop_count == 1) {
		const eigrp_rib_nexthop_t *nh = &route->nexthops[0];
		if (!eigrp_owrt_rib_attr(msg, capacity, RTA_OIF,
		    &nh->ifindex, sizeof(nh->ifindex)))
			return -1;
		if (nh->gateway_present &&
		    !eigrp_owrt_rib_attr(msg, capacity, RTA_GATEWAY,
		    nh->gateway.bytes, width))
			return -1;
	} else {
		/* Linux rtnexthop weight is encoded as hops = weight - 1. */
		union {
			struct rtnexthop alignment;
			unsigned char bytes[2048];
		} multipath = {0};
		size_t length = 0;
		for (i = 0; i < route->nexthop_count; i++) {
			const eigrp_rib_nexthop_t *nh = &route->nexthops[i];
			struct rtnexthop *rnh;
			size_t entry = RTNH_ALIGN(sizeof(*rnh));
			if (length + entry + RTA_SPACE(width) > sizeof(multipath.bytes))
				return -1;
			rnh = (struct rtnexthop *)(multipath.bytes + length);
			rnh->rtnh_len = sizeof(*rnh);
			rnh->rtnh_ifindex = (int)nh->ifindex;
			rnh->rtnh_hops = (unsigned char)
				(nh->weight > 256 ? 255 : nh->weight ? nh->weight - 1 : 0);
			if (nh->gateway_present) {
				struct rtattr *gw = (void *)(multipath.bytes + length + entry);
				gw->rta_type = RTA_GATEWAY;
				gw->rta_len = RTA_LENGTH(width);
				memcpy(RTA_DATA(gw), nh->gateway.bytes, width);
				rnh->rtnh_len = (unsigned short)(entry + RTA_ALIGN(gw->rta_len));
			}
			length += RTNH_ALIGN(rnh->rtnh_len);
		}
		if (!eigrp_owrt_rib_attr(msg, capacity, RTA_MULTIPATH,
		    multipath.bytes, length))
			return -1;
	}
	return (int)msg->nlmsg_len;
}

static eigrp_redist_protocol_t eigrp_owrt_rib_protocol(uint8_t proto)
{
	switch (proto) {
	case RTPROT_KERNEL:
		return EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED;
	case RTPROT_STATIC:
		return EIGRP_REDISTRIBUTE_PROTOCOL_STATIC;
	case RTPROT_RIP:
		return EIGRP_REDISTRIBUTE_PROTOCOL_RIP;
	case RTPROT_OSPF:
		return EIGRP_REDISTRIBUTE_PROTOCOL_OSPF;
	case RTPROT_ISIS:
		return EIGRP_REDISTRIBUTE_PROTOCOL_ISIS;
	case RTPROT_BGP:
		return EIGRP_REDISTRIBUTE_PROTOCOL_BGP;
	default:
		return EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED;
	}
}

int eigrp_owrt_rib_message_decode(const struct nlmsghdr *msg,
	eigrp_rib_route_t *route, eigrp_rib_nexthop_t *hops,
	size_t hop_capacity)
{
	const struct rtmsg *rtm;
	struct rtattr *attr;
	size_t width;
	uint32_t table, priority = 0, oif = 0;
	const void *dst = NULL, *gateway = NULL;
	const struct rtattr *multipath = NULL;
	int left, family;
	size_t count = 0;

	if (!msg || !route || !hops || !hop_capacity ||
	    msg->nlmsg_len < NLMSG_LENGTH(sizeof(*rtm)))
		return -1;
	if (msg->nlmsg_type != RTM_NEWROUTE && msg->nlmsg_type != RTM_DELROUTE)
		return 0;
	rtm = NLMSG_DATA(msg);
	family = rtm->rtm_family;
	if (family != AF_INET && family != AF_INET6)
		return 0;
	width = family == AF_INET ? 4U : 16U;
	if (rtm->rtm_dst_len > width * 8U)
		return -1;
	if (rtm->rtm_type != RTN_UNICAST ||
	    eigrp_owrt_rib_protocol(rtm->rtm_protocol) ==
	    EIGRP_REDISTRIBUTE_PROTOCOL_UNSPECIFIED)
		return 0;
	memset(route, 0, sizeof(*route));
	table = rtm->rtm_table;
	left = RTM_PAYLOAD(msg);
	for (attr = RTM_RTA(rtm); RTA_OK(attr, left); attr = RTA_NEXT(attr, left)) {
		switch (attr->rta_type) {
		case RTA_TABLE:
			if (RTA_PAYLOAD(attr) < sizeof(table))
				return -1;
			memcpy(&table, RTA_DATA(attr), sizeof(table));
			break;
		case RTA_DST:
			if (RTA_PAYLOAD(attr) < width)
				return -1;
			dst = RTA_DATA(attr);
			break;
		case RTA_PRIORITY:
			if (RTA_PAYLOAD(attr) < sizeof(priority))
				return -1;
			memcpy(&priority, RTA_DATA(attr), sizeof(priority));
			break;
		case RTA_OIF:
			if (RTA_PAYLOAD(attr) < sizeof(oif))
				return -1;
			memcpy(&oif, RTA_DATA(attr), sizeof(oif));
			break;
		case RTA_GATEWAY:
			if (RTA_PAYLOAD(attr) < width)
				return -1;
			gateway = RTA_DATA(attr);
			break;
		case RTA_MULTIPATH:
			multipath = attr;
			break;
		default:
			break;
		}
	}
	if (left != 0)
		return -1;
	if (table != RT_TABLE_MAIN)
		return 0;
	if (dst)
		memcpy(route->prefix.address.bytes, dst, width);
	route->prefix.address.afi = family == AF_INET ? EIGRP_AFI_IPV4 : EIGRP_AFI_IPV6;
	route->prefix.prefix_length = rtm->rtm_dst_len;
	route->metric = priority;
	route->redist.source.protocol = eigrp_owrt_rib_protocol(rtm->rtm_protocol);
	route->redist.source.route_instance = 0;
	if (multipath) {
		struct rtnexthop *rnh;
		int remainder = RTA_PAYLOAD(multipath);
		for (rnh = RTA_DATA(multipath); RTNH_OK(rnh, remainder);
		     rnh = RTNH_NEXT(rnh)) {
			struct rtattr *sub;
			int sublen = rnh->rtnh_len - sizeof(*rnh);
			if (count >= hop_capacity || rnh->rtnh_ifindex <= 0)
				return -1;
			memset(&hops[count], 0, sizeof(hops[count]));
			hops[count].ifindex = (eigrp_ifindex_t)rnh->rtnh_ifindex;
			hops[count].weight = rnh->rtnh_hops + 1U;
			for (sub = RTNH_DATA(rnh); RTA_OK(sub, sublen);
			     sub = RTA_NEXT(sub, sublen)) {
				if (sub->rta_type != RTA_GATEWAY)
					continue;
				if (RTA_PAYLOAD(sub) < width)
					return -1;
				hops[count].gateway_present = true;
				hops[count].gateway.afi = route->prefix.address.afi;
				memcpy(hops[count].gateway.bytes, RTA_DATA(sub), width);
			}
			if (sublen != 0)
				return -1;
			count++;
			remainder -= RTNH_ALIGN(rnh->rtnh_len);
		}
		if (remainder != 0)
			return -1;
	} else if (oif) {
		memset(&hops[0], 0, sizeof(hops[0]));
		hops[0].ifindex = oif;
		hops[0].weight = 1;
		if (gateway) {
			hops[0].gateway_present = true;
			hops[0].gateway.afi = route->prefix.address.afi;
			memcpy(hops[0].gateway.bytes, gateway, width);
		}
		count = 1;
	}
	route->nexthops = hops;
	route->nexthop_count = count;
	return 1;
}
