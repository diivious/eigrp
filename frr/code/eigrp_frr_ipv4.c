// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP FRR IPv4 integration.
 *
 * FRR-specific IPv4 northbound value adaptation and southbound socket,
 * multicast, and packet I/O live together here.  Public EIGRP API names
 * remain owned by the portable core interfaces they implement.
 *
 * Copyright (C) 2026 Donnie V. Savage
 */

#include <zebra.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eigrp.h"
#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_network.h"
#include "eigrp_neighbor.h"
#include "eigrp_auth.h"
#include "eigrp_cli.h"
#include "eigrp_filter.h"
#include "eigrp_eventlog.h"
#include "eigrp_instance.h"
#include "eigrp_metric.h"
#include "eigrp_summary.h"
#include "eigrp_sys.h"
#include "eigrp_rib.h"
#include "eigrp_timer.h"
#include "eigrp_topology.h"
#include "eigrp_packet.h"
#include "eigrp_frr_rib.h"
#include "eigrp_frr_classic.h"
#include "eigrp_frr_named.h"
#include "eigrp_northbound.h"
#include "eigrp_southbound.h"
#include "eigrp_frr.h"
#include "eigrp_frr_policy.h"

#include "lib/keychain.h"
#include "lib/distribute.h"
#include "lib/northbound.h"
#include "lib/sockopt.h"
#include "lib/zclient.h"
#include "vrf.h"

static void eigrp_northbound_ipv4_redistribute_metrics_get(
	const struct lyd_node *dnode, eigrp_metrics_t *metrics)
{
	memset(metrics, 0, sizeof(*metrics));

	if (yang_dnode_exists(dnode, "./bandwidth"))
		metrics->bandwidth = yang_dnode_get_uint32(dnode, "./bandwidth");
	if (yang_dnode_exists(dnode, "./delay"))
		metrics->delay = yang_dnode_get_uint32(dnode, "./delay");
	if (yang_dnode_exists(dnode, "./load"))
		metrics->load = yang_dnode_get_uint32(dnode, "./load");
	if (yang_dnode_exists(dnode, "./reliability"))
		metrics->reliability = yang_dnode_get_uint32(dnode, "./reliability");
}

static eigrp_intf_t *
eigrp_northbound_ipv4_interface_lookup_host(const struct interface *ifp)
{
	eigrp_vrf_id_t vrf_id;

	if (!ifp)
		return NULL;
	vrf_id = ifp->vrf ? (eigrp_vrf_id_t)ifp->vrf->vrf_id
			     : EIGRP_VRF_DEFAULT;
	return eigrp_intf_lookup_by_vrf_ifindex(vrf_id, ifp->ifindex);
}

