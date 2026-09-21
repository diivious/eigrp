// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Dump Functions and Debugging.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_dump.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_prefix.h"

#include "command.h"

#include <stdarg.h>

/* Enable debug option variables -- valid only session. */
unsigned long term_debug_eigrp = 0;
unsigned long term_debug_eigrp_nei = 0;
unsigned long term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX] = {0};
unsigned long term_debug_eigrp_zebra = 0;
unsigned long term_debug_eigrp_transmit = 0;

/* Configuration debug option variables. */
unsigned long conf_debug_eigrp = 0;
unsigned long conf_debug_eigrp_nei = 0;
unsigned long conf_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX] = {0};
unsigned long conf_debug_eigrp_zebra = 0;
unsigned long conf_debug_eigrp_transmit = 0;


#define EIGRP_DEBUG_AF_SLOT_MAX 32
#define EIGRP_DEBUG_VRF_NAME_MAX 64

typedef struct eigrp_debug_address_family_slot {
	bool used;
	eigrp_address_family_t afi;
	uint16_t asn;
	bool all_vrfs;
	char vrf_name[EIGRP_DEBUG_VRF_NAME_MAX];
	eigrp_debug_address_family_category_t category;
	bool neighbor_set;
	eigrp_address_t neighbor;
} eigrp_debug_address_family_slot_t;

static eigrp_debug_address_family_slot_t
	term_debug_eigrp_address_family[EIGRP_DEBUG_AF_SLOT_MAX];
static eigrp_debug_address_family_slot_t
	conf_debug_eigrp_address_family[EIGRP_DEBUG_AF_SLOT_MAX];

static bool eigrp_debug_scope_valid(eigrp_debug_scope_t scope)
{
	return scope == EIGRP_DEBUG_SCOPE_TERMINAL
	       || scope == EIGRP_DEBUG_SCOPE_CONFIG;
}

static eigrp_result_t eigrp_debug_flag_apply(unsigned long *term,
					      unsigned long *conf,
					      unsigned long valid_mask,
					      unsigned long flags,
					      eigrp_debug_scope_t scope,
					      bool enable)
{
	if (!term || !conf || !flags || (flags & ~valid_mask)
	    || !eigrp_debug_scope_valid(scope))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (enable) {
		*term |= flags;
		if (scope == EIGRP_DEBUG_SCOPE_CONFIG)
			*conf |= flags;
	} else {
		*term &= ~flags;
		if (scope == EIGRP_DEBUG_SCOPE_CONFIG)
			*conf &= ~flags;
	}
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_debug_apply(eigrp_debug_target_t target,
					unsigned long flags,
					eigrp_debug_scope_t scope,
					bool enable)
{
	unsigned long *term;
	unsigned long *conf;
	unsigned long valid_mask;

	switch (target) {
	case EIGRP_DEBUG_TARGET_GENERAL:
		term = &term_debug_eigrp;
		conf = &conf_debug_eigrp;
		valid_mask = EIGRP_DEBUG_EVENT | EIGRP_DEBUG_DETAIL
			     | EIGRP_DEBUG_TIMERS | EIGRP_DEBUG_FSM
			     | EIGRP_DEBUG_NSF | EIGRP_DEBUG_FAST_REROUTE;
		break;
	case EIGRP_DEBUG_TARGET_NEIGHBOR:
		term = &term_debug_eigrp_nei;
		conf = &conf_debug_eigrp_nei;
		valid_mask = EIGRP_DEBUG_NEI_ALL;
		break;
	case EIGRP_DEBUG_TARGET_NOTIFICATIONS:
		term = &term_debug_eigrp_zebra;
		conf = &conf_debug_eigrp_zebra;
		valid_mask = EIGRP_DEBUG_ZEBRA;
		break;
	case EIGRP_DEBUG_TARGET_TRANSMIT:
		term = &term_debug_eigrp_transmit;
		conf = &conf_debug_eigrp_transmit;
		valid_mask = EIGRP_DEBUG_TRANSMIT_ALL;
		break;
	default:
		return EIGRP_RESULT_INVALID_ARGUMENT;
	}

	return eigrp_debug_flag_apply(term, conf, valid_mask, flags, scope, enable);
}

eigrp_result_t eigrp_debug_set(eigrp_debug_target_t target, unsigned long flags,
				eigrp_debug_scope_t scope)
{
	return eigrp_debug_apply(target, flags, scope, true);
}

eigrp_result_t eigrp_debug_reset(eigrp_debug_target_t target, unsigned long flags,
				  eigrp_debug_scope_t scope)
{
	return eigrp_debug_apply(target, flags, scope, false);
}

static bool eigrp_debug_address_equal(const eigrp_address_t *a,
				      const eigrp_address_t *b)
{
	size_t length;

	if (!a || !b || a->afi != b->afi)
		return false;
	length = a->afi == EIGRP_ADDRESS_FAMILY_IPV4 ? 4U : 16U;
	return memcmp(a->bytes, b->bytes, length) == 0;
}

static bool eigrp_debug_address_family_slot_matches_request(
	const eigrp_debug_address_family_slot_t *slot,
	const eigrp_state_request_t *request,
	eigrp_debug_address_family_category_t category,
	const eigrp_address_t *neighbor, bool reset_wild_neighbor)
{
	const char *vrf;

	if (!slot || !slot->used || !request || slot->afi != request->afi
	    || slot->category != category)
		return false;
	if (request->asn && slot->asn != request->asn)
		return false;
	if (request->all_vrfs != slot->all_vrfs)
		return false;
	vrf = request->vrf_name ? request->vrf_name : "default";
	if (!request->all_vrfs && strcmp(slot->vrf_name, vrf) != 0)
		return false;
	if (category != EIGRP_DEBUG_AF_NEIGHBOR)
		return true;
	if (!neighbor)
		return reset_wild_neighbor || !slot->neighbor_set;
	return slot->neighbor_set
	       && eigrp_debug_address_equal(&slot->neighbor, neighbor);
}

static eigrp_result_t eigrp_debug_address_family_slots_set(
	eigrp_debug_address_family_slot_t *slots,
	const eigrp_state_request_t *request,
	eigrp_debug_address_family_category_t category,
	const eigrp_address_t *neighbor)
{
	const char *vrf;
	unsigned int i;
	int free_slot = -1;

	for (i = 0; i < EIGRP_DEBUG_AF_SLOT_MAX; i++) {
		if (!slots[i].used && free_slot < 0)
			free_slot = (int)i;
		if (eigrp_debug_address_family_slot_matches_request(
			    &slots[i], request, category, neighbor, false))
			return EIGRP_RESULT_SUCCESS;
	}
	if (free_slot < 0)
		return EIGRP_RESULT_INTERNAL_FAILURE;

	vrf = request->vrf_name ? request->vrf_name : "default";
	if (strlen(vrf) >= EIGRP_DEBUG_VRF_NAME_MAX)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	memset(&slots[free_slot], 0, sizeof(slots[free_slot]));
	slots[free_slot].used = true;
	slots[free_slot].afi = request->afi;
	slots[free_slot].asn = request->asn;
	slots[free_slot].all_vrfs = request->all_vrfs;
	strlcpy(slots[free_slot].vrf_name, vrf,
		sizeof(slots[free_slot].vrf_name));
	slots[free_slot].category = category;
	if (neighbor) {
		slots[free_slot].neighbor_set = true;
		slots[free_slot].neighbor = *neighbor;
	}
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_debug_address_family_slots_reset(
	eigrp_debug_address_family_slot_t *slots,
	const eigrp_state_request_t *request,
	eigrp_debug_address_family_category_t category,
	const eigrp_address_t *neighbor)
{
	unsigned int i;

	for (i = 0; i < EIGRP_DEBUG_AF_SLOT_MAX; i++)
		if (eigrp_debug_address_family_slot_matches_request(
			    &slots[i], request, category, neighbor, true))
			memset(&slots[i], 0, sizeof(slots[i]));
}

static bool eigrp_debug_address_family_request_valid(
	const eigrp_state_request_t *request,
	eigrp_debug_address_family_category_t category,
	const eigrp_address_t *neighbor)
{
	if (!request || category >= EIGRP_DEBUG_AF_CATEGORY_MAX)
		return false;
	if (request->afi != EIGRP_ADDRESS_FAMILY_IPV4
	    && request->afi != EIGRP_ADDRESS_FAMILY_IPV6)
		return false;
	if (neighbor && (category != EIGRP_DEBUG_AF_NEIGHBOR
			 || neighbor->afi != request->afi))
		return false;
	return true;
}

eigrp_result_t eigrp_debug_address_family_set(
	const eigrp_state_request_t *request,
	eigrp_debug_address_family_category_t category,
	const eigrp_address_t *neighbor, eigrp_debug_scope_t scope)
{
	eigrp_result_t result;

	if (!eigrp_debug_scope_valid(scope)
	    || !eigrp_debug_address_family_request_valid(request, category,
							 neighbor))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_debug_address_family_slots_set(
		term_debug_eigrp_address_family, request, category, neighbor);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (scope == EIGRP_DEBUG_SCOPE_CONFIG)
		return eigrp_debug_address_family_slots_set(
			conf_debug_eigrp_address_family, request, category, neighbor);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_debug_address_family_reset(
	const eigrp_state_request_t *request,
	eigrp_debug_address_family_category_t category,
	const eigrp_address_t *neighbor, eigrp_debug_scope_t scope)
{
	if (!eigrp_debug_scope_valid(scope)
	    || !eigrp_debug_address_family_request_valid(request, category,
							 neighbor))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	eigrp_debug_address_family_slots_reset(term_debug_eigrp_address_family,
					       request, category, neighbor);
	if (scope == EIGRP_DEBUG_SCOPE_CONFIG)
		eigrp_debug_address_family_slots_reset(
			conf_debug_eigrp_address_family, request, category, neighbor);
	return EIGRP_RESULT_SUCCESS;
}

static bool eigrp_debug_runtime_neighbor_matches(const eigrp_address_t *filter,
						 const eigrp_addr_t *neighbor)
{
	if (!filter || !neighbor)
		return false;
	if (filter->afi == EIGRP_ADDRESS_FAMILY_IPV4 && neighbor->afi == AF_INET)
		return memcmp(filter->bytes, &neighbor->ip.v4, 4) == 0;
	if (filter->afi == EIGRP_ADDRESS_FAMILY_IPV6 && neighbor->afi == AF_INET6)
		return memcmp(filter->bytes, &neighbor->ip.v6, 16) == 0;
	return false;
}

bool eigrp_debug_address_family_enabled(
	eigrp_instance_t *eigrp, eigrp_debug_address_family_category_t category,
	const eigrp_addr_t *neighbor)
{
	eigrp_address_family_t afi;
	unsigned int i;

	if (!eigrp || category >= EIGRP_DEBUG_AF_CATEGORY_MAX)
		return false;
	afi = neighbor && neighbor->afi == AF_INET6 ? EIGRP_ADDRESS_FAMILY_IPV6
						     : EIGRP_ADDRESS_FAMILY_IPV4;
	for (i = 0; i < EIGRP_DEBUG_AF_SLOT_MAX; i++) {
		const eigrp_debug_address_family_slot_t *slot =
			&term_debug_eigrp_address_family[i];

		if (!slot->used || slot->afi != afi || slot->category != category)
			continue;
		if (slot->asn && slot->asn != eigrp->AS)
			continue;
		/* Runtime is IPv4-only today. Default/non-default VRF separation is
		 * still honored; named non-default VRFs become exact when the portable
		 * runtime grows a host-independent VRF identity.
		 */
		if (!slot->all_vrfs) {
			bool wants_default = strcmp(slot->vrf_name, "default") == 0;
			/* The EIGRP runtime uses VRF id zero for the default VRF.  Do not
			 * pull an FRR-only default-VRF macro into common debug code.
			 */
			if (wants_default != (eigrp->vrf_id == 0))
				continue;
		}
		if (category == EIGRP_DEBUG_AF_NEIGHBOR && slot->neighbor_set
		    && !eigrp_debug_runtime_neighbor_matches(&slot->neighbor,
							      neighbor))
			continue;
		return true;
	}
	return false;
}

bool eigrp_debug_address_family_config_enabled(
	const eigrp_address_family_config_t *af,
	eigrp_debug_address_family_category_t category)
{
	unsigned int i;
	const char *vrf;

	if (!af || category >= EIGRP_DEBUG_AF_CATEGORY_MAX)
		return false;
	vrf = af->vrf_name ? af->vrf_name : "default";
	for (i = 0; i < EIGRP_DEBUG_AF_SLOT_MAX; i++) {
		const eigrp_debug_address_family_slot_t *slot =
			&term_debug_eigrp_address_family[i];

		if (!slot->used || slot->afi != af->afi || slot->category != category)
			continue;
		if (slot->asn && slot->asn != af->asn)
			continue;
		if (!slot->all_vrfs && strcmp(slot->vrf_name, vrf) != 0)
			continue;
		return true;
	}
	return false;
}

void eigrp_debug_neighbor_state(eigrp_neighbor_t *nbr, uint8_t old_state,
				uint8_t new_state)
{
	eigrp_instance_t *eigrp;

	if (!nbr || old_state == new_state)
		return;
	eigrp = nbr->ei ? nbr->ei->eigrp : NULL;
	if (!(term_debug_eigrp_nei & EIGRP_DEBUG_NEI)
	    && !eigrp_debug_address_family_enabled(
		    eigrp, EIGRP_DEBUG_AF_NEIGHBOR, &nbr->src))
		return;
	eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Neighbor %s on %s state %u -> %u",
		   eigrp_print_addr(&nbr->src),
		   nbr->ei ? EIGRP_INTF_NAME(nbr->ei) : "self", old_state,
		   new_state);
}

void eigrp_debug_neighbor_sia(eigrp_neighbor_t *nbr, const char *event)
{
	if (!nbr || !(term_debug_eigrp_nei & EIGRP_DEBUG_NEI_SIATIMER))
		return;
	eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Neighbor %s on %s SIA: %s",
		   eigrp_print_addr(&nbr->src),
		   nbr->ei ? EIGRP_INTF_NAME(nbr->ei) : "-",
		   event ? event : "event");
}

void eigrp_debug_nsf_event(const eigrp_instance_t *eigrp,
			   const eigrp_neighbor_t *nbr, uint32_t flags,
			   const char *event)
{
	if (!(term_debug_eigrp & EIGRP_DEBUG_NSF))
		return;
	eigrp_log(EIGRP_LOG_DEBUG, "EIGRP NSF AS %u nbr %s flags 0x%x: %s",
		   eigrp ? eigrp->AS : 0,
		   nbr ? eigrp_print_addr((eigrp_addr_t *)&nbr->src) : "-",
		   flags, event ? event : "event");
}

void eigrp_debug_transmit_event(unsigned long category,
	const eigrp_instance_t *eigrp, const eigrp_interface_t *ei,
	const eigrp_neighbor_t *nbr, const char *format, ...)
{
	char message[512];
	va_list ap;

	if (!(term_debug_eigrp_transmit & category) || !format)
		return;
	va_start(ap, format);
	vsnprintf(message, sizeof(message), format, ap);
	va_end(ap);
	if (nbr)
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP TX AS %u %s nbr %s: %s",
			   eigrp ? eigrp->AS : 0,
			   ei ? EIGRP_INTF_NAME((eigrp_interface_t *)ei) : "-",
			   eigrp_print_addr((eigrp_addr_t *)&nbr->src), message);
	else
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP TX AS %u %s: %s", eigrp ? eigrp->AS : 0,
			   ei ? EIGRP_INTF_NAME((eigrp_interface_t *)ei) : "-",
			   message);
}

