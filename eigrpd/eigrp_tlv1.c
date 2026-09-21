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
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_debug.h"

/* Classic metric section. */
#define EIGRP_TLV1_METRIC_SIZE 16

/* External data for a redistributed route. */
#define EIGRP_TLV1_EXTDATA_SIZE 20

/* Every classic destination starts with an 8-bit prefix length. */
#define EIGRP_TLV1_DEST_PREFIX_SIZE 1

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

static bool eigrp_tlv1_af_ready(const eigrp_instance_t *eigrp)
{
	return eigrp && eigrp->af_vectors.packet_address_bytes
	       && eigrp->af_vectors.packet_address_decode
	       && eigrp->af_vectors.packet_address_encode
	       && eigrp->af_vectors.packet_prefix_decode
	       && eigrp->af_vectors.packet_prefix_encode
	       && eigrp->af_vectors.classic_internal_tlv_type
	       && eigrp->af_vectors.classic_external_tlv_type;
}

static uint16_t eigrp_tlv1_min_length(const eigrp_instance_t *eigrp,
				      bool external)
{
	uint16_t length;

	length = EIGRP_TLV_HDR_SIZE + eigrp->af_vectors.packet_address_bytes
		 + EIGRP_TLV1_METRIC_SIZE + EIGRP_TLV1_DEST_PREFIX_SIZE;
	if (external)
		length += EIGRP_TLV1_EXTDATA_SIZE;

	return length;
}

static void eigrp_tlv1_decode_abort(eigrp_stream_t *pkt)
{
	eigrp_stream_set_getp(pkt, eigrp_stream_get_endp(pkt));
}

static void eigrp_tlv1_decode_skip(eigrp_stream_t *pkt, size_t tlv_end)
{
	if (tlv_end <= eigrp_stream_get_endp(pkt))
		eigrp_stream_set_getp(pkt, tlv_end);
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

	extdata->orig = eigrp_stream_getl(pkt);
	extdata->as = eigrp_stream_getl(pkt);
	extdata->tag = eigrp_stream_getl(pkt);
	extdata->metric = eigrp_stream_getl(pkt);
	extdata->reserved = eigrp_stream_getw(pkt);
	extdata->protocol = eigrp_stream_getc(pkt);
	extdata->flags = eigrp_stream_getc(pkt);

	return EIGRP_TLV1_EXTDATA_SIZE;
}

static uint16_t eigrp_tlv1_external_encode(eigrp_stream_t *pkt,
					   eigrp_extdata_t *extdata)
{
	eigrp_stream_putl(pkt, extdata->orig);
	eigrp_stream_putl(pkt, extdata->as);
	eigrp_stream_putl(pkt, extdata->tag);
	eigrp_stream_putl(pkt, extdata->metric);
	eigrp_stream_putw(pkt, extdata->reserved);
	eigrp_stream_putc(pkt, extdata->protocol);
	eigrp_stream_putc(pkt, extdata->flags);

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
	metric->delay = eigrp_scaled_to_delay(eigrp_stream_getl(pkt));
	metric->bandwidth = eigrp_stream_getl(pkt);
	metric->mtu[2] = eigrp_stream_getc(pkt);
	metric->mtu[1] = eigrp_stream_getc(pkt);
	metric->mtu[0] = eigrp_stream_getc(pkt);
	metric->hop_count = eigrp_stream_getc(pkt);
	metric->reliability = eigrp_stream_getc(pkt);
	metric->load = eigrp_stream_getc(pkt);
	metric->tag = eigrp_stream_getc(pkt);
	metric->flags = eigrp_stream_getc(pkt);

	return EIGRP_TLV1_METRIC_SIZE;
}

