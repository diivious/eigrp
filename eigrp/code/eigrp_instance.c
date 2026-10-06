// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP instance and address-family configuration ownership.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <stdlib.h>
#include <string.h>


#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_instance.h"
#include "eigrp_features.h"
#include "eigrp_interface.h"
#include "eigrp_neighbor.h"
#include "eigrp_packet.h"
#include "eigrp_network.h"
#include "eigrp_filter.h"
#include "eigrp_metric.h"
#include "eigrp_redistribute.h"
#include "eigrp_summary.h"
#include "eigrp_timer.h"
#include "eigrp_eventlog.h"
#include "eigrp_log.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
#include "eigrp_topology.h"
#include "eigrp_packetizer.h"
#include "eigrp_tlv1.h"
#include "eigrp_tlv2.h"

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

/*
 * Runtime hierarchy ownership.  Process-wide startup/shutdown remains in
 * eigrp.c; virtual-router and address-family runtime lifecycle lives here so
 * eigrp_instance_* navigation matches source ownership.
 */

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
	return eigrp && eigrp->virt_router ? eigrp->virt_router->name : NULL;
}

static bool eigrp_virt_router_name_equal(const eigrp_virt_router_t *virt_router,
	const char *name)
{
	if (!virt_router)
		return false;
	if (!name || !name[0])
		return !virt_router->name || !virt_router->name[0];
	return virt_router->name && strcmp(virt_router->name, name) == 0;
}

eigrp_virt_router_t *eigrp_virt_router_lookup(const char *name)
{
	eigrp_virt_router_t *virt_router;
	eigrp_list_item_t *node;

	if (!eigrp_process.virt_router)
		return NULL;
	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, node, virt_router)) {
		if (eigrp_virt_router_name_equal(virt_router, name))
			return virt_router;
	}
	return NULL;
}

eigrp_virt_router_t *eigrp_virt_router_create(const char *name, eigrp_vrid_t vrid)
{
	eigrp_virt_router_t *virt_router;

	if (!eigrp_process.virt_router)
		return NULL;
	if (eigrp_virt_router_lookup(name))
		return NULL;

	virt_router = calloc(1, sizeof(*virt_router));
	if (!virt_router)
		return NULL;
	virt_router->process = &eigrp_process;
	virt_router->vrid = vrid;
	virt_router->af_instance = eigrp_list_create();
	if (!virt_router->af_instance) {
		free(virt_router);
		return NULL;
	}
	if (name && name[0]) {
		virt_router->name = strdup(name);
		if (!virt_router->name) {
			eigrp_list_delete(&virt_router->af_instance);
			free(virt_router);
			return NULL;
		}
	}
	pthread_mutex_lock(&eigrp_process.hierarchy_lock);
	eigrp_list_add(eigrp_process.virt_router, virt_router);
	pthread_mutex_unlock(&eigrp_process.hierarchy_lock);
	return virt_router;
}

eigrp_result_t eigrp_virt_router_delete(eigrp_virt_router_t *virt_router)
{
	if (!virt_router)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (virt_router->af_instance && virt_router->af_instance->count)
		return EIGRP_RESULT_CONFLICT;

	pthread_mutex_lock(&eigrp_process.hierarchy_lock);
	if (eigrp_process.virt_router)
		eigrp_list_delete_data(eigrp_process.virt_router, virt_router);
	pthread_mutex_unlock(&eigrp_process.hierarchy_lock);
	eigrp_list_delete(&virt_router->af_instance);
	free(virt_router->name);
	free(virt_router);
	return EIGRP_RESULT_SUCCESS;
}

/* Allocate a protocol runtime/control context. */
eigrp_instance_t *eigrp_instance_create(eigrp_virt_router_t *virt_router,
				   eigrp_afi_t afi, uint16_t as, eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *eigrp;
	eigrp_addr_t src = {0};

	if (!virt_router || !virt_router->af_instance || virt_router->shutdown)
		return NULL;
	eigrp = calloc(1, sizeof(struct eigrp_instance));
	if (!eigrp) {
		eigrp_log(EIGRP_LOG_ERROR, "address-family %u AS %u runtime allocation failed",
				(unsigned)afi, (unsigned)as);
		return NULL;
	}

	if (pthread_mutex_init(&eigrp->work_lock, NULL) != 0) {
		free(eigrp);
		return NULL;
	}
	if (pthread_mutex_init(&eigrp->peer_lock, NULL) != 0) {
		(void)pthread_mutex_destroy(&eigrp->work_lock);
		free(eigrp);
		return NULL;
	}
	{
		pthread_condattr_t attr;

		if (pthread_condattr_init(&attr) != 0) {
			(void)pthread_mutex_destroy(&eigrp->peer_lock);
			(void)pthread_mutex_destroy(&eigrp->work_lock);
			free(eigrp);
			return NULL;
		}
		(void)pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
		if (pthread_cond_init(&eigrp->work_cond, &attr) != 0) {
			(void)pthread_condattr_destroy(&attr);
			(void)pthread_mutex_destroy(&eigrp->peer_lock);
			(void)pthread_mutex_destroy(&eigrp->work_lock);
			free(eigrp);
			return NULL;
		}
		(void)pthread_condattr_destroy(&attr);
	}

	/* Initialize address-family-independent control state first. */
	eigrp->virt_router = virt_router;
	eigrp->vrf_id = vrf_id;
	switch (afi) {
	case EIGRP_AFI_IPV4:
		eigrp_ipv4_init(&eigrp->af_vectors);
		break;
	case EIGRP_AFI_IPV6:
		eigrp_ipv6_init(&eigrp->af_vectors);
		break;
	default:
		(void)pthread_cond_destroy(&eigrp->work_cond);
		(void)pthread_mutex_destroy(&eigrp->peer_lock);
		(void)pthread_mutex_destroy(&eigrp->work_lock);
		free(eigrp);
		return NULL;
	}
	if (!eigrp_af_vectors_runtime_validate(&eigrp->af_vectors)) {
		(void)pthread_cond_destroy(&eigrp->work_cond);
		(void)pthread_mutex_destroy(&eigrp->peer_lock);
		(void)pthread_mutex_destroy(&eigrp->work_lock);
		free(eigrp);
		return NULL;
	}
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

	/* The self-neighbor is wire/data-path state and is created only there. */
	src.afi = afi == EIGRP_AFI_IPV6 ? AF_INET6 : AF_INET;
	eigrp->neighbor_self = eigrp_nbr_create(NULL, &src);
	eigrp_packetizer_init(eigrp);

	/* The AF packet consumer is part of AF initialization.  Start it before
	 * publishing the AF so the process packet thread can never discover an
	 * instance whose packetQ has no owner. */
	if (!eigrp_instance_thread_start(eigrp)) {
		eigrp_log(EIGRP_LOG_ERROR,
			"address-family %u AS %u packet thread start failed",
			(unsigned)afi, (unsigned)as);
		eigrp_instance_delete_final(eigrp);
		return NULL;
	}
	pthread_mutex_lock(&eigrp_process.hierarchy_lock);
	if (virt_router->shutdown) {
		pthread_mutex_unlock(&eigrp_process.hierarchy_lock);
		eigrp_instance_delete_final(eigrp);
		return NULL;
	}
	eigrp_list_add(virt_router->af_instance, eigrp);
	pthread_mutex_unlock(&eigrp_process.hierarchy_lock);

	/* Socket read admission begins only after the fully initialized AF is
	 * published in the process -> VR -> AF hierarchy. */
	eigrp_sys_read_add(&eigrp->t_read, eigrp, eigrp_packet_read, eigrp);
	return eigrp;
}