static const char *const eigrp_debug_packet_names[EIGRP_DEBUG_PACKET_CATEGORY_MAX] = {
	[EIGRP_DEBUG_PACKET_UPDATE] = "UPDATE",
	[EIGRP_DEBUG_PACKET_REQUEST] = "REQUEST",
	[EIGRP_DEBUG_PACKET_QUERY] = "QUERY",
	[EIGRP_DEBUG_PACKET_REPLY] = "REPLY",
	[EIGRP_DEBUG_PACKET_HELLO] = "HELLO",
	[EIGRP_DEBUG_PACKET_PROBE] = "PROBE",
	[EIGRP_DEBUG_PACKET_ACK] = "ACK",
	[EIGRP_DEBUG_PACKET_RETRY] = "RETRY",
	[EIGRP_DEBUG_PACKET_SIAQUERY] = "SIAQUERY",
	[EIGRP_DEBUG_PACKET_SIAREPLY] = "SIAREPLY",
};

static const char *const eigrp_debug_packet_cli_names[EIGRP_DEBUG_PACKET_CATEGORY_MAX] = {
	[EIGRP_DEBUG_PACKET_UPDATE] = "update",
	[EIGRP_DEBUG_PACKET_REQUEST] = "request",
	[EIGRP_DEBUG_PACKET_QUERY] = "query",
	[EIGRP_DEBUG_PACKET_REPLY] = "reply",
	[EIGRP_DEBUG_PACKET_HELLO] = "hello",
	[EIGRP_DEBUG_PACKET_PROBE] = "probe",
	[EIGRP_DEBUG_PACKET_ACK] = "ack",
	[EIGRP_DEBUG_PACKET_RETRY] = "retry",
	[EIGRP_DEBUG_PACKET_SIAQUERY] = "siaquery",
	[EIGRP_DEBUG_PACKET_SIAREPLY] = "siareply",
};

const char *eigrp_debug_packet_category_name(
	eigrp_debug_packet_category_t category)
{
	if (category >= EIGRP_DEBUG_PACKET_CATEGORY_MAX)
		return "UNKNOWN";
	return eigrp_debug_packet_names[category];
}

static eigrp_result_t eigrp_debug_packet_apply(uint32_t packet_mask,
					       unsigned long flags,
					       eigrp_debug_scope_t scope,
					       bool enable)
{
	unsigned int i;
	unsigned long *term = term_debug_eigrp_packet;
	unsigned long *conf = conf_debug_eigrp_packet;

	if (!packet_mask || (packet_mask & ~EIGRP_DEBUG_PACKET_VALID_MASK))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!(flags & EIGRP_DEBUG_SEND_RECV)
	    || (flags & ~EIGRP_DEBUG_PACKET_FLAG_MASK))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (scope != EIGRP_DEBUG_SCOPE_TERMINAL
	    && scope != EIGRP_DEBUG_SCOPE_CONFIG)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	for (i = 0; i < EIGRP_DEBUG_PACKET_CATEGORY_MAX; i++) {
		if (!(packet_mask & (1U << i)))
			continue;

		if (enable) {
			term[i] |= flags;
			if (scope == EIGRP_DEBUG_SCOPE_CONFIG)
				conf[i] |= flags;
			continue;
		}

		term[i] &= ~flags;
		if (!(term[i] & EIGRP_DEBUG_SEND_RECV))
			term[i] &= ~EIGRP_DEBUG_PACKET_DETAIL;
		if (scope == EIGRP_DEBUG_SCOPE_CONFIG) {
			conf[i] &= ~flags;
			if (!(conf[i] & EIGRP_DEBUG_SEND_RECV))
				conf[i] &= ~EIGRP_DEBUG_PACKET_DETAIL;
		}
	}

	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_debug_packet_set(uint32_t packet_mask,
					 unsigned long flags,
					 eigrp_debug_scope_t scope)
{
	return eigrp_debug_packet_apply(packet_mask, flags, scope, true);
}

eigrp_result_t eigrp_debug_packet_reset(uint32_t packet_mask,
					 unsigned long flags,
					 eigrp_debug_scope_t scope)
{
	return eigrp_debug_packet_apply(packet_mask, flags, scope, false);
}

bool eigrp_debug_packet_any_enabled(unsigned long direction)
{
	unsigned int i;

	if (!(direction & EIGRP_DEBUG_SEND_RECV))
		return false;
	for (i = 0; i < EIGRP_DEBUG_PACKET_CATEGORY_MAX; i++)
		if (term_debug_eigrp_packet[i] & direction)
			return true;
	return false;
}

