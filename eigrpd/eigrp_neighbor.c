// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Neighbor Handling.
 * Copyright (C) 2013-2016
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 *   Frantisek Gazo
 *   Tomas Hvorkovy
 *   Martin Kontsek
 *   Lukas Koribsky
 */

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_table.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_debug.h"

#define EIGRP_NEIGHBOR_SRTT_ALPHA_SHIFT 3U /* alpha = 1/8 */
#define EIGRP_NEIGHBOR_RTTVAR_BETA_SHIFT 2U /* beta = 1/4 */

static uint32_t eigrp_neighbor_rto_calculate(uint32_t srtt_msec)
{
	uint64_t rto = (uint64_t)srtt_msec * EIGRP_TRANSPORT_RTO_SRTT_MULTIPLIER;

	if (rto < EIGRP_TRANSPORT_RTO_MIN_MSEC)
		return EIGRP_TRANSPORT_RTO_MIN_MSEC;
	if (rto > EIGRP_TRANSPORT_RTO_MAX_MSEC)
		return EIGRP_TRANSPORT_RTO_MAX_MSEC;
	return (uint32_t)rto;
}

void eigrp_neighbor_rtt_reset(eigrp_neighbor_t *nbr)
{
	if (!nbr)
		return;

	nbr->srtt_valid = false;
	nbr->srtt_msec = 0;
	nbr->rttvar_msec = 0;
	nbr->rto_msec = EIGRP_TRANSPORT_RTO_INITIAL_MSEC;
}

static void eigrp_neighbor_rtt_sample(eigrp_neighbor_t *nbr,
				      uint32_t sample_msec)
{
	int64_t error;
	int64_t variance_error;
	int64_t srtt;
	int64_t rttvar;

	if (!nbr)
		return;

	if (!nbr->srtt_valid) {
		nbr->srtt_valid = true;
		nbr->srtt_msec = sample_msec;
		nbr->rttvar_msec = sample_msec / 2U;
		nbr->rto_msec = eigrp_neighbor_rto_calculate(nbr->srtt_msec);
		return;
	}

	/* RFC 6298 ordering: RTTVAR uses the old SRTT, then SRTT is updated. */
	error = (int64_t)sample_msec - (int64_t)nbr->srtt_msec;
	variance_error = (error < 0 ? -error : error)
			 - (int64_t)nbr->rttvar_msec;
	rttvar = (int64_t)nbr->rttvar_msec
		 + variance_error / (1U << EIGRP_NEIGHBOR_RTTVAR_BETA_SHIFT);
	if (rttvar < 0)
		rttvar = 0;
	nbr->rttvar_msec = (uint32_t)rttvar;

	srtt = (int64_t)nbr->srtt_msec
	       + error / (1U << EIGRP_NEIGHBOR_SRTT_ALPHA_SHIFT);
	if (srtt < 0)
		srtt = 0;
	nbr->srtt_msec = (uint32_t)srtt;
	nbr->rto_msec = eigrp_neighbor_rto_calculate(nbr->srtt_msec);
}

void eigrp_neighbor_srtt_update(eigrp_neighbor_t *nbr,
				const eigrp_packet_t *packet)
{
	uint64_t now_msec;
	uint64_t sample_msec;

	if (!nbr || !packet || packet->retrans_counter != 0
	    || packet->sent_msec == 0)
		return;

	now_msec = eigrp_sys_monotime_msec();
	if (now_msec < packet->sent_msec)
		return;

	sample_msec = now_msec - packet->sent_msec;
	if (sample_msec > UINT32_MAX)
		sample_msec = UINT32_MAX;
	eigrp_neighbor_rtt_sample(nbr, (uint32_t)sample_msec);
}

void eigrp_neighbor_rto_backoff(eigrp_neighbor_t *nbr)
{
	uint64_t rto;

	if (!nbr)
		return;

	rto = eigrp_neighbor_rto_get(nbr);
	rto *= 2U;
	if (rto > EIGRP_TRANSPORT_RTO_MAX_MSEC)
		rto = EIGRP_TRANSPORT_RTO_MAX_MSEC;
	nbr->rto_msec = (uint32_t)rto;
}

uint32_t eigrp_neighbor_rto_get(const eigrp_neighbor_t *nbr)
{
	if (!nbr || nbr->rto_msec == 0)
		return EIGRP_TRANSPORT_RTO_INITIAL_MSEC;
	return nbr->rto_msec;
}

struct eigrp_neighbor_config {
	eigrp_address_t address;
	char *interface_name;
	eigrp_neighbor_config_t *next;
};

struct eigrp_neighbor_policy_entry {
	eigrp_address_t address;
	char *description;
	bool maximum_prefix_configured;
	eigrp_prefix_limit_t maximum_prefix;
	struct eigrp_neighbor_policy_entry *next;
};

struct eigrp_neighbor_policy_state {
	struct eigrp_neighbor_policy_entry *entries;
	bool maximum_prefix_all_configured;
	eigrp_prefix_limit_t maximum_prefix_all;
	bool log_changes_configured;
	bool log_changes;
	bool log_warnings_configured;
	bool log_warnings;
	uint16_t log_warning_interval;
};

static char *eigrp_neighbor_string_duplicate(const char *value)
{
	size_t len;
	char *copy;

	if (!value)
		return NULL;
	len = strlen(value) + 1;
	copy = malloc(len);
	if (!copy)
		return NULL;
	memcpy(copy, value, len);
	return copy;
}

static bool eigrp_neighbor_address_equal(const eigrp_address_t *a,
					 const eigrp_address_t *b)
{
	size_t len;

	if (!a || !b || a->afi != b->afi)
		return false;
	len = a->afi == EIGRP_ADDRESS_FAMILY_IPV4 ? 4 : 16;
	return memcmp(a->bytes, b->bytes, len) == 0;
}

