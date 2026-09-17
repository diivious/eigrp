// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP IPv4 address-family vector binding.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_types.h"

#define EIGRP_IPV4_ADDRESS_BYTES 4U
#define EIGRP_IPV4_PREFIX_LENGTH_BYTES 1U
#define EIGRP_IPV4_PACKET_WRITE_IPHL_SHIFT 2

static void eigrp_ipv4_packet_header_dump(const struct ip *iph)
{
	if (!iph)
		return;

	zlog_debug("ip_v %u", iph->ip_v);
	zlog_debug("ip_hl %u", iph->ip_hl);
	zlog_debug("ip_tos %u", iph->ip_tos);
	zlog_debug("ip_len %u", iph->ip_len);
	zlog_debug("ip_id %u", (uint32_t)iph->ip_id);
	zlog_debug("ip_off %u", (uint32_t)iph->ip_off);
	zlog_debug("ip_ttl %u", iph->ip_ttl);
	zlog_debug("ip_p %u", iph->ip_p);
	zlog_debug("ip_sum 0x%x", (uint32_t)iph->ip_sum);
	zlog_debug("ip_src %pI4", &iph->ip_src);
	zlog_debug("ip_dst %pI4", &iph->ip_dst);
}

static bool eigrp_ipv4_stream_has(eigrp_stream_t *stream, size_t needed)
{
	if (!stream || stream->endp <= stream->getp)
		return needed == 0;

	return (stream->endp - stream->getp) >= needed;
}

static uint16_t eigrp_ipv4_prefix_bytes(uint8_t prefix_length)
{
	if (prefix_length == 0)
		return 0;

	return ((prefix_length - 1U) / 8U) + 1U;
}

static bool eigrp_ipv4_packet_source_on_link(eigrp_interface_t *ei,
					     const eigrp_addr_t *source)
{
	struct in_addr mask;
	struct in_addr local;
	struct in_addr remote;

	if (!ei || !source || source->afi != AF_INET)
		return false;

	if (ei->type == EIGRP_IFTYPE_POINTOPOINT)
		return true;

	if (ei->address.family != AF_INET)
		return false;

	masklen2ip(ei->address.prefixlen, &mask);
	local.s_addr = ei->address.u.prefix4.s_addr & mask.s_addr;
	remote.s_addr = source->ip.v4.s_addr & mask.s_addr;

	return IPV4_ADDR_SAME(&local, &remote);
}

static int eigrp_ipv4_packet_send(eigrp_instance_t *eigrp,
				  eigrp_interface_t *ei,
				  eigrp_packet_t *packet)
{
	struct sockaddr_in sa_dst;
	struct ip iph;
	struct msghdr msg;
	struct iovec iov[2];
	int flags = 0;
	int ret;

	if (!eigrp || !ei || !packet || !packet->s || packet->dst.afi != AF_INET)
		return -1;

	if (packet->dst.ip.v4.s_addr == htonl(EIGRP_MULTICAST_ADDRESS))
		eigrp_intf_ipmulticast(eigrp, &ei->address, ei->ifp->ifindex);

	memset(&iph, 0, sizeof(iph));
	memset(&sa_dst, 0, sizeof(sa_dst));
	memset(&msg, 0, sizeof(msg));

	sa_dst.sin_family = AF_INET;
#ifdef HAVE_STRUCT_SOCKADDR_IN_SIN_LEN
	sa_dst.sin_len = sizeof(sa_dst);
#endif
	sa_dst.sin_addr = packet->dst.ip.v4;
	sa_dst.sin_port = htons(0);

	if (!IN_MULTICAST(htonl(packet->dst.ip.v4.s_addr)))
		flags = MSG_DONTROUTE;

	iph.ip_hl = sizeof(struct ip) >> EIGRP_IPV4_PACKET_WRITE_IPHL_SHIFT;
	if (sizeof(struct ip)
	    > (unsigned int)(iph.ip_hl << EIGRP_IPV4_PACKET_WRITE_IPHL_SHIFT))
		iph.ip_hl++;

	iph.ip_v = IPVERSION;
	iph.ip_tos = IPTOS_PREC_INTERNETCONTROL;
	iph.ip_len = (iph.ip_hl << EIGRP_IPV4_PACKET_WRITE_IPHL_SHIFT)
		     + packet->length;

#if defined(__DragonFly__)
	iph.ip_len = htons(iph.ip_len);
#endif

	iph.ip_off = 0;
	iph.ip_ttl = EIGRP_IP_TTL;
	iph.ip_p = IPPROTO_EIGRPIGP;
	iph.ip_sum = 0;
	iph.ip_src.s_addr = ei->address.u.prefix4.s_addr;
	iph.ip_dst.s_addr = packet->dst.ip.v4.s_addr;

	msg.msg_name = (caddr_t)&sa_dst;
	msg.msg_namelen = sizeof(sa_dst);
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;

	iov[0].iov_base = (char *)&iph;
	iov[0].iov_len = iph.ip_hl << EIGRP_IPV4_PACKET_WRITE_IPHL_SHIFT;
	iov[1].iov_base = stream_pnt(packet->s);
	iov[1].iov_len = packet->length;

	sockopt_iphdrincl_swab_htosys(&iph);
	ret = sendmsg(eigrp->fd, &msg, flags);
	sockopt_iphdrincl_swab_systoh(&iph);

	if (ret < 0)
		zlog_warn("*** sendmsg in eigrp_ipv4_packet_send failed to %pI4, "
			  "id %d, off %d, len %d, interface %s, mtu %u: %s",
			  &iph.ip_dst, iph.ip_id, iph.ip_off, iph.ip_len,
			  ei->ifp->name, ei->ifp->mtu, safe_strerror(errno));

	return ret;
}

