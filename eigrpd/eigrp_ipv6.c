// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP IPv6 address-family vector binding.
 *
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
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_prefix.h"
#include "eigrpd/eigrp_types.h"

#define EIGRP_IPV6_ADDRESS_BYTES 16U
#define EIGRP_IPV6_PREFIX_LENGTH_BYTES 1U
#define EIGRP_IPV6_MAX_BITLEN 128U

static bool eigrp_ipv6_stream_has(eigrp_stream_t *stream, size_t needed)
{
	if (!stream || stream->endp <= stream->getp)
		return needed == 0;

	return (stream->endp - stream->getp) >= needed;
}

/*
 * RFC 7868 sections 6.8.4 and 6.9.5 define a different compressed
 * destination length for IPv6 than IPv4.  Prefix lengths that end on an
 * octet boundary still carry the following octet, except /128 which is the
 * full 16-byte address.  /0 carries no address bytes.
 */
static uint16_t eigrp_ipv6_prefix_bytes(uint8_t prefix_length)
{
	if (prefix_length == 0)
		return 0;
	if (prefix_length == EIGRP_IPV6_MAX_BITLEN)
		return EIGRP_IPV6_ADDRESS_BYTES;

	return (prefix_length / 8U) + 1U;
}

static bool eigrp_ipv6_packet_source_on_link(eigrp_interface_t *ei,
					     const eigrp_addr_t *source)
{
	if (!ei || !source || source->afi != AF_INET6)
		return false;

	/* EIGRP for IPv6 accepts neighbors by the receiving link and requires a
	 * link-local source.  The peers do not need a common global prefix.
	 */
	return IN6_IS_ADDR_LINKLOCAL(&source->ip.v6);
}

static uint16_t eigrp_ipv6_packet_address_decode(eigrp_stream_t *stream,
					 eigrp_addr_t *address)
{
	if (!stream || !address
	    || !eigrp_ipv6_stream_has(stream, EIGRP_IPV6_ADDRESS_BYTES))
		return 0;

	memset(address, 0, sizeof(*address));
	address->afi = AF_INET6;
	eigrp_stream_get(&address->ip.v6, stream, EIGRP_IPV6_ADDRESS_BYTES);
	return EIGRP_IPV6_ADDRESS_BYTES;
}

static uint16_t eigrp_ipv6_packet_address_encode(eigrp_stream_t *stream,
					 const eigrp_addr_t *address)
{
	if (!stream || !address
	    || (address->afi != 0 && address->afi != AF_INET6))
		return 0;

	eigrp_stream_put(stream, &address->ip.v6, EIGRP_IPV6_ADDRESS_BYTES);
	return EIGRP_IPV6_ADDRESS_BYTES;
}

static uint16_t eigrp_ipv6_packet_prefix_decode(eigrp_stream_t *stream,
					eigrp_prefix_t *prefix)
{
	size_t start;
	uint16_t address_length;
	uint8_t prefix_length;

	if (!stream || !prefix
	    || !eigrp_ipv6_stream_has(stream, EIGRP_IPV6_PREFIX_LENGTH_BYTES))
		return 0;

	start = eigrp_stream_get_getp(stream);
	prefix_length = eigrp_stream_getc(stream);
	if (prefix_length > EIGRP_IPV6_MAX_BITLEN) {
		eigrp_stream_set_getp(stream, start);
		return 0;
	}

	address_length = eigrp_ipv6_prefix_bytes(prefix_length);
	if (!eigrp_ipv6_stream_has(stream, address_length)) {
		eigrp_stream_set_getp(stream, start);
		return 0;
	}

	memset(prefix, 0, sizeof(*prefix));
	prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV6;
	prefix->prefix_length = prefix_length;
	if (address_length)
		eigrp_stream_get(prefix->address.bytes, stream, address_length);
	eigrp_prefix_normalize(prefix);

	return EIGRP_IPV6_PREFIX_LENGTH_BYTES + address_length;
}

static uint16_t eigrp_ipv6_packet_prefix_encode(eigrp_stream_t *stream,
					const eigrp_prefix_t *prefix)
{
	eigrp_prefix_t normalized;
	uint16_t address_length;

	if (!stream || !prefix
	    || prefix->address.afi != EIGRP_ADDRESS_FAMILY_IPV6
	    || !eigrp_prefix_valid(prefix))
		return 0;

	normalized = *prefix;
	eigrp_prefix_normalize(&normalized);
	address_length = eigrp_ipv6_prefix_bytes(normalized.prefix_length);
	eigrp_stream_putc(stream, normalized.prefix_length);
	if (address_length)
		eigrp_stream_put(stream, normalized.address.bytes, address_length);

	return EIGRP_IPV6_PREFIX_LENGTH_BYTES + address_length;
}