static void eigrp_neighbor_static_debug(const char *action,
					const eigrp_address_family_config_t *af,
					const eigrp_address_t *address,
					const char *interface_name)
{
	char address_text[INET6_ADDRSTRLEN];
	int family;

	if (!(term_debug_eigrp_nei & EIGRP_DEBUG_NEI_STATIC) || !address)
		return;
	family = address->afi == EIGRP_ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
	if (!inet_ntop(family, address->bytes, address_text, sizeof(address_text)))
		memcpy(address_text, "<invalid>", sizeof("<invalid>"));
	eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: %s static neighbor %s AS %u interface %s", action,
		   address_text, af ? af->asn : 0,
		   interface_name ? interface_name : "-");
}

/*
 * Syntax:
 *   Classic: `neighbor ADDRESS INTERFACE` / `no neighbor ADDRESS INTERFACE`
 *   Named: `neighbor ADDRESS INTERFACE` / `no neighbor ADDRESS INTERFACE`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: address-family mode
 * Description:
 * Creates or removes a static EIGRP neighbor definition.
 * Named mode terminates at EIGRP-owned neighbor state instead of invoking the classic FRR command callback.
 */
eigrp_result_t eigrp_neighbor_static_create(eigrp_address_family_config_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name)
{
	eigrp_neighbor_config_t *neighbor;

	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	if (!address || address->afi != af->afi || !interface_name
	    || !interface_name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;

	for (neighbor = af->neighbors; neighbor; neighbor = neighbor->next)
		if (eigrp_neighbor_address_equal(&neighbor->address, address)
		    && strcmp(neighbor->interface_name, interface_name) == 0)
			return EIGRP_RESULT_SUCCESS;

	neighbor = calloc(1, sizeof(*neighbor));
	if (!neighbor)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	neighbor->interface_name = eigrp_neighbor_string_duplicate(interface_name);
	if (!neighbor->interface_name) {
		free(neighbor);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	neighbor->address = *address;
	neighbor->next = af->neighbors;
	af->neighbors = neighbor;
	eigrp_neighbor_static_debug("add", af, address, interface_name);
	if (af->runtime) {
		eigrp_interface_t *ei =
			eigrp_intf_lookup_by_name(af->runtime, interface_name);
		if (ei)
			(void)eigrp_neighbor_static_hello_send(ei);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `neighbor ADDRESS INTERFACE` / `no neighbor ADDRESS INTERFACE`
 *   Named: `neighbor ADDRESS INTERFACE` / `no neighbor ADDRESS INTERFACE`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: address-family mode
 * Description:
 * Creates or removes a static EIGRP neighbor definition.
 * Named mode terminates at EIGRP-owned neighbor state instead of invoking the classic FRR command callback.
 */
eigrp_result_t eigrp_neighbor_static_delete(eigrp_address_family_config_t *af,
					    const eigrp_address_t *address,
					    const char *interface_name)
{
	eigrp_neighbor_config_t **cursor;
	eigrp_neighbor_config_t *neighbor;

	if (!address || !interface_name)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;

	for (cursor = &af->neighbors; *cursor; cursor = &(*cursor)->next) {
		neighbor = *cursor;
		if (!eigrp_neighbor_address_equal(&neighbor->address, address)
		    || strcmp(neighbor->interface_name, interface_name) != 0)
			continue;
		*cursor = neighbor->next;
		eigrp_neighbor_static_debug("remove", af, address, interface_name);
		free(neighbor->interface_name);
		free(neighbor);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

bool eigrp_neighbor_static_source_allowed(eigrp_interface_t *ei,
                                          const eigrp_addr_t *src)
{
	eigrp_address_family_config_t *af;
	eigrp_neighbor_config_t *neighbor;
	const char *interface_name;
	bool has_static = false;

	if (!ei || !ei->eigrp || !src || src->afi != AF_INET)
		return false;
	af = eigrp_instance_runtime_config(ei->eigrp);
	if (!af || af->afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return true;
	interface_name = eigrp_intf_name_string(ei);

	for (neighbor = af->neighbors; neighbor; neighbor = neighbor->next) {
		if (strcmp(neighbor->interface_name, interface_name) != 0)
			continue;
		has_static = true;
		if (memcmp(neighbor->address.bytes, &src->ip.v4,
			   sizeof(src->ip.v4)) == 0)
			return true;
	}
	return !has_static;
}

bool eigrp_neighbor_static_hello_send(eigrp_interface_t *ei)
{
	eigrp_address_family_config_t *af;
	eigrp_neighbor_config_t *neighbor;
	eigrp_addr_t dst;
	const char *interface_name;
	bool configured = false;

	if (!ei || !ei->eigrp)
		return false;
	af = eigrp_instance_runtime_config(ei->eigrp);
	if (!af || af->afi != EIGRP_ADDRESS_FAMILY_IPV4)
		return false;
	interface_name = eigrp_intf_name_string(ei);

	for (neighbor = af->neighbors; neighbor; neighbor = neighbor->next) {
		if (strcmp(neighbor->interface_name, interface_name) != 0)
			continue;
		configured = true;
		memset(&dst, 0, sizeof(dst));
		dst.afi = AF_INET;
		memcpy(&dst.ip.v4, neighbor->address.bytes, sizeof(dst.ip.v4));
		eigrp_hello_send_unicast(ei, &dst);
	}
	return configured;
}

void eigrp_neighbor_static_delete_all(eigrp_address_family_config_t *af)
{
	eigrp_neighbor_config_t *neighbor;
	eigrp_neighbor_config_t *next;

	if (!af)
		return;
	for (neighbor = af->neighbors; neighbor; neighbor = next) {
		next = neighbor->next;
		free(neighbor->interface_name);
		free(neighbor);
	}
	af->neighbors = NULL;
}

static void eigrp_neighbor_runtime_address(const eigrp_neighbor_t *nbr,
					   eigrp_address_t *address)
{
	memset(address, 0, sizeof(*address));
	if (nbr->src.afi == AF_INET6) {
		address->afi = EIGRP_ADDRESS_FAMILY_IPV6;
		memcpy(address->bytes, &nbr->src.ip.v6, 16);
		return;
	}
	address->afi = EIGRP_ADDRESS_FAMILY_IPV4;
	memcpy(address->bytes, &nbr->src.ip.v4, 4);
}

static uint32_t eigrp_neighbor_prefix_count(eigrp_instance_t *runtime,
					    eigrp_neighbor_t *neighbor)
{
	eigrp_prefix_descriptor_t *prefix;
	eigrp_route_descriptor_t *route;
	eigrp_table_node_t *route_node;
	eigrp_list_node_t *list_node;
	uint32_t count = 0;

	if (!runtime || !runtime->topology_table || !neighbor)
		return 0;
	for (route_node = eigrp_table_first(runtime->topology_table); route_node;
	     route_node = eigrp_table_next(route_node)) {
		prefix = route_node->info;
		if (!prefix)
			continue;
		for (EIGRP_LIST_ELEMENTS_RO(prefix->entries, list_node, route)) {
			if (route->adv_router == neighbor) {
				count++;
				break;
			}
		}
	}
	return count;
}

/*
 * Syntax:
 *   EXEC: `show eigrp address-family <ipv4|ipv6> ... neighbors [static] [detail] [IFNAME]`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Walks EIGRP neighbor state for operational display.
 * The common API exposes protocol state without FRR VTY objects.
 */
eigrp_result_t eigrp_neighbor_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, bool static_only,
	eigrp_neighbor_state_walk_cb callback, void *arg)
{
	eigrp_neighbor_config_t *configured;
	eigrp_interface_t *ei;
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *if_node;
	eigrp_list_node_t *nbr_node;
	eigrp_neighbor_state_t state;
	eigrp_result_t result;
	bool matched = false;

	if (!callback || (!config && !runtime))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (static_only) {
		if (!config)
			return EIGRP_RESULT_NOT_FOUND;
		for (configured = config->neighbors; configured;
		     configured = configured->next) {
			if (interface_name
			    && strcmp(configured->interface_name, interface_name) != 0)
				continue;
			memset(&state, 0, sizeof(state));
			state.address = configured->address;
			state.interface_name = configured->interface_name;
			state.state_name = "configured";
			state.static_configured = true;
			result = callback(&state, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
			matched = true;
		}
		return matched ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;
	}

	if (!runtime) {
		if (config && config->afi == EIGRP_ADDRESS_FAMILY_IPV6)
			return EIGRP_RESULT_NOT_IMPLEMENTED;
		return EIGRP_RESULT_NOT_FOUND;
	}
	if (!runtime->data_path_ready)
		return EIGRP_RESULT_NOT_IMPLEMENTED;

	for (EIGRP_LIST_ELEMENTS_RO(runtime->eiflist, if_node, ei)) {
		const char *name = eigrp_intf_name_string(ei);

		if (interface_name && strcmp(name, interface_name) != 0)
			continue;
		for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, nbr_node, nbr)) {
			/* Normal neighbor output contains established adjacencies only. */
			if (nbr->state != EIGRP_NEIGHBOR_UP)
				continue;
			memset(&state, 0, sizeof(state));
			eigrp_neighbor_runtime_address(nbr, &state.address);
			state.interface_name = name;
			state.state_name = eigrp_nbr_state_str(nbr);
			state.runtime_present = true;
			state.hold_time = (uint16_t)eigrp_sys_timer_remaining_seconds(
				nbr->t_holddown);
			if (nbr->up_since_msec) {
				uint64_t now = eigrp_sys_monotime_msec();

				if (now >= nbr->up_since_msec)
					state.uptime_seconds =
						(now - nbr->up_since_msec) / 1000U;
			}
			state.reliable_queue_count =
				nbr->retrans_queue ? nbr->retrans_queue->count : 0;
			state.sequence_number = nbr->recv_sequence_number;
			state.prefix_count = eigrp_neighbor_prefix_count(runtime, nbr);
			state.retransmit_count = nbr->retransmissions;
			if (nbr->retrans_queue && nbr->retrans_queue->tail)
				state.retry_count = nbr->retrans_queue->tail->retrans_counter;
			state.srtt_valid = nbr->srtt_valid;
			state.srtt_msec = nbr->srtt_msec;
			state.rto_msec = eigrp_neighbor_rto_get(nbr);
			state.os_major = nbr->os_rel_major;
			state.os_minor = nbr->os_rel_minor;
			state.tlv_major = nbr->tlv_rel_major;
			state.tlv_minor = nbr->tlv_rel_minor;
			state.tlv_version = nbr->tlv_version;
			result = callback(&state, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
			matched = true;
		}
	}

	return matched ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;
}
void eigrp_neighbor_codec_bind(eigrp_neighbor_t *nbr, uint8_t tlv_version)
{
	const eigrp_tlv_codec_t *codec;

	assert(nbr);
	assert(nbr->ei);
	assert(nbr->ei->eigrp);

	switch (tlv_version) {
	case EIGRP_TLV_32B_VERSION:
		codec = &nbr->ei->eigrp->tlv1_codec;
		break;
	case EIGRP_TLV_64B_VERSION:
		codec = &nbr->ei->eigrp->tlv2_codec;
		break;
	default:
		return;
	}

	assert(codec->decoder);
	assert(codec->encoder);
	nbr->tlv_version = tlv_version;
	nbr->decoder = codec->decoder;
	nbr->encoder = codec->encoder;
}

/**
 * initalize neighbor
 */
static void eigrp_nbr_init(eigrp_neighbor_t *nbr, eigrp_addr_t *src)
{
	eigrp_addr_copy(&nbr->src, src);

	/* copy over the values passed in by the neighbor */
	nbr->K1 = EIGRP_K1_DEFAULT;
	nbr->K2 = EIGRP_K2_DEFAULT;
	nbr->K3 = EIGRP_K3_DEFAULT;
	nbr->K4 = EIGRP_K4_DEFAULT;
	nbr->K5 = EIGRP_K5_DEFAULT;
	nbr->K6 = EIGRP_K6_DEFAULT;

	nbr->v_holddown = EIGRP_HOLD_INTERVAL_DEFAULT;

	nbr->tlv_version = 0;
	nbr->decoder = eigrp_packet_decoder_safe;
	nbr->encoder = eigrp_packet_encoder_safe;

	//  if (IS_DEBUG_EIGRP_EVENT)
		//               eigrp_print_routerid(nbr->router_id));
}

/**
 * Create a new neighbor structure and initalize it.
 */
eigrp_neighbor_t *eigrp_nbr_create(eigrp_interface_t *ei, eigrp_addr_t *src)
{
	eigrp_neighbor_t *nbr;

	/* Allcate new neighbor. */
	nbr = calloc(1, sizeof(eigrp_neighbor_t));

	/* Relate neighbor to the interface. */
	nbr->ei = ei;

	/* Set default values. */
	eigrp_nbr_init(nbr, src);
	eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);

	// If this is the 'self' neighbor, then you dont have an interface
	if (ei) {
		eigrp_list_add(ei->nbrs, nbr);
	}
	if (IS_DEBUG_EIGRP_EVENT) {
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP event: neighbor %s created%s%s",
			   eigrp_print_addr(&nbr->src),
			   ei ? " on " : "", ei ? eigrp_intf_name_string(ei) : "");
		if (IS_DEBUG_EIGRP(0, DETAIL) && ei && ei->eigrp)
			eigrp_log(EIGRP_LOG_DEBUG, "EIGRP event detail: AS %u hold %u state %u",
				   ei->eigrp->AS, nbr->v_holddown, nbr->state);
	}
	return nbr;
}

eigrp_neighbor_t *eigrp_nbr_lookup(eigrp_interface_t *ei, eigrp_header_t *eigrph,
				   eigrp_addr_t *src)
{
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *node, *nnode;

	for (EIGRP_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
	    if (eigrp_addr_same(src, &nbr->src)) {
		    return nbr;
		}
	}

	return NULL;
}

/**
 * @fn eigrp_nbr_lookup_by_addr
 *
 * @param[in]		ei			EIGRP interface
 * @param[in]		nbr_addr 	Address of neighbor
 *
 * @return void
 *
 * @par
 * Function is used for neighbor lookup by address
 * in specified interface.
 */
eigrp_neighbor_t *eigrp_nbr_lookup_by_addr(eigrp_interface_t *ei,
					   struct in_addr *addr)
{
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *node, *nnode;

	for (EIGRP_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
		if (addr->s_addr == nbr->src.ip.v4.s_addr) {
			return nbr;
		}
	}

	return NULL;
}

/**
 * @fn eigrp_nbr_lookup_by_addr_process
 *
 * @param[in]    eigrp          EIGRP process
 * @param[in]    nbr_addr       Address of neighbor
 *
 * @return void
 *
 * @par
 * Function is used for neighbor lookup by address
 * in whole EIGRP process.
 */
eigrp_neighbor_t *eigrp_nbr_lookup_by_addr_process(eigrp_instance_t *eigrp,
						   struct in_addr nbr_addr)
{
	eigrp_interface_t *ei;
	eigrp_list_node_t *node, *node2, *nnode2;
	eigrp_neighbor_t *nbr;

	/* iterate over all eigrp interfaces */
	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		/* iterate over all neighbors on eigrp interface */
		for (EIGRP_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
			/* compare if neighbor address is same as arg address */
			if (nbr->src.ip.v4.s_addr == nbr_addr.s_addr) {
				return nbr;
			}
		}
	}

	return NULL;
}


/* Delete specified EIGRP neighbor from interface. */
void eigrp_nbr_delete(eigrp_neighbor_t *nbr)
{
	if (nbr && IS_DEBUG_EIGRP_EVENT) {
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP event: neighbor %s delete%s%s",
			   eigrp_print_addr(&nbr->src), nbr->ei ? " on " : "",
			   nbr->ei ? eigrp_intf_name_string(nbr->ei) : "");
		if (IS_DEBUG_EIGRP(0, DETAIL) && nbr->ei && nbr->ei->eigrp)
			eigrp_log(EIGRP_LOG_DEBUG, "EIGRP event detail: AS %u state %u retrans %u",
				   nbr->ei->eigrp->AS, nbr->state, nbr->retrans_counter);
	}
	eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
	if (nbr->ei)
		eigrp_topology_neighbor_down(nbr->ei->eigrp, nbr);

	/* Cancel neighbor-owned host runtime events before releasing queues/state. */
	eigrp_sys_event_cancel(&nbr->t_nbr_send_gr);
	if (nbr->nbr_gr_prefixes)
		eigrp_list_delete(&nbr->nbr_gr_prefixes);
	if (nbr->nbr_gr_prefixes_send)
		eigrp_list_delete(&nbr->nbr_gr_prefixes_send);
	eigrp_packet_queue_free(nbr->retrans_queue);
	eigrp_sys_event_cancel(&nbr->t_holddown);

	if (nbr->ei)
		eigrp_list_delete_data(nbr->ei->nbrs, nbr);
	free(nbr);
}

void eigrp_neighbor_holddown_expired(void *arg)
{
	eigrp_neighbor_t *nbr = arg;
	if (IS_DEBUG_EIGRP(0, TIMERS))
		eigrp_log(EIGRP_LOG_DEBUG, "EIGRP: hold timer expired for neighbor %s",
			   eigrp_print_addr(&nbr->src));
	if (nbr->ei->eigrp->log_neighbor_changes)
		eigrp_log(EIGRP_LOG_INFO, "Neighbor %s (%s) is down: holding time expired",
			  eigrp_print_addr(&nbr->src),
			  nbr->ei->name);
	eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
	eigrp_nbr_delete(nbr);

	return;
}

uint8_t eigrp_nbr_state_get(eigrp_neighbor_t *nbr)
{
	return (nbr->state);
}

void eigrp_nbr_state_set(eigrp_neighbor_t *nbr, uint8_t state)
{
	uint8_t old_state;

	if (!nbr)
		return;

	old_state = nbr->state;

	if (old_state == EIGRP_NEIGHBOR_UP && state != EIGRP_NEIGHBOR_UP)
		eigrp_interface_encoder_unbind(nbr->ei, nbr->tlv_version);

	nbr->state = state;
	eigrp_debug_neighbor_state(nbr, old_state, state);
	if (old_state == EIGRP_NEIGHBOR_UP && state != EIGRP_NEIGHBOR_UP)
		eigrp_debug_transmit_event(EIGRP_DEBUG_TRANSMIT_PEERDOWN,
				   nbr->ei ? nbr->ei->eigrp : NULL, nbr->ei, nbr,
				   "neighbor left UP state");

	if (state == EIGRP_NEIGHBOR_UP && old_state != EIGRP_NEIGHBOR_UP) {
		eigrp_interface_encoder_bind(nbr->ei, nbr->tlv_version);
		nbr->up_since_msec = eigrp_sys_monotime_msec();
		nbr->retransmissions = 0;
	} else if (old_state == EIGRP_NEIGHBOR_UP && state != EIGRP_NEIGHBOR_UP) {
		nbr->up_since_msec = 0;
	}

	if (eigrp_nbr_state_get(nbr) == EIGRP_NEIGHBOR_DOWN) {
		// reset all the seq/ack counters
		nbr->recv_sequence_number = 0;
		nbr->init_sequence_number = 0;
		nbr->retrans_counter = 0;
		nbr->cr_mode = false;
		nbr->cr_sequence = 0;
		eigrp_neighbor_rtt_reset(nbr);

		// Kvalues
		nbr->K1 = EIGRP_K1_DEFAULT;
		nbr->K2 = EIGRP_K2_DEFAULT;
		nbr->K3 = EIGRP_K3_DEFAULT;
		nbr->K4 = EIGRP_K4_DEFAULT;
		nbr->K5 = EIGRP_K5_DEFAULT;
		nbr->K6 = EIGRP_K6_DEFAULT;

		// hold time..
		nbr->v_holddown = EIGRP_HOLD_INTERVAL_DEFAULT;
		eigrp_sys_event_cancel(&nbr->t_holddown);

		/* out with the old */
		if (nbr->retrans_queue)
			eigrp_packet_queue_free(nbr->retrans_queue);

		/* in with the new */
		nbr->retrans_queue = eigrp_packet_queue_new();

		nbr->crypt_seqnum = 0;
	}
}

const char *eigrp_nbr_state_str(eigrp_neighbor_t *nbr)
{
	const char *state;
	switch (nbr->state) {
	case EIGRP_NEIGHBOR_DOWN:
		state = "Down";
		break;
	case EIGRP_NEIGHBOR_PENDING:
		state = "Waiting for Init";
		break;
	case EIGRP_NEIGHBOR_UP:
		state = "Up";
		break;
	default:
		state = "Unknown";
		break;
	}

	return (state);
}

void eigrp_nbr_state_update(eigrp_neighbor_t *nbr)
{
	switch (nbr->state) {
	case EIGRP_NEIGHBOR_DOWN:
		eigrp_sys_event_cancel(&nbr->t_holddown);
		break;
	case EIGRP_NEIGHBOR_PENDING: {
		/*Reset Hold Down Timer for neighbor*/
		eigrp_sys_event_cancel(&nbr->t_holddown);
		eigrp_sys_timer_add(&nbr->t_holddown,
				  eigrp_neighbor_holddown_expired, nbr,
				  (uint32_t)nbr->v_holddown * 1000U);
		break;
	}
	case EIGRP_NEIGHBOR_UP: {
		/*Reset Hold Down Timer for neighbor*/
		eigrp_sys_event_cancel(&nbr->t_holddown);
		eigrp_sys_timer_add(&nbr->t_holddown,
				  eigrp_neighbor_holddown_expired, nbr,
				  (uint32_t)nbr->v_holddown * 1000U);
		break;
	}
	}
}

int eigrp_nbr_count_get(eigrp_instance_t *eigrp)
{
	eigrp_interface_t *iface;
	eigrp_list_node_t *node, *node2, *nnode2;
	eigrp_neighbor_t *nbr;
	uint32_t counter;

	counter = 0;
	for (EIGRP_LIST_ELEMENTS_RO(eigrp->eiflist, node, iface)) {
		for (EIGRP_LIST_ELEMENTS(iface->nbrs, node2, nnode2, nbr)) {
			if (nbr->state == EIGRP_NEIGHBOR_UP) {
				counter++;
			}
		}
	}
	return counter;
}

static bool eigrp_neighbor_clear_address_valid(const eigrp_address_t *address)
{
	return address && (address->afi == EIGRP_ADDRESS_FAMILY_IPV4
			   || address->afi == EIGRP_ADDRESS_FAMILY_IPV6);
}

static bool eigrp_neighbor_clear_address_match(const eigrp_neighbor_t *nbr,
				       const eigrp_address_t *address)
{
	if (!nbr || !address)
		return false;

	if (address->afi == EIGRP_ADDRESS_FAMILY_IPV6)
		return nbr->src.afi == AF_INET6
		       && memcmp(&nbr->src.ip.v6, address->bytes,
				 sizeof(nbr->src.ip.v6)) == 0;
	if (address->afi == EIGRP_ADDRESS_FAMILY_IPV4)
		return nbr->src.afi == AF_INET
		       && memcmp(&nbr->src.ip.v4, address->bytes,
				 sizeof(nbr->src.ip.v4)) == 0;
	return false;
}

static void eigrp_neighbor_clear_report(const eigrp_neighbor_t *nbr, bool soft,
					eigrp_neighbor_clear_cb callback,
					void *arg)
{
	eigrp_neighbor_clear_state_t state;

	if (!callback)
		return;
	memset(&state, 0, sizeof(state));
	if (nbr->src.afi == AF_INET) {
		state.address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
		memcpy(state.address.bytes, &nbr->src.ip.v4, sizeof(nbr->src.ip.v4));
	} else if (nbr->src.afi == AF_INET6) {
		state.address.afi = EIGRP_ADDRESS_FAMILY_IPV6;
		memcpy(state.address.bytes, &nbr->src.ip.v6, sizeof(nbr->src.ip.v6));
	}
	state.interface_name = nbr->ei ? eigrp_intf_name_string(nbr->ei) : NULL;
	state.soft = soft;
	callback(&state, arg);
}

static void eigrp_neighbor_clear_hard(eigrp_neighbor_t *nbr,
				      bool peer_termination,
				      eigrp_neighbor_clear_cb callback, void *arg)
{
	const char *interface_name = nbr->ei ? eigrp_intf_name_string(nbr->ei) : "?";

	eigrp_log(EIGRP_LOG_DEBUG, "Neighbor %s (%s) is down: manually cleared",
		   eigrp_print_addr(&nbr->src), interface_name);
	eigrp_neighbor_clear_report(nbr, false, callback, arg);

	if (peer_termination)
		eigrp_hello_send(nbr->ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN_NBR,
				 &nbr->src);

	eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
	eigrp_nbr_delete(nbr);
}

static void eigrp_neighbor_clear_soft(eigrp_neighbor_t *nbr,
				      eigrp_neighbor_clear_cb callback, void *arg)
{
	eigrp_neighbor_clear_report(nbr, true, callback, arg);
	eigrp_update_send_GR(nbr, EIGRP_GR_MANUAL);
}

/*
 * Syntax:
 *   EXEC: `clear eigrp address-family <ipv4|ipv6> ... neighbors [IFNAME|ADDRESS] [soft]`
 * Supported: EXEC
 * Placement:
 *   Privileged operational
 * Description:
 * Clears selected neighbor adjacency state without changing retained configuration.
 * Selection is normalized before the portable neighbor target executes.
 */
eigrp_result_t eigrp_neighbor_clear(
	eigrp_instance_t *runtime, const eigrp_neighbor_clear_request_t *request,
	eigrp_neighbor_clear_cb callback, void *arg, size_t *affected_count)
{
	eigrp_interface_t *ei;
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *if_node;
	eigrp_list_node_t *nbr_node;
	eigrp_list_node_t *next_node;
	size_t affected = 0;

	if (affected_count)
		*affected_count = 0;
	if (!runtime || !request)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!runtime->data_path_ready)
		return EIGRP_RESULT_NOT_IMPLEMENTED;
	if (request->interface_name && request->address)
		return EIGRP_RESULT_CONFLICT;
	if (request->address
	    && !eigrp_neighbor_clear_address_valid(request->address))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (request->address) {
		for (EIGRP_LIST_ELEMENTS_RO(runtime->eiflist, if_node, ei)) {
			for (EIGRP_LIST_ELEMENTS_RO(ei->nbrs, nbr_node, nbr)) {
				if (!eigrp_neighbor_clear_address_match(nbr,
								request->address))
					continue;
				if (request->soft)
					eigrp_neighbor_clear_soft(nbr, callback, arg);
				else
					eigrp_neighbor_clear_hard(nbr, true,
							  callback, arg);
				affected = 1;
				if (affected_count)
					*affected_count = affected;
				return EIGRP_RESULT_SUCCESS;
			}
		}
		return EIGRP_RESULT_NOT_FOUND;
	}

	if (request->interface_name) {
		ei = eigrp_intf_lookup_by_name(runtime, request->interface_name);
		if (!ei)
			return EIGRP_RESULT_NOT_FOUND;

		if (!request->soft)
			eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);

		for (EIGRP_LIST_ELEMENTS(ei->nbrs, nbr_node, next_node, nbr)) {
			if (!request->soft && nbr->state == EIGRP_NEIGHBOR_DOWN)
				continue;
			if (request->soft)
				eigrp_neighbor_clear_soft(nbr, callback, arg);
			else
				eigrp_neighbor_clear_hard(nbr, false,
							  callback, arg);
			affected++;
		}
		if (affected_count)
			*affected_count = affected;
		return EIGRP_RESULT_SUCCESS;
	}

	for (EIGRP_LIST_ELEMENTS_RO(runtime->eiflist, if_node, ei)) {
		if (!request->soft)
			eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);

		for (EIGRP_LIST_ELEMENTS(ei->nbrs, nbr_node, next_node, nbr)) {
			if (!request->soft && nbr->state == EIGRP_NEIGHBOR_DOWN)
				continue;
			if (request->soft)
				eigrp_neighbor_clear_soft(nbr, callback, arg);
			else
				eigrp_neighbor_clear_hard(nbr, false,
							  callback, arg);
			affected++;
		}
	}
	if (affected_count)
		*affected_count = affected;
	return EIGRP_RESULT_SUCCESS;
}

