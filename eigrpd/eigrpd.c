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
#include "eigrpd/eigrpd.h"

#include "table.h"
#include "eigrpd/eigrp_structs.h"
#include "eigrpd/eigrp_interface.h"
#include "eigrpd/eigrp_neighbor.h"
#include "eigrpd/eigrp_packet.h"
#include "eigrpd/eigrp_network.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_errors.h"
#include "eigrpd/eigrp_eventlog.h"
#include "eigrpd/eigrp_zebra.h"
#include "eigrpd/eigrp_packetizer.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_tlv1.h"
#include "eigrpd/eigrp_tlv2.h"

/* Current runtime creation is IPv4-only; AF modules expose only init binds. */

DEFINE_MGROUP(EIGRPD, "eigrpd");
DEFINE_MTYPE_STATIC(EIGRPD, EIGRP_TOP, "EIGRP structure");

static struct eigrpd eigrpd;
struct eigrpd *eigrp_om;


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
		eigrp_southbound_interfaces_refresh(eigrp);
}

void eigrp_init(void)
{
	struct timeval tv;

	memset(&eigrpd, 0, sizeof(struct eigrpd));

	eigrp_om = &eigrpd;
	eigrp_om->eigrp = list_new();

	monotime(&tv);
	eigrp_om->start_time = tv.tv_sec;
}

/* Allocate new eigrp structure. */
static eigrp_instance_t *eigrp_new(uint16_t as, eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp = XCALLOC(MTYPE_EIGRP_TOP, sizeof(struct eigrp_instance));

	/* init information relevant to peers */
	eigrp->vrf_id = vrf_id;
	/* All runtime instances are IPv4 until the IPv6 data path is enabled. */
	eigrp_ipv4_init(&eigrp->af_vectors);
	eigrp->vrid = 0;
	eigrp->AS = as;
	eigrp->router_id.s_addr = INADDR_ANY;
	eigrp->router_id_static.s_addr = INADDR_ANY;
	eigrp->sequence_number = 1;

	/*Configure default K Values for EIGRP Process*/
	eigrp->k_values[0] = EIGRP_K1_DEFAULT;
	eigrp->k_values[1] = EIGRP_K2_DEFAULT;
	eigrp->k_values[2] = EIGRP_K3_DEFAULT;
	eigrp->k_values[3] = EIGRP_K4_DEFAULT;
	eigrp->k_values[4] = EIGRP_K5_DEFAULT;
	eigrp->k_values[5] = EIGRP_K6_DEFAULT;

	eigrp_tlv1_init(&eigrp->tlv1_codec);
	eigrp_tlv2_init(&eigrp->tlv2_codec);

	/* init internal data structures */
	eigrp->eiflist = list_new();
	eigrp->passive_interface_default = EIGRP_INTF_ACTIVE;
	/* Configured network statements are not a topology table. */
	eigrp->networks = route_table_init();

	if (eigrp_southbound_socket_open(eigrp) != EIGRP_RESULT_SUCCESS) {
		flog_err_sys(
			EC_LIB_SOCKET,
			"eigrp_new: fatal error: host runtime was unable to open an EIGRP socket");
		exit(1);
	}

	eigrp->ibuf = stream_new(EIGRP_PACKET_MAX_LEN + 1);

	eigrp_southbound_read_add(&eigrp->t_read, eigrp->fd,
				   eigrp_packet_read, eigrp);
	eigrp->oi_write_q = list_new();

	// DVS: get it into a workable form, but this is an ugly hack
	//      cleaning these up as I get ipv6 fixed
	eigrp_addr_t src;
	src.afi = AF_INET;
	src.ip.v4.s_addr = INADDR_ANY;

	eigrp->neighbor_self = eigrp_nbr_create(NULL, &src);
	eigrp->topology_table = eigrp_topology_table_create();
	eigrp->variance = EIGRP_VARIANCE_DEFAULT;
	eigrp->max_paths = EIGRP_MAX_PATHS_DEFAULT;

	eigrp->serno = 0;
	eigrp->serno_last_update = 0;
	eigrp->topology_changes = list_new();
	eigrp_packetizer_init(eigrp);
	/* Diagnostic logging is best-effort and must not block protocol startup. */
	(void)eigrp_eventlog_init(eigrp, EIGRP_EVENTLOG_DEFAULT_SIZE);

	/* Host policy objects are created and retained by the southbound adapter. */
	(void)eigrp_southbound_policy_instance_create(eigrp);
	return eigrp;
}

