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
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"
#include "eigrpd/eigrp_debug.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_prefix.h"


#include <stdarg.h>

/* Enable debug option variables -- valid only session. */
unsigned long term_debug_eigrp = 0;
unsigned long term_debug_eigrp_nei = 0;
unsigned long term_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX] = {0};
unsigned long term_debug_eigrp_notifications = 0;
unsigned long term_debug_eigrp_transmit = 0;

/* Configuration debug option variables. */
unsigned long conf_debug_eigrp = 0;
unsigned long conf_debug_eigrp_nei = 0;
unsigned long conf_debug_eigrp_packet[EIGRP_DEBUG_PACKET_CATEGORY_MAX] = {0};
unsigned long conf_debug_eigrp_notifications = 0;
unsigned long conf_debug_eigrp_transmit = 0;


#define EIGRP_DEBUG_AF_SLOT_MAX 32
static eigrp_debug_address_family_state_t
	term_debug_eigrp_address_family[EIGRP_DEBUG_AF_SLOT_MAX];
static eigrp_debug_address_family_state_t
	conf_debug_eigrp_address_family[EIGRP_DEBUG_AF_SLOT_MAX];

static bool eigrp_debug_scope_valid(eigrp_debug_scope_t scope);

size_t eigrp_debug_address_family_state_count(void)
{
	return EIGRP_DEBUG_AF_SLOT_MAX;
}

bool eigrp_debug_address_family_state_get(
	eigrp_debug_scope_t scope, size_t index,
	eigrp_debug_address_family_state_t *state)
{
	const eigrp_debug_address_family_state_t *slots;

	if (!state || index >= EIGRP_DEBUG_AF_SLOT_MAX
	    || !eigrp_debug_scope_valid(scope))
		return false;

	slots = scope == EIGRP_DEBUG_SCOPE_CONFIG
			? conf_debug_eigrp_address_family
			: term_debug_eigrp_address_family;
	*state = slots[index];
	return true;
}

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
		term = &term_debug_eigrp_notifications;
		conf = &conf_debug_eigrp_notifications;
		valid_mask = EIGRP_DEBUG_NOTIFICATIONS;
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
	const eigrp_debug_address_family_state_t *slot,
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
	eigrp_debug_address_family_state_t *slots,
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
	if (strlen(vrf) >= sizeof(term_debug_eigrp_address_family[0].vrf_name))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	memset(&slots[free_slot], 0, sizeof(slots[free_slot]));
	slots[free_slot].used = true;
	slots[free_slot].afi = request->afi;
	slots[free_slot].asn = request->asn;
	slots[free_slot].all_vrfs = request->all_vrfs;
	memcpy(slots[free_slot].vrf_name, vrf, strlen(vrf) + 1U);
	slots[free_slot].category = category;
	if (neighbor) {
		slots[free_slot].neighbor_set = true;
		slots[free_slot].neighbor = *neighbor;
	}
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_debug_address_family_slots_reset(
	eigrp_debug_address_family_state_t *slots,
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
		const eigrp_debug_address_family_state_t *slot =
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
		const eigrp_debug_address_family_state_t *slot =
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
		   nbr->ei ? eigrp_intf_name_string(nbr->ei) : "self", old_state,
		   new_state);
}

void eigrp_debug_neighbor_sia(eigrp_neighbor_t *nbr, const char *event)
{
	if (!nbr || !(term_debug_eigrp_nei & EIGRP_DEBUG_NEI_SIATIMER))
		return;
	eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Neighbor %s on %s SIA: %s",
		   eigrp_print_addr(&nbr->src),
		   nbr->ei ? eigrp_intf_name_string(nbr->ei) : "-",
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
			   ei ? eigrp_intf_name_string((eigrp_interface_t *)ei) : "-",
			   eigrp_print_addr((eigrp_addr_t *)&nbr->src), message);
	else
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP TX AS %u %s: %s", eigrp ? eigrp->AS : 0,
			   ei ? eigrp_intf_name_string((eigrp_interface_t *)ei) : "-",
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

