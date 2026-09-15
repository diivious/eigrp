/*
 * EIGRP Packet Processor for EIGRP TLV version 1 processing
 * Copyright (C) 2018
 * Authors:
 *   Donnie Savage
 *
 * The intent of this file is to define a self contained TLV processor which
 * can be agnostic to the main EIGRP routing process.  This will eliminate
 * special processing to handle the narrow versus wide metrics and the TLV
 * packet format differences.
 * All data sent to, or returned from, this TLV processor will be normalized
 * to the system (meaning a delay is a delay and you dont have to worry about
 * conversion)
 */
#include <string.h>

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_types.h"
#include "eigrpd/eigrp_structs.h"

#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_tlv1.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_fsm.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_dump.h"

/**
 * Number of useful macros which map to the packet
 */

// Vector Metrics
#define EIGRP_TLV1_METRIC                                                      \
	uint32_t delay;                                                        \
	uint32_t bandwidth;                                                    \
	uint8_t mtu[3];                                                        \
	uint8_t hop_count;                                                     \
	uint8_t reliability;                                                   \
	uint8_t load;                                                          \
	uint8_t tag;                                                           \
	uint8_t flags;
#define EIGRP_TLV1_METRIC_SIZE 16

// Nexthop (0 if this router)
#define EIGRP_TLV1_IPV4_NEXTHOP uint32_t nexthop;
#define EIGRP_TLV1_IPV4_NEXTHOP_SIZE 4

// External Data for redistributed route
#define EIGRP_TLV1_EXTDATA                                                     \
	uint32_t orig;                                                         \
	uint32_t as;                                                           \
	uint32_t external_tag;                                                 \
	uint32_t metric;                                                       \
	uint16_t reserved;                                                     \
	uint8_t protocol;                                                      \
	uint8_t external_flags;
#define EIGRP_TLV1_EXTDATA_SIZE 20

/**
 * structure to lay over a packet for encoding/decoding
 */
typedef struct eigrp_tlv1_header {
	EIGRP_TLV_HDR
	EIGRP_TLV1_IPV4_NEXTHOP
} __attribute__((packed)) eigrp_tlv1_header_t;

typedef struct eigrp_tlv1_internal_ipv4 {
	EIGRP_TLV_HDR
	EIGRP_TLV1_IPV4_NEXTHOP
	EIGRP_TLV1_METRIC

	// destination address
	uint8_t prefix_length;
	unsigned char destination_part[4];
} __attribute__((packed)) eigrp_tlv1_internal_ipv4_t;

typedef struct eigrp_tlv1_external_ipv4 {
	EIGRP_TLV_HDR
	EIGRP_TLV1_IPV4_NEXTHOP
	EIGRP_TLV1_EXTDATA
	EIGRP_TLV1_METRIC

	// destination address
	uint8_t prefix_length;
	unsigned char destination_part[4];
} __attribute__((packed)) eigrp_tlv1_external_ipv4_t;

typedef struct eigrp_tlv1_extdata {
	EIGRP_TLV1_EXTDATA
} __attribute__((packed)) eigrp_tlv1_extdata_t;

#define EIGRP_TLV1_DEST_PREFIX_SIZE 1
#define EIGRP_IPV4_INT_MIN_TLV                                                \
	(EIGRP_TLV_HDR_SIZE + EIGRP_TLV1_IPV4_NEXTHOP_SIZE                     \
	 + EIGRP_TLV1_METRIC_SIZE + EIGRP_TLV1_DEST_PREFIX_SIZE)
#define EIGRP_IPV4_EXT_MIN_TLV                                                \
	(EIGRP_TLV_HDR_SIZE + EIGRP_TLV1_IPV4_NEXTHOP_SIZE                     \
	 + EIGRP_TLV1_EXTDATA_SIZE + EIGRP_TLV1_METRIC_SIZE                    \
	 + EIGRP_TLV1_DEST_PREFIX_SIZE)

static size_t eigrp_tlv1_stream_remaining(eigrp_stream_t *pkt)
{
	if (pkt->endp <= pkt->getp)
		return 0;

	return pkt->endp - pkt->getp;
}