int eigrp_nbr_split_horizon_check(eigrp_route_descriptor_t *erd,
				  eigrp_interface_t *ei)
{
	if (!ei || !ei->split_horizon || erd->distance == EIGRP_MAX_METRIC)
		return 0;

	return (erd->ei == ei);
}

static bool eigrp_neighbor_config_address_valid(const eigrp_address_t *address)
{
	return address && (address->afi == EIGRP_ADDRESS_FAMILY_IPV4
			   || address->afi == EIGRP_ADDRESS_FAMILY_IPV6);
}

static eigrp_neighbor_policy_state_t *eigrp_neighbor_policy_state_get(
	eigrp_address_family_config_t *af)
{
	if (!af)
		return NULL;
	if (!af->neighbor_policy) {
		af->neighbor_policy = calloc(1, sizeof(*af->neighbor_policy));
		if (!af->neighbor_policy)
			return NULL;
		af->neighbor_policy->log_changes = true;
		af->neighbor_policy->log_warnings = true;
		af->neighbor_policy->log_warning_interval = 10;
	}
	return af->neighbor_policy;
}

static struct eigrp_neighbor_policy_entry *eigrp_neighbor_policy_entry_find(
	eigrp_neighbor_policy_state_t *state, const eigrp_address_t *address)
{
	struct eigrp_neighbor_policy_entry *entry;

	if (!state)
		return NULL;
	for (entry = state->entries; entry; entry = entry->next)
		if (eigrp_neighbor_address_equal(&entry->address, address))
			return entry;
	return NULL;
}

