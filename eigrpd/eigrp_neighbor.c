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
#include "lib/table.h"

#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_dump.h"

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
		strlcpy(address_text, "<invalid>", sizeof(address_text));
	zlog_debug("EIGRP: %s static neighbor %s AS %u interface %s", action,
		   address_text, af ? af->asn : 0,
		   interface_name ? interface_name : "-");
}

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
	struct route_node *route_node;
	struct listnode *list_node;
	uint32_t count = 0;

	if (!runtime || !runtime->topology_table || !neighbor)
		return 0;
	for (route_node = route_top(runtime->topology_table); route_node;
	     route_node = route_next(route_node)) {
		prefix = route_node->info;
		if (!prefix)
			continue;
		for (ALL_LIST_ELEMENTS_RO(prefix->entries, list_node, route)) {
			if (route->adv_router == neighbor) {
				count++;
				break;
			}
		}
	}
	return count;
}

eigrp_result_t eigrp_neighbor_state_walk(
	eigrp_address_family_config_t *config, eigrp_instance_t *runtime,
	const char *interface_name, bool static_only,
	eigrp_neighbor_state_walk_cb callback, void *arg)
{
	eigrp_neighbor_config_t *configured;
	eigrp_interface_t *ei;
	eigrp_neighbor_t *nbr;
	struct listnode *if_node;
	struct listnode *nbr_node;
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

	for (ALL_LIST_ELEMENTS_RO(runtime->eiflist, if_node, ei)) {
		const char *name = eigrp_intf_name_string(ei);

		if (interface_name && strcmp(name, interface_name) != 0)
			continue;
		for (ALL_LIST_ELEMENTS_RO(ei->nbrs, nbr_node, nbr)) {
			/* Normal neighbor output contains established adjacencies only. */
			if (nbr->state != EIGRP_NEIGHBOR_UP)
				continue;
			memset(&state, 0, sizeof(state));
			eigrp_neighbor_runtime_address(nbr, &state.address);
			state.interface_name = name;
			state.state_name = eigrp_nbr_state_str(nbr);
			state.runtime_present = true;
			state.hold_time = (uint16_t)eigrp_southbound_timer_remaining_seconds(
				nbr->t_holddown);
			if (nbr->up_since_msec) {
				uint64_t now = eigrp_southbound_monotime_msec();

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
			state.srtt_valid = false;
			state.rto_msec = EIGRP_PACKET_RETRANS_TIME * 1000U;
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

DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_NEIGHBOR, "EIGRP neighbor");


void eigrp_neighbor_encoder_bind(eigrp_neighbor_t *nbr, eigrp_tlv_codec_t *codec)
{
	if (!nbr || !codec || !codec->encoder)
		return;

	nbr->encoder = codec->encoder;
}

void eigrp_neighbor_decoder_bind(eigrp_neighbor_t *nbr, eigrp_tlv_codec_t *codec)
{
	if (!nbr || !codec || !codec->decoder)
		return;

	nbr->decoder = codec->decoder;
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
	//    zlog_debug("NSM[%s:%s]: start", EIGRP_INTF_NAME (nbr->oi),
	//               eigrp_print_routerid(nbr->router_id));
}

/**
 * Create a new neighbor structure and initalize it.
 */
eigrp_neighbor_t *eigrp_nbr_create(eigrp_interface_t *ei, eigrp_addr_t *src)
{
	eigrp_neighbor_t *nbr;

	/* Allcate new neighbor. */
	nbr = XCALLOC(MTYPE_EIGRP_NEIGHBOR, sizeof(eigrp_neighbor_t));

	/* Relate neighbor to the interface. */
	nbr->ei = ei;

	/* Set default values. */
	eigrp_nbr_init(nbr, src);
	eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);

	// If this is the 'self' neighbor, then you dont have an interface
	if (ei) {
		listnode_add(ei->nbrs, nbr);
	}
	if (IS_DEBUG_EIGRP_EVENT) {
		zlog_debug("EIGRP event: neighbor %s created%s%s",
			   eigrp_print_addr(&nbr->src),
			   ei ? " on " : "", ei ? EIGRP_INTF_NAME(ei) : "");
		if (IS_DEBUG_EIGRP(0, DETAIL) && ei && ei->eigrp)
			zlog_debug("EIGRP event detail: AS %u hold %u state %u",
				   ei->eigrp->AS, nbr->v_holddown, nbr->state);
	}
	return nbr;
}

eigrp_neighbor_t *eigrp_nbr_lookup(eigrp_interface_t *ei, eigrp_header_t *eigrph,
				   eigrp_addr_t *src)
{
	eigrp_neighbor_t *nbr;
	struct listnode *node, *nnode;

	for (ALL_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
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
	struct listnode *node, *nnode;

	for (ALL_LIST_ELEMENTS(ei->nbrs, node, nnode, nbr)) {
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
	struct listnode *node, *node2, *nnode2;
	eigrp_neighbor_t *nbr;

	/* iterate over all eigrp interfaces */
	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, ei)) {
		/* iterate over all neighbors on eigrp interface */
		for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr)) {
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
		zlog_debug("EIGRP event: neighbor %s delete%s%s",
			   eigrp_print_addr(&nbr->src), nbr->ei ? " on " : "",
			   nbr->ei ? EIGRP_INTF_NAME(nbr->ei) : "");
		if (IS_DEBUG_EIGRP(0, DETAIL) && nbr->ei && nbr->ei->eigrp)
			zlog_debug("EIGRP event detail: AS %u state %u retrans %u",
				   nbr->ei->eigrp->AS, nbr->state, nbr->retrans_counter);
	}
	eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
	if (nbr->ei)
		eigrp_topology_neighbor_down(nbr->ei->eigrp, nbr);

	/* Cancel neighbor-owned host runtime events before releasing queues/state. */
	eigrp_southbound_event_cancel(&nbr->t_nbr_send_gr);
	eigrp_packet_queue_free(nbr->multicast_queue);
	eigrp_packet_queue_free(nbr->retrans_queue);
	eigrp_southbound_event_cancel(&nbr->t_holddown);

	if (nbr->ei)
		listnode_delete(nbr->ei->nbrs, nbr);
	XFREE(MTYPE_EIGRP_NEIGHBOR, nbr);
}

void eigrp_neighbor_holddown_expired(void *arg)
{
	eigrp_neighbor_t *nbr = arg;
	if (IS_DEBUG_EIGRP(0, TIMERS))
		zlog_debug("EIGRP: hold timer expired for neighbor %s",
			   eigrp_print_addr(&nbr->src));
	if (nbr->ei->eigrp->log_neighbor_changes)
		zlog_info("Neighbor %s (%s) is down: holding time expired",
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
		nbr->up_since_msec = eigrp_southbound_monotime_msec();
		nbr->retransmissions = 0;
	} else if (old_state == EIGRP_NEIGHBOR_UP && state != EIGRP_NEIGHBOR_UP) {
		nbr->up_since_msec = 0;
	}

	if (eigrp_nbr_state_get(nbr) == EIGRP_NEIGHBOR_DOWN) {
		// reset all the seq/ack counters
		nbr->recv_sequence_number = 0;
		nbr->init_sequence_number = 0;
		nbr->retrans_counter = 0;

		// Kvalues
		nbr->K1 = EIGRP_K1_DEFAULT;
		nbr->K2 = EIGRP_K2_DEFAULT;
		nbr->K3 = EIGRP_K3_DEFAULT;
		nbr->K4 = EIGRP_K4_DEFAULT;
		nbr->K5 = EIGRP_K5_DEFAULT;
		nbr->K6 = EIGRP_K6_DEFAULT;

		// hold time..
		nbr->v_holddown = EIGRP_HOLD_INTERVAL_DEFAULT;
		eigrp_southbound_event_cancel(&nbr->t_holddown);

		/* out with the old */
		if (nbr->multicast_queue)
			eigrp_packet_queue_free(nbr->multicast_queue);
		if (nbr->retrans_queue)
			eigrp_packet_queue_free(nbr->retrans_queue);

		/* in with the new */
		nbr->retrans_queue = eigrp_packet_queue_new();
		nbr->multicast_queue = eigrp_packet_queue_new();

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
		eigrp_southbound_event_cancel(&nbr->t_holddown);
		break;
	case EIGRP_NEIGHBOR_PENDING: {
		/*Reset Hold Down Timer for neighbor*/
		eigrp_southbound_event_cancel(&nbr->t_holddown);
		eigrp_southbound_timer_add(&nbr->t_holddown,
				  eigrp_neighbor_holddown_expired, nbr,
				  nbr->v_holddown);
		break;
	}
	case EIGRP_NEIGHBOR_UP: {
		/*Reset Hold Down Timer for neighbor*/
		eigrp_southbound_event_cancel(&nbr->t_holddown);
		eigrp_southbound_timer_add(&nbr->t_holddown,
				  eigrp_neighbor_holddown_expired, nbr,
				  nbr->v_holddown);
		break;
	}
	}
}

int eigrp_nbr_count_get(eigrp_instance_t *eigrp)
{
	eigrp_interface_t *iface;
	struct listnode *node, *node2, *nnode2;
	eigrp_neighbor_t *nbr;
	uint32_t counter;

	counter = 0;
	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, node, iface)) {
		for (ALL_LIST_ELEMENTS(iface->nbrs, node2, nnode2, nbr)) {
			if (nbr->state == EIGRP_NEIGHBOR_UP) {
				counter++;
			}
		}
	}
	return counter;
}

