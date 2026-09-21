// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Daemon Program.
 * Copyright (C) 2013-2014
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include <stdlib.h>
#include <string.h>
#include "eigrpd/eigrpd.h"

#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_eventlog.h"
#include "eigrpd/eigrp_packetizer.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_tlv1.h"
#include "eigrpd/eigrp_tlv2.h"

/* Address-family vectors are bound once when the runtime/control context is
 * created.  Common protocol code treats a validated binding as an invariant.
 */
static struct eigrpd eigrpd;
struct eigrpd *eigrp_om;

static bool eigrp_af_vectors_runtime_validate(const eigrp_af_vectors_t *vectors,
				       bool data_path_ready)
{
#define EIGRP_AF_VECTOR_REQUIRE(_field)                                      \
	do {                                                                   \
		if (!(vectors->_field)) {                                        \
			eigrp_log_error(                                           \
				"address-family %u missing required vector %s",      \
				(unsigned)vectors->afi, #_field);                    \
			return false;                                              \
		}                                                              \
	} while (0)

	if (!vectors) {
		eigrp_log_error("address-family runtime has no vector binding");
		return false;
	}

	if (vectors->afi != EIGRP_ADDRESS_FAMILY_IPV4
	    && vectors->afi != EIGRP_ADDRESS_FAMILY_IPV6) {
		eigrp_log_error("address-family runtime has invalid AF %u",
				(unsigned)vectors->afi);
		return false;
	}

	EIGRP_AF_VECTOR_REQUIRE(packet_source_on_link);
	EIGRP_AF_VECTOR_REQUIRE(packet_address_bytes);
	EIGRP_AF_VECTOR_REQUIRE(packet_address_decode);
	EIGRP_AF_VECTOR_REQUIRE(packet_address_encode);
	EIGRP_AF_VECTOR_REQUIRE(packet_prefix_decode);
	EIGRP_AF_VECTOR_REQUIRE(packet_prefix_encode);
	EIGRP_AF_VECTOR_REQUIRE(classic_internal_tlv_type);
	EIGRP_AF_VECTOR_REQUIRE(classic_external_tlv_type);
	EIGRP_AF_VECTOR_REQUIRE(multiprotocol_afi);
	EIGRP_AF_VECTOR_REQUIRE(addr_snprintf);
	EIGRP_AF_VECTOR_REQUIRE(summary_auto_prefix);

	if (data_path_ready) {
		EIGRP_AF_VECTOR_REQUIRE(packet_send);
		EIGRP_AF_VECTOR_REQUIRE(packet_receive);
	}

#undef EIGRP_AF_VECTOR_REQUIRE
	return true;
}

const char *eigrp_message_lookup(const eigrp_message_t *messages, int value,
                                 const char *fallback)
{
	const eigrp_message_t *message;
	if (!messages)
		return fallback;
	for (message = messages; message->name; message++)
		if (message->value == value)
			return message->name;
	return fallback;
}


/*
 * void eigrp_router_id_update(eigrp_instance_t *eigrp)
 *
 * Description:
 * update routerid associated with this instance of EIGRP.
 * If the id changes, then call if_update for each interface
 * to resync the topology database with all neighbors
 *
 * Select the router ID based on these priorities:
 *   1. Statically assigned router ID is always the first choice.
 *   2. If there is no statically assigned router ID, then try to stick
 *      with the most recent value, since changing router ID's is very
 *      disruptive.
 *   3. Last choice: just go with whatever the zebra daemon recommends.
 *
 * Note:
 * router id for EIGRP is really just a 32 bit number. Cisco historically
 * displays it in dotted decimal notation, and will pickup an IP address
 * from an interface so it can be 'auto-configed" to a uniqe value
 *
 * This does not work for IPv6, and to make the code simpler, its
 * stored and processed internerall as a 32bit number
 */
void eigrp_router_id_update(eigrp_instance_t *eigrp)
{
	struct in_addr router_id = {.s_addr = INADDR_ANY};
	struct in_addr router_id_old;

	if (!eigrp)
		return;

	router_id_old = eigrp->router_id;

	if (eigrp->router_id_static.s_addr != INADDR_ANY)
		router_id = eigrp->router_id_static;
	else if (eigrp->router_id.s_addr != INADDR_ANY)
		router_id = eigrp->router_id;
	else
		(void)eigrp_southbound_router_id_get(eigrp, &router_id);

	eigrp->router_id = router_id;
	if (router_id_old.s_addr != router_id.s_addr)
		eigrp_network_interfaces_refresh(eigrp);
}

void eigrp_init(void)
{
	struct timespec ts;

	memset(&eigrpd, 0, sizeof(struct eigrpd));

	eigrp_om = &eigrpd;
	eigrp_om->eigrp = eigrp_list_new();

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		eigrp_om->start_time = ts.tv_sec;
	else
		eigrp_om->start_time = 0;
}

/* Allocate a protocol runtime/control context. */
static eigrp_instance_t *eigrp_new(eigrp_address_family_t afi, uint16_t as,
				   eigrp_vrf_id_t vrf_id, bool data_path_ready)
{
	eigrp_instance_t *eigrp = calloc(1, sizeof(struct eigrp_instance));
	eigrp_addr_t src = {0};

	if (!eigrp) {
		eigrp_log_error("address-family %u AS %u runtime allocation failed",
				(unsigned)afi, (unsigned)as);
		return NULL;
	}

	/* Initialize address-family-independent control state first. */
	eigrp->vrf_id = vrf_id;
	eigrp->data_path_ready = data_path_ready;
	switch (afi) {
	case EIGRP_ADDRESS_FAMILY_IPV4:
		eigrp_ipv4_init(&eigrp->af_vectors);
		break;
	case EIGRP_ADDRESS_FAMILY_IPV6:
		eigrp_ipv6_init(&eigrp->af_vectors);
		break;
	default:
		free(eigrp);
		return NULL;
	}
	if (!eigrp_af_vectors_runtime_validate(&eigrp->af_vectors,
					       data_path_ready)) {
		free(eigrp);
		return NULL;
	}
	eigrp->vrid = 0;
	eigrp->AS = as;
	eigrp->router_id.s_addr = INADDR_ANY;
	eigrp->router_id_static.s_addr = INADDR_ANY;
	eigrp->sequence_number = 1;
	eigrp->fd = -1;

	/* Configure default K values for the control context. */
	eigrp->k_values[0] = EIGRP_K1_DEFAULT;
	eigrp->k_values[1] = EIGRP_K2_DEFAULT;
	eigrp->k_values[2] = EIGRP_K3_DEFAULT;
	eigrp->k_values[3] = EIGRP_K4_DEFAULT;
	eigrp->k_values[4] = EIGRP_K5_DEFAULT;
	eigrp->k_values[5] = EIGRP_K6_DEFAULT;

	eigrp_tlv1_init(&eigrp->tlv1_codec);
	eigrp_tlv2_init(&eigrp->tlv2_codec);

	/* Control/runtime state exists for both IPv4 and IPv6 named AFs. */
	eigrp->eiflist = eigrp_list_new();
	eigrp->passive_interface_default = EIGRP_INTF_ACTIVE;
	eigrp->networks = NULL;
	eigrp->oi_write_q = eigrp_list_new();
	eigrp->topology_table = eigrp_topology_table_create();
	eigrp->variance = EIGRP_VARIANCE_DEFAULT;
	eigrp->max_paths = EIGRP_MAX_PATHS_DEFAULT;
	eigrp->max_hops = EIGRP_MAX_HOPS;
	eigrp->log_neighbor_changes = true;
	eigrp->log_neighbor_warnings = true;
	eigrp->log_neighbor_warning_interval = 10;
	eigrp->topology_changes = eigrp_list_new();

	/* Diagnostic/control state is valid before a packet data path exists. */
	(void)eigrp_eventlog_init(eigrp, EIGRP_EVENTLOG_DEFAULT_SIZE);
	(void)eigrp_southbound_policy_instance_create(eigrp);

	if (!data_path_ready)
		return eigrp;

	if (eigrp_southbound_socket_open(eigrp) != EIGRP_RESULT_SUCCESS) {
		eigrp_log_error(
			"eigrp_new: fatal error: host runtime was unable to open an EIGRP socket");
		exit(1);
	}

	eigrp->ibuf = eigrp_stream_new(EIGRP_PACKET_MAX_LEN + 1);
	eigrp_southbound_read_add(&eigrp->t_read, eigrp->fd,
				   eigrp_packet_read, eigrp);

	/* The self-neighbor is wire/data-path state and is created only there. */
	src.afi = afi == EIGRP_ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
	eigrp->neighbor_self = eigrp_nbr_create(NULL, &src);
	eigrp_packetizer_init(eigrp);
	return eigrp;
}

/*
 * Legacy classic callers are IPv4.  Keep these lookups IPv4-only so adding a
 * named IPv6 control runtime cannot redirect Zebra/classic code to IPv6.
 */
eigrp_instance_t *eigrp_lookup(eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp;
	eigrp_list_node_t *node, *nnode;

	for (EIGRP_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->af_vectors.afi == EIGRP_ADDRESS_FAMILY_IPV4
		    && eigrp->vrf_id == vrf_id)
			return eigrp;
	}
	return NULL;
}