static eigrp_debug_packet_category_t
eigrp_debug_packet_category_get(const eigrp_header_t *header)
{
	if (!header)
		return EIGRP_DEBUG_PACKET_CATEGORY_MAX;

	/* RFC 7868 ACKs are HELLO packets with seq 0 and a non-zero ACK. */
	if (header->opcode == EIGRP_OPC_HELLO && ntohl(header->sequence) == 0
	    && ntohl(header->ack) != 0)
		return EIGRP_DEBUG_PACKET_ACK;

	switch (header->opcode) {
	case EIGRP_OPC_UPDATE:
		return EIGRP_DEBUG_PACKET_UPDATE;
	case EIGRP_OPC_REQUEST:
		return EIGRP_DEBUG_PACKET_REQUEST;
	case EIGRP_OPC_QUERY:
		return EIGRP_DEBUG_PACKET_QUERY;
	case EIGRP_OPC_REPLY:
		return EIGRP_DEBUG_PACKET_REPLY;
	case EIGRP_OPC_HELLO:
		return EIGRP_DEBUG_PACKET_HELLO;
	case EIGRP_OPC_PROBE:
		return EIGRP_DEBUG_PACKET_PROBE;
	case EIGRP_OPC_ACK:
		return EIGRP_DEBUG_PACKET_ACK;
	case EIGRP_OPC_SIAQUERY:
		return EIGRP_DEBUG_PACKET_SIAQUERY;
	case EIGRP_OPC_SIAREPLY:
		return EIGRP_DEBUG_PACKET_SIAREPLY;
	default:
		return EIGRP_DEBUG_PACKET_CATEGORY_MAX;
	}
}

static uint16_t eigrp_debug_get16(const uint8_t *data)
{
	uint16_t value;

	memcpy(&value, data, sizeof(value));
	return ntohs(value);
}

static uint32_t eigrp_debug_get32(const uint8_t *data)
{
	uint32_t value;

	memcpy(&value, data, sizeof(value));
	return ntohl(value);
}

static uint64_t eigrp_debug_get48(const uint8_t *data)
{
	uint64_t value = 0;
	unsigned int i;

	for (i = 0; i < 6; i++)
		value = (value << 8) | data[i];
	return value;
}

static const char *eigrp_debug_tlv_name(uint16_t type)
{
	switch (type) {
	case EIGRP_TLV_PARAMETER:
		return "Parameter";
	case EIGRP_TLV_AUTH:
		return "Authentication";
	case EIGRP_TLV_SEQ:
		return "Sequence";
	case EIGRP_TLV_SW_VERSION:
		return "Software-Version";
	case EIGRP_TLV_NEXT_MCAST_SEQ:
		return "Next-Multicast-Sequence";
	case EIGRP_TLV_PEER_TERMINATION:
		return "Peer-Termination";
	case EIGRP_TLV_PEER_TIDLIST:
		return "TID-List";
	case EIGRP_TLV_PEER_MTRLIST:
		return "Legacy-MTR-List";
	case EIGRP_TLV_IPv4_REQ:
		return "IPv4-Request";
	case EIGRP_TLV_IPv4_INT:
		return "IPv4-Internal";
	case EIGRP_TLV_IPv4_EXT:
		return "IPv4-External";
	case EIGRP_TLV_IPv4_COM:
		return "IPv4-Community";
	case EIGRP_TLV_IPv6_REQ:
		return "IPv6-Request";
	case EIGRP_TLV_IPv6_INT:
		return "IPv6-Internal";
	case EIGRP_TLV_IPv6_EXT:
		return "IPv6-External";
	case EIGRP_TLV_IPv6_COM:
		return "IPv6-Community";
	case EIGRP_TLV_MP_REQ:
		return "Multiprotocol-Request";
	case EIGRP_TLV_MP_INT:
		return "Multiprotocol-Internal";
	case EIGRP_TLV_MP_EXT:
		return "Multiprotocol-External";
	case EIGRP_TLV_MP_COM:
		return "Multiprotocol-Community";
	default:
		return "Unknown";
	}
}

static void eigrp_debug_ipv4_prefix_dump(const uint8_t *data, size_t length,
					 size_t prefix_offset)
{
	struct in_addr prefix = {0};
	uint8_t prefixlen;
	size_t bytes;

	if (prefix_offset >= length)
		return;
	prefixlen = data[prefix_offset];
	if (prefixlen > 32) {
		eigrp_log(EIGRP_LOG_DEBUG, "    invalid IPv4 prefix length %u", prefixlen);
		return;
	}
	bytes = (prefixlen + 7U) / 8U;
	if (prefix_offset + 1U + bytes > length) {
		eigrp_log(EIGRP_LOG_DEBUG, "    truncated IPv4 prefix /%u", prefixlen);
		return;
	}
	memcpy(&prefix.s_addr, data + prefix_offset + 1U, bytes);
	eigrp_log(EIGRP_LOG_DEBUG, "    prefix %pI4/%u", &prefix, prefixlen);
}

static void eigrp_debug_tlv_detail_dump(uint16_t type, const uint8_t *data,
					uint16_t length)
{
	if (length < EIGRP_TLV_HDR_SIZE)
		return;

	switch (type) {
	case EIGRP_TLV_PARAMETER:
		if (length >= EIGRP_TLV_PARAMETER_LEN)
			eigrp_log(EIGRP_LOG_DEBUG,
				"    K-values %u/%u/%u/%u/%u/%u, hold %u sec",
				data[4], data[5], data[6], data[7], data[8], data[9],
				eigrp_debug_get16(data + 10));
		break;
	case EIGRP_TLV_AUTH:
		if (length >= 16)
			eigrp_log(EIGRP_LOG_DEBUG,
				"    auth type %u, digest length %u, key-id %u, sequence %u",
				eigrp_debug_get16(data + 4), eigrp_debug_get16(data + 6),
				eigrp_debug_get32(data + 8), eigrp_debug_get32(data + 12));
		break;
	case EIGRP_TLV_SEQ:
		if (length >= EIGRP_TLV_SEQ_BASE_LEN)
			eigrp_log(EIGRP_LOG_DEBUG, "    sequence address-length %u", data[4]);
		break;
	case EIGRP_TLV_SW_VERSION:
		if (length >= EIGRP_TLV_SW_VERSION_LEN)
			eigrp_log(EIGRP_LOG_DEBUG, "    software %u.%u, EIGRP %u.%u", data[4], data[5],
				   data[6], data[7]);
		break;
	case EIGRP_TLV_NEXT_MCAST_SEQ:
		if (length >= 8)
			eigrp_log(EIGRP_LOG_DEBUG, "    next multicast sequence %u",
				   eigrp_debug_get32(data + 4));
		break;
	case EIGRP_TLV_PEER_TERMINATION:
		if (length >= EIGRP_TLV_PEER_TERMINATION_LEN) {
			struct in_addr peer;

			memcpy(&peer.s_addr, data + 5, sizeof(peer.s_addr));
			eigrp_log(EIGRP_LOG_DEBUG, "    peer termination neighbor %pI4", &peer);
		}
		break;
	case EIGRP_TLV_IPv4_INT:
		if (length >= 25) {
			struct in_addr nexthop;

			memcpy(&nexthop.s_addr, data + 4, sizeof(nexthop.s_addr));
			eigrp_log(EIGRP_LOG_DEBUG,
				"    next-hop %pI4, delay(raw) %u, bandwidth %u, hop %u, reliability %u, load %u",
				&nexthop, eigrp_debug_get32(data + 8),
				eigrp_debug_get32(data + 12), data[19], data[20], data[21]);
			eigrp_debug_ipv4_prefix_dump(data, length, 24);
		}
		break;
	case EIGRP_TLV_IPv4_EXT:
		if (length >= 45) {
			struct in_addr nexthop;
			struct in_addr origin;

			memcpy(&nexthop.s_addr, data + 4, sizeof(nexthop.s_addr));
			memcpy(&origin.s_addr, data + 8, sizeof(origin.s_addr));
			eigrp_log(EIGRP_LOG_DEBUG,
				"    next-hop %pI4, origin %pI4, origin-AS %u, external-metric %u, protocol %u",
				&nexthop, &origin, eigrp_debug_get32(data + 12),
				eigrp_debug_get32(data + 20), data[26]);
			eigrp_log(EIGRP_LOG_DEBUG,
				"    delay(raw) %u, bandwidth %u, hop %u, reliability %u, load %u",
				eigrp_debug_get32(data + 28), eigrp_debug_get32(data + 32),
				data[39], data[40], data[41]);
			eigrp_debug_ipv4_prefix_dump(data, length, 44);
		}
		break;
	case EIGRP_TLV_MP_INT:
	case EIGRP_TLV_MP_EXT:
		if (length >= 37) {
			uint16_t afi = eigrp_debug_get16(data + 4);
			uint16_t tid = eigrp_debug_get16(data + 6);
			uint32_t rid = eigrp_debug_get32(data + 8);
			uint16_t attr_len = (uint16_t)data[12] * 2U;
			size_t prefix_offset = 36U + attr_len;
			struct in_addr rid_addr;

			rid_addr.s_addr = htonl(rid);
			eigrp_log(EIGRP_LOG_DEBUG,
				"    AFI %u, TID %u, router-id %pI4, tag %u, reliability %u, load %u, hop %u",
				afi, tid, &rid_addr, data[13], data[14], data[15], data[19]);
			eigrp_log(EIGRP_LOG_DEBUG, "    wide delay %" PRIu64 ", bandwidth %" PRIu64,
				   eigrp_debug_get48(data + 20),
				   eigrp_debug_get48(data + 26));
			if (type == EIGRP_TLV_MP_EXT)
				prefix_offset += 16U;
			if (afi == EIGRP_AF_IPv4)
				eigrp_debug_ipv4_prefix_dump(data, length, prefix_offset);
		}
		break;
	default:
		break;
	}
}