static uint16_t eigrp_tlv1_metric_encode(eigrp_stream_t *pkt,
					 eigrp_metrics_t *metric)
{
	/*
	 * TLV1.2 supports classic metrics, need to scale it down to 32 bits.
	 */
	eigrp_stream_putl(pkt, eigrp_delay_to_scaled(metric->delay));

	eigrp_stream_putl(pkt, metric->bandwidth);
	eigrp_stream_putc(pkt, metric->mtu[2]);
	eigrp_stream_putc(pkt, metric->mtu[1]);
	eigrp_stream_putc(pkt, metric->mtu[0]);
	eigrp_stream_putc(pkt, metric->hop_count);
	eigrp_stream_putc(pkt, metric->reliability);
	eigrp_stream_putc(pkt, metric->load);
	eigrp_stream_putc(pkt, metric->tag);
	eigrp_stream_putc(pkt, metric->flags);

	return EIGRP_TLV1_METRIC_SIZE;
}

static uint16_t eigrp_tlv1_route_tlv_type(
	const eigrp_instance_t *eigrp, const eigrp_route_descriptor_t *route)
{
	if (!eigrp_tlv1_af_ready(eigrp) || !route)
		return 0;

	if (route->type == EIGRP_INT
	    || route->type == eigrp->af_vectors.classic_internal_tlv_type)
		return eigrp->af_vectors.classic_internal_tlv_type;

	if (route->type == EIGRP_EXT
	    || route->type == eigrp->af_vectors.classic_external_tlv_type)
		return eigrp->af_vectors.classic_external_tlv_type;

	return 0;
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
	size_t tlv_start;
	size_t tlv_end;
	size_t packet_end;
	size_t remaining;
	uint16_t type, length;
	uint16_t bytes = EIGRP_TLV_HDR_SIZE;
	uint16_t decoded;
	uint16_t min_length;
	bool external;

	(void)pktlen;

	if (!eigrp_tlv1_af_ready(eigrp) || !nbr || !pkt)
		return NULL;

	tlv_start = eigrp_stream_get_getp(pkt);
	remaining = eigrp_tlv1_stream_remaining(pkt);
	if (remaining < EIGRP_TLV_HDR_SIZE) {
		eigrp_tlv1_decode_abort(pkt);
		return NULL;
	}

	type = eigrp_stream_getw(pkt);
	length = eigrp_stream_getw(pkt);

	if (type == eigrp->af_vectors.classic_internal_tlv_type)
		external = false;
	else if (type == eigrp->af_vectors.classic_external_tlv_type)
		external = true;
	else {
		if (length >= EIGRP_TLV_HDR_SIZE && length <= remaining)
			eigrp_tlv1_decode_skip(pkt, tlv_start + length);
		else
			eigrp_tlv1_decode_abort(pkt);

		if (eigrp_debug_packet_any_enabled(EIGRP_DEBUG_RECV)) {
			eigrp_log_debug(
				"EIGRP TLV: Neighbor(%s): invalid TLV_type(%u)",
				eigrp_print_addr(&nbr->src), type);
		}
		return NULL;
	}

	min_length = eigrp_tlv1_min_length(eigrp, external);
	if (length < min_length || length > remaining) {
		if (eigrp_debug_packet_any_enabled(EIGRP_DEBUG_RECV)) {
			eigrp_log_debug(
				"EIGRP TLV: Neighbor(%s) corrupt packet type=%u length=%u remaining=%zu",
				eigrp_print_addr(&nbr->src), type, length, remaining);
		}
		eigrp_tlv1_decode_abort(pkt);
		return NULL;
	}

	tlv_end = tlv_start + length;
	packet_end = eigrp_stream_get_endp(pkt);
	eigrp_stream_set_endp(pkt, tlv_end);

	route = eigrp_topology_route_create(nbr->ei);
	if (!route) {
		eigrp_stream_set_endp(pkt, packet_end);
		eigrp_tlv1_decode_skip(pkt, tlv_end);
		return NULL;
	}

	route->type = type;

	decoded = eigrp->af_vectors.packet_address_decode(pkt, &route->nexthop);
	if (decoded != eigrp->af_vectors.packet_address_bytes)
		goto malformed;
	bytes += decoded;

	if (external) {
		decoded = eigrp_tlv1_external_decode(pkt, &route->extdata);
		if (!decoded)
			goto malformed;
		bytes += decoded;
	}

	decoded = eigrp_tlv1_metric_decode(pkt, &route->metric);
	if (!decoded)
		goto malformed;
	bytes += decoded;

	/*
	 * RFC 7868 allows more than one destination in the TLV.  This
	 * implementation currently consumes one route descriptor and skips any
	 * remaining destinations so the next decoder pass starts on the next TLV
	 * boundary.
	 */
	decoded = eigrp->af_vectors.packet_prefix_decode(pkt, &route->dest);
	if (!decoded)
		goto malformed;
	bytes += decoded;

	if (bytes > length || bytes == EIGRP_TLV_HDR_SIZE)
		goto malformed;

	eigrp_stream_set_endp(pkt, packet_end);
	eigrp_tlv1_decode_skip(pkt, tlv_end);
	return route;

malformed:
	if (eigrp_debug_packet_any_enabled(EIGRP_DEBUG_RECV)) {
		eigrp_log_debug(
			"EIGRP TLV: Neighbor(%s) malformed TLV type=%u length=%u decoded=%u",
			eigrp_print_addr(&nbr->src), type, length, bytes);
	}
	eigrp_topology_route_free(route);
	eigrp_stream_set_endp(pkt, packet_end);
	eigrp_tlv1_decode_abort(pkt);
	return NULL;
}