eigrp_instance_t *eigrp_lookup_by_af_as_vrf(eigrp_address_family_t afi,
					     uint16_t as,
					     eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp;
	eigrp_list_node_t *node, *nnode;

	for (EIGRP_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->af_vectors.afi == afi && eigrp->AS == as
		    && eigrp->vrf_id == vrf_id)
			return eigrp;
	}
	return NULL;
}

eigrp_instance_t *eigrp_lookup_by_as_vrf(uint16_t as, eigrp_vrf_id_t vrf_id)
{
	return eigrp_lookup_by_af_as_vrf(EIGRP_ADDRESS_FAMILY_IPV4, as, vrf_id);
}

eigrp_instance_t *eigrp_get_by_af(eigrp_address_family_t afi, uint16_t as,
				   eigrp_vrf_id_t vrf_id, bool data_path_ready)
{
	eigrp_instance_t *eigrp;

	eigrp = eigrp_lookup_by_af_as_vrf(afi, as, vrf_id);
	if (eigrp == NULL) {
		eigrp = eigrp_new(afi, as, vrf_id, data_path_ready);
		if (!eigrp)
			return NULL;
		eigrp_list_add(eigrp_om->eigrp, eigrp);
	}
	return eigrp;
}

eigrp_instance_t *eigrp_get(uint16_t as, eigrp_vrf_id_t vrf_id)
{
	return eigrp_get_by_af(EIGRP_ADDRESS_FAMILY_IPV4, as, vrf_id, true);
}