static bool eigrp_tlv1_stream_has(eigrp_stream_t *pkt, size_t needed)
{
	return eigrp_tlv1_stream_remaining(pkt) >= needed;
}

static uint16_t eigrp_tlv1_ipv4_prefix_bytes(uint8_t prefixlen)
{
	if (prefixlen == 0)
		return 0;

	return ((prefixlen - 1) / 8) + 1;
}

static void eigrp_tlv1_decode_abort(eigrp_stream_t *pkt)
{
	stream_set_getp(pkt, stream_get_endp(pkt));
}

static void eigrp_tlv1_decode_skip(eigrp_stream_t *pkt, size_t tlv_end)
{
	if (tlv_end <= stream_get_endp(pkt))
		stream_set_getp(pkt, tlv_end);
	else
		eigrp_tlv1_decode_abort(pkt);
}

/**
 * extract the external route information provide by the
 * router redistributing the original route
 */
static uint16_t eigrp_tlv1_external_decode(eigrp_stream_t *pkt,
					   eigrp_extdata_t *extdata)
{
	if (!eigrp_tlv1_stream_has(pkt, EIGRP_TLV1_EXTDATA_SIZE))
		return 0;

	extdata->orig = stream_getl(pkt);
	extdata->as = stream_getl(pkt);
	extdata->tag = stream_getl(pkt);
	extdata->metric = stream_getl(pkt);
	extdata->reserved = stream_getw(pkt);
	extdata->protocol = stream_getc(pkt);
	extdata->flags = stream_getc(pkt);

	return EIGRP_TLV1_EXTDATA_SIZE;
}

static uint16_t eigrp_tlv1_external_encode(eigrp_stream_t *pkt,
					   eigrp_extdata_t *extdata)
{
	stream_putl(pkt, extdata->orig);
	stream_putl(pkt, extdata->as);
	stream_putl(pkt, extdata->tag);
	stream_putl(pkt, extdata->metric);
	stream_putw(pkt, extdata->reserved);
	stream_putc(pkt, extdata->protocol);
	stream_putc(pkt, extdata->flags);

	return EIGRP_TLV1_EXTDATA_SIZE;
}

/**
 * extract the vector metric from the TLV and put it into a usable form
 */
static uint16_t eigrp_tlv1_metric_decode(eigrp_stream_t *pkt,
					 eigrp_metrics_t *metric)
{
	if (!eigrp_tlv1_stream_has(pkt, EIGRP_TLV1_METRIC_SIZE))
		return 0;

	/* TLV1.2 provides metric in 32bit form, need to scale */
	metric->delay = eigrp_scaled_to_delay(stream_getl(pkt));
	metric->bandwidth = stream_getl(pkt);
	metric->mtu[2] = stream_getc(pkt);
	metric->mtu[1] = stream_getc(pkt);
	metric->mtu[0] = stream_getc(pkt);
	metric->hop_count = stream_getc(pkt);
	metric->reliability = stream_getc(pkt);
	metric->load = stream_getc(pkt);
	metric->tag = stream_getc(pkt);
	metric->flags = stream_getc(pkt);

	return EIGRP_TLV1_METRIC_SIZE;
}

static uint16_t eigrp_tlv1_metric_encode(eigrp_stream_t *pkt,
					 eigrp_metrics_t *metric)
{
	/*
	 * TLV1.2 supports classic metrics, need to scale it down to 32 bits.
	 */
	stream_putl(pkt, eigrp_delay_to_scaled(metric->delay));

	stream_putl(pkt, metric->bandwidth);
	stream_putc(pkt, metric->mtu[2]);
	stream_putc(pkt, metric->mtu[1]);
	stream_putc(pkt, metric->mtu[0]);
	stream_putc(pkt, metric->hop_count);
	stream_putc(pkt, metric->reliability);
	stream_putc(pkt, metric->load);
	stream_putc(pkt, metric->tag);
	stream_putc(pkt, metric->flags);

	return EIGRP_TLV1_METRIC_SIZE;
}