static uint16_t eigrp_tlv1_encoder(eigrp_instance_t *eigrp,
				   eigrp_interface_t *ei,
				   eigrp_neighbor_t *nbr,
				   eigrp_stream_t *pkt,
				   eigrp_route_descriptor_t *route)
{
	const eigrp_prefix_t *filter_prefix;
	size_t tlv_start;
	size_t tlv_end;
	uint16_t type;
	uint16_t length;
	uint16_t encoded;

	if (!eigrp_tlv1_af_ready(eigrp) || !pkt || !route)
		return 0;

	if (!ei && nbr)
		ei = nbr->ei;

	filter_prefix = route->prefix ? &route->prefix->destination : &route->dest;
	if (ei && filter_prefix
	    && eigrp_filter_prefix_apply(eigrp, ei, EIGRP_FILTER_OUT,
					filter_prefix)) {
		eigrp_log_info("Prefix Filtered:  Setting Metric to EIGRP_MAX_METRIC");
		route->metric.delay = EIGRP_MAX_METRIC;
	}

	type = eigrp_tlv1_route_tlv_type(eigrp, route);
	if (!type) {
		if (eigrp_debug_packet_any_enabled(EIGRP_DEBUG_SEND)) {
			eigrp_log_debug("Neighbor(%s): invalid TLV_type(%u)",
				   nbr ? eigrp_print_addr(&nbr->src) : "multicast",
				   route->type);
		}
		return 0;
	}

	tlv_start = eigrp_stream_get_endp(pkt);
	eigrp_stream_putw(pkt, type);
	eigrp_stream_putw(pkt, 0);

	encoded = eigrp->af_vectors.packet_address_encode(pkt, &route->nexthop);
	if (encoded != eigrp->af_vectors.packet_address_bytes)
		goto encode_failed;

	if (type == eigrp->af_vectors.classic_external_tlv_type)
		eigrp_tlv1_external_encode(pkt, &route->extdata);

	eigrp_tlv1_metric_encode(pkt, &route->metric);

	encoded = eigrp->af_vectors.packet_prefix_encode(
		pkt, route->prefix ? &route->prefix->destination : &route->dest);
	if (!encoded)
		goto encode_failed;

	tlv_end = eigrp_stream_get_endp(pkt);
	length = tlv_end - tlv_start;

	eigrp_stream_set_endp(pkt, tlv_start + 2);
	eigrp_stream_putw(pkt, length);
	eigrp_stream_set_endp(pkt, tlv_end);

	return length;

encode_failed:
	eigrp_stream_set_endp(pkt, tlv_start);
	return 0;
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