/*
 * Legacy classic callers are IPv4.  Keep these lookups IPv4-only so adding a
 * named IPv6 control runtime cannot redirect Zebra/classic code to IPv6.
 */
eigrp_instance_t *eigrp_lookup(eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *instance;
	eigrp_virt_router_t *virt_router;
	eigrp_list_item_t *vr_node, *af_node;

	if (!eigrp_process.virt_router)
		return NULL;
	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, vr_node, virt_router)) {
		if (virt_router->name)
			continue;
		for (EIGRP_LIST_ITERATE_RO(virt_router->af_instance, af_node, instance)) {
			if (instance->af_vectors.afi == EIGRP_AFI_IPV4
			    && instance->vrf_id == vrf_id)
				return instance;
		}
	}
	return NULL;
}

eigrp_instance_t *eigrp_lookup_by_af_as_vrf(eigrp_afi_t afi, uint16_t as,
					     eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *instance;
	eigrp_virt_router_t *virt_router;
	eigrp_list_item_t *vr_node, *af_node;

	if (!eigrp_process.virt_router)
		return NULL;
	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, vr_node, virt_router)) {
		for (EIGRP_LIST_ITERATE_RO(virt_router->af_instance, af_node, instance)) {
			if (instance->af_vectors.afi == afi && instance->AS == as
			    && instance->vrf_id == vrf_id)
				return instance;
		}
	}
	return NULL;
}

eigrp_instance_t *eigrp_lookup_by_as_vrf(uint16_t as, eigrp_vrf_id_t vrf_id)
{
	return eigrp_lookup_by_af_as_vrf(EIGRP_AFI_IPV4, as, vrf_id);
}

