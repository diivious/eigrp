// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP Daemon Program.
 * Copyright (C) 2013-2014, 2026
 * Authors:
 *   Donnie Savage
 *   Jan Janovic
 *   Matej Perina
 *   Peter Orsag
 *   Peter Paluch
 */
#include <stdlib.h>
#include <string.h>
#include "eigrp.h"
#include "eigrp_log.h"

#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_packet.h"
#include "eigrp_checksum.h"
#include "eigrp_network.h"
#include "eigrp_instance.h"
#include "eigrp_topology.h"
#include "eigrp_filter.h"
#include "eigrp_metric.h"
#include "eigrp_eventlog.h"
#include "eigrp_timer.h"
#include "eigrp_packetizer.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
#include "eigrp_tlv1.h"
#include "eigrp_tlv2.h"

/* Address-family vectors are bound once when the runtime/control context is
 * created.  Common protocol code treats a validated binding as an invariant.
 */
eigrp_process_t eigrp_process;

#define EIGRP_PROCESS_RECEIVE_QUEUE_MAX 1024U

static void eigrp_process_packet_input_free(eigrp_packet_input_t *input)
{
	if (!input)
		return;
	if (input->stream)
		eigrp_stream_free(input->stream);
	free(input);
}

static eigrp_instance_t *eigrp_process_packet_target_lookup_locked(
	eigrp_vrid_t vrid, eigrp_afi_t afi, uint16_t asn, eigrp_vrf_id_t vrf_id)
{
	eigrp_virt_router_t *virt_router;
	eigrp_instance_t *instance;
	eigrp_instance_t *match = NULL;
	eigrp_list_item_t *vr_node, *af_node;

	if (!eigrp_process.virt_router)
		return NULL;

	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, vr_node, virt_router)) {
		if (!virt_router || virt_router->shutdown || virt_router->vrid != vrid)
			continue;
		for (EIGRP_LIST_ITERATE_RO(virt_router->af_instance, af_node, instance)) {
			if (!instance || instance->shutdown)
				continue;
			if (instance->af_vectors.afi != afi || instance->AS != asn
			    || instance->vrf_id != vrf_id)
				continue;
			if (match) {
				eigrp_log(EIGRP_LOG_ERROR,
					"packet demux identity is ambiguous: vrid %u afi %u as %u vrf %u",
					(unsigned)vrid, (unsigned)afi, (unsigned)asn,
					(unsigned)vrf_id);
				return NULL;
			}
			match = instance;
		}
	}
	return match;
}

static bool eigrp_process_packet_validate(const eigrp_packet_input_t *input,
	const eigrp_header_t **header)
{
	const uint8_t *data;
	size_t endp;
	size_t offset;

	if (header)
		*header = NULL;
	if (!input || !input->stream || !header)
		return false;
	endp = eigrp_stream_get_endp(input->stream);
	offset = input->meta.network_header_length;
	if (input->meta.eigrp_length < EIGRP_HEADER_LEN || offset > endp
	    || input->meta.eigrp_length > endp - offset)
		return false;
	data = eigrp_stream_const_data(input->stream);
	*header = (const eigrp_header_t *)(data + offset);
	if ((*header)->version != EIGRP_HEADER_VERSION)
		return false;
	if (eigrp_checksum(*header, input->meta.eigrp_length) != 0)
		return false;
	return true;
}

static void eigrp_process_packet_demux(eigrp_packet_input_t *input)
{
	const eigrp_header_t *header;
	eigrp_instance_t *instance;

	if (!eigrp_process_packet_validate(input, &header)) {
		eigrp_process_packet_input_free(input);
		return;
	}

	pthread_mutex_lock(&eigrp_process.hierarchy_lock);
	instance = eigrp_process_packet_target_lookup_locked(
		ntohs(header->vrid), input->afi, ntohs(header->ASNumber),
		input->meta.ingress_vrf_id);
	if (instance && !instance->shutdown)
		(void)eigrp_nbr_ingress_holddown_refresh(instance, input->ifindex,
			&input->source);
	if (!instance || !eigrp_packet_input_enqueue(instance, input)) {
		pthread_mutex_unlock(&eigrp_process.hierarchy_lock);
		eigrp_process_packet_input_free(input);
		return;
	}
	pthread_mutex_unlock(&eigrp_process.hierarchy_lock);
}

static void *eigrp_packet_thread(void *arg)
{
	eigrp_process_t *process = arg;

	eigrp_log_start();
	for (;;) {
		eigrp_packet_input_t *input;

		pthread_mutex_lock(&process->receive_lock);
		while (process->receive_running && !process->receive_head)
			pthread_cond_wait(&process->receive_cond, &process->receive_lock);
		if (!process->receive_running && !process->receive_head) {
			pthread_mutex_unlock(&process->receive_lock);
			break;
		}
		input = process->receive_head;
		process->receive_head = input->next;
		if (process->receive_count)
			process->receive_count--;
		if (!process->receive_head)
			process->receive_tail = NULL;
		input->next = NULL;
		pthread_mutex_unlock(&process->receive_lock);

		eigrp_process_packet_demux(input);
	}
	eigrp_log_stop();
	return NULL;
}