static void eigrp_debug_packet_detail_dump(const eigrp_header_t *header,
					   uint16_t length)
{
	const uint8_t *data = (const uint8_t *)header;
	size_t offset = EIGRP_HEADER_LEN;

	if (!header || length < EIGRP_HEADER_LEN)
		return;

	eigrp_log(EIGRP_LOG_DEBUG,
		"  header version %u, opcode %u, checksum 0x%04x, vrid %u, AS %u",
		header->version, header->opcode, ntohs(header->checksum),
		ntohs(header->vrid), ntohs(header->ASNumber));

	while (offset < length) {
		uint16_t type;
		uint16_t tlv_length;

		if (length - offset < EIGRP_TLV_HDR_SIZE) {
			eigrp_log(EIGRP_LOG_DEBUG, "  malformed TLV framing: %zu trailing byte(s)",
				   length - offset);
			return;
		}
		type = eigrp_debug_get16(data + offset);
		tlv_length = eigrp_debug_get16(data + offset + 2);
		if (tlv_length < EIGRP_TLV_HDR_SIZE || tlv_length > length - offset) {
			eigrp_log(EIGRP_LOG_DEBUG,
				"  malformed TLV 0x%04x: length %u exceeds remaining %zu",
				type, tlv_length, length - offset);
			return;
		}

		eigrp_log(EIGRP_LOG_DEBUG, "  TLV 0x%04x (%s), length %u", type,
			   eigrp_debug_tlv_name(type), tlv_length);
		eigrp_debug_tlv_detail_dump(type, data + offset, tlv_length);
		offset += tlv_length;
	}
}

void eigrp_debug_packet_send(eigrp_interface_t *ei,
			     const eigrp_packet_t *packet, int send_result)
{
	const eigrp_header_t *header;
	eigrp_debug_packet_category_t category;
	const char *ifname;
	unsigned long state;

	if (!ei || !packet || !packet->s || packet->length < EIGRP_HEADER_LEN)
		return;
	header = (const eigrp_header_t *)STREAM_DATA(packet->s);
	category = eigrp_debug_packet_category_get(header);
	if (category >= EIGRP_DEBUG_PACKET_CATEGORY_MAX)
		return;
	state = term_debug_eigrp_packet[category];
	if (!(state & EIGRP_DEBUG_SEND))
		return;

	ifname = EIGRP_INTF_NAME(ei);
	if (packet->nbr)
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Sending %s on %s nbr %s",
			   eigrp_debug_packet_category_name(category), ifname,
			   eigrp_print_addr(&packet->nbr->src));
	else if (packet->dst.ip.v4.s_addr == htonl(EIGRP_MULTICAST_ADDRESS))
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Sending %s on %s",
			   eigrp_debug_packet_category_name(category), ifname);
	else
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Sending %s on %s dst %s",
			   eigrp_debug_packet_category_name(category), ifname,
			   eigrp_print_addr((eigrp_addr_t *)&packet->dst));
	eigrp_log(EIGRP_LOG_DEBUG, "  AS %u, Flags 0x%x, Seq %u/%u",
		   ntohs(header->ASNumber), ntohl(header->flags),
		   ntohl(header->sequence), ntohl(header->ack));

	if (state & EIGRP_DEBUG_PACKET_DETAIL) {
		eigrp_log(EIGRP_LOG_DEBUG, "  packet length %u, send-result %d", packet->length,
			   send_result);
		eigrp_debug_packet_detail_dump(header, packet->length);
	}
}

void eigrp_debug_packet_receive(eigrp_interface_t *ei,
				const eigrp_addr_t *source,
				const eigrp_addr_t *destination,
				const eigrp_header_t *header, uint16_t length)
{
	eigrp_debug_packet_category_t category;
	unsigned long state;

	if (!ei || !source || !header || length < EIGRP_HEADER_LEN)
		return;
	category = eigrp_debug_packet_category_get(header);
	if (category >= EIGRP_DEBUG_PACKET_CATEGORY_MAX)
		return;
	state = term_debug_eigrp_packet[category];
	if (!(state & EIGRP_DEBUG_RECV))
		return;

	eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Received %s on %s nbr %s",
		   eigrp_debug_packet_category_name(category), EIGRP_INTF_NAME(ei),
		   eigrp_print_addr((eigrp_addr_t *)source));
	eigrp_log(EIGRP_LOG_DEBUG, "  AS %u, Flags 0x%x, Seq %u/%u",
		   ntohs(header->ASNumber), ntohl(header->flags),
		   ntohl(header->sequence), ntohl(header->ack));

	if (state & EIGRP_DEBUG_PACKET_DETAIL) {
		if (destination)
			eigrp_log(EIGRP_LOG_DEBUG, "  packet length %u, dst %s", length,
				   eigrp_print_addr((eigrp_addr_t *)destination));
		else
			eigrp_log(EIGRP_LOG_DEBUG, "  packet length %u", length);
		eigrp_debug_packet_detail_dump(header, length);
	}
}

void eigrp_debug_packet_retry(eigrp_neighbor_t *nbr,
			      const eigrp_packet_t *packet, uint8_t retry_count)
{
	const eigrp_header_t *header;
	unsigned long state = term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_RETRY];

	if (!(state & EIGRP_DEBUG_SEND) || !nbr || !packet || !packet->s
	    || packet->length < EIGRP_HEADER_LEN)
		return;
	header = (const eigrp_header_t *)STREAM_DATA(packet->s);

	eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Sending %s on %s nbr %s, retry %u, RTO %u",
		   eigrp_debug_packet_category_name(
			   eigrp_debug_packet_category_get(header)),
		   EIGRP_INTF_NAME(nbr->ei), eigrp_print_addr(&nbr->src), retry_count,
		   eigrp_neighbor_rto_get(nbr));
	eigrp_log(EIGRP_LOG_DEBUG, "  AS %u, Flags 0x%x, Seq %u/%u",
		   ntohs(header->ASNumber), ntohl(header->flags),
		   ntohl(header->sequence), ntohl(header->ack));
	if (state & EIGRP_DEBUG_PACKET_DETAIL) {
		eigrp_log(EIGRP_LOG_DEBUG, "  packet length %u", packet->length);
		eigrp_debug_packet_detail_dump(header, packet->length);
	}
}

static void eigrp_debug_transmit_write(struct vty *vty, unsigned long state)
{
	if (!state)
		return;
	if ((state & EIGRP_DEBUG_TRANSMIT_ALL) == EIGRP_DEBUG_TRANSMIT_ALL) {
		vty_out(vty, "debug eigrp transmit\n");
		return;
	}
	vty_out(vty, "debug eigrp transmit");
	if (state & EIGRP_DEBUG_TRANSMIT_ACK)
		vty_out(vty, " ack");
	if (state & EIGRP_DEBUG_TRANSMIT_BUILD)
		vty_out(vty, " build");
	if (state & EIGRP_DEBUG_TRANSMIT_DETAIL)
		vty_out(vty, " detail");
	if (state & EIGRP_DEBUG_TRANSMIT_LINK)
		vty_out(vty, " link");
	if (state & EIGRP_DEBUG_TRANSMIT_PACKETIZE)
		vty_out(vty, " packetize");
	if (state & EIGRP_DEBUG_TRANSMIT_PEERDOWN)
		vty_out(vty, " peerdown");
	if (state & EIGRP_DEBUG_TRANSMIT_SIA)
		vty_out(vty, " sia");
	if (state & EIGRP_DEBUG_TRANSMIT_STARTUP)
		vty_out(vty, " startup");
	if (state & EIGRP_DEBUG_TRANSMIT_STRANGE)
		vty_out(vty, " strange");
	vty_out(vty, "\n");
}

static void eigrp_debug_address_family_slot_write(
	struct vty *vty, const eigrp_debug_address_family_slot_t *slot)
{
	char address[INET6_ADDRSTRLEN];
	const char *afi;
	const char *suffix = "";

	if (!slot || !slot->used)
		return;
	afi = slot->afi == EIGRP_ADDRESS_FAMILY_IPV6 ? "ipv6" : "ipv4";
	vty_out(vty, "debug eigrp address-family %s", afi);
	if (!slot->all_vrfs && strcmp(slot->vrf_name, "default") != 0)
		vty_out(vty, " vrf %s", slot->vrf_name);
	if (slot->asn)
		vty_out(vty, " %u", slot->asn);

	switch (slot->category) {
	case EIGRP_DEBUG_AF_ROUTE:
		break;
	case EIGRP_DEBUG_AF_NEIGHBOR:
		vty_out(vty, " neighbor");
		if (slot->neighbor_set) {
			int family = slot->afi == EIGRP_ADDRESS_FAMILY_IPV6
					     ? AF_INET6
					     : AF_INET;
			if (inet_ntop(family, slot->neighbor.bytes, address,
				      sizeof(address)))
				vty_out(vty, " %s", address);
		}
		break;
	case EIGRP_DEBUG_AF_NOTIFICATIONS:
		suffix = " notifications";
		break;
	case EIGRP_DEBUG_AF_SUMMARY:
		suffix = " summary";
		break;
	case EIGRP_DEBUG_AF_CATEGORY_MAX:
		break;
	}
	vty_out(vty, "%s\n", suffix);
}