static uint16_t eigrp_tlv1_addr_decode(eigrp_stream_t *pkt,
				       struct prefix *dest)
{
	uint8_t addr[4] = {0, 0, 0, 0};
	uint8_t prefixlen;
	uint16_t addr_len;

	if (!eigrp_tlv1_stream_has(pkt, EIGRP_TLV1_DEST_PREFIX_SIZE))
		return 0;

	prefixlen = stream_getc(pkt);
	if (prefixlen > IPV4_MAX_BITLEN) {
		zlog_err("%s: Unexpected IPv4 prefix length: %u", __func__,
			 prefixlen);
		return 0;
	}

	addr_len = eigrp_tlv1_ipv4_prefix_bytes(prefixlen);
	if (!eigrp_tlv1_stream_has(pkt, addr_len))
		return 0;

	for (uint16_t i = 0; i < addr_len; i++)
		addr[i] = stream_getc(pkt);

	dest->family = AF_INET;
	dest->prefixlen = prefixlen;
	memcpy(&dest->u.prefix4.s_addr, addr, sizeof(addr));

	return EIGRP_TLV1_DEST_PREFIX_SIZE + addr_len;
}

static uint16_t eigrp_tlv1_addr_encode(eigrp_stream_t *pkt,
				       eigrp_route_descriptor_t *route)
{
	struct prefix *dest = route->prefix->destination;
	uint8_t addr[4];
	uint16_t addr_len;

	if (dest->family != AF_INET || dest->prefixlen > IPV4_MAX_BITLEN) {
		zlog_err("%s: Unexpected IPv4 prefix length: %u", __func__,
			 dest->prefixlen);
		return 0;
	}

	addr_len = eigrp_tlv1_ipv4_prefix_bytes(dest->prefixlen);
	memcpy(addr, &dest->u.prefix4.s_addr, sizeof(addr));

	stream_putc(pkt, dest->prefixlen);
	for (uint16_t i = 0; i < addr_len; i++)
		stream_putc(pkt, addr[i]);

	return EIGRP_TLV1_DEST_PREFIX_SIZE + addr_len;
}

static uint16_t eigrp_tlv1_nexthop_decode(eigrp_instance_t *eigrp,
					  eigrp_stream_t *pkt,
					  eigrp_route_descriptor_t *route)
{
	if (!eigrp_tlv1_stream_has(pkt, EIGRP_TLV1_IPV4_NEXTHOP_SIZE))
		return 0;

	/* if doing no-nexthop-self, then use the source peer */
	if (/*eigrp->no_nextop_self == */ FALSE) {
		route->nexthop.ip.v4.s_addr = stream_getl(pkt);
	} else {
		route->nexthop.ip.v4.s_addr = 0L;
	}
	return EIGRP_TLV1_IPV4_NEXTHOP_SIZE;
}

static uint16_t eigrp_tlv1_nexthop_encode(eigrp_stream_t *pkt,
					  eigrp_route_descriptor_t *route)
{
	stream_putl(pkt, route->nexthop.ip.v4.s_addr);
	return EIGRP_TLV1_IPV4_NEXTHOP_SIZE;
}

static uint16_t eigrp_tlv1_route_tlv_type(eigrp_route_descriptor_t *route)
{
	switch (route->type) {
	case EIGRP_TLV_IPv4_INT:
	case EIGRP_TLV_IPv4_EXT:
		return route->type;
	case EIGRP_INT:
		return EIGRP_TLV_IPv4_INT;
	case EIGRP_EXT:
		return EIGRP_TLV_IPv4_EXT;
	default:
		return 0;
	}
}

/**
 * decode an incoming TLV into a topology route and return it for processing
 */