static int eigrp_ipv6_packet_send(eigrp_instance_t *eigrp,
                                  eigrp_interface_t *ei,
                                  eigrp_packet_t *packet)
{
	eigrp_address_t destination;

	if (!eigrp || !ei || !packet || !packet->s || packet->dst.afi != AF_INET6)
		return -1;
	memset(&destination, 0, sizeof(destination));
	destination.afi = EIGRP_ADDRESS_FAMILY_IPV6;
	memcpy(destination.bytes, &packet->dst.ip.v6, sizeof(packet->dst.ip.v6));
	return eigrp_sys_ipv6_packet_send(
		eigrp, ei, &destination, eigrp_stream_data(packet->s), packet->length);
}

static bool eigrp_ipv6_packet_receive(eigrp_instance_t *eigrp,
                                      eigrp_stream_t *stream,
                                      eigrp_interface_t **ei,
                                      eigrp_addr_t *source,
                                      eigrp_addr_t *destination,
                                      eigrp_packet_rx_meta_t *meta)
{
	eigrp_ifindex_t ifindex = 0;
	eigrp_address_t public_source, public_destination;
	size_t received_length = 0;

	if (!eigrp || !stream || !ei || !source || !destination || !meta)
		return false;
	*ei = NULL;
	eigrp_stream_reset(stream);
	if (!eigrp_sys_ipv6_packet_receive(
		    eigrp, eigrp_stream_data(stream), stream->size, &received_length,
		    &ifindex, &public_source, &public_destination, meta))
		return false;
	if (public_source.afi != EIGRP_ADDRESS_FAMILY_IPV6
	    || public_destination.afi != EIGRP_ADDRESS_FAMILY_IPV6
	    || received_length > stream->size)
		return false;
	eigrp_stream_set_endp(stream, received_length);
	memset(source, 0, sizeof(*source));
	memset(destination, 0, sizeof(*destination));
	source->afi = AF_INET6;
	destination->afi = AF_INET6;
	memcpy(&source->ip.v6, public_source.bytes, sizeof(source->ip.v6));
	memcpy(&destination->ip.v6, public_destination.bytes, sizeof(destination->ip.v6));
	*ei = eigrp_intf_lookup_by_ifindex(eigrp, ifindex);
	if (!*ei || !eigrp_ipv6_packet_source_on_link(*ei, source)) {
		*ei = NULL;
		return false;
	}
	return true;
}

static int eigrp_ipv6_addr_snprintf(char *buf, size_t len,
				    const eigrp_addr_t *address)
{
	if (!buf || !len || !address || address->afi != AF_INET6)
		return -1;

	if (!inet_ntop(AF_INET6, &address->ip.v6, buf, len)) {
		buf[0] = '\0';
		return -1;
	}

	return (int)strlen(buf);
}

static eigrp_result_t eigrp_ipv6_summary_auto_prefix(
	const eigrp_prefix_t *component, eigrp_prefix_t *summary)
{
	(void)component;
	(void)summary;
	return EIGRP_RESULT_UNSUPPORTED;
}

void eigrp_ipv6_init(eigrp_af_vectors_t *vectors)
{
	assert(vectors);

	memset(vectors, 0, sizeof(*vectors));
	vectors->afi = EIGRP_ADDRESS_FAMILY_IPV6;

	vectors->packet_send = eigrp_ipv6_packet_send;
	vectors->packet_receive = eigrp_ipv6_packet_receive;
	vectors->packet_source_on_link = eigrp_ipv6_packet_source_on_link;
	vectors->packet_address_bytes = EIGRP_IPV6_ADDRESS_BYTES;
	vectors->packet_address_decode = eigrp_ipv6_packet_address_decode;
	vectors->packet_address_encode = eigrp_ipv6_packet_address_encode;
	vectors->packet_prefix_decode = eigrp_ipv6_packet_prefix_decode;
	vectors->packet_prefix_encode = eigrp_ipv6_packet_prefix_encode;
	vectors->classic_internal_tlv_type = EIGRP_TLV_IPv6_INT;
	vectors->classic_external_tlv_type = EIGRP_TLV_IPv6_EXT;
	vectors->multiprotocol_afi = EIGRP_AF_IPv6;
	vectors->addr_snprintf = eigrp_ipv6_addr_snprintf;
	vectors->summary_auto_prefix = eigrp_ipv6_summary_auto_prefix;

}
