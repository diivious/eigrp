// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP IPv4 address-family vector binding.
 *
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <arpa/inet.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_types.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"

#define EIGRP_IPV4_ADDRESS_BYTES 4U
#define EIGRP_IPV4_PREFIX_LENGTH_BYTES 1U

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
	uint32_t local, remote, mask;

	if (!ei || !source || source->afi != AF_INET)
		return false;
	if (ei->type == EIGRP_IFTYPE_POINTOPOINT)
		return true;
	if (ei->address.address.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || ei->address.prefix_length > 32)
		return false;

	memcpy(&local, ei->address.address.bytes, sizeof(local));
	remote = source->ip.v4.s_addr;
	local = ntohl(local);
	remote = ntohl(remote);
	mask = ei->address.prefix_length == 0 ? 0
	       : 0xffffffffU << (32U - ei->address.prefix_length);
	return (local & mask) == (remote & mask);
}

static int eigrp_ipv4_packet_send(eigrp_instance_t *eigrp,
                                  eigrp_interface_t *ei,
                                  eigrp_packet_t *packet)
{
	eigrp_address_t destination;

	if (!eigrp || !ei || !packet || !packet->s || packet->dst.afi != AF_INET)
		return -1;
	memset(&destination, 0, sizeof(destination));
	destination.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	memcpy(destination.bytes, &packet->dst.ip.v4, sizeof(packet->dst.ip.v4));

	return eigrp_sys_ipv4_packet_send(
		eigrp, ei, &destination, eigrp_stream_data(packet->s),
		packet->length);
}

static bool eigrp_ipv4_packet_receive(eigrp_instance_t *eigrp,
                                      eigrp_stream_t *stream,
                                      eigrp_interface_t **ei,
                                      eigrp_addr_t *source,
                                      eigrp_addr_t *destination,
                                      eigrp_packet_rx_meta_t *meta)
{
	eigrp_ifindex_t ifindex = 0;
	eigrp_address_t public_source;
	eigrp_address_t public_destination;
	size_t received_length = 0;

	if (!eigrp || !stream || !ei || !source || !destination || !meta)
		return false;
	*ei = NULL;
	eigrp_stream_reset(stream);
	if (!eigrp_sys_ipv4_packet_receive(
		eigrp, eigrp_stream_data(stream), stream->size, &received_length,
		&ifindex, &public_source, &public_destination, meta))
		return false;
	if (public_source.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || public_destination.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || received_length > stream->size)
		return false;
	eigrp_stream_set_endp(stream, received_length);
	memset(source, 0, sizeof(*source));
	memset(destination, 0, sizeof(*destination));
	source->afi = AF_INET;
	destination->afi = AF_INET;
	memcpy(&source->ip.v4, public_source.bytes, sizeof(source->ip.v4));
	memcpy(&destination->ip.v4, public_destination.bytes,
	       sizeof(destination->ip.v4));

	*ei = eigrp_intf_lookup_by_ifindex(eigrp, ifindex);
	if (!*ei) {
		eigrp_interface_t *candidate;
		eigrp_list_node_t *node;
		for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, candidate)) {
			if (!eigrp_ipv4_packet_source_on_link(candidate, source))
				continue;
			*ei = candidate;
			break;
		}
	}
	if (!*ei || !eigrp_ipv4_packet_source_on_link(*ei, source)) {
		*ei = NULL;
		return false;
	}
	if (eigrp_intf_lookup_by_local_addr(eigrp, source)) {
		*ei = NULL;
		return false;
	}
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
	eigrp_stream_get(&address->ip.v4, stream, EIGRP_IPV4_ADDRESS_BYTES);
	return EIGRP_IPV4_ADDRESS_BYTES;
}

static uint16_t eigrp_ipv4_packet_address_encode(eigrp_stream_t *stream,
					 const eigrp_addr_t *address)
{
	if (!stream || !address || (address->afi != 0 && address->afi != AF_INET))
		return 0;

	eigrp_stream_put(stream, &address->ip.v4, EIGRP_IPV4_ADDRESS_BYTES);
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

	start = eigrp_stream_get_getp(stream);
	prefix_length = eigrp_stream_getc(stream);
	if (prefix_length > EIGRP_IPV4_MAX_BITLEN) {
		eigrp_stream_set_getp(stream, start);
		return 0;
	}

	address_length = eigrp_ipv4_prefix_bytes(prefix_length);
	if (!eigrp_ipv4_stream_has(stream, address_length)) {
		eigrp_stream_set_getp(stream, start);
		return 0;
	}

	memset(prefix, 0, sizeof(*prefix));
	prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	prefix->prefix_length = prefix_length;
	if (address_length)
		eigrp_stream_get(prefix->address.bytes, stream, address_length);
	eigrp_prefix_normalize(prefix);

	return EIGRP_IPV4_PREFIX_LENGTH_BYTES + address_length;
}

static uint16_t eigrp_ipv4_packet_prefix_encode(eigrp_stream_t *stream,
					const eigrp_prefix_t *prefix)
{
	eigrp_prefix_t normalized;
	uint16_t address_length;

	if (!stream || !prefix
	    || prefix->address.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || prefix->prefix_length > EIGRP_IPV4_MAX_BITLEN)
		return 0;

	normalized = *prefix;
	eigrp_prefix_normalize(&normalized);
	address_length = eigrp_ipv4_prefix_bytes(normalized.prefix_length);
	eigrp_stream_putc(stream, normalized.prefix_length);
	if (address_length)
		eigrp_stream_put(stream, normalized.address.bytes, address_length);

	return EIGRP_IPV4_PREFIX_LENGTH_BYTES + address_length;
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

static eigrp_result_t eigrp_ipv4_summary_auto_prefix(
	const eigrp_prefix_t *component, eigrp_prefix_t *summary)
{
	uint8_t classful_length;
	uint8_t first_octet;

	if (!component || component->address.afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || !eigrp_prefix_valid(component) || !summary)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	first_octet = component->address.bytes[0];
	if (first_octet < 128)
		classful_length = 8;
	else if (first_octet < 192)
		classful_length = 16;
	else if (first_octet < 224)
		classful_length = 24;
	else
		return EIGRP_RESULT_UNSUPPORTED;

	/* Auto-summary aggregates subnets at their classful major-network
	 * boundary.  A supernet is already less specific than that boundary and
	 * therefore has no classful auto-summary to derive.
	 */
	if (component->prefix_length < classful_length)
		return EIGRP_RESULT_NOT_FOUND;

	memset(summary, 0, sizeof(*summary));
	summary->address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	summary->prefix_length = classful_length;
	memcpy(summary->address.bytes, component->address.bytes,
	       classful_length / 8U);
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_ipv4_init(eigrp_af_vectors_t *vectors)
{
	assert(vectors);

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
	vectors->addr_snprintf = eigrp_ipv4_addr_snprintf;
	vectors->summary_auto_prefix = eigrp_ipv4_summary_auto_prefix;
}