eigrp_result_t eigrp_instance_iterate(eigrp_instance_iterate_cb callback,
	void *arg)
{
	eigrp_instance_t *instance;
	eigrp_virt_router_t *virt_router;
	eigrp_list_item_t *vr_node, *af_node;
	eigrp_result_t result;

	if (!callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_process.virt_router)
		return EIGRP_RESULT_SUCCESS;

	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, vr_node, virt_router)) {
		for (EIGRP_LIST_ITERATE_RO(virt_router->af_instance, af_node, instance)) {
			result = callback(instance, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
	}
	return EIGRP_RESULT_SUCCESS;
}

void eigrp_instance_delete(eigrp_instance_t *eigrp)
{
	if (!eigrp)
		return;
	pthread_mutex_lock(&eigrp_process.hierarchy_lock);
	eigrp->shutdown = true;
	if (eigrp->virt_router && eigrp->virt_router->af_instance)
		eigrp_list_delete_data(eigrp->virt_router->af_instance, eigrp);
	pthread_mutex_unlock(&eigrp_process.hierarchy_lock);

	/* No new packets can be admitted after shutdown+unlink.  Stop and join
	 * the AF packet owner before tearing down any protocol state it may use. */
	eigrp_instance_thread_stop(eigrp);
	eigrp_timer_cancel_all(eigrp);
	eigrp_instance_delete_final(eigrp);
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
	eigrp_af_config_runtime_remove(eigrp);
	eigrp_instance_thread_stop(eigrp);
	eigrp_timer_cancel_all(eigrp);
	eigrp_packet_input_queue_clear(eigrp);

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

	if (eigrp->ibuf)
		eigrp_stream_free(eigrp->ibuf);
	eigrp_sys_policy_instance_delete(eigrp);
	eigrp_rib_instance_delete(eigrp);
	eigrp_filter_runtime_state_clear(&eigrp->filter);
	eigrp_nbr_warning_state_clear(eigrp);
	(void)pthread_cond_destroy(&eigrp->work_cond);
	(void)pthread_mutex_destroy(&eigrp->peer_lock);
	(void)pthread_mutex_destroy(&eigrp->work_lock);
	free(eigrp);
}

/* AF modules intentionally expose only their vector bind entry points. */

static eigrp_named_config_t *eigrp_named_configs;


static char *eigrp_instance_string_dup(const char *value)
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

static bool eigrp_instance_afi_valid(eigrp_afi_t afi)
{
	return afi == EIGRP_AFI_IPV4
	       || afi == EIGRP_AFI_IPV6;
}

static eigrp_result_t eigrp_af_config_runtime_create(
	const char *name, eigrp_af_config_t *af)
{
	eigrp_named_config_t *parent;
	eigrp_instance_t *runtime;
	eigrp_vrf_id_t vrf_id;
	eigrp_result_t result;

	if (!name || !af)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	parent = eigrp_named_config_read(name);
	if (!parent || !parent->runtime)
		return EIGRP_RESULT_NOT_FOUND;

	result = eigrp_sys_vrf_resolve(af->vrf_name, &vrf_id);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	/* {AF, VRF, AS} is the protocol runtime identity.  The named parent is
	 * local configuration ownership and is never part of wire identity.
	 */
	runtime = eigrp_lookup_by_af_as_vrf(af->afi, af->asn, vrf_id);
	if (runtime) {
		if (runtime->virt_router != parent->runtime)
			return EIGRP_RESULT_CONFLICT;
	} else {
		runtime = eigrp_instance_create(parent->runtime, af->afi, af->asn, vrf_id);
		if (!runtime)
			return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	/* Runtime gets the same immutable AF dispatch selected by configuration. */
	runtime->af_vectors = af->af_vectors;
	af->runtime = runtime;
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_af_config_runtime_delete(
	const char *name, eigrp_af_config_t *af)
{
	if (!name || !af)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af->runtime)
		return EIGRP_RESULT_SUCCESS;
	if (!af->runtime->virt_router || !af->runtime->virt_router->name
	    || strcmp(af->runtime->virt_router->name, name) != 0)
		return EIGRP_RESULT_CONFLICT;

	eigrp_instance_delete(af->runtime);
	af->runtime = NULL;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_classic_validate(
	uint16_t asn, eigrp_vrf_id_t vrf_id, const char **owner_name)
{
	eigrp_instance_t *runtime;

	if (owner_name)
		*owner_name = NULL;
	if (!asn)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	runtime = eigrp_lookup_by_as_vrf(asn, vrf_id);
	if (!runtime || !eigrp_instance_name(runtime))
		return EIGRP_RESULT_SUCCESS;
	if (owner_name)
		*owner_name = eigrp_instance_name(runtime);
	return EIGRP_RESULT_CONFLICT;
}

eigrp_result_t eigrp_instance_classic_create(
	uint16_t asn, eigrp_vrf_id_t vrf_id, eigrp_instance_t **runtime)
{
	eigrp_result_t result;

	if (!runtime)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	*runtime = NULL;
	result = eigrp_instance_classic_validate(asn, vrf_id, NULL);
	if (result != EIGRP_RESULT_SUCCESS)
		return result;

	*runtime = eigrp_lookup_by_as_vrf(asn, vrf_id);
	if (!*runtime) {
		eigrp_virt_router_t *virt_router = eigrp_virt_router_lookup(NULL);

		if (!virt_router)
			virt_router = eigrp_virt_router_create(NULL, EIGRP_VRID_AF_BASE);
		if (!virt_router)
			return EIGRP_RESULT_INTERNAL_FAILURE;
		*runtime = eigrp_instance_create(virt_router, EIGRP_AFI_IPV4, asn, vrf_id);
	}
	return *runtime ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_INTERNAL_FAILURE;
}

eigrp_instance_t *eigrp_instance_classic_read(uint16_t asn,
					eigrp_vrf_id_t vrf_id)
{
	eigrp_instance_t *runtime = eigrp_lookup_by_as_vrf(asn, vrf_id);

	return runtime && !eigrp_instance_name(runtime) ? runtime : NULL;
}

eigrp_result_t eigrp_instance_classic_delete(eigrp_instance_t *runtime)
{
	eigrp_virt_router_t *virt_router;

	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (eigrp_instance_name(runtime))
		return EIGRP_RESULT_CONFLICT;
	virt_router = runtime->virt_router;
	eigrp_instance_delete(runtime);
	if (virt_router && virt_router->af_instance && virt_router->af_instance->count == 0)
		(void)eigrp_virt_router_delete(virt_router);
	return EIGRP_RESULT_SUCCESS;
}

eigrp_named_config_t *eigrp_named_config_read(const char *name)
{
	eigrp_named_config_t *parent;

	if (!name || !name[0])
		return NULL;

	for (parent = eigrp_named_configs; parent; parent = parent->next) {
		if (strcmp(parent->name, name) == 0)
			return parent;
	}
	return NULL;
}

/*
 * Syntax:
 *   Named: `router eigrp NAME` / `no router eigrp NAME`
 * Supported: Named
 * Placement:
 *   Named: global configuration
 * Description:
 * Creates or removes the named EIGRP parent configuration object.
 * The parent is a configuration container; address-family creation owns AS/family runtime creation.
 */
eigrp_result_t eigrp_named_config_create(const char *name)
{
	eigrp_named_config_t *parent;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (eigrp_named_config_read(name))
		return EIGRP_RESULT_SUCCESS;

	parent = calloc(1, sizeof(*parent));
	if (!parent)
		return EIGRP_RESULT_INTERNAL_FAILURE;
	parent->name = eigrp_instance_string_dup(name);
	if (!parent->name) {
		free(parent);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	parent->runtime = eigrp_virt_router_create(name, EIGRP_VRID_AF_BASE);
	if (!parent->runtime) {
		free(parent->name);
		free(parent);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	parent->next = eigrp_named_configs;
	eigrp_named_configs = parent;
	return EIGRP_RESULT_SUCCESS;
}

eigrp_af_config_t *eigrp_af_config_read(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_named_config_t *parent;
	eigrp_af_config_t *af;

	parent = eigrp_named_config_read(name);
	if (!parent || !vrf_name)
		return NULL;

	for (af = parent->address_families; af; af = af->next) {
		if (af->afi == afi && af->asn == asn
		    && strcmp(af->vrf_name, vrf_name) == 0)
			return af;
	}
	return NULL;
}

eigrp_result_t eigrp_af_config_context_read(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn, eigrp_instance_context_t *context)
{
	eigrp_af_config_t *af;

	if (!context)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	memset(context, 0, sizeof(*context));
	af = eigrp_af_config_read(name, afi, vrf_name, asn);
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	context->config = af;
	context->runtime = af->runtime;
	context->topology_id = EIGRP_TOPOLOGY_ID_BASE;
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Named: `address-family <ipv4|ipv6> [unicast] [vrf NAME] autonomous-system AS` / `no address-family ...`
 * Supported: Named
 * Placement:
 *   Named: router EIGRP parent mode
 * Description:
 * Creates or removes one named EIGRP address-family and its EIGRP-owned runtime context.
 * IPv4 and IPv6 retain separate AF state while sharing the named parent.
 */
eigrp_result_t eigrp_af_config_create(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_named_config_t *parent;
	eigrp_af_config_t *af;
	eigrp_result_t result;
	bool parent_created = false;

	if (!name || !name[0] || !vrf_name || !vrf_name[0] || asn == 0
	    || !eigrp_instance_afi_valid(afi))
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_afi_supported(afi))
		return EIGRP_RESULT_UNSUPPORTED;

	parent = eigrp_named_config_read(name);
	if (!parent) {
		result = eigrp_named_config_create(name);
		if (result != EIGRP_RESULT_SUCCESS)
			return result;
		parent_created = true;
	}
	if (eigrp_af_config_read(name, afi, vrf_name, asn))
		return EIGRP_RESULT_SUCCESS;

	parent = eigrp_named_config_read(name);
	if (!parent) {
		if (parent_created)
			(void)eigrp_named_config_delete(name);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}

	af = calloc(1, sizeof(*af));
	if (!af) {
		if (parent_created)
			(void)eigrp_named_config_delete(name);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	af->vrf_name = eigrp_instance_string_dup(vrf_name);
	if (!af->vrf_name) {
		free(af);
		if (parent_created)
			(void)eigrp_named_config_delete(name);
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
	af->afi = afi;
	af->asn = asn;

	/* Bind AF behavior once; common feature code calls through this vector. */
	switch (afi) {
	case EIGRP_AFI_IPV4:
		eigrp_ipv4_init(&af->af_vectors);
		break;
	case EIGRP_AFI_IPV6:
		eigrp_ipv6_init(&af->af_vectors);
		break;
	}

	result = eigrp_af_config_runtime_create(name, af);
	if (result != EIGRP_RESULT_SUCCESS) {
		free(af->vrf_name);
		free(af);
		if (parent_created)
			(void)eigrp_named_config_delete(name);
		return result;
	}
	af->next = parent->address_families;
	parent->address_families = af;

	/* A newly created address-family is administratively enabled unless the
	 * retained shutdown leaf is present.  Start it after linking the AF so
	 * runtime-to-config lookups work during interface discovery.  Runtime
	 * inability (for example, no usable IPv6 router ID yet) must not reject
	 * retained configuration.
	 */
	if (!parent->shutdown && !af->shutdown)
		(void)eigrp_instance_start(af->runtime);
	return EIGRP_RESULT_SUCCESS;
}

static void eigrp_af_config_free(eigrp_af_config_t *af)
{
	if (!af)
		return;

	eigrp_network_config_delete_all(af);
	eigrp_nbr_static_delete_all(af);
	eigrp_intf_config_delete_all(af);
	eigrp_redist_config_delete_all(af);
	eigrp_redist_policy_delete_all(af);
	eigrp_distribute_list_config_delete_all(af);
	eigrp_offset_config_delete_all(af);
	eigrp_metric_config_delete_all(af);
	eigrp_summary_state_delete_all(af);
	eigrp_timer_config_delete_all(af);
	eigrp_nbr_policy_delete_all(af);
	free(af->default_information_access_list[EIGRP_DEFAULT_INFORMATION_IN]);
	free(af->default_information_access_list[EIGRP_DEFAULT_INFORMATION_OUT]);
	free(af->vrf_name);
	free(af);
}

/*
 * Syntax:
 *   Named: `address-family <ipv4|ipv6> [unicast] [vrf NAME] autonomous-system AS` / `no address-family ...`
 * Supported: Named
 * Placement:
 *   Named: router EIGRP parent mode
 * Description:
 * Creates or removes one named EIGRP address-family and its EIGRP-owned runtime context.
 * IPv4 and IPv6 retain separate AF state while sharing the named parent.
 */
eigrp_result_t eigrp_af_config_delete(
	const char *name, eigrp_afi_t afi, const char *vrf_name,
	uint16_t asn)
{
	eigrp_named_config_t *parent;
	eigrp_af_config_t **cursor;
	eigrp_af_config_t *af;

	if (!name || !name[0] || !vrf_name || !vrf_name[0] || asn == 0
	    || !eigrp_instance_afi_valid(afi))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	parent = eigrp_named_config_read(name);
	if (!parent)
		return EIGRP_RESULT_NOT_FOUND;

	for (cursor = &parent->address_families; *cursor;
	     cursor = &(*cursor)->next) {
		af = *cursor;
		if (af->afi != afi || af->asn != asn
		    || strcmp(af->vrf_name, vrf_name) != 0)
			continue;
		{
			eigrp_result_t result =
				eigrp_af_config_runtime_delete(
					name, af);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
		*cursor = af->next;
		eigrp_af_config_free(af);
		return EIGRP_RESULT_SUCCESS;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

static bool eigrp_instance_vrf_seen_before(const eigrp_instance_t *target,
	eigrp_vrf_id_t vrf_id)
{
	eigrp_virt_router_t *virt_router;
	eigrp_instance_t *runtime;
	eigrp_list_item_t *vr_node, *af_node;

	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, vr_node, virt_router)) {
		for (EIGRP_LIST_ITERATE_RO(virt_router->af_instance, af_node, runtime)) {
			if (runtime == target)
				return false;
			if (eigrp_instance_vrf_id(runtime) == vrf_id)
				return true;
		}
	}
	return false;
}

eigrp_result_t eigrp_instance_vrf_iterate(
	eigrp_instance_vrf_iterate_cb callback, void *arg)
{
	eigrp_virt_router_t *virt_router;
	eigrp_instance_t *runtime;
	eigrp_list_item_t *vr_node, *af_node;
	eigrp_vrf_id_t vrf_id;
	eigrp_result_t result;

	if (!callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!eigrp_process.virt_router)
		return EIGRP_RESULT_SUCCESS;

	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, vr_node, virt_router)) {
		for (EIGRP_LIST_ITERATE_RO(virt_router->af_instance, af_node, runtime)) {
			vrf_id = eigrp_instance_vrf_id(runtime);
			if (eigrp_instance_vrf_seen_before(runtime, vrf_id))
				continue;
			result = callback(vrf_id, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   EXEC: named address-family show commands and `show eigrp protocols`
 * Supported: EXEC
 * Placement:
 *   Operational/read-only
 * Description:
 * Walks EIGRP-owned address-family state using an EIGRP request and callback.
 * FRR VTY and YANG objects remain outside the common API.
 */
eigrp_result_t eigrp_af_config_iterate(
	const eigrp_state_request_t *request,
	eigrp_af_config_iterate_cb callback, void *arg)
{
	eigrp_named_config_t *parent;
	eigrp_af_config_t *af;
	eigrp_result_t result;
	const char *vrf_name;
	bool matched = false;

	if (!request || !callback)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (request->afi != EIGRP_AFI_IPV4
	    && request->afi != EIGRP_AFI_IPV6)
		return EIGRP_RESULT_UNSUPPORTED;
	if (request->multicast)
		return EIGRP_RESULT_UNSUPPORTED;
	/* A normal show request without an explicit VRF is scoped to default. */
	vrf_name = request->vrf_name ? request->vrf_name : "default";

	for (parent = eigrp_named_configs; parent; parent = parent->next) {
		for (af = parent->address_families; af; af = af->next) {
			if (af->afi != request->afi)
				continue;
			if (request->asn && af->asn != request->asn)
				continue;
			if (!request->all_vrfs
			    && strcmp(af->vrf_name, vrf_name) != 0)
				continue;

			matched = true;
			result = callback(parent->name, af, arg);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}
	}

	return matched ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;
}

/*
 * Syntax:
 *   Named: `router eigrp NAME` / `no router eigrp NAME`
 * Supported: Named
 * Placement:
 *   Named: global configuration
 * Description:
 * Creates or removes the named EIGRP parent configuration object.
 * The parent is a configuration container; address-family creation owns AS/family runtime creation.
 */
eigrp_result_t eigrp_named_config_delete(const char *name)
{
	eigrp_named_config_t **cursor;
	eigrp_named_config_t *parent;
	eigrp_af_config_t *af;
	eigrp_af_config_t *next;
	eigrp_result_t result = EIGRP_RESULT_SUCCESS;

	if (!name || !name[0])
		return EIGRP_RESULT_INVALID_ARGUMENT;

	for (cursor = &eigrp_named_configs; *cursor;
	     cursor = &(*cursor)->next) {
		parent = *cursor;
		if (strcmp(parent->name, name) != 0)
			continue;

		/* Stop every bound runtime before discarding configuration ownership. */
		for (af = parent->address_families; af; af = af->next) {
			result = eigrp_af_config_runtime_delete(
				parent->name, af);
			if (result != EIGRP_RESULT_SUCCESS)
				return result;
		}

		*cursor = parent->next;
		for (af = parent->address_families; af; af = next) {
			next = af->next;
			eigrp_af_config_free(af);
		}
		if (parent->runtime) {
			eigrp_result_t vr_result = eigrp_virt_router_delete(parent->runtime);
			if (result == EIGRP_RESULT_SUCCESS && vr_result != EIGRP_RESULT_SUCCESS)
				result = vr_result;
		}
		free(parent->name);
		free(parent);
		return result;
	}
	return EIGRP_RESULT_NOT_FOUND;
}

void eigrp_af_config_runtime_remove(eigrp_instance_t *runtime)
{
	eigrp_named_config_t *parent;
	eigrp_af_config_t *af;

	if (!runtime)
		return;

	for (parent = eigrp_named_configs; parent; parent = parent->next) {
		for (af = parent->address_families; af; af = af->next) {
			if (af->runtime == runtime)
				af->runtime = NULL;
		}
	}
}


eigrp_af_config_t *eigrp_af_config_runtime_read(eigrp_instance_t *runtime)
{
	eigrp_named_config_t *parent;
	eigrp_af_config_t *af;

	if (!runtime)
		return NULL;

	for (parent = eigrp_named_configs; parent; parent = parent->next)
		for (af = parent->address_families; af; af = af->next)
			if (af->runtime == runtime)
				return af;
	return NULL;
}

static void eigrp_instance_router_id_runtime_update(eigrp_instance_t *runtime)
{
	if (!runtime)
		return;
	eigrp_router_id_update(runtime);
}

void eigrp_instance_event_process(eigrp_instance_t *runtime,
	eigrp_af_event_t *event)
{
	if (!runtime || !event)
		return;

	switch (event->type) {
	case EIGRP_AF_EVENT_ROUTERID_UPDATE:
		/* Router-id policy and selection are AF-owned.  The process only
		 * fans the host notification out to affected AF execution contexts. */
		eigrp_router_id_update(runtime);
		break;
	case EIGRP_AF_EVENT_INTF_UPDATE:
		eigrp_network_intf_update(runtime, &event->data.intf_update);
		break;
	case EIGRP_AF_EVENT_INTF_DOWN: {
		eigrp_intf_t *ei;
		eigrp_intf_runtime_state_t state;

		ei = eigrp_intf_lookup_by_ifindex(
			runtime, event->data.intf_down.ifindex);
		if (!ei)
			break;
		memset(&state, 0, sizeof(state));
		state.interface_name = ei->name;
		state.ifindex = event->data.intf_down.ifindex;
		state.address = ei->address;
		state.type = event->data.intf_down.type;
		state.operative = false;
		state.bandwidth = event->data.intf_down.bandwidth;
		state.mtu = event->data.intf_down.mtu;
		eigrp_intf_runtime_state_update_values(ei, &state);
		eigrp_intf_down(ei);
		break;
	}
	case EIGRP_AF_EVENT_INTF_REMOVE: {
		eigrp_intf_t *ei = eigrp_intf_lookup_by_ifindex(
			runtime, event->data.intf_remove.ifindex);
		if (ei)
			eigrp_intf_runtime_delete(ei, event->data.intf_remove.reason);
		break;
	}
	case EIGRP_AF_EVENT_INTF_ADDR_UPDATE: {
		const eigrp_prefix_t *address =
			&event->data.intf_addr_update.address;
		eigrp_intf_t *ei = eigrp_intf_lookup_by_ifindex(
			runtime, event->data.intf_addr_update.ifindex);
		if (!ei || ei->address.prefix_length != address->prefix_length
		    || ei->address.address.afi != address->address.afi
		    || memcmp(ei->address.address.bytes, address->address.bytes,
			      sizeof(address->address.bytes)) != 0)
			break;
		eigrp_intf_runtime_delete(ei, event->data.intf_addr_update.reason);
		break;
	}
	}
}

void eigrp_process_routerid_cb(eigrp_vrf_id_t vrf_id)
{
	eigrp_virt_router_t *virt_router;
	eigrp_instance_t *runtime;
	eigrp_list_item_t *vr_node, *af_node;

	if (!eigrp_process.virt_router)
		return;

	/* Hold the hierarchy stable while each matching AF accepts its event.
	 * The enqueue path then transfers ownership of the allocated event to
	 * the AF thread. */
	pthread_mutex_lock(&eigrp_process.hierarchy_lock);
	for (EIGRP_LIST_ITERATE_RO(eigrp_process.virt_router, vr_node, virt_router)) {
		for (EIGRP_LIST_ITERATE_RO(virt_router->af_instance, af_node, runtime)) {
			if (runtime->vrf_id != vrf_id || runtime->shutdown)
				continue;
			if (!eigrp_instance_event_enqueue(
				runtime, EIGRP_AF_EVENT_ROUTERID_UPDATE))
				eigrp_log(EIGRP_LOG_WARNING,
					"router-id notification dropped for AS(%u) VRF(%u)",
					runtime->AS, (unsigned)vrf_id);
		}
	}
	pthread_mutex_unlock(&eigrp_process.hierarchy_lock);
}

typedef enum eigrp_instance_control_message_type {
	EIGRP_INSTANCE_CONTROL_START,
	EIGRP_INSTANCE_CONTROL_STOP,
	EIGRP_INSTANCE_CONTROL_AF_SHUTDOWN,
	EIGRP_INSTANCE_CONTROL_DISTANCE,
} eigrp_instance_control_message_type_t;

typedef struct eigrp_instance_control_message_args {
	eigrp_instance_control_message_type_t type;
	eigrp_operation_t operation;
	eigrp_af_config_t *af;
	uint8_t internal_distance;
	uint8_t external_distance;
} eigrp_instance_control_message_args_t;

static eigrp_result_t eigrp_instance_control_message_process(
	eigrp_instance_t *eigrp, void *arg);

eigrp_result_t eigrp_instance_stop(eigrp_instance_t *runtime)
{
	eigrp_instance_control_message_args_t message = { .type = EIGRP_INSTANCE_CONTROL_STOP };

	if (eigrp_instance_thread_dispatch_needed(runtime))
		return eigrp_instance_message_call(runtime,
			eigrp_instance_control_message_process, &message);
	eigrp_intf_t *ei;
	eigrp_list_item_t *node;

	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;

	for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, node, ei)) {
		if (!ei->t_hello)
			continue;
		eigrp_hello_send(ei, EIGRP_HELLO_GRACEFUL_SHUTDOWN, NULL);
		eigrp_intf_down(ei);
	}
	return EIGRP_RESULT_SUCCESS;
}

eigrp_result_t eigrp_instance_start(eigrp_instance_t *runtime)
{
	eigrp_instance_control_message_args_t message = { .type = EIGRP_INSTANCE_CONTROL_START };

	if (eigrp_instance_thread_dispatch_needed(runtime))
		return eigrp_instance_message_call(runtime,
			eigrp_instance_control_message_process, &message);
	eigrp_af_config_t *af;
	eigrp_intf_config_t *config;
	eigrp_intf_t *ei;
	eigrp_list_item_t *node;

	if (!runtime)
		return EIGRP_RESULT_NOT_FOUND;
	if (runtime->router_id.s_addr == INADDR_ANY)
		eigrp_router_id_update(runtime);
	if (runtime->router_id.s_addr == INADDR_ANY) {
		if (eigrp_instance_afi(runtime) == EIGRP_AFI_IPV6)
			eigrp_log(EIGRP_LOG_WARNING,
				  "EIGRP: Ignored HELLO, no routerid for IPv6 AS(%u)",
				  runtime->AS);
		return EIGRP_RESULT_SUCCESS;
	}

	/* Re-read host interface state before deciding which EIGRP interfaces can
	 * run.  The host adapter only enumerates and normalizes interface state.
	 */
	eigrp_network_intfs_update(runtime);
	af = eigrp_af_config_runtime_read(runtime);
	for (EIGRP_LIST_ITERATE_RO(runtime->eiflist, node, ei)) {
		config = af ? eigrp_intf_config_read(af, ei->name) : NULL;
		if (!config && af)
			config = eigrp_intf_config_read(af, "default");
		if (config)
			eigrp_intf_config_update(ei, config);
		if (eigrp_intf_shutdown_effective(ei)
		    || !ei->operative || ei->t_hello)
			continue;
		eigrp_intf_up(runtime, ei);
	}
	return EIGRP_RESULT_SUCCESS;
}


typedef struct eigrp_instance_router_id_message_args {
	eigrp_operation_t operation;
	eigrp_instance_context_t *context;
	uint32_t router_id;
} eigrp_instance_router_id_message_args_t;

static eigrp_result_t eigrp_instance_router_id_message_process(
	eigrp_instance_t *eigrp, void *arg);

/*
 * Syntax:
 *   Classic: `eigrp router-id A.B.C.D` / `no eigrp router-id [A.B.C.D]`
 *   Named: `eigrp router-id A.B.C.D` / `no eigrp router-id [A.B.C.D]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: address-family mode
 * Description:
 * Sets or resets the 32-bit EIGRP router ID for the selected instance.
 * Named mode reaches the same EIGRP-owned router-ID behavior instead of duplicating protocol state in the FRR CLI.
 */
eigrp_result_t eigrp_instance_router_id_update(eigrp_operation_t operation, eigrp_instance_context_t *context, uint32_t router_id)
{
	eigrp_instance_router_id_message_args_t message = { operation, context, router_id };

	if (context && eigrp_instance_thread_dispatch_needed(context->runtime))
		return eigrp_instance_message_call(context->runtime,
			eigrp_instance_router_id_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (context->config) {
		context->config->router_id = 0;
		context->config->router_id_configured = false;
	}
	if (context->runtime) {
		context->runtime->router_id_static.s_addr = INADDR_ANY;
		eigrp_instance_router_id_runtime_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (!context || (!context->config && !context->runtime))
		return EIGRP_RESULT_NOT_FOUND;
	if (router_id == 0 || router_id == UINT32_MAX)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (context->config) {
		context->config->router_id = router_id;
		context->config->router_id_configured = true;
	}
	if (context->runtime) {
		context->runtime->router_id_static.s_addr = htonl(router_id);
		eigrp_instance_router_id_runtime_update(context->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

/*
 * Syntax:
 *   Classic: `eigrp router-id A.B.C.D` / `no eigrp router-id [A.B.C.D]`
 *   Named: `eigrp router-id A.B.C.D` / `no eigrp router-id [A.B.C.D]`
 * Supported: Classic / Named
 * Placement:
 *   Classic: router mode
 *   Named: address-family mode
 * Description:
 * Sets or resets the 32-bit EIGRP router ID for the selected instance.
 * Named mode reaches the same EIGRP-owned router-ID behavior instead of duplicating protocol state in the FRR CLI.
 */


static eigrp_result_t eigrp_instance_router_id_message_process(
	eigrp_instance_t *eigrp, void *arg)
{
	eigrp_instance_router_id_message_args_t *message = arg;
	(void)eigrp;
	return eigrp_instance_router_id_update(message->operation, message->context,
		message->router_id);
}

/*
 * Syntax:
 *   Named: `shutdown` / `no shutdown`
 * Supported: Named
 * Placement:
 *   Named: address-family mode
 * Description:
 * Changes the administrative state of one named address-family.
 * The common target owns retained state and runtime start/stop behavior.
 */
eigrp_result_t eigrp_af_config_shutdown_update(eigrp_operation_t operation, eigrp_af_config_t *af)
{
	eigrp_instance_control_message_args_t message = {
		.type = EIGRP_INSTANCE_CONTROL_AF_SHUTDOWN,
		.operation = operation, .af = af,
	};

	if (af && eigrp_instance_thread_dispatch_needed(af->runtime))
		return eigrp_instance_message_call(af->runtime,
			eigrp_instance_control_message_process, &message);
	bool shutdown;

	if (operation != EIGRP_SET && operation != EIGRP_RESET)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	shutdown = operation == EIGRP_SET;
	eigrp_result_t result;

	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	if (af->shutdown == shutdown)
		return EIGRP_RESULT_SUCCESS;

	/* Commit retained configuration first, then change runtime state. */
	af->shutdown = shutdown;
	if (!af->runtime)
		return af->afi == EIGRP_AFI_IPV6
		       ? EIGRP_RESULT_SUCCESS : EIGRP_RESULT_NOT_FOUND;

	result = shutdown
		 ? eigrp_instance_stop(af->runtime)
		 : eigrp_instance_start(af->runtime);
	if (result != EIGRP_RESULT_SUCCESS)
		af->shutdown = !shutdown;
	return result;
}







/*
 * Syntax:
 *   Named: `shutdown` / `no shutdown`
 * Supported: Named
 * Placement:
 *   Named: router EIGRP parent mode
 * Description:
 * Represents administrative shutdown of the named parent rather than one address-family.
 * Retained parent state is committed first.  Each child keeps its own shutdown
 * state; parent no-shutdown therefore restarts only administratively enabled
 * child address families.
 */
eigrp_result_t eigrp_named_config_shutdown_update(eigrp_operation_t operation, eigrp_named_config_t *parent)
{
	eigrp_af_config_t *af;
	eigrp_result_t result;
	eigrp_result_t aggregate = EIGRP_RESULT_SUCCESS;
	bool shutdown;

	if (operation != EIGRP_SET && operation != EIGRP_RESET)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!parent)
		return EIGRP_RESULT_NOT_FOUND;

	shutdown = operation == EIGRP_SET;
	if (parent->shutdown == shutdown)
		return EIGRP_RESULT_SUCCESS;

	/* Parent configuration is retained even when one child data path is absent. */
	parent->shutdown = shutdown;
	for (af = parent->address_families; af; af = af->next) {
		if (!af->runtime || (!shutdown && af->shutdown))
			continue;
		result = shutdown ? eigrp_instance_stop(af->runtime)
				  : eigrp_instance_start(af->runtime);
		if (result == EIGRP_RESULT_SUCCESS)
			continue;
		/* Keep deterministic retained state; report the first failure. */
		if (aggregate == EIGRP_RESULT_SUCCESS)
			aggregate = result;
	}
	return aggregate;
}







/*
 * Syntax:
 *   Named: `distance eigrp INTERNAL EXTERNAL` / `no distance eigrp`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets internal and external EIGRP administrative distance.
 * The runtime stores both values and replays installed routes so Zebra sees the new distance immediately.
 */
eigrp_result_t eigrp_af_config_distance_update(eigrp_operation_t operation, eigrp_af_config_t *af, uint8_t internal_distance, uint8_t external_distance)
{
	eigrp_instance_control_message_args_t message = {
		.type = EIGRP_INSTANCE_CONTROL_DISTANCE, .operation = operation,
		.af = af, .internal_distance = internal_distance,
		.external_distance = external_distance,
	};

	if (af && eigrp_instance_thread_dispatch_needed(af->runtime))
		return eigrp_instance_message_call(af->runtime,
			eigrp_instance_control_message_process, &message);
	if (operation == EIGRP_RESET) {
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	if (af->runtime) {
		af->runtime->distance_internal = EIGRP_DISTANCE_INTERNAL_DEFAULT;
		af->runtime->distance_external = EIGRP_DISTANCE_EXTERNAL_DEFAULT;
		(void)eigrp_rib_routes_replay_instance(af->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
	}

	if (operation != EIGRP_SET)
		return EIGRP_RESULT_INVALID_ARGUMENT;

	if (!internal_distance || !external_distance)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	if (!af)
		return EIGRP_RESULT_NOT_FOUND;
	if (af->runtime) {
		af->runtime->distance_internal = internal_distance;
		af->runtime->distance_external = external_distance;
		(void)eigrp_rib_routes_replay_instance(af->runtime);
	}
	return EIGRP_RESULT_SUCCESS;
}

static eigrp_result_t eigrp_instance_control_message_process(
	eigrp_instance_t *eigrp, void *arg)
{
	eigrp_instance_control_message_args_t *message = arg;

	switch (message->type) {
	case EIGRP_INSTANCE_CONTROL_START:
		return eigrp_instance_start(eigrp);
	case EIGRP_INSTANCE_CONTROL_STOP:
		return eigrp_instance_stop(eigrp);
	case EIGRP_INSTANCE_CONTROL_AF_SHUTDOWN:
		return eigrp_af_config_shutdown_update(message->operation, message->af);
	case EIGRP_INSTANCE_CONTROL_DISTANCE:
		return eigrp_af_config_distance_update(message->operation, message->af,
			message->internal_distance, message->external_distance);
	}
	return EIGRP_RESULT_INVALID_ARGUMENT;
}



/*
 * Syntax:
 *   Named: `distance eigrp INTERNAL EXTERNAL` / `no distance eigrp`
 * Supported: Named
 * Placement:
 *   Named: topology base mode
 * Description:
 * Sets or resets internal and external EIGRP administrative distance.
 * The runtime stores both values and replays installed routes so Zebra sees the new distance immediately.
 */


void eigrp_named_config_delete_all(void)
{
	eigrp_named_config_t *parent;
	eigrp_named_config_t *next;
	eigrp_af_config_t *af;
	eigrp_af_config_t *af_next;

	for (parent = eigrp_named_configs; parent; parent = next) {
		next = parent->next;
		for (af = parent->address_families; af; af = af_next) {
			af_next = af->next;
			(void)eigrp_af_config_runtime_delete(
				parent->name, af);
			eigrp_af_config_free(af);
		}
		if (parent->runtime)
			(void)eigrp_virt_router_delete(parent->runtime);
		free(parent->name);
		free(parent);
	}
	eigrp_named_configs = NULL;
}