bool eigrp_northbound_ipv4_neighbor_address_copy(
	eigrp_address_t *destination, const struct in_addr *address)
{
	if (!destination || !address)
		return false;

	memset(destination, 0, sizeof(*destination));
	destination->afi = EIGRP_AFI_IPV4;
	memcpy(destination->bytes, address, sizeof(*address));
	return true;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance
 * Target: eigrp_instance_classic_create()
 * Description:
 * This is the `create` northbound callback for the `classic EIGRP instance` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_instance_classic_create()` rather than duplicating EIGRP behavior in the
 * FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_create(struct nb_cb_create_args *args)
{
	eigrp_instance_t *eigrp = NULL;
	eigrp_vrf_id_t vrf_id = EIGRP_VRF_DEFAULT;
	eigrp_result_t result;
	const char *owner_name = NULL;
	const char *vrf;
	struct vrf *host_vrf;
	uint16_t asn;

	vrf = yang_dnode_get_string(args->dnode, "./vrf");
	host_vrf = vrf_lookup_by_name(vrf);
	if (host_vrf)
		vrf_id = (eigrp_vrf_id_t)host_vrf->vrf_id;
	asn = yang_dnode_get_uint16(args->dnode, "./asn");

	switch (args->event) {
	case NB_EV_VALIDATE:
		result = eigrp_instance_classic_validate(asn, vrf_id, &owner_name);
		if (result == EIGRP_RESULT_CONFLICT) {
			snprintf(args->errmsg, args->errmsg_len,
				 "EIGRP AS %u in VRF %s is owned by named process %s",
				 asn, vrf, owner_name ? owner_name : "<unknown>");
			return NB_ERR_VALIDATION;
		}
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
		result = eigrp_instance_classic_create(asn, vrf_id, &eigrp);
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_RESOURCE;
		args->resource->ptr = eigrp;
		break;
	case NB_EV_ABORT:
		(void)eigrp_instance_classic_delete(args->resource->ptr);
		args->resource->ptr = NULL;
		break;
	case NB_EV_APPLY:
		nb_running_set_entry(args->dnode, args->resource->ptr);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance
 * Target: eigrp_instance_classic_delete()
 * Description:
 * This is the `destroy` northbound callback for the `classic EIGRP instance` configuration
 * node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_instance_classic_delete()` rather than duplicating EIGRP behavior
 * in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_unset_entry(args->dnode);
		if (eigrp_instance_classic_delete(eigrp) != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/router-id
 * Target: eigrp_instance_router_id_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `router id` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_instance_router_id_update(EIGRP_SET)` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_router_id_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};
	struct in_addr router_id;
	eigrp_result_t result;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		yang_dnode_get_ipv4(&router_id, args->dnode, NULL);
		context.runtime = eigrp;
		result = eigrp_instance_router_id_update(EIGRP_SET, &context, ntohl(router_id.s_addr));
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/router-id
 * Target: eigrp_instance_router_id_update(EIGRP_RESET, 0)
 * Description:
 * This is the `destroy` northbound callback for the `router id` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_instance_router_id_update(EIGRP_RESET, 0)` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_router_id_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};
	eigrp_result_t result;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		context.runtime = eigrp;
		result = eigrp_instance_router_id_update(EIGRP_RESET, &context, 0);
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/passive-interface
 * Target: eigrp_intf_passive_update(EIGRP_SET)
 * Description:
 * This is the `create` northbound callback for the `passive interface` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_intf_passive_update(EIGRP_SET)` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_passive_interface_create(struct nb_cb_create_args *args)
{
	eigrp_intf_t *intf;
	eigrp_instance_t *eigrp;
	eigrp_intf_context_t context = {0};
	const char *ifname;

	switch (args->event) {
	case NB_EV_VALIDATE:
		eigrp = nb_running_get_entry(args->dnode, NULL, false);
		if (eigrp == NULL)
			break;
		ifname = yang_dnode_get_string(args->dnode, NULL);
		intf = eigrp_intf_lookup_by_name(eigrp, ifname);
		if (intf == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		ifname = yang_dnode_get_string(args->dnode, NULL);
		intf = eigrp_intf_lookup_by_name(eigrp, ifname);
		if (intf == NULL)
			return NB_ERR_INCONSISTENCY;
		context.runtime = intf;
		if (eigrp_intf_passive_update(EIGRP_SET, &context)
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/passive-interface
 * Target: eigrp_intf_passive_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `passive interface` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_intf_passive_update(EIGRP_SET)` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_passive_interface_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_intf_t *intf;
	eigrp_instance_t *eigrp;
	eigrp_intf_context_t context = {0};
	const char *ifname;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		ifname = yang_dnode_get_string(args->dnode, NULL);
		intf = eigrp_intf_lookup_by_name(eigrp, ifname);
		if (intf == NULL)
			break;
		context.runtime = intf;
		if (eigrp_intf_passive_update(EIGRP_RESET, &context)
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/active-time
 * Target: none; classic runtime unsupported
 * Description:
 * This is the `modify` northbound callback for the `active time` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The target is `none; classic runtime unsupported` and the callback does not invent protocol
 * behavior that the classic runtime does not implement.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_active_time_modify(struct nb_cb_modify_args *args)
{
	if (args->event == NB_EV_VALIDATE) {
		snprintf(args->errmsg, args->errmsg_len,
			 "classic EIGRP active-time configuration is unsupported");
		return NB_ERR_VALIDATION;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/variance
 * Target: eigrp_metric_variance_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `variance` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_metric_variance_update(EIGRP_SET)` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_variance_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		context.runtime = eigrp;
		if (eigrp_metric_variance_update(EIGRP_SET, &context, yang_dnode_get_uint8(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/variance
 * Target: eigrp_metric_variance_update(EIGRP_RESET, 0)
 * Description:
 * This is the `destroy` northbound callback for the `variance` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_metric_variance_update(EIGRP_RESET, 0)` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_variance_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		context.runtime = eigrp;
		if (eigrp_metric_variance_update(EIGRP_RESET, &context, 0) != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/maximum-paths
 * Target: eigrp_topology_maximum_paths_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `maximum paths` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_topology_maximum_paths_update(EIGRP_SET)` rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_maximum_paths_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	memset(&context, 0, sizeof(context));
	context.runtime = eigrp;
	return eigrp_topology_maximum_paths_update(EIGRP_SET, &context, yang_dnode_get_uint8(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/maximum-paths
 * Target: eigrp_topology_maximum_paths_update(EIGRP_RESET, 0)
 * Description:
 * This is the `destroy` northbound callback for the `maximum paths` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_topology_maximum_paths_update(EIGRP_RESET, 0)` rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_maximum_paths_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	memset(&context, 0, sizeof(context));
	context.runtime = eigrp;
	return eigrp_topology_maximum_paths_update(EIGRP_RESET, &context, 0)
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/event-log-size
 * Target: eigrp_eventlog_size_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `event log size` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_eventlog_size_update(EIGRP_SET)` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_event_log_size_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	context.runtime = eigrp;
	context.topology_id = EIGRP_TOPOLOGY_ID_BASE;
	return eigrp_eventlog_size_update(EIGRP_SET, &context, yang_dnode_get_uint32(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/event-log-size
 * Target: eigrp_eventlog_size_update(EIGRP_RESET, 0)
 * Description:
 * This is the `destroy` northbound callback for the `event log size` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_eventlog_size_update(EIGRP_RESET, 0)` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_event_log_size_destroy(
	struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	context.runtime = eigrp;
	context.topology_id = EIGRP_TOPOLOGY_ID_BASE;
	return eigrp_eventlog_size_update(EIGRP_RESET, &context, 0) == EIGRP_RESULT_SUCCESS
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static eigrp_result_t eigrpd_instance_metric_weight_update(
	eigrp_instance_t *eigrp, unsigned int index, uint8_t value)
{
	eigrp_instance_context_t context = {.runtime = eigrp};
	eigrp_metric_weights_t weights;

	if (!eigrp || index >= 6)
		return EIGRP_RESULT_INVALID_ARGUMENT;
	weights.tos = 0;
	weights.k1 = eigrp->k_values[0];
	weights.k2 = eigrp->k_values[1];
	weights.k3 = eigrp->k_values[2];
	weights.k4 = eigrp->k_values[3];
	weights.k5 = eigrp->k_values[4];
	weights.k6 = eigrp->k_values[5];
	switch (index) {
	case 0:
		weights.k1 = value;
		break;
	case 1:
		weights.k2 = value;
		break;
	case 2:
		weights.k3 = value;
		break;
	case 3:
		weights.k4 = value;
		break;
	case 4:
		weights.k5 = value;
		break;
	case 5:
		weights.k6 = value;
		break;
	default:
		return EIGRP_RESULT_INVALID_ARGUMENT;
	}
	return eigrp_metric_weights_update(EIGRP_SET, &context, &weights);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K1
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K1` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K1_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(
		       eigrp, 0, yang_dnode_get_uint8(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K1
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K1` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K1_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(eigrp, 0, EIGRP_K1_DEFAULT)
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K2
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K2` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K2_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(
		       eigrp, 1, yang_dnode_get_uint8(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K2
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K2` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K2_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(eigrp, 1, EIGRP_K2_DEFAULT)
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K3
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K3` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K3_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(
		       eigrp, 2, yang_dnode_get_uint8(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K3
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K3` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K3_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(eigrp, 2, EIGRP_K3_DEFAULT)
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K4
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K4` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K4_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(
		       eigrp, 3, yang_dnode_get_uint8(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K4
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K4` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K4_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(eigrp, 3, EIGRP_K4_DEFAULT)
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K5
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K5` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K5_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(
		       eigrp, 4, yang_dnode_get_uint8(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K5
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K5` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K5_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(eigrp, 4, EIGRP_K5_DEFAULT)
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K6
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K6` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K6_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(
		       eigrp, 5, yang_dnode_get_uint8(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K6
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K6` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_metric_weights_K6_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	return eigrpd_instance_metric_weight_update(eigrp, 5, EIGRP_K6_DEFAULT)
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/network
 * Target: eigrp_network_create()
 * Description:
 * This callback creates one classic IPv4 EIGRP network entry.
 * The YANG IPv4 prefix is converted to `eigrp_prefix_t` before it crosses into common EIGRP code.
 * VALIDATE uses `eigrp_network_runtime_exists()` so a duplicate runtime entry is rejected before APPLY.
 * APPLY resolves the owning instance and calls `eigrp_network_create()` with an EIGRP-owned instance context.
 * Named IPv4 `network` reaches the same `eigrp_network_create()` target, so classic and named mode converge below northbound.
 * Prefix conversion or common-target failures return `NB_ERR_INCONSISTENCY` instead of being hidden by the FRR adapter.
 */
int eigrpd_instance_network_create(struct nb_cb_create_args *args)
{
	eigrp_instance_context_t context = {0};
	eigrp_prefix_t network;
	struct prefix prefix;
	eigrp_instance_t *eigrp;
	eigrp_result_t result;
	bool exists;

	yang_dnode_get_ipv4p(&prefix, args->dnode, NULL);
	if (eigrp_frr_prefix_import(&prefix, &network) != EIGRP_RESULT_SUCCESS)
		return NB_ERR_INCONSISTENCY;

	switch (args->event) {
	case NB_EV_VALIDATE:
		eigrp = nb_running_get_entry(args->dnode, NULL, false);
		/* If entry doesn't exist it means the list is empty. */
		if (eigrp == NULL)
			break;

		result = eigrp_network_runtime_exists(eigrp, &network, &exists);
		if (result != EIGRP_RESULT_SUCCESS || exists)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		context.runtime = eigrp;
		result = eigrp_network_create(&context, &network);
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/network
 * Target: eigrp_network_delete()
 * Description:
 * This callback removes one classic IPv4 EIGRP network entry.
 * The YANG IPv4 prefix is converted to `eigrp_prefix_t` before it crosses into common EIGRP code.
 * VALIDATE uses `eigrp_network_runtime_exists()` so a missing runtime entry is detected before APPLY.
 * APPLY resolves the owning instance and calls `eigrp_network_delete()` with an EIGRP-owned instance context.
 * Named IPv4 `network` reaches the same `eigrp_network_delete()` target, so classic and named mode converge below northbound.
 * A final `NOT_FOUND` is treated as already removed, while conversion and other target failures return `NB_ERR_INCONSISTENCY`.
 */
int eigrpd_instance_network_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_context_t context = {0};
	eigrp_prefix_t network;
	struct prefix prefix;
	eigrp_instance_t *eigrp;
	eigrp_result_t result;
	bool exists;

	yang_dnode_get_ipv4p(&prefix, args->dnode, NULL);
	if (eigrp_frr_prefix_import(&prefix, &network) != EIGRP_RESULT_SUCCESS)
		return NB_ERR_INCONSISTENCY;

	switch (args->event) {
	case NB_EV_VALIDATE:
		eigrp = nb_running_get_entry(args->dnode, NULL, false);
		/* If entry doesn't exist it means the list is empty. */
		if (eigrp == NULL)
			break;

		result = eigrp_network_runtime_exists(eigrp, &network, &exists);
		if (result != EIGRP_RESULT_SUCCESS || !exists)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		context.runtime = eigrp;
		result = eigrp_network_delete(&context, &network);
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/neighbor
 * Target: none; classic static-neighbor runtime unsupported
 * Description:
 * This is the `create` northbound callback for the `neighbor` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The target is `none; classic static-neighbor runtime unsupported` and the callback does not
 * invent protocol behavior that the classic runtime does not implement.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_neighbor_create(struct nb_cb_create_args *args)
{
	if (args->event == NB_EV_VALIDATE) {
		snprintf(args->errmsg, args->errmsg_len,
			 "classic EIGRP static-neighbor configuration is unsupported");
		return NB_ERR_VALIDATION;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/neighbor
 * Target: stale configuration cleanup only
 * Description:
 * This is the `destroy` northbound callback for the `neighbor` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The target is `stale configuration cleanup only` and the callback does not invent protocol
 * behavior that the classic runtime does not implement.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_neighbor_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	/* Permit deletion of stale classic configuration if it already exists. */
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/distribute-list
 * Target: eigrp_policy_distribute_context()
 * Description:
 * This is the `create` northbound callback for the `distribute list` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_policy_distribute_context()` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrp_northbound_distribute_list_create(
	struct nb_cb_create_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;

	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	if (!eigrp || !eigrp_policy_distribute_context(eigrp))
		return NB_ERR_INCONSISTENCY;
	group_distribute_list_create_helper(
		args, eigrp_policy_distribute_context(eigrp));

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute
 * Target: eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `create` northbound callback for the `redistribute` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_create(struct nb_cb_create_args *args)
{
	eigrp_metrics_t metrics;
	const char *vrfname;
	eigrp_instance_t *eigrp;
	uint32_t proto;
	vrf_id_t vrfid;
	struct vrf *pVrf;

	switch (args->event) {
	case NB_EV_VALIDATE:
		proto = yang_dnode_get_enum(args->dnode, "./protocol");
		vrfname = yang_dnode_get_string(args->dnode, "../vrf");

		pVrf = vrf_lookup_by_name(vrfname);
		if (pVrf)
			vrfid = pVrf->vrf_id;
		else
			vrfid = VRF_DEFAULT;

		if (vrf_bitmap_check(&eigrp_zclient->redist[AFI_IP][proto], vrfid))
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		proto = yang_dnode_get_enum(args->dnode, "./protocol");
		eigrp_northbound_ipv4_redistribute_metrics_get(args->dnode, &metrics);
		eigrp_redistribute_update(EIGRP_SET, eigrp, proto, metrics);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute
 * Target: eigrp_redistribute_update(EIGRP_RESET)
 * Description:
 * This is the `destroy` northbound callback for the `redistribute` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_redistribute_update(EIGRP_RESET)` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;
	uint32_t proto;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		proto = yang_dnode_get_enum(args->dnode, "./protocol");
		eigrp_redistribute_update(EIGRP_RESET, eigrp, proto, (struct eigrp_metrics){0});
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/route-map
 * Target: none; classic redistribute route-map runtime unsupported
 * Description:
 * This is the `modify` northbound callback for the `route map` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The target is `none; classic redistribute route-map runtime unsupported` and the callback
 * does not invent protocol behavior that the classic runtime does not implement.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_redistribute_route_map_modify(struct nb_cb_modify_args *args)
{
	if (args->event == NB_EV_VALIDATE) {
		snprintf(args->errmsg, args->errmsg_len,
			 "classic EIGRP redistribute route-map configuration is unsupported");
		return NB_ERR_VALIDATION;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/route-map
 * Target: stale configuration cleanup only
 * Description:
 * This is the `destroy` northbound callback for the `route map` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The target is `stale configuration cleanup only` and the callback does not invent protocol
 * behavior that the classic runtime does not implement.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_redistribute_route_map_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	/* Permit deletion of stale classic configuration if it already exists. */
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/bandwidth
 * Target: eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `bandwidth` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_metrics_bandwidth_modify(
	struct nb_cb_modify_args *args)
{
	eigrp_metrics_t metrics;
	eigrp_instance_t *eigrp;
	uint32_t proto;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		proto = yang_dnode_get_enum(args->dnode, "../../protocol");
		eigrp_northbound_ipv4_redistribute_metrics_get(args->dnode, &metrics);
		eigrp_redistribute_update(EIGRP_SET, eigrp, proto, metrics);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/bandwidth
 * Target: eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `bandwidth` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_metrics_bandwidth_destroy(
	struct nb_cb_destroy_args *args)
{
	eigrp_metrics_t metrics;
	eigrp_instance_t *eigrp;
	uint32_t proto;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		proto = yang_dnode_get_enum(args->dnode, "../../protocol");
		eigrp_northbound_ipv4_redistribute_metrics_get(args->dnode, &metrics);
		eigrp_redistribute_update(EIGRP_SET, eigrp, proto, metrics);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/delay
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_modify() -> eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `delay` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_modify() ->
 * eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_metrics_delay_modify(
	struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/delay
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_destroy() -> eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `delay` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_destroy() ->
 * eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_metrics_delay_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/reliability
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_modify() -> eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `reliability` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_modify() ->
 * eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_metrics_reliability_modify(
	struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/reliability
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_destroy() -> eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `reliability` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_destroy() ->
 * eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_metrics_reliability_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/load
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_modify() -> eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `load` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_modify() ->
 * eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_redistribute_metrics_load_modify(struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/load
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_destroy() -> eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `load` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_destroy() ->
 * eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_metrics_load_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/mtu
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_modify() -> eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `mtu` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_modify() ->
 * eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
eigrpd_instance_redistribute_metrics_mtu_modify(struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/mtu
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_destroy() -> eigrp_redistribute_update(EIGRP_SET)
 * Description:
 * This is the `destroy` northbound callback for the `mtu` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_destroy() ->
 * eigrp_redistribute_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int eigrpd_instance_redistribute_metrics_mtu_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/delay
 * Target: eigrp_intf_delay_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `delay` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_intf_delay_update(EIGRP_SET)` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int lib_interface_eigrp_delay_modify(struct nb_cb_modify_args *args)
{
	eigrp_intf_context_t context = {0};
	eigrp_intf_t *ei;
	struct interface *ifp;

	switch (args->event) {
	case NB_EV_VALIDATE:
		ifp = nb_running_get_entry(args->dnode, NULL, false);
		if (ifp == NULL) {
			/*
			 * XXX: we can't verify if the interface exists
			 * and is active until EIGRP is up.
			 */
			break;
		}

		ei = eigrp_northbound_ipv4_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = eigrp_northbound_ipv4_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;

		context.runtime = ei;
		if (eigrp_intf_delay_update(EIGRP_SET, &context, yang_dnode_get_uint32(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/bandwidth
 * Target: eigrp_intf_bandwidth_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `bandwidth` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_intf_bandwidth_update(EIGRP_SET)` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int lib_interface_eigrp_bandwidth_modify(struct nb_cb_modify_args *args)
{
	eigrp_intf_context_t context = {0};
	struct interface *ifp;
	eigrp_intf_t *ei;

	switch (args->event) {
	case NB_EV_VALIDATE:
		ifp = nb_running_get_entry(args->dnode, NULL, false);
		if (ifp == NULL) {
			/*
			 * XXX: we can't verify if the interface exists
			 * and is active until EIGRP is up.
			 */
			break;
		}

		ei = eigrp_northbound_ipv4_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = eigrp_northbound_ipv4_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;

		context.runtime = ei;
		if (eigrp_intf_bandwidth_update(EIGRP_SET, &context, yang_dnode_get_uint32(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hello-interval
 * Target: eigrp_intf_hello_interval_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `hello interval` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_intf_hello_interval_update(EIGRP_SET)` rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
lib_interface_eigrp_hello_interval_modify(struct nb_cb_modify_args *args)
{
	struct interface *ifp;
	eigrp_intf_t *ei;
	eigrp_intf_context_t context = {0};

	switch (args->event) {
	case NB_EV_VALIDATE:
		ifp = nb_running_get_entry(args->dnode, NULL, false);
		if (ifp == NULL)
			break;
		ei = eigrp_northbound_ipv4_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = eigrp_northbound_ipv4_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		context.runtime = ei;
		if (eigrp_intf_hello_interval_update(EIGRP_SET, &context, yang_dnode_get_uint16(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hold-time
 * Target: eigrp_intf_hold_time_update(EIGRP_SET)
 * Description:
 * This is the `modify` northbound callback for the `hold time` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_intf_hold_time_update(EIGRP_SET)` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int lib_interface_eigrp_hold_time_modify(struct nb_cb_modify_args *args)
{
	struct interface *ifp;
	eigrp_intf_t *ei;
	eigrp_intf_context_t context = {0};

	switch (args->event) {
	case NB_EV_VALIDATE:
		ifp = nb_running_get_entry(args->dnode, NULL, false);
		if (ifp == NULL)
			break;
		ei = eigrp_northbound_ipv4_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = eigrp_northbound_ipv4_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		context.runtime = ei;
		if (eigrp_intf_hold_time_update(EIGRP_SET, &context, yang_dnode_get_uint16(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/split-horizon
 * Target: none; classic split-horizon runtime unsupported
 * Description:
 * This is the `modify` northbound callback for the `split horizon` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The target is `none; classic split-horizon runtime unsupported` and the callback does not
 * invent protocol behavior that the classic runtime does not implement.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
lib_interface_eigrp_split_horizon_modify(struct nb_cb_modify_args *args)
{
	if (args->event == NB_EV_VALIDATE) {
		snprintf(args->errmsg, args->errmsg_len,
			 "classic EIGRP interface split-horizon configuration is unsupported");
		return NB_ERR_VALIDATION;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance
 * Target: eigrp_instance_classic_read()
 * Description:
 * This is the `create` northbound callback for the `classic EIGRP instance` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_instance_classic_read()` rather than duplicating EIGRP behavior in the
 * FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int lib_interface_eigrp_instance_create(struct nb_cb_create_args *args)
{
	eigrp_intf_t *intf;
	struct interface *ifp;
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
		ifp = nb_running_get_entry(args->dnode, NULL, false);
		if (ifp == NULL) {
			/*
			 * XXX: we can't verify if the interface exists
			 * and is active until EIGRP is up.
			 */
			break;
		}

		eigrp = eigrp_instance_classic_read(
			yang_dnode_get_uint16(args->dnode, "./asn"),
			ifp->vrf ? (eigrp_vrf_id_t)ifp->vrf->vrf_id
				 : EIGRP_VRF_DEFAULT);
		intf = eigrp ? eigrp_intf_lookup_by_name(eigrp, ifp->name) : NULL;
		if (intf == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp = eigrp_instance_classic_read(
			yang_dnode_get_uint16(args->dnode, "./asn"),
			ifp->vrf ? (eigrp_vrf_id_t)ifp->vrf->vrf_id
				 : EIGRP_VRF_DEFAULT);
		intf = eigrp ? eigrp_intf_lookup_by_name(eigrp, ifp->name) : NULL;
		if (intf == NULL)
			return NB_ERR_INCONSISTENCY;

		nb_running_set_entry(args->dnode, intf);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance
 * Target: nb_running_unset_entry()
 * Description:
 * This is the `destroy` northbound callback for the `classic EIGRP instance` configuration
 * node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `nb_running_unset_entry()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int lib_interface_eigrp_instance_destroy(struct nb_cb_destroy_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		nb_running_unset_entry(args->dnode);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/summarize-addresses
 * Target: none; classic summary runtime unsupported
 * Description:
 * This is the `create` northbound callback for the `summarize addresses` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The target is `none; classic summary runtime unsupported` and the callback does not invent
 * protocol behavior that the classic runtime does not implement.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int lib_interface_eigrp_instance_summarize_addresses_create(
	struct nb_cb_create_args *args)
{
	if (args->event == NB_EV_VALIDATE) {
		snprintf(args->errmsg, args->errmsg_len,
			 "classic EIGRP interface summary configuration is unsupported");
		return NB_ERR_VALIDATION;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/summarize-addresses
 * Target: stale configuration cleanup only
 * Description:
 * This is the `destroy` northbound callback for the `summarize addresses` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The target is `stale configuration cleanup only` and the callback does not invent protocol
 * behavior that the classic runtime does not implement.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int lib_interface_eigrp_instance_summarize_addresses_destroy(
	struct nb_cb_destroy_args *args)
{
	(void)args;
	/* Permit deletion of stale classic configuration if it already exists. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/authentication
 * Target: eigrp_auth_mode_update(EIGRP_SET)/eigrp_auth_mode_update(EIGRP_RESET, 0, 0)
 * Description:
 * This is the `modify` northbound callback for the `authentication` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at the EIGRP-owned authentication target rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int lib_interface_eigrp_instance_authentication_modify(
	struct nb_cb_modify_args *args)
{
	eigrp_intf_context_t context = {0};
	eigrp_authentication_mode_t mode;
	eigrp_intf_t *intf;
	const char *mode_text;
	eigrp_result_t result;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		intf = nb_running_get_entry(args->dnode, NULL, true);
		if (!intf)
			return NB_ERR_INCONSISTENCY;
		context.runtime = intf;
		mode_text = yang_dnode_get_string(args->dnode, NULL);
		if (strcmp(mode_text, "none") == 0)
			result = eigrp_auth_mode_update(EIGRP_RESET, &context, 0, 0);
		else {
			mode = strcmp(mode_text, "md5") == 0
				       ? EIGRP_AUTHENTICATION_MD5
				       : EIGRP_AUTHENTICATION_HMAC_SHA256;
			result = eigrp_auth_mode_update(EIGRP_SET, &context, mode, NULL);
		}
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain
 * Target: eigrp_auth_keychain_update(EIGRP_SET)/eigrp_auth_keychain_update(EIGRP_RESET, 0)
 * Description:
 * This is the `modify` northbound callback for the `keychain` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at the EIGRP-owned key-chain target rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
lib_interface_eigrp_instance_keychain_modify(struct nb_cb_modify_args *args)
{
	eigrp_intf_context_t context = {0};
	eigrp_intf_t *intf;
	struct keychain *keychain;

	switch (args->event) {
	case NB_EV_VALIDATE:
		keychain = keychain_lookup(yang_dnode_get_string(args->dnode, NULL));
		if (!keychain)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		intf = nb_running_get_entry(args->dnode, NULL, true);
		if (!intf)
			return NB_ERR_INCONSISTENCY;
		context.runtime = intf;
		if (eigrp_auth_keychain_update(EIGRP_SET, &context, yang_dnode_get_string(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain
 * Target: eigrp_auth_keychain_update(EIGRP_SET)/eigrp_auth_keychain_update(EIGRP_RESET, 0)
 * Description:
 * This is the `destroy` northbound callback for the `keychain` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at the EIGRP-owned key-chain target rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
int
lib_interface_eigrp_instance_keychain_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_intf_context_t context = {0};
	eigrp_intf_t *intf;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		intf = nb_running_get_entry(args->dnode, NULL, true);
		if (!intf)
			return NB_ERR_INCONSISTENCY;
		context.runtime = intf;
		if (eigrp_auth_keychain_update(EIGRP_RESET, &context, 0) != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/* FRR IPv4 packet/socket services. */

static bool eigrp_southbound_ipv4_interface_address(
	const eigrp_intf_t *ei, struct in_addr *address)
{
	eigrp_prefix_t prefix;

	if (!ei || !address
	    || eigrp_intf_address_read(ei, &prefix) != EIGRP_RESULT_SUCCESS
	    || prefix.address.afi != EIGRP_AFI_IPV4)
		return false;
	memcpy(address, prefix.address.bytes, sizeof(*address));
	return true;
}

eigrp_result_t eigrp_southbound_ipv4_socket_configure(int fd)
{
	int ret;
#ifdef IP_HDRINCL
	int hincl = 1;

	ret = setsockopt(fd, IPPROTO_IP, IP_HDRINCL, &hincl, sizeof(hincl));
	if (ret < 0)
		zlog_warn("Can't set IP_HDRINCL option for fd %d: %s", fd,
			  safe_strerror(errno));
#elif defined(IPTOS_PREC_INTERNETCONTROL)
	ret = setsockopt_ipv4_tos(fd, IPTOS_PREC_INTERNETCONTROL);
	if (ret < 0) {
		zlog_warn("can't set EIGRP IP_TOS on socket %d: %s", fd,
			  safe_strerror(errno));
		return EIGRP_RESULT_INTERNAL_FAILURE;
	}
#else
	zlog_warn("IP_HDRINCL option not available");
#endif
	ret = setsockopt_ifindex(AF_INET, fd, 1);
	if (ret < 0)
		zlog_warn("Can't set pktinfo option for fd %d", fd);
	return EIGRP_RESULT_SUCCESS;
}

int eigrp_southbound_ipv4_multicast_interface_update(eigrp_operation_t operation,
                                                   eigrp_instance_t *eigrp,
                                                   eigrp_intf_t *ei)
{
	if (operation != EIGRP_SET)
		return -1;
	struct in_addr address;
	eigrp_ifindex_t ifindex;
	const char *name;
	uint8_t val = 0;
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	int ret;

	if (fd < 0)
		return -1;
	ifindex = eigrp_intf_ifindex(ei);
	if (!ifindex || !eigrp_southbound_ipv4_interface_address(ei, &address))
		return -1;
	name = eigrp_intf_name(ei);
	ret = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &val, sizeof(val));
	if (ret < 0)
		zlog_warn("can't disable IP_MULTICAST_LOOP for fd %d: %s", fd,
			  safe_strerror(errno));
	val = 1;
	ret = setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &val, sizeof(val));
	if (ret < 0)
		zlog_warn("can't set IP_MULTICAST_TTL for fd %d: %s", fd,
			  safe_strerror(errno));
	ret = setsockopt_ipv4_multicast_if(fd, address, ifindex);
	if (ret < 0)
		zlog_warn("can't set multicast interface %s[%u]: %s",
			  name ? name : "?", ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_southbound_ipv4_multicast_join(eigrp_instance_t *eigrp,
                                         eigrp_intf_t *ei)
{
	struct in_addr address;
	eigrp_ifindex_t ifindex = eigrp_intf_ifindex(ei);
	const char *name = eigrp_intf_name(ei);
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	int ret;

	if (fd < 0 || !eigrp_southbound_ipv4_interface_address(ei, &address))
		return -1;
	ret = setsockopt_ipv4_multicast(fd, IP_ADD_MEMBERSHIP, address,
					htonl(EIGRP_MULTICAST_ADDRESS), ifindex);
	if (ret < 0)
		zlog_warn("can't join EIGRP multicast group on %s[%u]: %s",
			  name ? name : "?", ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_southbound_ipv4_multicast_leave(eigrp_instance_t *eigrp,
                                          eigrp_intf_t *ei)
{
	struct in_addr address;
	eigrp_ifindex_t ifindex = eigrp_intf_ifindex(ei);
	const char *name = eigrp_intf_name(ei);
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	int ret;

	if (fd < 0 || !eigrp_southbound_ipv4_interface_address(ei, &address))
		return -1;
	ret = setsockopt_ipv4_multicast(fd, IP_DROP_MEMBERSHIP, address,
					htonl(EIGRP_MULTICAST_ADDRESS), ifindex);
	if (ret < 0)
		zlog_warn("can't leave EIGRP multicast group on %s[%u]: %s",
			  name ? name : "?", ifindex, safe_strerror(errno));
	return ret;
}

int eigrp_sys_ipv4_packet_send(eigrp_instance_t *eigrp,
			       eigrp_intf_t *ei,
			       const eigrp_address_t *destination,
			       const uint8_t *payload, size_t length)
{
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	eigrp_prefix_t local;
	struct sockaddr_in sa_dst;
	struct in_addr dst;
	struct ip iph;
	struct msghdr msg;
	struct iovec iov[2];
	struct cmsghdr *cmsg;
	struct in_pktinfo *pktinfo;
	eigrp_ifindex_t ifindex;
	char control[CMSG_SPACE(sizeof(struct in_pktinfo))];
	int flags = 0;
	int ret;

	if (fd < 0 || !ei || !destination || !payload
	    || destination->afi != EIGRP_AFI_IPV4
	    || eigrp_intf_address_read(ei, &local) != EIGRP_RESULT_SUCCESS)
		return -1;
	ifindex = eigrp_intf_ifindex(ei);
	if (!ifindex)
		return -1;
	memcpy(&dst, destination->bytes, sizeof(dst));
	if (dst.s_addr == htonl(EIGRP_MULTICAST_ADDRESS)
	    && eigrp_sys_multicast_interface_update(EIGRP_SET, eigrp, ei) < 0)
		return -1;
	memset(&iph, 0, sizeof(iph));
	memset(&sa_dst, 0, sizeof(sa_dst));
	memset(&msg, 0, sizeof(msg));
	memset(control, 0, sizeof(control));
	sa_dst.sin_family = AF_INET;
	sa_dst.sin_addr = dst;
	if (!IN_MULTICAST(ntohl(dst.s_addr)))
		flags = MSG_DONTROUTE;
	iph.ip_hl = sizeof(struct ip) / 4;
	iph.ip_v = IPVERSION;
	iph.ip_tos = IPTOS_PREC_INTERNETCONTROL;
	iph.ip_len = (uint16_t)(sizeof(struct ip) + length);
	iph.ip_ttl = EIGRP_IP_TTL;
	iph.ip_p = IPPROTO_EIGRPIGP;
	memcpy(&iph.ip_src, local.address.bytes, sizeof(iph.ip_src));
	iph.ip_dst = dst;
	msg.msg_name = &sa_dst;
	msg.msg_namelen = sizeof(sa_dst);
	msg.msg_iov = iov;
	msg.msg_iovlen = 2;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = IPPROTO_IP;
	cmsg->cmsg_type = IP_PKTINFO;
	cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
	pktinfo = (struct in_pktinfo *)CMSG_DATA(cmsg);
	pktinfo->ipi_ifindex = ifindex;
	memcpy(&pktinfo->ipi_spec_dst, local.address.bytes,
	       sizeof(pktinfo->ipi_spec_dst));
	msg.msg_controllen = CMSG_SPACE(sizeof(struct in_pktinfo));
	iov[0].iov_base = &iph;
	iov[0].iov_len = sizeof(iph);
	iov[1].iov_base = (void *)payload;
	iov[1].iov_len = length;
	sockopt_iphdrincl_swab_htosys(&iph);
	ret = sendmsg(fd, &msg, flags);
	sockopt_iphdrincl_swab_systoh(&iph);
	return ret;
}

bool eigrp_sys_ipv4_packet_receive(eigrp_instance_t *eigrp,
				   uint8_t *buffer, size_t capacity,
				   size_t *received_length,
				   eigrp_ifindex_t *ifindex,
				   eigrp_address_t *source,
				   eigrp_address_t *destination,
				   eigrp_packet_rx_meta_t *meta)
{
	int fd = eigrp_southbound_socket_fd_get(eigrp);
	struct ip *iph;
	struct iovec iov;
	struct msghdr msgh;
	uint16_t header_length, ip_length;
	ssize_t ret;
	char control[CMSG_SPACE(SOPT_SIZE_CMSG_IFINDEX_IPV4())];

	if (fd < 0 || !buffer || capacity == 0 || !received_length || !ifindex
	    || !source || !destination || !meta)
		return false;
	memset(&msgh, 0, sizeof(msgh));
	iov.iov_base = buffer;
	iov.iov_len = capacity;
	msgh.msg_iov = &iov;
	msgh.msg_iovlen = 1;
	msgh.msg_control = control;
	msgh.msg_controllen = sizeof(control);
	ret = recvmsg(fd, &msgh, 0);
	if (ret < (ssize_t)sizeof(struct ip) || (size_t)ret > capacity)
		return false;
	iph = (struct ip *)buffer;
	sockopt_iphdrincl_swab_systoh(iph);
	if (iph->ip_v != 4 || iph->ip_hl < 5)
		return false;
	header_length = (uint16_t)iph->ip_hl * 4U;
	if (header_length > (uint16_t)ret)
		return false;
	ip_length = iph->ip_len;
	if (ip_length != (uint16_t)ret
	    || ip_length < header_length + EIGRP_HEADER_LEN)
		return false;
	memset(source, 0, sizeof(*source));
	memset(destination, 0, sizeof(*destination));
	source->afi = EIGRP_AFI_IPV4;
	destination->afi = EIGRP_AFI_IPV4;
	memcpy(source->bytes, &iph->ip_src, sizeof(iph->ip_src));
	memcpy(destination->bytes, &iph->ip_dst, sizeof(iph->ip_dst));
	*ifindex = (eigrp_ifindex_t)getsockopt_ifindex(AF_INET, &msgh);
	{
		struct interface *ifp = if_lookup_by_index((ifindex_t)*ifindex,
			(vrf_id_t)eigrp_instance_vrf_id(eigrp));
		if (!ifp || !ifp->vrf)
			return false;
		meta->ingress_vrf_id = (eigrp_vrf_id_t)ifp->vrf->vrf_id;
	}
	*received_length = (size_t)ret;
	meta->network_header_length = header_length;
	meta->eigrp_length = ip_length - header_length;
	meta->destination_multicast =
		iph->ip_dst.s_addr == htonl(EIGRP_MULTICAST_ADDRESS);
	return true;
}