const char *eigrp_debug_packet_category_cli_name(
	eigrp_debug_packet_category_t category)
{
	if (category >= EIGRP_DEBUG_PACKET_CATEGORY_MAX)
		return "unknown";
	return eigrp_debug_packet_cli_names[category];
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

static const char *eigrp_debug_ipv4_string(struct in_addr address,
                                           char *buffer, size_t length)
{
	if (!inet_ntop(AF_INET, &address, buffer, length))
		return "invalid";
	return buffer;
}

static void eigrp_debug_ipv4_prefix_dump(const uint8_t *data, size_t length,
					 size_t prefix_offset)
{
	struct in_addr prefix = {0};
	char address[INET_ADDRSTRLEN];
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
	eigrp_log(EIGRP_LOG_DEBUG, "    prefix %s/%u",
			eigrp_debug_ipv4_string(prefix, address, sizeof(address)),
			prefixlen);
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
			char address[INET_ADDRSTRLEN];

			memcpy(&peer.s_addr, data + 5, sizeof(peer.s_addr));
			eigrp_log(EIGRP_LOG_DEBUG,
				"    peer termination neighbor %s",
				eigrp_debug_ipv4_string(peer, address, sizeof(address)));
		}
		break;
	case EIGRP_TLV_IPv4_INT:
		if (length >= 25) {
			struct in_addr nexthop;
			char address[INET_ADDRSTRLEN];

			memcpy(&nexthop.s_addr, data + 4, sizeof(nexthop.s_addr));
			eigrp_log(EIGRP_LOG_DEBUG,
				"    next-hop %s, delay(raw) %u, bandwidth %u, hop %u, reliability %u, load %u",
				eigrp_debug_ipv4_string(nexthop, address, sizeof(address)),
				eigrp_debug_get32(data + 8),
				eigrp_debug_get32(data + 12), data[19], data[20], data[21]);
			eigrp_debug_ipv4_prefix_dump(data, length, 24);
		}
		break;
	case EIGRP_TLV_IPv4_EXT:
		if (length >= 45) {
			struct in_addr nexthop;
			struct in_addr origin;
			char nexthop_address[INET_ADDRSTRLEN];
			char origin_address[INET_ADDRSTRLEN];

			memcpy(&nexthop.s_addr, data + 4, sizeof(nexthop.s_addr));
			memcpy(&origin.s_addr, data + 8, sizeof(origin.s_addr));
			eigrp_log(EIGRP_LOG_DEBUG,
				"    next-hop %s, origin %s, origin-AS %u, external-metric %u, protocol %u",
				eigrp_debug_ipv4_string(nexthop, nexthop_address, sizeof(nexthop_address)),
				eigrp_debug_ipv4_string(origin, origin_address, sizeof(origin_address)),
				eigrp_debug_get32(data + 12),
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
			char router_id[INET_ADDRSTRLEN];

			rid_addr.s_addr = htonl(rid);
			eigrp_log(EIGRP_LOG_DEBUG,
				"    AFI %u, TID %u, router-id %s, tag %u, reliability %u, load %u, hop %u",
				afi, tid,
				eigrp_debug_ipv4_string(rid_addr, router_id, sizeof(router_id)),
				data[13], data[14], data[15], data[19]);
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
	header = (const eigrp_header_t *)eigrp_stream_data(packet->s);
	category = eigrp_debug_packet_category_get(header);
	if (category >= EIGRP_DEBUG_PACKET_CATEGORY_MAX)
		return;
	state = term_debug_eigrp_packet[category];
	if (!(state & EIGRP_DEBUG_SEND))
		return;

	ifname = eigrp_intf_name_string(ei);
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
		   eigrp_debug_packet_category_name(category), eigrp_intf_name_string(ei),
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
	header = (const eigrp_header_t *)eigrp_stream_data(packet->s);

	eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: Sending %s on %s nbr %s, retry %u, RTO %u",
		   eigrp_debug_packet_category_name(
			   eigrp_debug_packet_category_get(header)),
		   eigrp_intf_name_string(nbr->ei), eigrp_print_addr(&nbr->src), retry_count,
		   eigrp_neighbor_rto_get(nbr));
	eigrp_log(EIGRP_LOG_DEBUG, "  AS %u, Flags 0x%x, Seq %u/%u",
		   ntohs(header->ASNumber), ntohl(header->flags),
		   ntohl(header->sequence), ntohl(header->ack));
	if (state & EIGRP_DEBUG_PACKET_DETAIL) {
		eigrp_log(EIGRP_LOG_DEBUG, "  packet length %u", packet->length);
		eigrp_debug_packet_detail_dump(header, packet->length);
	}
}


void eigrp_debug_header_dump(const eigrp_header_t *eigrph)
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