bool eigrp_process_packet_submit(eigrp_packet_input_t *input)
{
	if (!input)
		return false;
	pthread_mutex_lock(&eigrp_process.receive_lock);
	if (!eigrp_process.receive_running
	    || eigrp_process.receive_count >= EIGRP_PROCESS_RECEIVE_QUEUE_MAX) {
		if (eigrp_process.receive_running)
			eigrp_process.receive_drop_count++;
		pthread_mutex_unlock(&eigrp_process.receive_lock);
		return false;
	}
	input->next = NULL;
	if (eigrp_process.receive_tail)
		eigrp_process.receive_tail->next = input;
	else
		eigrp_process.receive_head = input;
	eigrp_process.receive_tail = input;
	eigrp_process.receive_count++;
	pthread_cond_signal(&eigrp_process.receive_cond);
	pthread_mutex_unlock(&eigrp_process.receive_lock);
	return true;
}

static void eigrp_process_receive_stop(void)
{
	eigrp_packet_input_t *input;

	pthread_mutex_lock(&eigrp_process.receive_lock);
	eigrp_process.receive_running = false;
	pthread_cond_broadcast(&eigrp_process.receive_cond);
	pthread_mutex_unlock(&eigrp_process.receive_lock);
	if (eigrp_process.receive_started) {
		(void)pthread_join(eigrp_process.receive_thread, NULL);
		eigrp_process.receive_started = false;
	}

	pthread_mutex_lock(&eigrp_process.receive_lock);
	while ((input = eigrp_process.receive_head) != NULL) {
		eigrp_process.receive_head = input->next;
		eigrp_process_packet_input_free(input);
	}
	eigrp_process.receive_tail = NULL;
	eigrp_process.receive_count = 0;
	pthread_mutex_unlock(&eigrp_process.receive_lock);
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

void eigrp_init(void)
{
	struct timespec ts;

	memset(&eigrp_process, 0, sizeof(eigrp_process));
	eigrp_process.virt_router = eigrp_list_create();
	(void)pthread_mutex_init(&eigrp_process.hierarchy_lock, NULL);
	(void)pthread_mutex_init(&eigrp_process.receive_lock, NULL);
	(void)pthread_cond_init(&eigrp_process.receive_cond, NULL);

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		eigrp_process.start_time = ts.tv_sec;
	else
		eigrp_process.start_time = 0;

	/* Start packet reception only after every process-owned object used by
	 * the receive/demux path has been initialized.  pthread_create() is the
	 * publication point for the packet thread; nothing below this point may
	 * add a dependency that the thread can observe uninitialized.
	 */
	eigrp_process.receive_running = true;
	if (pthread_create(&eigrp_process.receive_thread, NULL,
			eigrp_packet_thread, &eigrp_process) == 0)
		eigrp_process.receive_started = true;
	else {
		eigrp_process.receive_running = false;
		eigrp_log(EIGRP_LOG_ERROR,
			"unable to start EIGRP packet thread");
	}
}

/* Shut down the entire process */
void eigrp_terminate(void)
{
	eigrp_virt_router_t *virt_router;
	eigrp_instance_t *instance;

	/* shutdown already in progress */
	if ((eigrp_process.options & EIGRP_SHUTDOWN) != 0)
		return;

	eigrp_process.options |= EIGRP_SHUTDOWN;
	eigrp_process_receive_stop();

	/* Named configuration owns its named runtime hierarchy. */
	eigrp_named_config_delete_all();

	/* Any remaining runtime hierarchy is classic/default ownership. */
	while (eigrp_process.virt_router && eigrp_process.virt_router->count) {
		virt_router = eigrp_list_item_data(
			eigrp_list_first(eigrp_process.virt_router));
		while (virt_router->af_instance && virt_router->af_instance->count) {
			instance = eigrp_list_item_data(
				eigrp_list_first(virt_router->af_instance));
			eigrp_instance_delete(instance);
		}
		(void)eigrp_virt_router_delete(virt_router);
	}
	eigrp_list_delete(&eigrp_process.virt_router);
	eigrp_rib_finish();
	eigrp_sys_runtime_finish();
	(void)pthread_cond_destroy(&eigrp_process.receive_cond);
	(void)pthread_mutex_destroy(&eigrp_process.receive_lock);
	(void)pthread_mutex_destroy(&eigrp_process.hierarchy_lock);
}