/*
 * DVS: broken
 *	if you try to run multiple eigrp instances over single VRF
 *	lot of code does not pass vrf_id?
 *
 * Look for existing eigrp process based on the VRF its running over
 */
eigrp_instance_t *eigrp_lookup(eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp;
	struct listnode *node, *nnode;

	for (ALL_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->vrf_id == vrf_id)
			return eigrp;
	}
	return NULL;
}

eigrp_instance_t *eigrp_lookup_by_as_vrf(uint16_t as, eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp;
	struct listnode *node, *nnode;

	for (ALL_LIST_ELEMENTS(eigrp_om->eigrp, node, nnode, eigrp)) {
		if (eigrp->AS == as && eigrp->vrf_id == vrf_id)
			return eigrp;
	}
	return NULL;
}

eigrp_instance_t *eigrp_get(uint16_t as, eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp;

	eigrp = eigrp_lookup_by_as_vrf(as, vrf_id);
	if (eigrp == NULL) {
		eigrp = eigrp_new(as, vrf_id);
		listnode_add(eigrp_om->eigrp, eigrp);
	}

	return eigrp;
}

void eigrp_name_set(eigrp_instance_t *eigrp, const char *name)
{
	if (!eigrp || !name || !name[0])
		return;

	if (eigrp->name && strcmp(eigrp->name, name) == 0)
		return;

	if (eigrp->name)
		XFREE(MTYPE_EIGRP_TOP, eigrp->name);

	eigrp->name = XSTRDUP(MTYPE_EIGRP_TOP, name);
}

/* Shut down the entire process */
void eigrp_terminate(void)
{
	eigrp_instance_t *eigrp;

	/* shutdown already in progress */
	if (CHECK_FLAG(eigrp_om->options, EIGRPD_SHUTDOWN))
		return;

	SET_FLAG(eigrp_om->options, EIGRPD_SHUTDOWN);

	while (listcount(eigrp_om->eigrp)) {
		eigrp = listnode_head(eigrp_om->eigrp);
		eigrp_finish(eigrp);
	}

	eigrp_instance_config_finish();
	eigrp_zebra_stop();
	vrf_terminate();
	frr_fini();
}

void eigrp_finish(eigrp_instance_t *eigrp)
{
	eigrp_finish_final(eigrp);

	/* eigrp being shut-down? If so, was this the last eigrp instance? */
	if (CHECK_FLAG(eigrp_om->options, EIGRPD_SHUTDOWN)
	    && (listcount(eigrp_om->eigrp) == 0))
		return;

	return;
}

/* Final cleanup of eigrp instance */
void eigrp_finish_final(eigrp_instance_t *eigrp)
{
	eigrp_interface_t *ei;
	eigrp_neighbor_t *nbr;
	struct listnode *node, *nnode, *node2, *nnode2;

	/* Named address-family configuration owns its runtime binding.  Clear
	 * that binding before any runtime storage is released so later config
	 * cleanup cannot dereference or attempt to destroy a stale instance.
	 */
	eigrp_instance_runtime_unbind(eigrp);

	for (ALL_LIST_ELEMENTS(eigrp->eiflist, node, nnode, ei)) {
		for (ALL_LIST_ELEMENTS(ei->nbrs, node2, nnode2, nbr))
			eigrp_nbr_delete(nbr);
		eigrp_intf_free(eigrp, ei, INTERFACE_DOWN_BY_FINAL);
	}

	eigrp_southbound_event_cancel(&eigrp->t_write);
	eigrp_southbound_event_cancel(&eigrp->t_read);
	eigrp_packetizer_finish(eigrp);
	eigrp_eventlog_finish(eigrp);
	eigrp_southbound_socket_close(eigrp);

	list_delete(&eigrp->eiflist);
	list_delete(&eigrp->oi_write_q);

	eigrp_topology_table_delete(eigrp, eigrp->topology_table);
	eigrp_nbr_delete(eigrp->neighbor_self);

	list_delete(&eigrp->topology_changes);
	listnode_delete(eigrp_om->eigrp, eigrp);

	if (eigrp->name)
		XFREE(MTYPE_EIGRP_TOP, eigrp->name);

	stream_free(eigrp->ibuf);
	eigrp_southbound_policy_instance_delete(eigrp);
	eigrp_filter_runtime_state_clear(&eigrp->filter);
	XFREE(MTYPE_EIGRP_TOP, eigrp);
}