static bool eigrp_ipv4_packet_receive(eigrp_instance_t *eigrp, int fd,
				      eigrp_stream_t *stream,
				      eigrp_interface_t **ei,
				      eigrp_addr_t *source,
				      eigrp_addr_t *destination,
				      eigrp_packet_rx_meta_t *meta)
{
	struct interface *ifp;
	struct connected *connected;
	struct ip *iph;
	struct iovec iov;
	struct msghdr msgh;
	unsigned int ifindex = 0;
	uint16_t ip_header_length;
	uint16_t ip_length;
	int ret;
	char control[CMSG_SPACE(SOPT_SIZE_CMSG_IFINDEX_IPV4())];

	if (!eigrp || !stream || !ei || !source || !destination || !meta)
		return false;

	*ei = NULL;
	memset(source, 0, sizeof(*source));
	memset(destination, 0, sizeof(*destination));
	memset(meta, 0, sizeof(*meta));
	memset(&msgh, 0, sizeof(msgh));

	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;
	msgh.msg_control = (caddr_t)control;
	msgh.msg_controllen = sizeof(control);

	ret = stream_recvmsg(stream, fd, &msgh, 0, EIGRP_PACKET_MAX_LEN + 1);
	if (ret < 0) {
		zlog_warn("stream_recvmsg failed: %s", safe_strerror(errno));
		return false;
	}

	if ((unsigned int)ret < sizeof(*iph)) {
		zlog_warn("eigrp_ipv4_packet_receive: discarding runt packet of length %d "
			  "(IPv4 header size is %u)",
			  ret, (unsigned int)sizeof(*iph));
		return false;
	}

	iph = (struct ip *)STREAM_DATA(stream);
	sockopt_iphdrincl_swab_systoh(iph);

	if (iph->ip_v != 4) {
		zlog_warn("eigrp_ipv4_packet_receive: discarding non-IPv4 packet version %u",
			  iph->ip_v);
		return false;
	}

	ip_header_length = iph->ip_hl * 4U;
	if (iph->ip_hl < 5 || ip_header_length > (uint16_t)ret) {
		zlog_warn("eigrp_ipv4_packet_receive: discarding packet with invalid IPv4 header length %u",
			  (uint8_t)ip_header_length);
		return false;
	}

	ip_length = iph->ip_len;

#if defined(__FreeBSD__) && (__FreeBSD_version < 1000000)
	ip_length = ip_length + ip_header_length;
#endif

#if defined(__DragonFly__)
	ip_length = ntohs(iph->ip_len) + ip_header_length;
#endif

	if (ret != ip_length) {
		zlog_warn("eigrp_ipv4_packet_receive read length mismatch: ip_len is %u, "
			  "but recvmsg returned %d",
			  ip_length, ret);
		return false;
	}

	if (ip_length < ip_header_length + EIGRP_HEADER_LEN) {
		zlog_warn("eigrp_ipv4_packet_receive: discarding runt EIGRP packet from %pI4 length %u",
			  &iph->ip_src, ip_length);
		return false;
	}

	source->afi = AF_INET;
	source->ip.v4 = iph->ip_src;
	destination->afi = AF_INET;
	destination->ip.v4 = iph->ip_dst;

	if (IS_DEBUG_EIGRP_TRANSMIT(0, DETAIL))
		eigrp_ipv4_packet_header_dump(iph);

	ifindex = getsockopt_ifindex(AF_INET, &msgh);
	ifp = if_lookup_by_index(ifindex, eigrp->vrf_id);
	if (!ifp) {
		connected = if_lookup_address((void *)&iph->ip_src, AF_INET,
					      eigrp->vrf_id);
		if (!connected)
			return false;
		ifp = connected->ifp;
	}

	*ei = ifp->info;
	if (!*ei)
		return false;

	if ((*ei)->ifp != ifp) {
		if (IS_DEBUG_EIGRP_TRANSMIT(0, STRANGE))
			zlog_warn("Packet from [%pI4] received on wrong link %s",
				  &iph->ip_src, ifp->name);
		*ei = NULL;
		return false;
	}

	if (eigrp_intf_lookup_by_local_addr(eigrp, NULL, iph->ip_src)
	    || IPV4_ADDR_SAME(&iph->ip_src, &(*ei)->address.u.prefix4)) {
		if (IS_DEBUG_EIGRP_TRANSMIT(0, STRANGE))
			zlog_debug("eigrp_ipv4_packet_receive[%pI4]: Dropping self-originated packet",
				   &iph->ip_src);
		*ei = NULL;
		return false;
	}

	meta->network_header_length = ip_header_length;
	meta->eigrp_length = ip_length - ip_header_length;
	meta->destination_multicast =
		iph->ip_dst.s_addr == htonl(EIGRP_MULTICAST_ADDRESS);

	return true;
}