static struct eigrp_neighbor_policy_entry *eigrp_neighbor_policy_entry_get(
	eigrp_address_family_config_t *af, const eigrp_address_t *address)
{
	eigrp_neighbor_policy_state_t *state;
	struct eigrp_neighbor_policy_entry *entry;

	state = eigrp_neighbor_policy_state_get(af);
	if (!state)
		return NULL;
	entry = eigrp_neighbor_policy_entry_find(state, address);
	if (entry)
		return entry;
	entry = calloc(1, sizeof(*entry));
	if (!entry)
		return NULL;
	entry->address = *address;
	entry->next = state->entries;
	state->entries = entry;
	return entry;
}

static void eigrp_neighbor_policy_entry_prune(
	eigrp_neighbor_policy_state_t *state,
	struct eigrp_neighbor_policy_entry *entry)
{
	struct eigrp_neighbor_policy_entry **cursor;

	if (!state || !entry || entry->description
	    || entry->maximum_prefix_configured)
		return;
	for (cursor = &state->entries; *cursor; cursor = &(*cursor)->next) {
		if (*cursor != entry)
			continue;
		*cursor = entry->next;
		free(entry);
		return;
	}
}

static eigrp_result_t eigrp_neighbor_policy_context_validate(
	eigrp_instance_context_t *context, const eigrp_address_t *address)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (address && context->config && address->afi != context->config->afi)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `neighbor ADDRESS description TEXT` / `no neighbor ADDRESS description [TEXT]`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Sets or removes retained descriptive text for a configured neighbor.
 * Description metadata does not own adjacency behavior.
 */
