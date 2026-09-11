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
#include "eigrpd/eigrpd.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_zebra.h"

struct eigrp_neighbor_config {
	eigrp_address_t address;
	char *interface_name;
	eigrp_neighbor_config_t *next;
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
		free(neighbor->interface_name);
		free(neighbor);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
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
	eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);
	if (nbr->ei)
		eigrp_topology_neighbor_down(nbr->ei->eigrp, nbr);

	/* Cancel all events. */ /* Event lookup cost would be negligible. */
	event_cancel_event(eigrpd_event, nbr);
	eigrp_packet_queue_free(nbr->multicast_queue);
	eigrp_packet_queue_free(nbr->retrans_queue);
	event_cancel(&nbr->t_holddown);

	if (nbr->ei)
		listnode_delete(nbr->ei->nbrs, nbr);
	XFREE(MTYPE_EIGRP_NEIGHBOR, nbr);
}

void holddown_timer_expired(struct event *event)
{
	eigrp_neighbor_t *nbr = EVENT_ARG(event);
	eigrp_instance_t *eigrp = nbr->ei->eigrp;

	zlog_info("Neighbor %s (%s) is down: holding time expired",
		  eigrp_print_addr(&nbr->src),
		  ifindex2ifname(nbr->ei->ifp->ifindex, eigrp->vrf_id));
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

	if (state == EIGRP_NEIGHBOR_UP && old_state != EIGRP_NEIGHBOR_UP)
		eigrp_interface_encoder_bind(nbr->ei, nbr->tlv_version);

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
		event_cancel(&nbr->t_holddown);

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
	case EIGRP_NEIGHBOR_DOWN: {
		/*Start Hold Down Timer for neighbor*/
		//     event_cancel(&nbr->t_holddown);
		//     EVENT_TIMER_ON(eigrpd_event, nbr->t_holddown,
		//     holddown_timer_expired,
		//     nbr, nbr->v_holddown);
		break;
	}
	case EIGRP_NEIGHBOR_PENDING: {
		/*Reset Hold Down Timer for neighbor*/
		event_cancel(&nbr->t_holddown);
		event_add_timer(eigrpd_event, holddown_timer_expired, nbr,
				 nbr->v_holddown, &nbr->t_holddown);
		break;
	}
	case EIGRP_NEIGHBOR_UP: {
		/*Reset Hold Down Timer for neighbor*/
		event_cancel(&nbr->t_holddown);
		event_add_timer(eigrpd_event, holddown_timer_expired, nbr,
				 nbr->v_holddown, &nbr->t_holddown);
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

/**
 * @fn eigrp_nbr_hard_restart
 *
 * @param[in]		nbr	Neighbor who would receive hard restart
 * @param[in]		vty Virtual terminal for log output
 * @return void
 *
 * @par
 * Function used for executing hard restart for neighbor:
 * Send Hello packet with Peer Termination TLV with
 * neighbor's address, set it's state to DOWN and delete the neighbor
 */
void eigrp_nbr_hard_restart(eigrp_instance_t *eigrp, eigrp_neighbor_t *nbr,
			    struct vty *vty)
{
	zlog_debug("Neighbor %s (%s) is down: manually cleared",
		   eigrp_print_addr(&nbr->src),
		   ifindex2ifname(nbr->ei->ifp->ifindex, eigrp->vrf_id));
	if (vty != NULL) {
		vty_time_print(vty, 0);
		vty_out(vty, "Neighbor %s (%s) is down: manually cleared\n",
			eigrp_print_addr(&nbr->src),
			ifindex2ifname(nbr->ei->ifp->ifindex, eigrp->vrf_id));
	}

	/* send Hello with Peer Termination TLV */
	eigrp_hello_send(nbr->ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN_NBR, &nbr->src);

	/* set neighbor to DOWN */
	eigrp_nbr_state_set(nbr, EIGRP_NEIGHBOR_DOWN);

	/* delete neighbor */
	eigrp_nbr_delete(nbr);
}

int eigrp_nbr_split_horizon_check(eigrp_route_descriptor_t *erd,
				  eigrp_interface_t *ei)
{
	if (erd->distance == EIGRP_MAX_METRIC)
		return 0;

	return (erd->ei == ei);
}

static bool eigrp_neighbor_config_address_valid(const eigrp_address_t *address)
{
	return address && (address->afi == EIGRP_ADDRESS_FAMILY_IPV4
			   || address->afi == EIGRP_ADDRESS_FAMILY_IPV6);
}

eigrp_result_t eigrp_neighbor_description_update(
	eigrp_instance_context_t *context, const eigrp_address_t *address,
	const char *description)
{
	if (!eigrp_neighbor_config_address_valid(address) || !description
	    || !description[0] || strlen(description) > 80)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_neighbor_description_delete(
	eigrp_instance_context_t *context, const eigrp_address_t *address)
{
	if (!eigrp_neighbor_config_address_valid(address))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_neighbor_maximum_prefix_update(
	eigrp_instance_context_t *context, const eigrp_address_t *address,
	const eigrp_prefix_limit_t *limit)
{
	if (!eigrp_neighbor_config_address_valid(address) || !limit
	    || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_neighbor_maximum_prefix_delete(
	eigrp_instance_context_t *context, const eigrp_address_t *address)
{
	if (!eigrp_neighbor_config_address_valid(address))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_neighbor_maximum_prefix_all_update(
	eigrp_instance_context_t *context, const eigrp_prefix_limit_t *limit)
{
	if (!limit || !limit->maximum || limit->threshold > 100)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_neighbor_maximum_prefix_all_delete(
	eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_neighbor_log_changes_update(
	eigrp_instance_context_t *context, bool enabled)
{
	(void)enabled;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_neighbor_log_warnings_update(
	eigrp_instance_context_t *context, bool enabled, uint16_t seconds)
{
	(void)enabled;
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (enabled && !seconds)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}

eigrp_result_t eigrp_neighbor_log_warnings_delete(
	eigrp_instance_context_t *context)
{
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	return EIGRP_RESULT_NOT_IMPLEMENTED;
}
