// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP IPv6 address-family vector binding.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "eigrpd/eigrp_types.h"

/*
 * IPv6 runtime/data-path behavior is intentionally not implemented in this
 * pass.  These private functions establish the AF boundary so later IPv6 work
 * does not require family conditionals in common modules.
 */
static int eigrp_ipv6_packet_send(eigrp_instance_t *eigrp,
				  eigrp_interface_t *ei,
				  eigrp_packet_t *packet)
{
	(void)eigrp;
	(void)ei;
	(void)packet;
	return -1;
}

static bool eigrp_ipv6_packet_receive(eigrp_instance_t *eigrp, int fd,
				      eigrp_stream_t *stream,
				      eigrp_interface_t **ei,
				      eigrp_addr_t *source,
				      eigrp_addr_t *destination,
				      eigrp_packet_rx_meta_t *meta)
{
	(void)eigrp;
	(void)fd;
	(void)stream;
	(void)ei;
	(void)source;
	(void)destination;
	(void)meta;
	return false;
}

static bool eigrp_ipv6_packet_source_on_link(eigrp_interface_t *ei,
					     const eigrp_addr_t *source)
{
	(void)ei;
	(void)source;
	return false;
}

static uint16_t
eigrp_ipv6_packet_address_decode(eigrp_stream_t *stream,
				 eigrp_addr_t *address)
{
	(void)stream;
	(void)address;
	return 0;
}

static uint16_t
eigrp_ipv6_packet_address_encode(eigrp_stream_t *stream,
				 const eigrp_addr_t *address)
{
	(void)stream;
	(void)address;
	return 0;
}

static uint16_t
eigrp_ipv6_packet_prefix_decode(eigrp_stream_t *stream, eigrp_prefix_t *prefix)
{
	(void)stream;
	(void)prefix;
	return 0;
}

static uint16_t
eigrp_ipv6_packet_prefix_encode(eigrp_stream_t *stream,
				const eigrp_prefix_t *prefix)
{
	(void)stream;
	(void)prefix;
	return 0;
}

static uint16_t
eigrp_ipv6_packet_route_prefix_decode(eigrp_stream_t *stream,
				      eigrp_route_descriptor_t *route)
{
	(void)stream;
	(void)route;
	return 0;
}

static uint16_t
eigrp_ipv6_packet_route_prefix_encode(eigrp_stream_t *stream,
				      const eigrp_route_descriptor_t *route)
{
	(void)stream;
	(void)route;
	return 0;
}

static int eigrp_ipv6_addr_snprintf(char *buf, size_t len,
				    const eigrp_addr_t *address)
{
	(void)address;
	if (buf && len)
		buf[0] = '\0';
	return -1;
}

static int eigrp_ipv6_prefix_snprintf(char *buf, size_t len,
				      const eigrp_prefix_t *prefix)
{
	(void)prefix;
	if (buf && len)
		buf[0] = '\0';
	return -1;
}

static eigrp_result_t
eigrp_ipv6_address_validate(const eigrp_address_t *address)
{
	(void)address;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

static eigrp_result_t
eigrp_ipv6_prefix_validate(const eigrp_prefix_t *prefix)
{
	(void)prefix;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

static eigrp_result_t
eigrp_ipv6_network_validate(const eigrp_prefix_t *network)
{
	(void)network;
	return EIGRP_RESULT_UNSUPPORTED;
}

static bool eigrp_ipv6_network_interface_match(
	const eigrp_prefix_t *network, const eigrp_prefix_t *interface_address)
{
	(void)network;
	(void)interface_address;
	return false;
}

void eigrp_ipv6_init(eigrp_af_vectors_t *vectors)
{
	if (!vectors)
		return;

	memset(vectors, 0, sizeof(*vectors));
	vectors->afi = EIGRP_ADDRESS_FAMILY_IPV6;
	vectors->packet_send = eigrp_ipv6_packet_send;
	vectors->packet_receive = eigrp_ipv6_packet_receive;
	vectors->packet_source_on_link = eigrp_ipv6_packet_source_on_link;

	vectors->packet_address_bytes = 16;
	vectors->packet_address_decode = eigrp_ipv6_packet_address_decode;
	vectors->packet_address_encode = eigrp_ipv6_packet_address_encode;
	vectors->packet_prefix_decode = eigrp_ipv6_packet_prefix_decode;
	vectors->packet_prefix_encode = eigrp_ipv6_packet_prefix_encode;
	vectors->classic_internal_tlv_type = EIGRP_TLV_IPv6_INT;
	vectors->classic_external_tlv_type = EIGRP_TLV_IPv6_EXT;
	vectors->multiprotocol_afi = EIGRP_AF_IPv6;
	vectors->packet_route_prefix_decode =
		eigrp_ipv6_packet_route_prefix_decode;
	vectors->packet_route_prefix_encode =
		eigrp_ipv6_packet_route_prefix_encode;
	vectors->addr_snprintf = eigrp_ipv6_addr_snprintf;
	vectors->prefix_snprintf = eigrp_ipv6_prefix_snprintf;
	vectors->address_validate = eigrp_ipv6_address_validate;
	vectors->prefix_validate = eigrp_ipv6_prefix_validate;
	vectors->network_validate = eigrp_ipv6_network_validate;
	vectors->network_interface_match = eigrp_ipv6_network_interface_match;
}