static int config_write_debug(struct vty *vty)
{
	int write = 0;
	unsigned int i;

	if (conf_debug_eigrp & EIGRP_DEBUG_EVENT) {
		vty_out(vty, "debug eigrp event%s\n",
			(conf_debug_eigrp & EIGRP_DEBUG_DETAIL) ? " detail" : "");
		write = 1;
	}
	if (conf_debug_eigrp & EIGRP_DEBUG_TIMERS) {
		vty_out(vty, "debug eigrp timers\n");
		write = 1;
	}
	if (conf_debug_eigrp & EIGRP_DEBUG_FSM) {
		vty_out(vty, "debug eigrp fsm\n");
		write = 1;
	}
	if (conf_debug_eigrp & EIGRP_DEBUG_NSF) {
		vty_out(vty, "debug eigrp nsf\n");
		write = 1;
	}
	if (conf_debug_eigrp & EIGRP_DEBUG_FAST_REROUTE) {
		vty_out(vty, "debug eigrp frr\n");
		write = 1;
	}
	if (conf_debug_eigrp_nei & EIGRP_DEBUG_NEI) {
		vty_out(vty, "debug eigrp neighbor");
		if (conf_debug_eigrp_nei & EIGRP_DEBUG_NEI_SIATIMER)
			vty_out(vty, " siatimer");
		if (conf_debug_eigrp_nei & EIGRP_DEBUG_NEI_STATIC)
			vty_out(vty, " static");
		vty_out(vty, "\n");
		write = 1;
	}
	if (conf_debug_eigrp_zebra & EIGRP_DEBUG_ZEBRA_RIB) {
		vty_out(vty, "debug eigrp notifications rib\n");
		write = 1;
	}
	if (conf_debug_eigrp_zebra & EIGRP_DEBUG_ZEBRA_INTERFACE) {
		vty_out(vty, "debug eigrp notifications interface\n");
		write = 1;
	}
	if (conf_debug_eigrp_transmit) {
		eigrp_debug_transmit_write(vty, conf_debug_eigrp_transmit);
		write = 1;
	}

	for (i = 0; i < EIGRP_DEBUG_AF_SLOT_MAX; i++) {
		if (!conf_debug_eigrp_address_family[i].used)
			continue;
		eigrp_debug_address_family_slot_write(
			vty, &conf_debug_eigrp_address_family[i]);
		write = 1;
	}

	/* Persist packet debug categories independently so partial directions survive. */
	for (i = 0; i < EIGRP_DEBUG_PACKET_CATEGORY_MAX; i++) {
		unsigned long state = conf_debug_eigrp_packet[i];
		const char *direction = "";
		const char *detail = "";

		if (!(state & EIGRP_DEBUG_SEND_RECV))
			continue;
		if ((state & EIGRP_DEBUG_SEND_RECV) == EIGRP_DEBUG_SEND)
			direction = " send";
		else if ((state & EIGRP_DEBUG_SEND_RECV) == EIGRP_DEBUG_RECV)
			direction = " receive";
		if (state & EIGRP_DEBUG_PACKET_DETAIL)
			detail = " detail";

		vty_out(vty, "debug eigrp packet %s%s%s\n",
			eigrp_debug_packet_cli_names[i], direction, detail);
		write = 1;
	}

	return write;
}

static int eigrp_neighbor_packet_queue_sum(eigrp_interface_t *ei)
{
	eigrp_neighbor_t *nbr;
	struct listnode *node, *nnode;
	int sum;
	sum = 0;

	for (ALL_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
		sum += nbr->retrans_queue->count;
	}

	return sum;
}

/*
 * Expects header to be in host order
 */
void eigrp_header_dump(struct eigrp_header *eigrph)
{
	/* EIGRP Header dump. */
	eigrp_log(EIGRP_LOG_DEBUG, "eigrp_version %u", eigrph->version);
	eigrp_log(EIGRP_LOG_DEBUG, "eigrp_opcode %u", eigrph->opcode);
	eigrp_log(EIGRP_LOG_DEBUG, "eigrp_checksum 0x%x", ntohs(eigrph->checksum));
	eigrp_log(EIGRP_LOG_DEBUG, "eigrp_flags 0x%x", ntohl(eigrph->flags));
	eigrp_log(EIGRP_LOG_DEBUG, "eigrp_sequence %u", ntohl(eigrph->sequence));
	eigrp_log(EIGRP_LOG_DEBUG, "eigrp_ack %u", ntohl(eigrph->ack));
	eigrp_log(EIGRP_LOG_DEBUG, "eigrp_vrid %u", ntohs(eigrph->vrid));
	eigrp_log(EIGRP_LOG_DEBUG, "eigrp_AS %u", ntohs(eigrph->ASNumber));
}

void show_ip_eigrp_interface_header(struct vty *vty, eigrp_instance_t *eigrp)
{

	vty_out(vty,
		"\nEIGRP interfaces for AS(%d)\n\n %-10s %-10s %-10s %-6s %-12s %-7s %-14s %-12s %-8s %-8s %-8s\n %-39s %-12s %-7s %-14s %-12s %-8s\n",
		eigrp->AS, "Interface", "Bandwidth", "Delay", "Peers",
		"Xmit Queue", "Mean", "Pacing Time", "Multicast", "Pending",
		"Hello", "Holdtime", "", "Un/Reliable", "SRTT", "Un/Reliable",
		"Flow Timer", "Routes");
}

void show_ip_eigrp_interface_sub(struct vty *vty, eigrp_instance_t *eigrp,
				 eigrp_interface_t *ei)
{
	vty_out(vty, "%-11s ", EIGRP_INTF_NAME(ei));
	vty_out(vty, "%-11u", ei->params.bandwidth);
	vty_out(vty, "%-11u", ei->params.delay);
	vty_out(vty, "%-7u", ei->nbrs->count);
	vty_out(vty, "%u %c %-10u", 0, '/',
		eigrp_neighbor_packet_queue_sum(ei));
	vty_out(vty, "%-7u %-14u %-12u %-8u", 0, 0, 0, 0);
	vty_out(vty, "%-8u %-8u \n", ei->params.v_hello, ei->params.v_wait);
}

void show_ip_eigrp_interface_detail(struct vty *vty, eigrp_instance_t *eigrp,
				    eigrp_interface_t *ei)
{
	vty_out(vty, "%-2s %s %d %-3s \n", "", "Hello interval is ", 0, " sec");
	vty_out(vty, "%-2s %s %s \n", "", "Next xmit serial", "<none>");
	vty_out(vty, "%-2s %s %d %s %d %s %d %s %d \n", "",
		"Un/reliable mcasts: ", 0, "/", 0, "Un/reliable ucasts: ", 0,
		"/", 0);
	vty_out(vty, "%-2s %s %d %s %d %s %d \n", "", "Mcast exceptions: ", 0,
		"  CR packets: ", 0, "  ACKs suppressed: ", 0);
	vty_out(vty, "%-2s %s %d %s %d \n", "", "Retransmissions sent: ", 0,
		"Out-of-sequence rcvd: ", 0);
	vty_out(vty, "%-2s %s %s %s \n", "", "Authentication mode is ", "not",
		"set");
	vty_out(vty, "%-2s TLV peers: v1 %u, v2 %u\n", "",
		ei->tlv1_peer_count, ei->tlv2_peer_count);
	vty_out(vty, "%-2s %s \n", "", "Use multicast");
}

void show_ip_eigrp_neighbor_header(struct vty *vty, eigrp_instance_t *eigrp)
{
	vty_out(vty, "\nIP-EIGRP neighbors for process %u\n", eigrp->AS);
	vty_out(vty,
		"H   Address                 Interface       Hold Uptime   SRTT   RTO  Q  Seq\n");
	vty_out(vty,
		"                                           (sec)          (ms)       Cnt Num\n");
}

static const char *eigrp_dump_duration_string(uint64_t seconds, char *buffer,
					      size_t size)
{
	uint64_t days = seconds / 86400U;
	uint64_t hours = (seconds % 86400U) / 3600U;
	uint64_t minutes = (seconds % 3600U) / 60U;
	uint64_t secs = seconds % 60U;

	if (days)
		snprintf(buffer, size, "%llud%02lluh", (unsigned long long)days,
			 (unsigned long long)hours);
	else
		snprintf(buffer, size, "%02llu:%02llu:%02llu",
			 (unsigned long long)hours, (unsigned long long)minutes,
			 (unsigned long long)secs);
	return buffer;
}

void show_ip_eigrp_neighbor_sub(struct vty *vty, eigrp_neighbor_t *nbr,
				int detail)
{
	char hold[16];
	char uptime[32];
	char srtt[16];
	uint64_t uptime_seconds = 0;
	uint8_t retry_count = 0;

	if (nbr->t_holddown)
		snprintf(hold, sizeof(hold), "%u",
			 eigrp_southbound_timer_remaining_seconds(nbr->t_holddown));
	else
		snprintf(hold, sizeof(hold), "-");
	if (nbr->up_since_msec) {
		uint64_t now = eigrp_southbound_monotime_msec();

		if (now >= nbr->up_since_msec)
			uptime_seconds = (now - nbr->up_since_msec) / 1000U;
		eigrp_dump_duration_string(uptime_seconds, uptime, sizeof(uptime));
	} else
		snprintf(uptime, sizeof(uptime), "-");
	if (nbr->retrans_queue && nbr->retrans_queue->tail)
		retry_count = nbr->retrans_queue->tail->retrans_counter;
	if (nbr->srtt_valid)
		snprintf(srtt, sizeof(srtt), "%u", nbr->srtt_msec);
	else
		snprintf(srtt, sizeof(srtt), "n/a");

	vty_out(vty, "%-3s %-23s %-15s %-5s %-8s %-6s %-5u %-3lu %u\n", "-",
		eigrp_print_addr(&nbr->src), EIGRP_INTF_NAME(nbr->ei), hold, uptime,
		srtt, eigrp_neighbor_rto_get(nbr),
		nbr->retrans_queue ? nbr->retrans_queue->count : 0,
		nbr->recv_sequence_number);


	if (detail) {
		vty_out(vty, "   Version %u.%u/%u.%u", nbr->os_rel_major,
			nbr->os_rel_minor, nbr->tlv_rel_major,
			nbr->tlv_rel_minor);
		vty_out(vty, ", Retrans: %" PRIu64 ", Retries: %u\n",
			nbr->retransmissions, retry_count);
	}
}

/*
 * Print standard header for show EIGRP topology output
 */
void show_ip_eigrp_topology_header(struct vty *vty, eigrp_instance_t *eigrp)
{
	vty_out(vty, "\nIP-EIGRP Topology Table for AS(%d)/ID(%s)\n\n",
		eigrp->AS, eigrp_print_routerid(eigrp->router_id));
	vty_out(vty,
		"Codes: P - Passive, A - Active, U - Update, Q - Query, "
		"R - Reply,\n       r - reply Status, s - sia Status\n\n");
}