static bool eigrp_neighbor_clear_address_valid(const eigrp_addr_t *address)
{
	return address && (address->afi == AF_INET || address->afi == AF_INET6);
}

static bool eigrp_neighbor_clear_address_match(const eigrp_neighbor_t *nbr,
					       const eigrp_addr_t *address)
{
	if (!nbr || !address || nbr->src.afi != address->afi)
		return false;

	if (address->afi == AF_INET6)
		return memcmp(&nbr->src.ip.v6, &address->ip.v6,
			      sizeof(address->ip.v6)) == 0;
	if (address->afi == AF_INET)
		return memcmp(&nbr->src.ip.v4, &address->ip.v4,
			      sizeof(address->ip.v4)) == 0;
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
	state.address = nbr->src;
	state.interface_name = nbr->ei ? eigrp_intf_name_string(nbr->ei) : NULL;
	state.soft = soft;
	callback(&state, arg);
}

static void eigrp_neighbor_clear_hard(eigrp_neighbor_t *nbr,
				      bool peer_termination,
				      eigrp_neighbor_clear_cb callback, void *arg)
{
	const char *interface_name = nbr->ei ? eigrp_intf_name_string(nbr->ei) : "?";

	zlog_debug("Neighbor %s (%s) is down: manually cleared",
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

eigrp_result_t eigrp_neighbor_clear(
	eigrp_instance_t *runtime, const eigrp_neighbor_clear_request_t *request,
	eigrp_neighbor_clear_cb callback, void *arg, size_t *affected_count)
{
	eigrp_interface_t *ei;
	eigrp_neighbor_t *nbr;
	struct listnode *if_node;
	struct listnode *nbr_node;
	struct listnode *next_node;
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
		for (ALL_LIST_ELEMENTS_RO(runtime->eiflist, if_node, ei)) {
			for (ALL_LIST_ELEMENTS_RO(ei->nbrs, nbr_node, nbr)) {
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

		for (ALL_LIST_ELEMENTS(ei->nbrs, nbr_node, next_node, nbr)) {
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

	for (ALL_LIST_ELEMENTS_RO(runtime->eiflist, if_node, ei)) {
		if (!request->soft)
			eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);

		for (ALL_LIST_ELEMENTS(ei->nbrs, nbr_node, next_node, nbr)) {
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

eigrp_result_t eigrp_neighbor_description_update(
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

eigrp_result_t eigrp_neighbor_description_delete(
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

eigrp_result_t eigrp_neighbor_maximum_prefix_update(
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

eigrp_result_t eigrp_neighbor_maximum_prefix_delete(
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

eigrp_result_t eigrp_neighbor_maximum_prefix_all_update(
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

eigrp_result_t eigrp_neighbor_maximum_prefix_all_delete(
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

eigrp_result_t eigrp_neighbor_log_changes_update(
	eigrp_instance_context_t *context, bool enabled)
{
	eigrp_neighbor_policy_state_t *state;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		state = eigrp_neighbor_policy_state_get(context->config);
		if (!state)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		state->log_changes_configured = true;
		state->log_changes = enabled;
	}
	if (context->runtime)
		context->runtime->log_neighbor_changes = enabled;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_neighbor_log_changes_reset(eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config && context->config->neighbor_policy) {
		context->config->neighbor_policy->log_changes_configured = false;
		context->config->neighbor_policy->log_changes = true;
	}
	if (context->runtime)
		context->runtime->log_neighbor_changes = true;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_neighbor_log_warnings_update(
	eigrp_instance_context_t *context, bool enabled, uint16_t seconds)
{
	eigrp_neighbor_policy_state_t *state;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (enabled && !seconds)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (context->config) {
		state = eigrp_neighbor_policy_state_get(context->config);
		if (!state)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		state->log_warnings_configured = true;
		state->log_warnings = enabled;
		state->log_warning_interval = seconds ? seconds : 10;
	}
	if (context->runtime) {
		context->runtime->log_neighbor_warnings = enabled;
		context->runtime->log_neighbor_warning_interval =
			seconds ? seconds : 10;
	}
	/* Warning de-duplication/rate limiting is not yet in the runtime path. */
	return context->runtime ? EIGRP_RESULT_NOT_IMPLEMENTED
				: EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_neighbor_log_warnings_delete(
	eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
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