static eigrp_route_descriptor_t *eigrp_tlv1_decoder(eigrp_instance_t *eigrp,
						    eigrp_neighbor_t *nbr,
						    eigrp_stream_t *pkt,
						    uint16_t pktlen)
{
	eigrp_route_descriptor_t *route = NULL;
	size_t tlv_start = stream_get_getp(pkt);
	size_t tlv_end;
	size_t packet_end;
	size_t remaining;
	uint16_t type, length;
	uint16_t bytes = EIGRP_TLV_HDR_SIZE;
	uint16_t decoded;
	uint16_t min_length;

	(void)pktlen;

	remaining = eigrp_tlv1_stream_remaining(pkt);
	if (remaining < EIGRP_TLV_HDR_SIZE) {
		eigrp_tlv1_decode_abort(pkt);
		return NULL;
	}

	type = stream_getw(pkt);
	length = stream_getw(pkt);

	switch (type) {
	case EIGRP_TLV_IPv4_INT:
		min_length = EIGRP_IPV4_INT_MIN_TLV;
		break;
	case EIGRP_TLV_IPv4_EXT:
		min_length = EIGRP_IPV4_EXT_MIN_TLV;
		break;
	default:
		if (length >= EIGRP_TLV_HDR_SIZE && length <= remaining)
			eigrp_tlv1_decode_skip(pkt, tlv_start + length);
		else
			eigrp_tlv1_decode_abort(pkt);

		if (eigrp_debug_packet_any_enabled(EIGRP_DEBUG_RECV)) {
			zlog_debug(
				"EIGRP TLV: Neighbor(%s): invalid TLV_type(%u)",
				eigrp_print_addr(&nbr->src), type);
		}
		return NULL;
	}

	if (length < min_length || length > remaining) {
		if (eigrp_debug_packet_any_enabled(EIGRP_DEBUG_RECV)) {
			zlog_debug(
				"EIGRP TLV: Neighbor(%s) corrupt packet type=%u length=%u remaining=%zu",
				eigrp_print_addr(&nbr->src), type, length, remaining);
		}
		eigrp_tlv1_decode_abort(pkt);
		return NULL;
	}

	tlv_end = tlv_start + length;
	packet_end = stream_get_endp(pkt);
	stream_set_endp(pkt, tlv_end);

	/* allocate buffer */
	route = eigrp_topology_route_create(nbr->ei);
	if (!route) {
		stream_set_endp(pkt, packet_end);
		eigrp_tlv1_decode_skip(pkt, tlv_end);
		return NULL;
	}

	route->type = type;

	/* decode nexthop */
	bytes += eigrp_tlv1_nexthop_decode(eigrp, pkt, route);
	if (bytes != EIGRP_TLV_HDR_SIZE + EIGRP_TLV1_IPV4_NEXTHOP_SIZE)
		goto malformed;

	/* figure out what type of TLV we are processing */
	switch (type) {
	case EIGRP_TLV_IPv4_EXT:
		bytes += eigrp_tlv1_external_decode(pkt, &route->extdata);
		if (bytes != EIGRP_TLV_HDR_SIZE + EIGRP_TLV1_IPV4_NEXTHOP_SIZE
			     + EIGRP_TLV1_EXTDATA_SIZE)
			goto malformed;

		bytes += eigrp_tlv1_metric_decode(pkt, &route->metric);
		if (bytes != EIGRP_TLV_HDR_SIZE + EIGRP_TLV1_IPV4_NEXTHOP_SIZE
			     + EIGRP_TLV1_EXTDATA_SIZE + EIGRP_TLV1_METRIC_SIZE)
			goto malformed;

		decoded = eigrp_tlv1_addr_decode(pkt, &route->dest);
		if (!decoded)
			goto malformed;
		bytes += decoded;
		break;

	case EIGRP_TLV_IPv4_INT:
		bytes += eigrp_tlv1_metric_decode(pkt, &route->metric);
		if (bytes != EIGRP_TLV_HDR_SIZE + EIGRP_TLV1_IPV4_NEXTHOP_SIZE
			     + EIGRP_TLV1_METRIC_SIZE)
			goto malformed;

		/*
		 * RFC 7868 allows more than one destination in the TLV. This
		 * implementation currently consumes one route descriptor and skips
		 * any remaining destinations so the next decoder pass starts on the
		 * next TLV boundary.
		 */
		decoded = eigrp_tlv1_addr_decode(pkt, &route->dest);
		if (!decoded)
			goto malformed;
		bytes += decoded;
		break;
	}

	if (bytes > length || bytes == EIGRP_TLV_HDR_SIZE)
		goto malformed;

	stream_set_endp(pkt, packet_end);
	eigrp_tlv1_decode_skip(pkt, tlv_end);
	return route;

malformed:
	if (eigrp_debug_packet_any_enabled(EIGRP_DEBUG_RECV)) {
		zlog_debug(
			"EIGRP TLV: Neighbor(%s) malformed TLV type=%u length=%u decoded=%u",
			eigrp_print_addr(&nbr->src), type, length, bytes);
	}
	eigrp_topology_route_free(route);
	stream_set_endp(pkt, packet_end);
	eigrp_tlv1_decode_abort(pkt);
	return NULL;
}