void show_ip_eigrp_prefix_descriptor(struct vty *vty,
				     eigrp_prefix_descriptor_t *tn,
				     bool include_serial)
{
	struct list *successors = eigrp_topology_get_successor(tn);
	char buffer[EIGRP_PREFIX_STRLEN] = "invalid";

	eigrp_prefix_snprintf(buffer, sizeof(buffer), &tn->destination);
	vty_out(vty, "%c %s, %u successors, FD is ",
		(tn->state > 0) ? 'A' : 'P', buffer,
		(successors) ? successors->count : 0);
	if (tn->fdistance == EIGRP_MAX_METRIC)
		vty_out(vty, "Inaccessible");
	else
		vty_out(vty, "%u", tn->fdistance);
	if (include_serial)
		vty_out(vty, ", serno %" PRIu64, tn->serno);
	vty_out(vty, "\n");

	if (successors)
		list_delete(&successors);
}

void show_ip_eigrp_route_descriptor(struct vty *vty, eigrp_instance_t *eigrp,
				    eigrp_route_descriptor_t *te, bool *first,
				    bool include_serial)
{
	if (te->reported_distance == EIGRP_MAX_METRIC)
		return;

	if (*first) {
		show_ip_eigrp_prefix_descriptor(vty, te->prefix, include_serial);
		*first = false;
	}

	if (te->adv_router == eigrp->neighbor_self)
		vty_out(vty, "        via Connected, %s\n", EIGRP_INTF_NAME(te->ei));
	else
		vty_out(vty, "        via %s (%u/%u), %s\n",
			eigrp_print_addr(&te->adv_router->src), te->distance,
			te->reported_distance, EIGRP_INTF_NAME(te->ei));
}


DEFUN_NOSH(show_debugging_eigrp, show_debugging_eigrp_cmd,
	   "show debugging [eigrp]", SHOW_STR DEBUG_STR EIGRP_STR)
{
	unsigned int i;

	vty_out(vty, "EIGRP debugging status:\n");
	if (IS_DEBUG_EIGRP(event, EVENT))
		vty_out(vty, "  EIGRP event%s debugging is on\n",
			 IS_DEBUG_EIGRP(event, DETAIL) ? " detail" : "");
	if (IS_DEBUG_EIGRP(event, TIMERS))
		vty_out(vty, "  EIGRP timers debugging is on\n");
	if (IS_DEBUG_EIGRP(event, FSM))
		vty_out(vty, "  EIGRP FSM debugging is on\n");
	if (IS_DEBUG_EIGRP(event, NSF))
		vty_out(vty, "  EIGRP NSF debugging is on\n");
	if (IS_DEBUG_EIGRP(event, FAST_REROUTE))
		vty_out(vty, "  EIGRP fast-reroute debugging is on\n");
	if (term_debug_eigrp_nei & EIGRP_DEBUG_NEI) {
		vty_out(vty, "  EIGRP neighbor debugging is on");
		if (term_debug_eigrp_nei & EIGRP_DEBUG_NEI_SIATIMER)
			vty_out(vty, " (siatimer)");
		if (term_debug_eigrp_nei & EIGRP_DEBUG_NEI_STATIC)
			vty_out(vty, " (static)");
		vty_out(vty, "\n");
	}
	if (term_debug_eigrp_zebra & EIGRP_DEBUG_ZEBRA_RIB)
		vty_out(vty, "  EIGRP RIB notification debugging is on\n");
	if (term_debug_eigrp_zebra & EIGRP_DEBUG_ZEBRA_INTERFACE)
		vty_out(vty, "  EIGRP interface notification debugging is on\n");
	if (term_debug_eigrp_transmit) {
		vty_out(vty, "  EIGRP transmit debugging is on:");
#define EIGRP_SHOW_TRANSMIT(_flag, _name)                                     \
		do {                                                                 \
			if (term_debug_eigrp_transmit & (_flag))                        \
				vty_out(vty, " %s", (_name));                            \
		} while (0)
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_ACK, "ACK");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_BUILD, "BUILD");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_DETAIL, "DETAIL");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_LINK, "LINK");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_PACKETIZE, "PACKETIZE");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_PEERDOWN, "PEERDOWN");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_SIA, "SIA");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_STARTUP, "STARTUP");
		EIGRP_SHOW_TRANSMIT(EIGRP_DEBUG_TRANSMIT_STRANGE, "STRANGE");
#undef EIGRP_SHOW_TRANSMIT
		vty_out(vty, "\n");
	}

	for (i = 0; i < EIGRP_DEBUG_AF_SLOT_MAX; i++) {
		const eigrp_debug_address_family_slot_t *slot =
			&term_debug_eigrp_address_family[i];
		char address[INET6_ADDRSTRLEN];

		if (!slot->used)
			continue;
		vty_out(vty, "  EIGRP address-family %s",
			slot->afi == EIGRP_ADDRESS_FAMILY_IPV6 ? "ipv6" : "ipv4");
		if (slot->asn)
			vty_out(vty, " AS %u", slot->asn);
		if (!slot->all_vrfs && strcmp(slot->vrf_name, "default") != 0)
			vty_out(vty, " vrf %s", slot->vrf_name);
		switch (slot->category) {
		case EIGRP_DEBUG_AF_ROUTE:
			vty_out(vty, " route");
			break;
		case EIGRP_DEBUG_AF_NEIGHBOR:
			vty_out(vty, " neighbor");
			if (slot->neighbor_set
			    && inet_ntop(slot->afi == EIGRP_ADDRESS_FAMILY_IPV6
						 ? AF_INET6
						 : AF_INET,
					 slot->neighbor.bytes, address, sizeof(address)))
				vty_out(vty, " %s", address);
			break;
		case EIGRP_DEBUG_AF_NOTIFICATIONS:
			vty_out(vty, " notifications");
			break;
		case EIGRP_DEBUG_AF_SUMMARY:
			vty_out(vty, " summary");
			break;
		case EIGRP_DEBUG_AF_CATEGORY_MAX:
			break;
		}
		vty_out(vty, " debugging is on\n");
	}

	for (i = 0; i < EIGRP_DEBUG_PACKET_CATEGORY_MAX; i++) {
		const char *name = eigrp_debug_packet_category_name(i);

		if (IS_DEBUG_EIGRP_PACKET(i, SEND)
		    && IS_DEBUG_EIGRP_PACKET(i, RECV)) {
			vty_out(vty, "  EIGRP packet %s%s debugging is on\n", name,
				IS_DEBUG_EIGRP_PACKET(i, PACKET_DETAIL)
					? " detail"
					: "");
		} else {
			if (IS_DEBUG_EIGRP_PACKET(i, SEND))
				vty_out(vty,
					"  EIGRP packet %s send%s debugging is on\n",
					name,
					IS_DEBUG_EIGRP_PACKET(i, PACKET_DETAIL)
						? " detail"
						: "");
			if (IS_DEBUG_EIGRP_PACKET(i, RECV))
				vty_out(vty,
					"  EIGRP packet %s receive%s debugging is on\n",
					name,
					IS_DEBUG_EIGRP_PACKET(i, PACKET_DETAIL)
						? " detail"
						: "");
		}
	}

	return CMD_SUCCESS;
}


static eigrp_debug_scope_t eigrp_debug_cli_scope(const struct vty *vty)
{
	return vty->node == CONFIG_NODE ? EIGRP_DEBUG_SCOPE_CONFIG
					       : EIGRP_DEBUG_SCOPE_TERMINAL;
}

static int eigrp_debug_cli_result(eigrp_result_t result)
{
	return result == EIGRP_RESULT_SUCCESS ? CMD_SUCCESS
					      : CMD_WARNING_CONFIG_FAILED;
}

DEFUN(debug_eigrp_event, debug_eigrp_event_cmd,
      "debug eigrp event [detail]",
      DEBUG_STR EIGRP_STR
      "EIGRP event debugging\n"
      "Detailed information\n")
{
	int idx = 0;

	return eigrp_debug_cli_result(eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL,
		EIGRP_DEBUG_EVENT | (argv_find(argv, argc, "detail", &idx)
				     ? EIGRP_DEBUG_DETAIL : 0),
		eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_event, no_debug_eigrp_event_cmd,
      "no debug eigrp event [detail]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP event debugging\n"
      "Detailed information\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL,
			 EIGRP_DEBUG_EVENT | EIGRP_DEBUG_DETAIL,
			 eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_timers, debug_eigrp_timers_cmd,
      "debug eigrp timers",
      DEBUG_STR EIGRP_STR "EIGRP timer debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_TIMERS,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_timers, no_debug_eigrp_timers_cmd,
      "no debug eigrp timers",
      NO_STR UNDEBUG_STR EIGRP_STR "EIGRP timer debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_TIMERS,
			  eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_fsm, debug_eigrp_fsm_cmd,
      "debug eigrp fsm",
      DEBUG_STR EIGRP_STR "EIGRP DUAL finite-state-machine debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_FSM,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_fsm, no_debug_eigrp_fsm_cmd,
      "no debug eigrp fsm",
      NO_STR UNDEBUG_STR EIGRP_STR "EIGRP DUAL finite-state-machine debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_FSM,
			  eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_nsf, debug_eigrp_nsf_cmd,
      "debug eigrp nsf",
      DEBUG_STR EIGRP_STR "EIGRP NSF/graceful-restart debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_NSF,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_nsf, no_debug_eigrp_nsf_cmd,
      "no debug eigrp nsf",
      NO_STR UNDEBUG_STR EIGRP_STR "EIGRP NSF/graceful-restart debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_NSF,
			  eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_frr, debug_eigrp_frr_cmd,
      "debug eigrp frr",
      DEBUG_STR EIGRP_STR "EIGRP fast-reroute debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_GENERAL, EIGRP_DEBUG_FAST_REROUTE,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_frr, no_debug_eigrp_frr_cmd,
      "no debug eigrp frr",
      NO_STR UNDEBUG_STR EIGRP_STR "EIGRP fast-reroute debugging\n")
{
	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_GENERAL,
			  EIGRP_DEBUG_FAST_REROUTE,
			  eigrp_debug_cli_scope(vty)));
}

static unsigned long eigrp_debug_neighbor_flags(int argc,
						 struct cmd_token **argv)
{
	unsigned long flags = EIGRP_DEBUG_NEI;
	int idx = 0;

	if (argv_find(argv, argc, "siatimer", &idx))
		flags |= EIGRP_DEBUG_NEI_SIATIMER;
	if (argv_find(argv, argc, "static", &idx))
		flags |= EIGRP_DEBUG_NEI_STATIC;
	return flags;
}