static uint16_t eigrp_ipv4_packet_address_decode(eigrp_stream_t *stream,
					 eigrp_addr_t *address)
{
	if (!stream || !address
	    || !eigrp_ipv4_stream_has(stream, EIGRP_IPV4_ADDRESS_BYTES))
		return 0;

	memset(address, 0, sizeof(*address));
	address->afi = AF_INET;
	stream_get(&address->ip.v4, stream, EIGRP_IPV4_ADDRESS_BYTES);
	return EIGRP_IPV4_ADDRESS_BYTES;
}

static uint16_t eigrp_ipv4_packet_address_encode(eigrp_stream_t *stream,
					 const eigrp_addr_t *address)
{
	if (!stream || !address || (address->afi != 0 && address->afi != AF_INET))
		return 0;

	stream_put(stream, &address->ip.v4, EIGRP_IPV4_ADDRESS_BYTES);
	return EIGRP_IPV4_ADDRESS_BYTES;
}

static uint16_t eigrp_ipv4_packet_prefix_decode(eigrp_stream_t *stream,
					eigrp_prefix_t *prefix)
{
	size_t start;
	uint16_t address_length;
	uint8_t prefix_length;

	if (!stream || !prefix
	    || !eigrp_ipv4_stream_has(stream, EIGRP_IPV4_PREFIX_LENGTH_BYTES))
		return 0;

	start = stream_get_getp(stream);
	prefix_length = stream_getc(stream);
	if (prefix_length > IPV4_MAX_BITLEN) {
		stream_set_getp(stream, start);
		return 0;
	}

	address_length = eigrp_ipv4_prefix_bytes(prefix_length);
	if (!eigrp_ipv4_stream_has(stream, address_length)) {
		stream_set_getp(stream, start);
		return 0;
	}

	memset(prefix, 0, sizeof(*prefix));
	prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	prefix->prefix_length = prefix_length;
	if (address_length)
		stream_get(prefix->address.bytes, stream, address_length);

	return EIGRP_IPV4_PREFIX_LENGTH_BYTES + address_length;
}

static uint16_t eigrp_ipv4_packet_prefix_encode(eigrp_stream_t *stream,
					const eigrp_prefix_t *prefix)
{
	uint16_t address_length;

	if (!stream || !prefix
	    || prefix->address.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || prefix->prefix_length > IPV4_MAX_BITLEN)
		return 0;

	address_length = eigrp_ipv4_prefix_bytes(prefix->prefix_length);
	stream_putc(stream, prefix->prefix_length);
	if (address_length)
		stream_put(stream, prefix->address.bytes, address_length);

	return EIGRP_IPV4_PREFIX_LENGTH_BYTES + address_length;
}

/*
 * Route descriptors still carry the legacy runtime struct prefix.  Keep that
 * representation detail inside the AF module while TLV1/TLV2 operate only on
 * the selected AF vector.  When the topology descriptor is converted to the
 * portable eigrp_prefix_t, these two wrappers can collapse into the generic
 * packet_prefix_encode/decode entries above.
 */
static uint16_t eigrp_ipv4_packet_route_prefix_decode(
	eigrp_stream_t *stream, eigrp_route_descriptor_t *route)
{
	eigrp_prefix_t prefix;
	size_t start;
	uint16_t decoded;

	if (!stream || !route)
		return 0;

	start = stream_get_getp(stream);
	decoded = eigrp_ipv4_packet_prefix_decode(stream, &prefix);
	if (!decoded)
		return 0;

	if (prefix.address.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || prefix.prefix_length > IPV4_MAX_BITLEN) {
		stream_set_getp(stream, start);
		return 0;
	}

	memset(&route->dest, 0, sizeof(route->dest));
	route->dest.family = AF_INET;
	route->dest.prefixlen = prefix.prefix_length;
	memcpy(&route->dest.u.prefix4, prefix.address.bytes,
	       EIGRP_IPV4_ADDRESS_BYTES);
	return decoded;
}