void eigrp_name_set(eigrp_instance_t *eigrp, const char *name)
{
	if (!eigrp || !name || !name[0])
		return;

	if (eigrp->name && strcmp(eigrp->name, name) == 0)
		return;

	if (eigrp->name)
		free(eigrp->name);

	eigrp->name = strdup(name);
}

/* Shut down the entire process */
void eigrp_terminate(void)
{
	eigrp_instance_t *eigrp;

	/* shutdown already in progress */
	if ((eigrp_om->options & EIGRPD_SHUTDOWN) != 0)
		return;

	eigrp_om->options |= EIGRPD_SHUTDOWN;

	while (eigrp_om->eigrp && eigrp_om->eigrp->count) {
		eigrp = eigrp_list_node_data(eigrp_list_head(eigrp_om->eigrp));
		eigrp_finish(eigrp);
	}

	eigrp_instance_config_finish();
	eigrp_southbound_rib_finish();
	eigrp_southbound_runtime_finish();
}

void eigrp_finish(eigrp_instance_t *eigrp)
{
	eigrp_finish_final(eigrp);

	/* eigrp being shut-down? If so, was this the last eigrp instance? */
	if ((eigrp_om->options & EIGRPD_SHUTDOWN) != 0
	    && (eigrp_om->eigrp == NULL || eigrp_om->eigrp->count == 0))
		return;

	return;
}

/* Final cleanup of eigrp instance */
void eigrp_finish_final(eigrp_instance_t *eigrp)
{
	eigrp_interface_t *ei;
	eigrp_neighbor_t *nbr;
	eigrp_list_node_t *node, *nnode, *node2, *nnode2;

	/* Named address-family configuration owns its runtime binding.  Clear
	 * that binding before any runtime storage is released so later config
	 * cleanup cannot dereference or attempt to destroy a stale instance.
	 */
	eigrp_instance_runtime_unbind(eigrp);

	for (EIGRP_LIST_ELEMENTS(eigrp->eiflist, node, nnode, ei)) {
		for (EIGRP_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr))
			eigrp_nbr_delete(nbr);
		eigrp_intf_free(eigrp, ei, EIGRP_INTERFACE_REMOVE_FINAL);
	}

	eigrp_network_runtime_delete_all(eigrp);
	eigrp_southbound_event_cancel(&eigrp->t_write);
	eigrp_southbound_event_cancel(&eigrp->t_read);
	eigrp_packetizer_finish(eigrp);
	eigrp_eventlog_finish(eigrp);
	eigrp_southbound_socket_close(eigrp);

	eigrp_list_delete(&eigrp->eiflist);
	eigrp_list_delete(&eigrp->oi_write_q);

	eigrp_topology_table_delete(eigrp, eigrp->topology_table);
	if (eigrp->neighbor_self)
		eigrp_nbr_delete(eigrp->neighbor_self);

	eigrp_list_delete(&eigrp->topology_changes);
	eigrp_list_delete_data(eigrp_om->eigrp, eigrp);

	if (eigrp->name)
		free(eigrp->name);

	if (eigrp->ibuf)
		eigrp_stream_free(eigrp->ibuf);
	eigrp_southbound_policy_instance_delete(eigrp);
	eigrp_southbound_rib_instance_delete(eigrp);
	eigrp_filter_runtime_state_clear(&eigrp->filter);
	free(eigrp);
}