DEFUN(debug_eigrp_neighbor, debug_eigrp_neighbor_cmd,
      "debug eigrp neighbor [siatimer] [static]",
      DEBUG_STR EIGRP_STR
      "EIGRP neighbor debugging\n"
      "Stuck-in-active timer messages\n"
      "Static-neighbor messages\n")
{
	return eigrp_debug_cli_result(eigrp_debug_set(EIGRP_DEBUG_TARGET_NEIGHBOR,
		eigrp_debug_neighbor_flags(argc, argv), eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_neighbor, no_debug_eigrp_neighbor_cmd,
      "no debug eigrp neighbor [siatimer] [static]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP neighbor debugging\n"
      "Stuck-in-active timer messages\n"
      "Static-neighbor messages\n")
{
	return eigrp_debug_cli_result(eigrp_debug_reset(EIGRP_DEBUG_TARGET_NEIGHBOR,
		eigrp_debug_neighbor_flags(argc, argv), eigrp_debug_cli_scope(vty)));
}

DEFUN(debug_eigrp_notifications, debug_eigrp_notifications_cmd,
      "debug eigrp notifications <rib|interface>",
      DEBUG_STR EIGRP_STR
      "EIGRP host notifications\n"
      "RIB notifications\n"
      "Interface notifications\n")
{
	int idx = 0;
	unsigned long flags = argv_find(argv, argc, "rib", &idx)
				      ? EIGRP_DEBUG_ZEBRA_RIB
				      : EIGRP_DEBUG_ZEBRA_INTERFACE;

	return eigrp_debug_cli_result(
		eigrp_debug_set(EIGRP_DEBUG_TARGET_NOTIFICATIONS, flags,
			eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_notifications, no_debug_eigrp_notifications_cmd,
      "no debug eigrp notifications <rib|interface>",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP host notifications\n"
      "RIB notifications\n"
      "Interface notifications\n")
{
	int idx = 0;
	unsigned long flags = argv_find(argv, argc, "rib", &idx)
				      ? EIGRP_DEBUG_ZEBRA_RIB
				      : EIGRP_DEBUG_ZEBRA_INTERFACE;

	return eigrp_debug_cli_result(
		eigrp_debug_reset(EIGRP_DEBUG_TARGET_NOTIFICATIONS, flags,
			  eigrp_debug_cli_scope(vty)));
}

static unsigned long eigrp_debug_transmit_flags(int argc,
						 struct cmd_token **argv)
{
	unsigned long flags = 0;
	bool category = false;
	int idx = 0;

#define EIGRP_TRANSMIT_ARG(_name, _flag)                                      \
	do {                                                                     \
		if (argv_find(argv, argc, (_name), &idx)) {                        \
			flags |= (_flag);                                             \
			if ((_flag) != EIGRP_DEBUG_TRANSMIT_DETAIL)                   \
				category = true;                                        \
		}                                                                \
	} while (0)
	EIGRP_TRANSMIT_ARG("ack", EIGRP_DEBUG_TRANSMIT_ACK);
	EIGRP_TRANSMIT_ARG("build", EIGRP_DEBUG_TRANSMIT_BUILD);
	EIGRP_TRANSMIT_ARG("detail", EIGRP_DEBUG_TRANSMIT_DETAIL);
	EIGRP_TRANSMIT_ARG("link", EIGRP_DEBUG_TRANSMIT_LINK);
	EIGRP_TRANSMIT_ARG("packetize", EIGRP_DEBUG_TRANSMIT_PACKETIZE);
	EIGRP_TRANSMIT_ARG("peerdown", EIGRP_DEBUG_TRANSMIT_PEERDOWN);
	EIGRP_TRANSMIT_ARG("sia", EIGRP_DEBUG_TRANSMIT_SIA);
	EIGRP_TRANSMIT_ARG("startup", EIGRP_DEBUG_TRANSMIT_STARTUP);
	EIGRP_TRANSMIT_ARG("strange", EIGRP_DEBUG_TRANSMIT_STRANGE);
#undef EIGRP_TRANSMIT_ARG

	/* Cisco's unqualified command enables the full transmit-debug family.
	 * "detail" by itself is likewise a request for detailed transmit output.
	 */
	if (!category)
		flags = EIGRP_DEBUG_TRANSMIT_ALL;
	return flags;
}

DEFUN(debug_eigrp_transmit, debug_eigrp_transmit_cmd,
      "debug eigrp transmit [ack] [build] [detail] [link] [packetize] [peerdown] [sia] [startup] [strange]",
      DEBUG_STR EIGRP_STR
      "EIGRP transmission events\n"
      "Acknowledgment processing\n"
      "Packet-build processing\n"
      "Detailed information\n"
      "Topology linked-list processing\n"
      "Packetizer processing\n"
      "Peer-down impact on packet generation\n"
      "Stuck-in-active processing\n"
      "Peer startup and initialization\n"
      "Unusual packet-processing events\n")
{
	return eigrp_debug_cli_result(eigrp_debug_set(EIGRP_DEBUG_TARGET_TRANSMIT,
		eigrp_debug_transmit_flags(argc, argv), eigrp_debug_cli_scope(vty)));
}

DEFUN(no_debug_eigrp_transmit, no_debug_eigrp_transmit_cmd,
      "no debug eigrp transmit [ack] [build] [detail] [link] [packetize] [peerdown] [sia] [startup] [strange]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP transmission events\n"
      "Acknowledgment processing\n"
      "Packet-build processing\n"
      "Detailed information\n"
      "Topology linked-list processing\n"
      "Packetizer processing\n"
      "Peer-down impact on packet generation\n"
      "Stuck-in-active processing\n"
      "Peer startup and initialization\n"
      "Unusual packet-processing events\n")
{
	return eigrp_debug_cli_result(eigrp_debug_reset(EIGRP_DEBUG_TARGET_TRANSMIT,
		eigrp_debug_transmit_flags(argc, argv), eigrp_debug_cli_scope(vty)));
}

static bool eigrp_debug_cli_address_family_request_build(
	int argc, struct cmd_token **argv, eigrp_state_request_t *request)
{
	int idx = 0;
	unsigned int i;

	if (!request)
		return false;
	memset(request, 0, sizeof(*request));
	if (argv_find(argv, argc, "ipv6", &idx))
		request->afi = EIGRP_ADDRESS_FAMILY_IPV6;
	else if (argv_find(argv, argc, "ipv4", &idx))
		request->afi = EIGRP_ADDRESS_FAMILY_IPV4;
	else
		return false;

	if (argv_find(argv, argc, "vrf", &idx) && idx + 1 < argc)
		request->vrf_name = argv[idx + 1]->arg;

	for (i = 0; i < (unsigned int)argc; i++) {
		const char *arg = argv[i]->arg;
		char *end = NULL;
		unsigned long value;

		if (!arg || !arg[0] || strspn(arg, "0123456789") != strlen(arg))
			continue;
		value = strtoul(arg, &end, 10);
		if (end && !*end && value > 0 && value <= 65535) {
			request->asn = (uint16_t)value;
			break;
		}
	}
	return true;
}

static bool eigrp_debug_cli_neighbor_address_build(
	const eigrp_state_request_t *request, const char *text,
	eigrp_address_t *address)
{
	int family;

	if (!request || !text || !address)
		return false;
	memset(address, 0, sizeof(*address));
	address->afi = request->afi;
	family = request->afi == EIGRP_ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
	return inet_pton(family, text, address->bytes) == 1;
}

static int eigrp_debug_cli_address_family_apply(
	struct vty *vty, int argc, struct cmd_token **argv,
	eigrp_debug_address_family_category_t category, bool enable)
{
	eigrp_state_request_t request;
	eigrp_address_t address;
	const eigrp_address_t *neighbor = NULL;
	const char *neighbor_text = NULL;
	eigrp_result_t result;
	int idx = 0;

	if (!eigrp_debug_cli_address_family_request_build(argc, argv, &request))
		return CMD_WARNING_CONFIG_FAILED;
	if (category == EIGRP_DEBUG_AF_NEIGHBOR
	    && argv_find(argv, argc, "neighbor", &idx) && idx + 1 < argc) {
		neighbor_text = argv[idx + 1]->arg;
		if (strcmp(neighbor_text, "vrf") != 0
		    && strcmp(neighbor_text, "notifications") != 0
		    && strcmp(neighbor_text, "summary") != 0) {
			if (!eigrp_debug_cli_neighbor_address_build(&request,
							      neighbor_text,
							      &address)) {
				vty_out(vty, "%% Invalid EIGRP neighbor address %s\n",
					neighbor_text);
				return CMD_WARNING_CONFIG_FAILED;
			}
			neighbor = &address;
		}
	}

	result = enable ? eigrp_debug_address_family_set(
				   &request, category, neighbor, eigrp_debug_cli_scope(vty))
			: eigrp_debug_address_family_reset(
				   &request, category, neighbor, eigrp_debug_cli_scope(vty));
	return eigrp_debug_cli_result(result);
}

DEFUN(debug_eigrp_address_family, debug_eigrp_address_family_cmd,
      "debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)]",
      DEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_ROUTE, true);
}

DEFUN(no_debug_eigrp_address_family, no_debug_eigrp_address_family_cmd,
      "no debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_ROUTE, false);
}

DEFUN(debug_eigrp_address_family_neighbor,
      debug_eigrp_address_family_neighbor_cmd,
      "debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] neighbor [WORD]",
      DEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP neighbor debugging\n"
      "Neighbor address\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_NEIGHBOR, true);
}

DEFUN(no_debug_eigrp_address_family_neighbor,
      no_debug_eigrp_address_family_neighbor_cmd,
      "no debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] neighbor [WORD]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP neighbor debugging\n"
      "Neighbor address\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_NEIGHBOR, false);
}

DEFUN(debug_eigrp_address_family_notifications,
      debug_eigrp_address_family_notifications_cmd,
      "debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] notifications",
      DEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP event notifications\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_NOTIFICATIONS, true);
}

DEFUN(no_debug_eigrp_address_family_notifications,
      no_debug_eigrp_address_family_notifications_cmd,
      "no debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] notifications",
      NO_STR UNDEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP event notifications\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_NOTIFICATIONS, false);
}

DEFUN(debug_eigrp_address_family_summary,
      debug_eigrp_address_family_summary_cmd,
      "debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] summary",
      DEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP summary route processing\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_SUMMARY, true);
}