static uint16_t eigrp_ipv4_packet_route_prefix_encode(
	eigrp_stream_t *stream, const eigrp_route_descriptor_t *route)
{
	const struct prefix *destination;
	eigrp_prefix_t prefix;

	if (!stream || !route)
		return 0;

	destination = route->prefix ? route->prefix->destination : &route->dest;
	if (!destination || destination->family != AF_INET
	    || destination->prefixlen > IPV4_MAX_BITLEN)
		return 0;

	memset(&prefix, 0, sizeof(prefix));
	prefix.address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	prefix.prefix_length = destination->prefixlen;
	memcpy(prefix.address.bytes, &destination->u.prefix4,
	       EIGRP_IPV4_ADDRESS_BYTES);

	return eigrp_ipv4_packet_prefix_encode(stream, &prefix);
}

static int eigrp_ipv4_addr_snprintf(char *buf, size_t len,
				    const eigrp_addr_t *address)
{
	if (!buf || !len || !address || address->afi != AF_INET)
		return -1;

	if (!inet_ntop(AF_INET, &address->ip.v4, buf, len)) {
		buf[0] = '\0';
		return -1;
	}

	return (int)strlen(buf);
}

static int eigrp_ipv4_prefix_snprintf(char *buf, size_t len,
				      const eigrp_prefix_t *prefix)
{
	char address[INET_ADDRSTRLEN];
	int written;

	if (!buf || !len || !prefix
	    || prefix->address.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || prefix->prefix_length > IPV4_MAX_BITLEN)
		return -1;

	if (!inet_ntop(AF_INET, prefix->address.bytes, address, sizeof(address))) {
		buf[0] = '\0';
		return -1;
	}

	written = snprintf(buf, len, "%s/%u", address, prefix->prefix_length);
	if (written < 0 || (size_t)written >= len) {
		buf[0] = '\0';
		return -1;
	}

	return written;
}

static eigrp_result_t eigrp_ipv4_address_validate(
	const eigrp_address_t *address)
{
	if (!address || address->afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_ipv4_prefix_validate(const eigrp_prefix_t *prefix)
{
	if (!prefix || prefix->address.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || prefix->prefix_length > IPV4_MAX_BITLEN)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_ipv4_network_validate(const eigrp_prefix_t *network)
{
	return eigrp_ipv4_prefix_validate(network);
}

static eigrp_result_t eigrp_ipv4_summary_auto_prefix(
	const eigrp_prefix_t *component, eigrp_prefix_t *summary)
{
	(void)component;
	(void)summary;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

void eigrp_ipv4_init(eigrp_af_vectors_t *vectors)
{
	if (!vectors)
		return;

	memset(vectors, 0, sizeof(*vectors));
	vectors->afi = EIGRP_ADDRESS_FAMILY_IPV4;
	vectors->packet_send = eigrp_ipv4_packet_send;
	vectors->packet_receive = eigrp_ipv4_packet_receive;
	vectors->packet_source_on_link = eigrp_ipv4_packet_source_on_link;
	vectors->packet_address_bytes = EIGRP_IPV4_ADDRESS_BYTES;
	vectors->packet_address_decode = eigrp_ipv4_packet_address_decode;
	vectors->packet_address_encode = eigrp_ipv4_packet_address_encode;
	vectors->packet_prefix_decode = eigrp_ipv4_packet_prefix_decode;
	vectors->packet_prefix_encode = eigrp_ipv4_packet_prefix_encode;
	vectors->classic_internal_tlv_type = EIGRP_TLV_IPv4_INT;
	vectors->classic_external_tlv_type = EIGRP_TLV_IPv4_EXT;
	vectors->multiprotocol_afi = EIGRP_AF_IPv4;
	vectors->packet_route_prefix_decode =
		eigrp_ipv4_packet_route_prefix_decode;
	vectors->packet_route_prefix_encode =
		eigrp_ipv4_packet_route_prefix_encode;
	vectors->addr_snprintf = eigrp_ipv4_addr_snprintf;
	vectors->prefix_snprintf = eigrp_ipv4_prefix_snprintf;
	vectors->address_validate = eigrp_ipv4_address_validate;
	vectors->prefix_validate = eigrp_ipv4_prefix_validate;
	vectors->network_validate = eigrp_ipv4_network_validate;
	vectors->summary_auto_prefix = eigrp_ipv4_summary_auto_prefix;
}