static uint16_t eigrp_tlv1_encoder(eigrp_instance_t *eigrp, eigrp_interface_t *ei,
				   eigrp_neighbor_t *nbr, eigrp_stream_t *pkt,
				   eigrp_route_descriptor_t *route)
{
	size_t tlv_start = stream_get_endp(pkt);
	size_t tlv_end;
	uint16_t type;
	uint16_t length;
	uint16_t encoded;

	if (!ei && nbr)
		ei = nbr->ei;

	/* Filtering
	 * TODO: Work in progress
	 */
	if (ei) {
		if (eigrp_update_prefix_apply(eigrp, ei, EIGRP_FILTER_OUT,
					      route->prefix->destination)) {
			zlog_info(
				"Prefix Filtered:  Setting Metric to EIGRP_MAX_METRIC");
			route->metric.delay = EIGRP_MAX_METRIC;
		}
	}

	type = eigrp_tlv1_route_tlv_type(route);
	if (!type) {
		if (eigrp_debug_packet_any_enabled(EIGRP_DEBUG_SEND)) {
			zlog_debug("Neighbor(%s): invalid TLV_type(%u)",
				   eigrp_print_addr(&nbr->src), route->type);
		}
		return 0;
	}

	stream_putw(pkt, type);
	stream_putw(pkt, 0);

	/* encode nexthop */
	eigrp_tlv1_nexthop_encode(pkt, route);

	switch (type) {
	case EIGRP_TLV_IPv4_EXT:
		eigrp_tlv1_external_encode(pkt, &route->extdata);
		eigrp_tlv1_metric_encode(pkt, &route->metric);
		encoded = eigrp_tlv1_addr_encode(pkt, route);
		break;

	case EIGRP_TLV_IPv4_INT:
		eigrp_tlv1_metric_encode(pkt, &route->metric);
		encoded = eigrp_tlv1_addr_encode(pkt, route);
		break;

	default:
		encoded = 0;
		break;
	}

	if (!encoded) {
		stream_set_endp(pkt, tlv_start);
		return 0;
	}

	tlv_end = stream_get_endp(pkt);
	length = tlv_end - tlv_start;

	stream_set_endp(pkt, tlv_start + 2);
	stream_putw(pkt, length);
	stream_set_endp(pkt, tlv_end);

	return length;
}

void eigrp_tlv1_init(eigrp_tlv_codec_t *codec)
{
	if (!codec)
		return;

	codec->decoder = eigrp_tlv1_decoder;
	codec->encoder = eigrp_tlv1_encoder;
}

void eigrp_tlv1_neighbor_bind(eigrp_neighbor_t *nbr, eigrp_tlv_codec_t *codec)
{
	if (!nbr || !codec)
		return;

	nbr->tlv_version = EIGRP_TLV_32B_VERSION;
	eigrp_neighbor_decoder_bind(nbr, codec);
	eigrp_neighbor_encoder_bind(nbr, codec);
}

void eigrp_tlv1_interface_bind(eigrp_interface_t *ei, eigrp_tlv_codec_t *codec)
{
	if (!ei || !codec || !codec->encoder)
		return;

	ei->encoder = codec->encoder;
}
