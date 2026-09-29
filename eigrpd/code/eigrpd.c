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
#include "eigrpd.h"

#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_packet.h"
#include "eigrp_network.h"
#include "eigrp_instance.h"
#include "eigrp_topology.h"
#include "eigrp_filter.h"
#include "eigrp_metric.h"
#include "eigrp_eventlog.h"
#include "eigrp_packetizer.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
#include "eigrp_tlv1.h"
#include "eigrp_tlv2.h"

/* Address-family vectors are bound once when the runtime/control context is
 * created.  Common protocol code treats a validated binding as an invariant.
 */
static struct eigrpd eigrpd;
struct eigrpd *eigrp_om;

static bool eigrp_af_vectors_runtime_validate(const eigrp_af_vectors_t *vectors)
{
#define EIGRP_AF_VECTOR_REQUIRE(_field)                                      \
	do {                                                                   \
		if (!(vectors->_field)) {                                        \
			eigrp_log(EIGRP_LOG_ERROR,                                            \
				"address-family %u missing required vector %s",      \
				(unsigned)vectors->afi, #_field);                    \
			return false;                                              \
		}                                                              \
	} while (0)

	if (!vectors) {
		eigrp_log(EIGRP_LOG_ERROR, "address-family runtime has no vector binding");
		return false;
	}

	if (vectors->afi != EIGRP_AFI_IPV4
	    && vectors->afi != EIGRP_AFI_IPV6) {
		eigrp_log(EIGRP_LOG_ERROR, "address-family runtime has invalid AF %u",
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

	EIGRP_AF_VECTOR_REQUIRE(packet_send);
	EIGRP_AF_VECTOR_REQUIRE(packet_receive);

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
 *   3. Last choice: select from normalized host interface state.
 *
 * Note:
 * router id for EIGRP is really just a 32 bit number. Cisco historically
 * displays it in dotted decimal notation, and will pickup an IP address
 * from an interface so it can be 'auto-configed" to a uniqe value
 *
 * IPv6 EIGRP uses the same 32-bit router ID even though its protocol
 * interfaces and neighbors use IPv6 addresses.  It is stored internally
 * as a 32-bit IPv4-format value.
 */
struct eigrp_router_id_select {
	eigrp_ifindex_t loopback_ifindex;
	eigrp_ifindex_t physical_ifindex;
	uint32_t loopback;
	uint32_t physical;
};

static void eigrp_router_id_ipv4_candidate_read(
	const eigrp_intf_runtime_state_t *state, void *arg)
{
	struct eigrp_router_id_select *selection = arg;
	struct in_addr candidate;
	char address[INET_ADDRSTRLEN] = {0};
	uint32_t host_address;

	if (!state || !selection || state->address.address.afi != EIGRP_AFI_IPV4)
		return;

	memcpy(&candidate.s_addr, state->address.address.bytes,
	       sizeof(candidate.s_addr));
	(void)inet_ntop(AF_INET, &candidate, address, sizeof(address));
	eigrp_log(EIGRP_LOG_DEBUG,
		  "EIGRP router-id auto candidate: interface %s[%u] address %s type %u operative %u secondary %u",
		  state->interface_name ? state->interface_name : "?", state->ifindex,
		  address[0] ? address : "?", state->type, state->operative,
		  state->secondary);

	if (state->secondary || !state->operative)
		return;

	host_address = ntohl(candidate.s_addr);
	if (!host_address || host_address == UINT32_MAX)
		return;

	/* Cisco EIGRP router-ID selection prefers an IPv4 loopback.  Otherwise
	 * retain the first operative physical interface in host interface order.
	 * The selected 32-bit ID is independent of the address-family carried by
	 * EIGRP; IPv6 EIGRP therefore does not require the IPv4 candidate itself
	 * to belong to an IPv6 EIGRP runtime interface. */
	if (state->type == EIGRP_IFTYPE_LOOPBACK) {
		if (!selection->loopback_ifindex
		    || state->ifindex < selection->loopback_ifindex) {
			selection->loopback_ifindex = state->ifindex;
			selection->loopback = host_address;
		}
		return;
	}

	if (!selection->physical_ifindex
	    || state->ifindex < selection->physical_ifindex) {
		selection->physical_ifindex = state->ifindex;
		selection->physical = host_address;
	}
}

static bool eigrp_router_id_auto_select(eigrp_instance_t *eigrp,
					uint32_t *router_id)
{
	struct eigrp_router_id_select selection = {0};
	eigrp_result_t result;
	struct in_addr selected = {.s_addr = INADDR_ANY};
	char address[INET_ADDRSTRLEN] = {0};

	if (!eigrp || !router_id)
		return false;

	result = eigrp_sys_interface_walk(
		eigrp, eigrp_router_id_ipv4_candidate_read, &selection);
	if (result != EIGRP_RESULT_SUCCESS) {
		eigrp_log(EIGRP_LOG_DEBUG,
			  "EIGRP router-id auto selection: interface walk failed for AS(%u), result %u",
			  eigrp->AS, (unsigned)result);
		return false;
	}

	*router_id = selection.loopback ? selection.loopback : selection.physical;
	if (!*router_id) {
		eigrp_log(EIGRP_LOG_DEBUG,
			  "EIGRP router-id auto selection: no eligible IPv4 address for AS(%u)",
			  eigrp->AS);
		return false;
	}

	selected.s_addr = htonl(*router_id);
	(void)inet_ntop(AF_INET, &selected, address, sizeof(address));
	eigrp_log(EIGRP_LOG_DEBUG,
		  "EIGRP router-id auto selection: selected %s for AS(%u)",
		  address[0] ? address : "?", eigrp->AS);
	return true;
}

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
	else {
		uint32_t selected_router_id = 0;

		if (eigrp_router_id_auto_select(eigrp, &selected_router_id))
			router_id.s_addr = htonl(selected_router_id);
	}

	if (router_id.s_addr == INADDR_ANY
	    && eigrp_instance_afi(eigrp) == EIGRP_AFI_IPV6) {
		if (!eigrp->router_id_missing_event_logged) {
			(void)eigrp_eventlog_msg_add(
				eigrp, EIGRP_EVENTLOG_OPCODE_IPV6_NO_ROUTER_ID, NULL,
				eigrp->AS, 0, 0, 0);
			eigrp->router_id_missing_event_logged = true;
		}
	} else {
		eigrp->router_id_missing_event_logged = false;
	}

	eigrp->router_id = router_id;
	if (router_id_old.s_addr != router_id.s_addr)
		eigrp_network_intfs_update(eigrp);
}

eigrp_vrf_id_t eigrp_instance_vrf_id(const eigrp_instance_t *eigrp)
{
	return eigrp ? eigrp->vrf_id : EIGRP_VRF_DEFAULT;
}

eigrp_afi_t eigrp_instance_afi(const eigrp_instance_t *eigrp)
{
	return eigrp ? eigrp->af_vectors.afi : 0;
}

uint16_t eigrp_instance_asn(const eigrp_instance_t *eigrp)
{
	return eigrp ? eigrp->AS : 0;
}

const char *eigrp_instance_name(const eigrp_instance_t *eigrp)
{
	return eigrp ? eigrp->name : NULL;
}

void eigrp_init(void)
{
	struct timespec ts;

	memset(&eigrpd, 0, sizeof(struct eigrpd));

	eigrp_om = &eigrpd;
	eigrp_om->eigrp = eigrp_list_create();

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		eigrp_om->start_time = ts.tv_sec;
	else
		eigrp_om->start_time = 0;
}

/* Allocate a protocol runtime/control context. */
static eigrp_instance_t *eigrp_instance_create(eigrp_afi_t afi, uint16_t as,
				   eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp = calloc(1, sizeof(struct eigrp_instance));
	eigrp_addr_t src = {0};

	if (!eigrp) {
		eigrp_log(EIGRP_LOG_ERROR, "address-family %u AS %u runtime allocation failed",
				(unsigned)afi, (unsigned)as);
		return NULL;
	}

	/* Initialize address-family-independent control state first. */
	eigrp->vrf_id = vrf_id;
	switch (afi) {
	case EIGRP_AFI_IPV4:
		eigrp_ipv4_init(&eigrp->af_vectors);
		break;
	case EIGRP_AFI_IPV6:
		eigrp_ipv6_init(&eigrp->af_vectors);
		break;
	default:
		free(eigrp);
		return NULL;
	}
	if (!eigrp_af_vectors_runtime_validate(&eigrp->af_vectors)) {
		free(eigrp);
		return NULL;
	}
	eigrp->vrid = EIGRP_VRID_AF_BASE;
	eigrp->AS = as;
	eigrp->router_id.s_addr = INADDR_ANY;
	eigrp->router_id_static.s_addr = INADDR_ANY;
	eigrp->sequence_number = 1;

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
	eigrp->eiflist = eigrp_list_create();
	eigrp->passive_interface_default = EIGRP_INTF_ACTIVE;
	eigrp->networks = NULL;
	eigrp->oi_write_q = eigrp_list_create();
	eigrp->topology_table = eigrp_topology_table_create();
	eigrp->variance = EIGRP_VARIANCE_DEFAULT;
	eigrp->traffic_share_balanced = true;
	eigrp->max_paths = EIGRP_MAX_PATHS_DEFAULT;
	eigrp->max_hops = EIGRP_MAX_HOPS;
	eigrp->distance_internal = EIGRP_DISTANCE_INTERNAL_DEFAULT;
	eigrp->distance_external = EIGRP_DISTANCE_EXTERNAL_DEFAULT;
	eigrp->metric_version = EIGRP_MAJOR_VERSION;
	eigrp->log_neighbor_changes = true;
	eigrp->log_neighbor_warnings = true;
	eigrp->log_neighbor_warning_interval = 10;
	eigrp->topology_changes = eigrp_list_create();

	/* Diagnostic/control state is valid before a packet data path exists. */
	(void)eigrp_eventlog_init(eigrp, EIGRP_EVENTLOG_DEFAULT_SIZE);
	(void)eigrp_sys_policy_instance_create(eigrp);


	if (eigrp_sys_socket_open(eigrp) != EIGRP_RESULT_SUCCESS) {
		eigrp_log(EIGRP_LOG_ERROR,
			"eigrp_instance_create: fatal error: host runtime was unable to open an EIGRP socket");
		exit(1);
	}

	eigrp->ibuf = eigrp_stream_create(EIGRP_PACKET_MAX_LEN + 1);
	eigrp_sys_read_add(&eigrp->t_read, eigrp,
				   eigrp_packet_read, eigrp);

	/* The self-neighbor is wire/data-path state and is created only there. */
	src.afi = afi == EIGRP_AFI_IPV6 ? AF_INET6 : AF_INET;
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
	eigrp_list_item_t *node, *nnode;

	for (EIGRP_LIST_ITERATE(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->af_vectors.afi == EIGRP_AFI_IPV4
		    && eigrp->vrf_id == vrf_id)
			return eigrp;
	}
	return NULL;
}

eigrp_instance_t *eigrp_lookup_by_af_as_vrf(eigrp_afi_t afi,
					     uint16_t as,
					     eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp;
	eigrp_list_item_t *node, *nnode;

	for (EIGRP_LIST_ITERATE(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->af_vectors.afi == afi && eigrp->AS == as
		    && eigrp->vrf_id == vrf_id)
			return eigrp;
	}
	return NULL;
}

eigrp_instance_t *eigrp_lookup_by_as_vrf(uint16_t as, eigrp_vrf_id_t vrf_id)
{
	return eigrp_lookup_by_af_as_vrf(EIGRP_AFI_IPV4, as, vrf_id);
}

eigrp_instance_t *eigrp_instance_lookup_or_create_by_af(eigrp_afi_t afi, uint16_t as,
				   eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp;

	eigrp = eigrp_lookup_by_af_as_vrf(afi, as, vrf_id);
	if (eigrp == NULL) {
		eigrp = eigrp_instance_create(afi, as, vrf_id);
		if (!eigrp)
			return NULL;
		eigrp_list_add(eigrp_om->eigrp, eigrp);
	}
	return eigrp;
}

eigrp_instance_t *eigrp_instance_lookup_or_create(uint16_t as, eigrp_vrf_id_t vrf_id)
{
	return eigrp_instance_lookup_or_create_by_af(EIGRP_AFI_IPV4, as, vrf_id);
}

void eigrp_name_update(eigrp_operation_t operation, eigrp_instance_t *eigrp, const char *name)
{
	if (operation != EIGRP_SET || !eigrp || !name || !name[0])
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
		eigrp = eigrp_list_item_data(eigrp_list_first(eigrp_om->eigrp));
		eigrp_instance_delete(eigrp);
	}

	eigrp_instance_config_delete_all();
	eigrp_rib_finish();
	eigrp_sys_runtime_finish();
}

void eigrp_instance_delete(eigrp_instance_t *eigrp)
{
	eigrp_instance_delete_final(eigrp);

	/* eigrp being shut-down? If so, was this the last eigrp instance? */
	if ((eigrp_om->options & EIGRPD_SHUTDOWN) != 0
	    && (eigrp_om->eigrp == NULL || eigrp_om->eigrp->count == 0))
		return;

	return;
}

/* Final cleanup of eigrp instance */
void eigrp_instance_delete_final(eigrp_instance_t *eigrp)
{
	eigrp_intf_t *ei;
	eigrp_nbr_t *nbr;
	eigrp_list_item_t *node, *nnode, *node2, *nnode2;

	/* Named address-family configuration owns its runtime binding.  Clear
	 * that binding before any runtime storage is released so later config
	 * cleanup cannot dereference or attempt to destroy a stale instance.
	 */
	eigrp_instance_runtime_remove(eigrp);

	for (EIGRP_LIST_ITERATE(eigrp->eiflist, node, nnode, ei)) {
		for (EIGRP_LIST_ITERATE(ei->nbrs, node2, nnode2, nbr))
			eigrp_nbr_delete(nbr);
		eigrp_intf_free(eigrp, ei, EIGRP_INTERFACE_REMOVE_FINAL);
	}

	eigrp_network_runtime_delete_all(eigrp);
	eigrp_sys_event_cancel(&eigrp->t_write);
	eigrp_sys_event_cancel(&eigrp->t_read);
	eigrp_packetizer_delete(eigrp);
	eigrp_eventlog_delete(eigrp);
	eigrp_sys_socket_close(eigrp);

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
	eigrp_sys_policy_instance_delete(eigrp);
	eigrp_rib_instance_delete(eigrp);
	eigrp_filter_runtime_state_clear(&eigrp->filter);
	eigrp_nbr_warning_state_clear(eigrp);
	free(eigrp);
}