eigrp_result_t eigrp_neighbor_description_set(
	eigrp_instance_context_t *context, const eigrp_address_t *address,
	const char *description)
{
	struct eigrp_neighbor_policy_entry *entry;
	char *copy;
	eigrp_result_t result;

	if (!eigrp_neighbor_config_address_valid(address) || !description
	    || !description[0] || strlen(description) > 80)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_neighbor_policy_context_validate(context, address);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (!context->config)
		return EIGRP_RESULT_SUCCESS;

	copy = eigrp_neighbor_string_duplicate(description);
	if (!copy)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	entry = eigrp_neighbor_policy_entry_get(context->config, address);
	if (!entry) {
		free(copy);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	free(entry->description);
	entry->description = copy;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `neighbor ADDRESS description TEXT` / `no neighbor ADDRESS description [TEXT]`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Sets or removes retained descriptive text for a configured neighbor.
 * Description metadata does not own adjacency behavior.
 */
eigrp_result_t eigrp_neighbor_description_reset(
	eigrp_instance_context_t *context, const eigrp_address_t *address)
{
	eigrp_neighbor_policy_state_t *state;
	struct eigrp_neighbor_policy_entry *entry;
	eigrp_result_t result;

	if (!eigrp_neighbor_config_address_valid(address))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_neighbor_policy_context_validate(context, address);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (!context->config || !context->config->neighbor_policy)
		return EIGRP_RESULT_NOT_FOUND;
	state = context->config->neighbor_policy;
	entry = eigrp_neighbor_policy_entry_find(state, address);
	if (!entry || !entry->description)
		return EIGRP_RESULT_NOT_FOUND;
	free(entry->description);
	entry->description = NULL;
	eigrp_neighbor_policy_entry_prune(state, entry);
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `neighbor ADDRESS maximum-prefix LIMIT [...]` / `no neighbor ADDRESS maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Sets or removes a per-neighbor maximum-prefix policy.
 * The target retains the policy and reports NOT_IMPLEMENTED until enforcement is complete.
 */
eigrp_result_t eigrp_neighbor_maximum_prefix_set(
	eigrp_instance_context_t *context, const eigrp_address_t *address,
	const eigrp_prefix_limit_t *limit)
{
	struct eigrp_neighbor_policy_entry *entry;
	eigrp_result_t result;

	if (!eigrp_neighbor_config_address_valid(address) || !limit
	    || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_neighbor_policy_context_validate(context, address);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (context->config) {
		entry = eigrp_neighbor_policy_entry_get(context->config, address);
		if (!entry)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		entry->maximum_prefix = *limit;
		entry->maximum_prefix_configured = true;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `neighbor ADDRESS maximum-prefix LIMIT [...]` / `no neighbor ADDRESS maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Sets or removes a per-neighbor maximum-prefix policy.
 * The target retains the policy and reports NOT_IMPLEMENTED until enforcement is complete.
 */
eigrp_result_t eigrp_neighbor_maximum_prefix_reset(
	eigrp_instance_context_t *context, const eigrp_address_t *address)
{
	eigrp_neighbor_policy_state_t *state;
	struct eigrp_neighbor_policy_entry *entry;
	eigrp_result_t result;

	if (!eigrp_neighbor_config_address_valid(address))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	result = eigrp_neighbor_policy_context_validate(context, address);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;
	if (context->config && context->config->neighbor_policy) {
		state = context->config->neighbor_policy;
		entry = eigrp_neighbor_policy_entry_find(state, address);
		if (entry && entry->maximum_prefix_configured) {
			entry->maximum_prefix_configured = false;
			memset(&entry->maximum_prefix, 0,
			       sizeof(entry->maximum_prefix));
			eigrp_neighbor_policy_entry_prune(state, entry);
			return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
						: EIGRP_RESULT_SUCCESS;
		}
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_NOT_FOUND;
}

/*
 * Syntax:
 *   Named: `neighbor maximum-prefix LIMIT [...]` / `no neighbor maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Sets or removes the address-family default maximum-prefix policy for neighbors.
 * The target preserves configuration separately from future enforcement mechanics.
 */
eigrp_result_t eigrp_neighbor_maximum_prefix_all_set(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit)
{
	eigrp_neighbor_policy_state_t *state;

	if (!limit || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		state = eigrp_neighbor_policy_state_get(context->config);
		if (!state)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		state->maximum_prefix_all = *limit;
		state->maximum_prefix_all_configured = true;
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `neighbor maximum-prefix LIMIT [...]` / `no neighbor maximum-prefix`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Sets or removes the address-family default maximum-prefix policy for neighbors.
 * The target preserves configuration separately from future enforcement mechanics.
 */
eigrp_result_t eigrp_neighbor_maximum_prefix_all_reset(
	eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->neighbor_policy) {
		context->config->neighbor_policy->maximum_prefix_all_configured = false;
		memset(&context->config->neighbor_policy->maximum_prefix_all, 0,
		       sizeof(context->config->neighbor_policy->maximum_prefix_all));
	}
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `eigrp log-neighbor-changes`
 *   Named: `eigrp log-neighbor-warnings [INTERVAL]`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Sets neighbor logging policy selected by type.  The selector changes the
 * logging attribute, not the semantic action exposed by the public API.
 */
eigrp_result_t eigrp_neighbor_log_set(eigrp_instance_context_t *context,
				      eigrp_neighbor_log_type_t type,
				      bool enabled, uint16_t seconds)
{
	eigrp_neighbor_policy_state_t *state = NULL;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		state = eigrp_neighbor_policy_state_get(context->config);
		if (!state)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	switch (type) {
	case EIGRP_NEIGHBOR_LOG_CHANGES:
		if (state) {
			state->log_changes_configured = true;
			state->log_changes = enabled;
		}
		if (context->runtime)
			context->runtime->log_neighbor_changes = enabled;
		return EIGRP_RESULT_SUCCESS;
	case EIGRP_NEIGHBOR_LOG_WARNINGS:
		if (enabled && !seconds)
			return EIGRP_RESULT_INVALID_ARGUMENT;
		if (state) {
			state->log_warnings_configured = true;
			state->log_warnings = enabled;
			state->log_warning_interval = seconds ? seconds : 10;
		}
		if (context->runtime) {
			context->runtime->log_neighbor_warnings = enabled;
			context->runtime->log_neighbor_warning_interval =
				seconds ? seconds : 10;
		}
		/* Warning de-duplication/rate limiting is not yet wired. */
		return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
					: EIGRP_RESULT_SUCCESS;
	default:
		return EIGRP_RESULT_INVALID_ARGUMENT;
	}
}

/* Restore the selected neighbor logging attribute to its default. */
eigrp_result_t eigrp_neighbor_log_reset(eigrp_instance_context_t *context,
					eigrp_neighbor_log_type_t type)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;

	switch (type) {
	case EIGRP_NEIGHBOR_LOG_CHANGES:
		if (context->config && context->config->neighbor_policy) {
			context->config->neighbor_policy->log_changes_configured = false;
			context->config->neighbor_policy->log_changes = true;
		}
		if (context->runtime)
			context->runtime->log_neighbor_changes = true;
		return EIGRP_RESULT_SUCCESS;
	case EIGRP_NEIGHBOR_LOG_WARNINGS:
		if (context->config && context->config->neighbor_policy) {
			context->config->neighbor_policy->log_warnings_configured = false;
			context->config->neighbor_policy->log_warnings = true;
			context->config->neighbor_policy->log_warning_interval = 10;
		}
		if (context->runtime) {
			context->runtime->log_neighbor_warnings = true;
			context->runtime->log_neighbor_warning_interval = 10;
		}
		return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
					: EIGRP_RESULT_SUCCESS;
	default:
		return EIGRP_RESULT_INVALID_ARGUMENT;
	}
}

void eigrp_neighbor_policy_delete_all(eigrp_address_family_config_t *af)
{
	struct eigrp_neighbor_policy_entry *entry;
	struct eigrp_neighbor_policy_entry *next;

	if (!af || !af->neighbor_policy)
		return;
	for (entry = af->neighbor_policy->entries; entry; entry = next) {
		next = entry->next;
		free(entry->description);
		free(entry);
	}
	free(af->neighbor_policy);
	af->neighbor_policy = NULL;
}