DEFUN(no_debug_eigrp_address_family_summary,
      no_debug_eigrp_address_family_summary_cmd,
      "no debug eigrp address-family <ipv4|ipv6> [vrf NAME] [(1-65535)] summary",
      NO_STR UNDEBUG_STR EIGRP_STR
      "Address-family debugging\n"
      "IPv4 address-family\n"
      "IPv6 address-family\n"
      "Virtual Routing and Forwarding instance\n"
      "VRF name\n"
      "Autonomous-system number\n"
      "EIGRP summary route processing\n")
{
	return eigrp_debug_cli_address_family_apply(
		vty, argc, argv, EIGRP_DEBUG_AF_SUMMARY, false);
}

DEFUN(debug_eigrp_packet, debug_eigrp_packet_cmd,
      "debug eigrp packet <siaquery|siareply|ack|hello|probe|query|reply|request|retry|terse|update|all> [send|receive] [detail]",
      DEBUG_STR EIGRP_STR
      "EIGRP packets\n"
      "EIGRP SIA-Query packets\n"
      "EIGRP SIA-Reply packets\n"
      "EIGRP ack packets\n"
      "EIGRP hello packets\n"
      "EIGRP probe packets\n"
      "EIGRP query packets\n"
      "EIGRP reply packets\n"
      "EIGRP request packets\n"
      "EIGRP retransmissions\n"
      "Display all EIGRP packets except Hellos\n"
      "EIGRP update packets\n"
      "Display all EIGRP packets\n"
      "Send Packets\n"
      "Receive Packets\n"
      "Detail Information\n")
{
	uint32_t type = 0;
	unsigned long flag = EIGRP_DEBUG_SEND_RECV;
	eigrp_debug_scope_t scope = vty->node == CONFIG_NODE
					? EIGRP_DEBUG_SCOPE_CONFIG
					: EIGRP_DEBUG_SCOPE_TERMINAL;
	eigrp_result_t result;
	int idx = 0;

	if (argv_find(argv, argc, "hello", &idx))
		type = EIGRP_DEBUG_HELLO;
	else if (argv_find(argv, argc, "update", &idx))
		type = EIGRP_DEBUG_UPDATE;
	else if (argv_find(argv, argc, "query", &idx))
		type = EIGRP_DEBUG_QUERY;
	else if (argv_find(argv, argc, "ack", &idx))
		type = EIGRP_DEBUG_ACK;
	else if (argv_find(argv, argc, "probe", &idx))
		type = EIGRP_DEBUG_PROBE;
	else if (argv_find(argv, argc, "reply", &idx))
		type = EIGRP_DEBUG_REPLY;
	else if (argv_find(argv, argc, "request", &idx))
		type = EIGRP_DEBUG_REQUEST;
	else if (argv_find(argv, argc, "retry", &idx))
		type = EIGRP_DEBUG_RETRY;
	else if (argv_find(argv, argc, "siaquery", &idx))
		type = EIGRP_DEBUG_SIAQUERY;
	else if (argv_find(argv, argc, "siareply", &idx))
		type = EIGRP_DEBUG_SIAREPLY;
	else if (argv_find(argv, argc, "terse", &idx))
		type = EIGRP_DEBUG_PACKETS_TERSE;
	else if (argv_find(argv, argc, "all", &idx))
		type = EIGRP_DEBUG_PACKETS_ALL;

	if (argv_find(argv, argc, "send", &idx))
		flag = EIGRP_DEBUG_SEND;
	else if (argv_find(argv, argc, "receive", &idx))
		flag = EIGRP_DEBUG_RECV;
	if (argv_find(argv, argc, "detail", &idx))
		flag |= EIGRP_DEBUG_PACKET_DETAIL;

	result = eigrp_debug_packet_set(type, flag, scope);
	return result == EIGRP_RESULT_SUCCESS ? CMD_SUCCESS
					      : CMD_WARNING_CONFIG_FAILED;
}

DEFUN(no_debug_eigrp_packet, no_debug_eigrp_packet_cmd,
      "no debug eigrp packet <siaquery|siareply|ack|hello|probe|query|reply|request|retry|terse|update|all> [send|receive] [detail]",
      NO_STR UNDEBUG_STR EIGRP_STR
      "EIGRP packets\n"
      "EIGRP SIA-Query packets\n"
      "EIGRP SIA-Reply packets\n"
      "EIGRP ack packets\n"
      "EIGRP hello packets\n"
      "EIGRP probe packets\n"
      "EIGRP query packets\n"
      "EIGRP reply packets\n"
      "EIGRP request packets\n"
      "EIGRP retransmissions\n"
      "Display all EIGRP packets except Hellos\n"
      "EIGRP update packets\n"
      "Display all EIGRP packets\n"
      "Send Packets\n"
      "Receive Packets\n"
      "Detailed Information\n")
{
	uint32_t type = 0;
	unsigned long flag = EIGRP_DEBUG_SEND_RECV;
	eigrp_debug_scope_t scope = vty->node == CONFIG_NODE
					? EIGRP_DEBUG_SCOPE_CONFIG
					: EIGRP_DEBUG_SCOPE_TERMINAL;
	eigrp_result_t result;
	int idx = 0;

	if (argv_find(argv, argc, "hello", &idx))
		type = EIGRP_DEBUG_HELLO;
	else if (argv_find(argv, argc, "update", &idx))
		type = EIGRP_DEBUG_UPDATE;
	else if (argv_find(argv, argc, "query", &idx))
		type = EIGRP_DEBUG_QUERY;
	else if (argv_find(argv, argc, "ack", &idx))
		type = EIGRP_DEBUG_ACK;
	else if (argv_find(argv, argc, "probe", &idx))
		type = EIGRP_DEBUG_PROBE;
	else if (argv_find(argv, argc, "reply", &idx))
		type = EIGRP_DEBUG_REPLY;
	else if (argv_find(argv, argc, "request", &idx))
		type = EIGRP_DEBUG_REQUEST;
	else if (argv_find(argv, argc, "retry", &idx))
		type = EIGRP_DEBUG_RETRY;
	else if (argv_find(argv, argc, "siaquery", &idx))
		type = EIGRP_DEBUG_SIAQUERY;
	else if (argv_find(argv, argc, "siareply", &idx))
		type = EIGRP_DEBUG_SIAREPLY;
	else if (argv_find(argv, argc, "terse", &idx))
		type = EIGRP_DEBUG_PACKETS_TERSE;
	else if (argv_find(argv, argc, "all", &idx))
		type = EIGRP_DEBUG_PACKETS_ALL;

	if (argv_find(argv, argc, "send", &idx))
		flag = EIGRP_DEBUG_SEND;
	else if (argv_find(argv, argc, "receive", &idx))
		flag = EIGRP_DEBUG_RECV;
	if (argv_find(argv, argc, "detail", &idx))
		flag |= EIGRP_DEBUG_PACKET_DETAIL;

	result = eigrp_debug_packet_reset(type, flag, scope);
	return result == EIGRP_RESULT_SUCCESS ? CMD_SUCCESS
					      : CMD_WARNING_CONFIG_FAILED;
}

/* Debug node. */
static int config_write_debug(struct vty *vty);
static struct cmd_node eigrp_debug_node = {
	.name = "debug",
	.node = DEBUG_NODE,
	.prompt = "",
	.config_write = config_write_debug,
};

/* Initialize debug commands. */
void eigrp_debug_init(void)
{
	install_node(&eigrp_debug_node);

#define EIGRP_INSTALL_DEBUG_NODE(_node)                                       \
	do {                                                                     \
		install_element((_node), &show_debugging_eigrp_cmd);               \
		install_element((_node), &debug_eigrp_event_cmd);                   \
		install_element((_node), &no_debug_eigrp_event_cmd);                \
		install_element((_node), &debug_eigrp_timers_cmd);                  \
		install_element((_node), &no_debug_eigrp_timers_cmd);               \
		install_element((_node), &debug_eigrp_fsm_cmd);                     \
		install_element((_node), &no_debug_eigrp_fsm_cmd);                  \
		install_element((_node), &debug_eigrp_nsf_cmd);                     \
		install_element((_node), &no_debug_eigrp_nsf_cmd);                  \
		install_element((_node), &debug_eigrp_frr_cmd);                     \
		install_element((_node), &no_debug_eigrp_frr_cmd);                  \
		install_element((_node), &debug_eigrp_neighbor_cmd);                \
		install_element((_node), &no_debug_eigrp_neighbor_cmd);             \
		install_element((_node), &debug_eigrp_notifications_cmd);           \
		install_element((_node), &no_debug_eigrp_notifications_cmd);        \
		install_element((_node), &debug_eigrp_packet_cmd);                  \
		install_element((_node), &no_debug_eigrp_packet_cmd);               \
		install_element((_node), &debug_eigrp_transmit_cmd);                \
		install_element((_node), &no_debug_eigrp_transmit_cmd);             \
		install_element((_node), &debug_eigrp_address_family_cmd);          \
		install_element((_node), &no_debug_eigrp_address_family_cmd);       \
		install_element((_node), &debug_eigrp_address_family_neighbor_cmd); \
		install_element((_node),                                            \
				&no_debug_eigrp_address_family_neighbor_cmd);          \
		install_element((_node),                                            \
				&debug_eigrp_address_family_notifications_cmd);        \
		install_element((_node),                                            \
				&no_debug_eigrp_address_family_notifications_cmd);     \
		install_element((_node), &debug_eigrp_address_family_summary_cmd);  \
		install_element((_node),                                            \
				&no_debug_eigrp_address_family_summary_cmd);           \
	} while (0)

	EIGRP_INSTALL_DEBUG_NODE(ENABLE_NODE);
	EIGRP_INSTALL_DEBUG_NODE(CONFIG_NODE);
#undef EIGRP_INSTALL_DEBUG_NODE
}
