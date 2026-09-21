// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * EIGRP daemon northbound implementation.
 *
 * Copyright (C) 2019 Network Device Education Foundation, Inc. ("NetDEF")
 *                    Rafael Zalamena
 */

#include "eigrpd.h"
#include "eigrp_structs.h"
#include "eigrp_interface.h"
#include "eigrp_network.h"
#include "eigrp_neighbor.h"
#include "eigrpd/eigrp_auth.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_eventlog.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_redistribute.h"
#include "eigrpd/eigrp_summary.h"
#include "eigrpd/eigrp_southbound.h"
#include "eigrpd/eigrp_timer.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrp_zebra.h"
#include "eigrp_cli_classic.h"
#include "eigrp_cli_named.h"
#include "eigrp_northbound.h"
#include "eigrp_frr.h"
#include "eigrp_policy.h"

#include "lib/keychain.h"
#include "lib/distribute.h"
#include "lib/northbound.h"
#include "lib/zclient.h"

/* Helper functions. */
static int eigrpd_named_config_result(eigrp_result_t result, bool removing);

static bool eigrp_northbound_neighbor_address_copy(
	eigrp_addr_t *destination, eigrp_address_family_t afi,
	const struct in_addr *ipv4_address,
	const struct in6_addr *ipv6_address)
{
	if (!destination)
		return false;

	memset(destination, 0, sizeof(*destination));
	if (afi == EIGRP_ADDRESS_FAMILY_IPV4 && ipv4_address) {
		destination->afi = AF_INET;
		destination->ip.v4 = *ipv4_address;
		return true;
	}
	if (afi == EIGRP_ADDRESS_FAMILY_IPV6 && ipv6_address) {
		destination->afi = AF_INET6;
		destination->ip.v6 = *ipv6_address;
		return true;
	}
	return false;
}

eigrp_result_t eigrp_northbound_neighbor_clear_address(
	eigrp_instance_t *runtime, eigrp_address_family_t afi,
	const struct in_addr *ipv4_address,
	const struct in6_addr *ipv6_address, bool soft,
	eigrp_neighbor_clear_cb callback, void *arg, size_t *affected_count)
{
	eigrp_addr_t address;
	eigrp_neighbor_clear_request_t request = {
		.address = &address,
		.soft = soft,
	};

	if (!eigrp_northbound_neighbor_address_copy(
		    &address, afi, ipv4_address, ipv6_address))
		return EIGRP_RESULT_INVALID_ARGUMENT;

	return eigrp_neighbor_clear(runtime, &request, callback, arg,
				    affected_count);
}
static void eigrpd_named_prefix_limit_get(const struct lyd_node *dnode,
                                          eigrp_prefix_limit_t *limit);
static void redistribute_get_metrics(const struct lyd_node *dnode,
				     eigrp_metrics_t *em)
{
	memset(em, 0, sizeof(*em));

	if (yang_dnode_exists(dnode, "./bandwidth"))
		em->bandwidth = yang_dnode_get_uint32(dnode, "./bandwidth");
	if (yang_dnode_exists(dnode, "./delay"))
		em->delay = yang_dnode_get_uint32(dnode, "./delay");
#if 0 /* TODO: How does MTU work? */
	if (yang_dnode_exists(dnode, "./mtu"))
		em->mtu[0] = yang_dnode_get_uint32(dnode, "./mtu");
#endif
	if (yang_dnode_exists(dnode, "./load"))
		em->load = yang_dnode_get_uint32(dnode, "./load");
	if (yang_dnode_exists(dnode, "./reliability"))
		em->reliability = yang_dnode_get_uint32(dnode, "./reliability");
}

static eigrp_interface_t *eigrp_interface_lookup_host(const struct interface *ifp)
{
	eigrp_vrf_id_t vrf_id;

	if (!ifp)
		return NULL;
	vrf_id = ifp->vrf ? (eigrp_vrf_id_t)ifp->vrf->vrf_id
			     : EIGRP_VRF_DEFAULT;
	return eigrp_intf_lookup_by_vrf_ifindex(vrf_id, ifp->ifindex);
}

/*
 * Named-mode configuration is retained in FRR YANG, then normalized here
 * before it reaches the portable EIGRP named configuration model.
 */
/*
 * XPath: /frr-eigrpd:eigrpd/named
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_parent_create()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_create(struct nb_cb_create_args *args)
{
	eigrp_result_t result;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		result = eigrp_instance_parent_create(
			yang_dnode_get_string(args->dnode, "name"));
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_parent_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_result_t result;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		result = eigrp_instance_parent_delete(
			yang_dnode_get_string(args->dnode, "name"));
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND)
			return NB_ERR_INCONSISTENCY;
		break;
	}
	return NB_OK;
}

static eigrp_address_family_t
eigrpd_named_address_family_afi(const struct lyd_node *dnode)
{
	const char *afi = yang_dnode_get_string(dnode, "afi");

	if (afi && strcmp(afi, "ipv6") == 0)
		return EIGRP_ADDRESS_FAMILY_IPV6;
	return EIGRP_ADDRESS_FAMILY_IPV4;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_address_family_create()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int
eigrpd_named_address_family_create(struct nb_cb_create_args *args)
{
	eigrp_result_t result;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		result = eigrp_instance_address_family_create(
			yang_dnode_get_string(args->dnode, "../name"),
			eigrpd_named_address_family_afi(args->dnode),
			yang_dnode_get_string(args->dnode, "vrf"),
			yang_dnode_get_uint16(args->dnode, "asn"));
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_address_family_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int
eigrpd_named_address_family_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_result_t result;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		result = eigrp_instance_address_family_delete(
			yang_dnode_get_string(args->dnode, "../name"),
			eigrpd_named_address_family_afi(args->dnode),
			yang_dnode_get_string(args->dnode, "vrf"),
			yang_dnode_get_uint16(args->dnode, "asn"));
		if (result != EIGRP_RESULT_SUCCESS
		    && result != EIGRP_RESULT_NOT_FOUND)
			return NB_ERR_INCONSISTENCY;
		break;
	}
	return NB_OK;
}


static eigrp_address_family_t
eigrpd_named_child_afi(const struct lyd_node *dnode)
{
	const char *afi = yang_dnode_get_string(dnode, "../afi");

	if (afi && strcmp(afi, "ipv6") == 0)
		return EIGRP_ADDRESS_FAMILY_IPV6;
	return EIGRP_ADDRESS_FAMILY_IPV4;
}

static bool eigrpd_named_child_context(const struct lyd_node *dnode,
				       const char **name,
				       eigrp_address_family_t *afi,
				       const char **vrf, uint16_t *asn)
{
	if (!dnode || !name || !afi || !vrf || !asn)
		return false;

	*name = yang_dnode_get_string(dnode, "../../name");
	*afi = eigrpd_named_child_afi(dnode);
	*vrf = yang_dnode_get_string(dnode, "../vrf");
	*asn = yang_dnode_get_uint16(dnode, "../asn");
	return *name && *vrf && *asn != 0;
}

static eigrp_address_family_config_t *eigrpd_named_address_family_config_read(
	const char *name, eigrp_address_family_t afi, const char *vrf, uint16_t asn)
{
	return eigrp_instance_address_family_read(name, afi, vrf, asn);
}

static bool eigrpd_named_instance_context_resolve(
	const char *name, eigrp_address_family_t afi, const char *vrf, uint16_t asn,
	eigrp_instance_context_t *context)
{
	if (!context)
		return false;
	memset(context, 0, sizeof(*context));
	context->config =
		eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	context->topology_id = EIGRP_TOPOLOGY_ID_BASE;
	if (!context->config)
		return false;
	context->runtime = context->config->runtime;
	return true;
}

static bool eigrpd_named_runtime_context_resolve(
	const char *name, eigrp_address_family_t afi, const char *vrf, uint16_t asn,
	eigrp_instance_context_t *context)
{
	return eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						    context);
}

static bool eigrpd_named_interface_context_resolve(
	const char *name, eigrp_address_family_t afi, const char *vrf, uint16_t asn,
	const char *interface_name, eigrp_interface_context_t *context)
{
	eigrp_address_family_config_t *af;
	eigrp_instance_t *runtime;

	if (!context)
		return false;
	memset(context, 0, sizeof(*context));
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	if (!af)
		return false;
	context->config = eigrp_interface_config_read(af, interface_name);
	if (!context->config)
		return false;

	/* The address-family lifecycle owns the runtime binding.  Commands only
	 * consume that binding; they never create or rediscover a process.  The
	 * special af-interface default configuration has no single runtime
	 * interface.
	 */
	if (afi == EIGRP_ADDRESS_FAMILY_IPV4
	    && strcmp(interface_name, "default") != 0) {
		runtime = af->runtime;
		if (runtime)
			context->runtime =
				eigrp_intf_lookup_by_name(runtime, interface_name);
	}

	return true;
}

static bool eigrpd_named_network_context_resolve(
	const char *name, eigrp_address_family_t afi, const char *vrf, uint16_t asn,
	eigrp_instance_context_t *context)
{
	return eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						    context);
}

static bool eigrpd_named_address_parse(const char *text,
				       eigrp_address_family_t afi,
				       eigrp_address_t *address)
{
	int family;

	if (!text || !address)
		return false;

	memset(address, 0, sizeof(*address));
	address->afi = afi;
	family = afi == EIGRP_ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
	return inet_pton(family, text, address->bytes) == 1;
}

static bool eigrpd_named_prefix_parse(const char *text,
                                      eigrp_prefix_t *prefix)
{
    char address[INET6_ADDRSTRLEN];
    const char *slash;
    char *end = NULL;
    unsigned long prefix_length;
    size_t address_length;
    eigrp_address_family_t afi;
    int family;

    if (!text || !prefix)
        return false;
    slash = strchr(text, '/');
    if (!slash)
        return false;
    address_length = (size_t)(slash - text);
    if (address_length == 0 || address_length >= sizeof(address))
        return false;
    memcpy(address, text, address_length);
    address[address_length] = '\0';

    afi = strchr(address, ':') ? EIGRP_ADDRESS_FAMILY_IPV6
                               : EIGRP_ADDRESS_FAMILY_IPV4;
    family = afi == EIGRP_ADDRESS_FAMILY_IPV6 ? AF_INET6 : AF_INET;
    prefix_length = strtoul(slash + 1, &end, 10);
    if (!end || *end != '\0'
        || prefix_length > (afi == EIGRP_ADDRESS_FAMILY_IPV6 ? 128 : 32))
        return false;

    memset(prefix, 0, sizeof(*prefix));
    prefix->address.afi = afi;
    prefix->prefix_length = (uint8_t)prefix_length;
    return inet_pton(family, address, prefix->address.bytes) == 1;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/router-id
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_router_id_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_router_id_modify(struct nb_cb_modify_args *args)
{
	const char *name;
	const char *vrf;
	const char *router_id;
	eigrp_address_family_t afi;
	eigrp_address_t address;
	eigrp_instance_context_t context;
	eigrp_result_t result;
	uint16_t asn;
	uint32_t value;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;

	router_id = yang_dnode_get_string(args->dnode, NULL);
	if (!eigrpd_named_address_parse(router_id, EIGRP_ADDRESS_FAMILY_IPV4,
					&address))
		return NB_ERR_INCONSISTENCY;
	memcpy(&value, address.bytes, sizeof(value));
	value = ntohl(value);
	result = eigrp_instance_router_id_update(&context, value);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/router-id
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_router_id_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_router_id_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_instance_router_id_delete(&context);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/network
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_network_create()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_network_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_prefix_t prefix;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || !eigrpd_named_prefix_parse(yang_dnode_get_string(args->dnode, NULL),
					 &prefix)
	    || !eigrpd_named_network_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_network_create(&context, &prefix);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/network
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_network_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_network_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_prefix_t prefix;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_prefix_parse(yang_dnode_get_string(args->dnode, NULL),
					 &prefix))
		return NB_ERR_INCONSISTENCY;
	if (!eigrpd_named_network_context_resolve(name, afi, vrf, asn, &context))
		return NB_OK;
	result = eigrp_network_delete(&context, &prefix);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_static_create()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_address_family_t afi;
	eigrp_address_family_config_t *af;
	eigrp_address_t address;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	if (!eigrpd_named_address_parse(
		    yang_dnode_get_string(args->dnode, "address"), afi, &address))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	if (!af)
		return NB_ERR_INCONSISTENCY;
	interface_name = yang_dnode_get_string(args->dnode, "interface");
	result = eigrp_neighbor_static_create(af, &address, interface_name);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_static_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_address_family_t afi;
	eigrp_address_family_config_t *af;
	eigrp_address_t address;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	if (!eigrpd_named_address_parse(
		    yang_dnode_get_string(args->dnode, "address"), afi, &address))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	if (!af)
		return NB_OK;
	interface_name = yang_dnode_get_string(args->dnode, "interface");
	result = eigrp_neighbor_static_delete(af, &address, interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/shutdown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_address_family_shutdown_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_shutdown_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_address_family_config_t *af;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	result = eigrp_instance_address_family_shutdown_update(af, true);
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/shutdown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_address_family_shutdown_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_shutdown_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_address_family_config_t *af;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	result = eigrp_instance_address_family_shutdown_update(af, false);
	return eigrpd_named_config_result(result, true);
}

static bool eigrpd_named_af_interface_context(
	const struct lyd_node *dnode, bool child, const char **name,
	eigrp_address_family_t *afi, const char **vrf, uint16_t *asn,
	const char **interface_name)
{
	const char *afi_text;

	if (!dnode || !name || !afi || !vrf || !asn || !interface_name)
		return false;
	if (child) {
		*name = yang_dnode_get_string(dnode, "../../../name");
		afi_text = yang_dnode_get_string(dnode, "../../afi");
		*vrf = yang_dnode_get_string(dnode, "../../vrf");
		*asn = yang_dnode_get_uint16(dnode, "../../asn");
		*interface_name = yang_dnode_get_string(dnode, "../interface");
	} else {
		*name = yang_dnode_get_string(dnode, "../../name");
		afi_text = yang_dnode_get_string(dnode, "../afi");
		*vrf = yang_dnode_get_string(dnode, "../vrf");
		*asn = yang_dnode_get_uint16(dnode, "../asn");
		*interface_name = yang_dnode_get_string(dnode, "interface");
	}
	*afi = afi_text && strcmp(afi_text, "ipv6") == 0
		       ? EIGRP_ADDRESS_FAMILY_IPV6
		       : EIGRP_ADDRESS_FAMILY_IPV4;
	return *name && *vrf && *asn != 0 && *interface_name
	       && (*interface_name)[0] != '\0';
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_config_create()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_address_family_t afi;
	eigrp_address_family_config_t *af;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, false, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	result = eigrp_interface_config_create(af, interface_name);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_config_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_address_family_t afi;
	eigrp_address_family_config_t *af;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, false, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	if (!af)
		return NB_OK;
	result = eigrp_interface_config_delete(af, interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth-percent
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_bandwidth_percent_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_bandwidth_percent_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_bandwidth_percent_update(&context, yang_dnode_get_uint32(args->dnode, NULL));
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth-percent
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_bandwidth_percent_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_bandwidth_percent_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_bandwidth_percent_delete(&context);
	return eigrpd_named_config_result(result, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_bandwidth_set()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_bandwidth_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_bandwidth_set(
		&context, yang_dnode_get_uint32(args->dnode, NULL));
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_bandwidth_reset()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_bandwidth_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_bandwidth_reset(&context);
	return eigrpd_named_config_result(result, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/delay
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_delay_set()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_delay_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_delay_set(
		&context, yang_dnode_get_uint32(args->dnode, NULL));
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/delay
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_delay_reset()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_delay_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_delay_reset(&context);
	return eigrpd_named_config_result(result, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hello-interval
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_hello_interval_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_hello_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_hello_interval_update(&context, yang_dnode_get_uint16(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hello-interval
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_hello_interval_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_hello_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_hello_interval_delete(&context);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hold-time
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_hold_time_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_hold_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_hold_time_update(&context, yang_dnode_get_uint16(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hold-time
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_hold_time_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_hold_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_hold_time_delete(&context);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/passive-interface
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_passive_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_passive_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_passive_update(&context, true);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/passive-interface
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_passive_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_passive_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_passive_update(&context, false);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_authentication_apply(
    const struct lyd_node *interface_dnode)
{
    const char *name, *vrf, *interface_name, *mode_text;
    eigrp_address_family_t afi;
    eigrp_authentication_mode_t mode;
    eigrp_auth_hmac_config_t hmac = {0};
    const eigrp_auth_hmac_config_t *hmac_ptr = NULL;
    eigrp_interface_context_t context;
    uint16_t asn;

    if (!yang_dnode_exists(interface_dnode, "authentication-mode"))
        return NB_OK;
    if (!eigrpd_named_af_interface_context(interface_dnode, false, &name, &afi,
                                            &vrf, &asn, &interface_name)
        || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
                                                    interface_name, &context))
        return NB_ERR_INCONSISTENCY;
    mode_text = yang_dnode_get_string(interface_dnode, "authentication-mode");
    if (strcmp(mode_text, "md5") == 0)
        mode = EIGRP_AUTHENTICATION_MD5;
    else if (strcmp(mode_text, "hmac-sha-256") == 0) {
        mode = EIGRP_AUTHENTICATION_HMAC_SHA256;
        if (!yang_dnode_exists(interface_dnode, "authentication-encryption-type")
            || !yang_dnode_exists(interface_dnode, "authentication-password"))
            return NB_ERR_INCONSISTENCY;
        hmac.encryption_type = yang_dnode_get_uint8(
            interface_dnode, "authentication-encryption-type");
        hmac.password = yang_dnode_get_string(interface_dnode,
                                               "authentication-password");
        hmac_ptr = &hmac;
    } else
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(
        eigrp_auth_mode_update(&context, mode, hmac_ptr), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-mode
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_af_interface_authentication_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_authentication_modify(
    struct nb_cb_modify_args *args)
{
    return args->event == NB_EV_APPLY
               ? eigrpd_named_af_interface_authentication_apply(
                     lyd_parent(args->dnode))
               : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-encryption-type
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-password
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_af_interface_authentication_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_authentication_detail_modify(
    struct nb_cb_modify_args *args)
{
    return args->event == NB_EV_APPLY
               ? eigrpd_named_af_interface_authentication_apply(
                     lyd_parent(args->dnode))
               : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-encryption-type
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-password
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_authentication_detail_destroy(
	struct nb_cb_destroy_args *args)
{
	/* The mode callback owns the portable authentication object.  The detail
	 * leaves are removed in the same transaction when HMAC is changed or
	 * disabled; their individual destroy callbacks exist to satisfy FRR's
	 * optional-leaf callback contract. */
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-mode
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_auth_mode_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_authentication_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_auth_mode_delete(&context);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-key-chain
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_auth_keychain_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_keychain_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_auth_keychain_update(&context, yang_dnode_get_string(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-key-chain
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_auth_keychain_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_keychain_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_auth_keychain_delete(&context);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/next-hop-self
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_next_hop_self_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_next_hop_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_next_hop_self_update(&context, yang_dnode_get_bool(args->dnode, NULL));
	return eigrpd_named_config_result(result, false);
}


/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/split-horizon
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_split_horizon_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_split_horizon_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_split_horizon_update(&context, yang_dnode_get_bool(args->dnode, NULL));
	return eigrpd_named_config_result(result, false);
}


static int eigrpd_named_af_interface_summary_apply_options(
	const struct lyd_node *dnode, bool omit_distance, bool omit_leak_map)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_prefix_t prefix;
	eigrp_interface_context_t context;
	eigrp_summary_options_t options = {0};
	uint16_t asn;

	if (!eigrpd_named_af_interface_context(dnode, true, &name, &afi,
						    &vrf, &asn, &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context)
	    || !eigrpd_named_prefix_parse(yang_dnode_get_string(dnode, "prefix"),
					   &prefix)
	    || prefix.address.afi != afi)
		return NB_ERR_INCONSISTENCY;
	if (!omit_distance && yang_dnode_exists(dnode, "administrative-distance"))
		options.administrative_distance =
			yang_dnode_get_uint8(dnode, "administrative-distance");
	if (!omit_leak_map && yang_dnode_exists(dnode, "leak-map"))
		options.leak_map = yang_dnode_get_string(dnode, "leak-map");
	return eigrpd_named_config_result(
		eigrp_summary_create(&context, &prefix, &options), false);
}

static int eigrpd_named_af_interface_summary_apply(const struct lyd_node *dnode)
{
	return eigrpd_named_af_interface_summary_apply_options(dnode, false, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_af_interface_summary_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_summary_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_af_interface_summary_apply(args->dnode)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address/administrative-distance
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address/leak-map
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_af_interface_summary_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_summary_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_af_interface_summary_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address/administrative-distance
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address/leak-map
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_af_interface_summary_apply_options()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_summary_detail_destroy(
	struct nb_cb_destroy_args *args)
{
	const char *detail;
	bool omit_distance;
	bool omit_leak_map;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	detail = args->dnode->schema->name;
	omit_distance = strcmp(detail, "administrative-distance") == 0;
	omit_leak_map = strcmp(detail, "leak-map") == 0;
	if (!omit_distance && !omit_leak_map)
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_af_interface_summary_apply_options(
		lyd_parent(args->dnode), omit_distance, omit_leak_map);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_summary_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_summary_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_prefix_t prefix;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn, &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context)
	    || !eigrpd_named_prefix_parse(
		    yang_dnode_get_string(args->dnode, "prefix"), &prefix)
	    || prefix.address.afi != afi)
		return NB_ERR_INCONSISTENCY;
	result = eigrp_summary_delete(&context, &prefix);
	return eigrpd_named_config_result(result, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/shutdown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_shutdown_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_shutdown_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_shutdown_update(&context, true);
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/shutdown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_interface_shutdown_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_shutdown_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_interface_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_interface_context_resolve(name, afi, vrf, asn,
						       interface_name, &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_interface_shutdown_update(&context, false);
	return eigrpd_named_config_result(result, true);
}


/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_policy_create(struct nb_cb_create_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_policy_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/description
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_description_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_description_modify(struct nb_cb_modify_args *args)
{
    const struct lyd_node *policy = lyd_parent(args->dnode);
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    eigrp_address_t address;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_child_context(policy, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_address_parse(yang_dnode_get_string(policy, "address"), afi, &address))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_neighbor_description_update(
        &context, &address, yang_dnode_get_string(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/description
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_description_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_description_destroy(struct nb_cb_destroy_args *args)
{
    const struct lyd_node *policy = lyd_parent(args->dnode);
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    eigrp_address_t address;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_child_context(policy, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_address_parse(yang_dnode_get_string(policy, "address"), afi, &address))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_neighbor_description_delete(&context, &address), true);
}

static int eigrpd_named_neighbor_prefix_limit_apply(const struct lyd_node *dnode)
{
    const struct lyd_node *policy = lyd_parent(dnode);
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    eigrp_address_t address;
    eigrp_prefix_limit_t limit;
    uint16_t asn;
    if (!eigrpd_named_child_context(policy, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_address_parse(yang_dnode_get_string(policy, "address"), afi, &address))
        return NB_ERR_INCONSISTENCY;
    eigrpd_named_prefix_limit_get(dnode, &limit);
    return eigrpd_named_config_result(eigrp_neighbor_maximum_prefix_update(
        &context, &address, &limit), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_neighbor_prefix_limit_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_create(struct nb_cb_create_args *args)
{ return args->event == NB_EV_APPLY ? eigrpd_named_neighbor_prefix_limit_apply(args->dnode) : NB_OK; }
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix/maximum
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix/threshold
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_neighbor_prefix_limit_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_modify(struct nb_cb_modify_args *args)
{ return args->event == NB_EV_APPLY ? eigrpd_named_neighbor_prefix_limit_apply(lyd_parent(args->dnode)) : NB_OK; }
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix/warning-only
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_empty_create(struct nb_cb_create_args *args)
{
	(void)args;
	return NB_OK;
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix/threshold
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix/warning-only
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_detail_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix
 * Target: eigrpd_named_neighbor_prefix_limit_apply() -> eigrp_neighbor_maximum_prefix_update()
 * Description:
 * This is the `apply_finish` northbound callback for the `maximum prefix` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_neighbor_prefix_limit_apply() ->
 * eigrp_neighbor_maximum_prefix_update()` rather than duplicating EIGRP behavior in the FRR
 * northbound layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_neighbor_prefix_limit_apply_finish(struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_neighbor_prefix_limit_apply(args->dnode);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_maximum_prefix_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_destroy(struct nb_cb_destroy_args *args)
{
    const struct lyd_node *policy = lyd_parent(args->dnode);
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    eigrp_address_t address;
    uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(policy, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_address_parse(yang_dnode_get_string(policy, "address"), afi, &address))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_neighbor_maximum_prefix_delete(&context, &address), true);
}

static int eigrpd_named_neighbor_prefix_limit_all_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit;
    uint16_t asn;
    if (!eigrpd_named_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    eigrpd_named_prefix_limit_get(dnode, &limit);
    return eigrpd_named_config_result(eigrp_neighbor_maximum_prefix_all_update(&context, &limit), false);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_neighbor_prefix_limit_all_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_all_create(struct nb_cb_create_args *args)
{ return args->event == NB_EV_APPLY ? eigrpd_named_neighbor_prefix_limit_all_apply(args->dnode) : NB_OK; }
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/maximum
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/threshold
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/reset-time
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/restart
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/restart-count
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_neighbor_prefix_limit_all_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_all_modify(struct nb_cb_modify_args *args)
{ return args->event == NB_EV_APPLY ? eigrpd_named_neighbor_prefix_limit_all_apply(lyd_parent(args->dnode)) : NB_OK; }
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/warning-only
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/dampened
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_all_empty_create(struct nb_cb_create_args *args)
{
	(void)args;
	return NB_OK;
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/threshold
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/warning-only
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/dampened
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/reset-time
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/restart
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/restart-count
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_all_detail_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix
 * Target: eigrpd_named_neighbor_prefix_limit_all_apply() -> eigrp_neighbor_maximum_prefix_all_update()
 * Description:
 * This is the `apply_finish` northbound callback for the `neighbor maximum prefix`
 * configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_neighbor_prefix_limit_all_apply() ->
 * eigrp_neighbor_maximum_prefix_all_update()` rather than duplicating EIGRP behavior in the FRR
 * northbound layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_neighbor_prefix_limit_all_apply_finish(struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_neighbor_prefix_limit_all_apply(args->dnode);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_maximum_prefix_all_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_all_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf; eigrp_address_family_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_neighbor_maximum_prefix_all_delete(&context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-changes
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_log_set()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_changes_modify(struct nb_cb_modify_args *args)
{
    const char *name, *vrf; eigrp_address_family_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_neighbor_log_set(
        &context, EIGRP_NEIGHBOR_LOG_CHANGES,
        yang_dnode_get_bool(args->dnode, NULL), 0), false);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-changes
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_log_reset()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_changes_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf; eigrp_address_family_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_neighbor_log_reset(&context, EIGRP_NEIGHBOR_LOG_CHANGES), true);
}

static int eigrpd_named_log_neighbor_warnings_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf; eigrp_address_family_t afi; eigrp_instance_context_t context; uint16_t asn;
    bool enabled; uint16_t seconds = 10;
    if (!eigrpd_named_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    enabled = yang_dnode_get_bool(dnode, "enabled");
    if (yang_dnode_exists(dnode, "interval")) seconds = yang_dnode_get_uint16(dnode, "interval");
    return eigrpd_named_config_result(eigrp_neighbor_log_set(&context, EIGRP_NEIGHBOR_LOG_WARNINGS, enabled, seconds), false);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_log_neighbor_warnings_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_warnings_create(struct nb_cb_create_args *args)
{ return args->event == NB_EV_APPLY ? eigrpd_named_log_neighbor_warnings_apply(args->dnode) : NB_OK; }
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings/enabled
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings/interval
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_log_neighbor_warnings_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_warnings_modify(struct nb_cb_modify_args *args)
{ return args->event == NB_EV_APPLY ? eigrpd_named_log_neighbor_warnings_apply(lyd_parent(args->dnode)) : NB_OK; }
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings/interval
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_warnings_interval_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings
 * Target: eigrpd_named_log_neighbor_warnings_apply() -> eigrp_neighbor_log_set()
 * Description:
 * This is the `apply_finish` northbound callback for the `log neighbor warnings` configuration
 * node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_log_neighbor_warnings_apply() ->
 * eigrp_neighbor_log_set()` rather than duplicating EIGRP behavior in the FRR
 * northbound layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_log_neighbor_warnings_apply_finish(struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_log_neighbor_warnings_apply(args->dnode);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_neighbor_log_reset()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_warnings_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf; eigrp_address_family_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_neighbor_log_reset(&context, EIGRP_NEIGHBOR_LOG_WARNINGS), true);
}

static int eigrpd_named_config_result(eigrp_result_t result, bool removing)
{
	if (result == EIGRP_RESULT_SUCCESS
	    || result == EIGRP_RESULT_NOT_IMPLEMENTED
	    || (removing && result == EIGRP_RESULT_NOT_FOUND))
		return NB_OK;
	return NB_ERR_INCONSISTENCY;
}

static bool eigrpd_named_topology_child_context(
	const struct lyd_node *dnode, const char **name,
	eigrp_address_family_t *afi, const char **vrf, uint16_t *asn)
{
	const char *afi_text;

	if (!dnode || !name || !afi || !vrf || !asn)
		return false;
	*name = yang_dnode_get_string(dnode, "../../../name");
	afi_text = yang_dnode_get_string(dnode, "../../afi");
	*afi = afi_text && strcmp(afi_text, "ipv6") == 0
		       ? EIGRP_ADDRESS_FAMILY_IPV6
		       : EIGRP_ADDRESS_FAMILY_IPV4;
	*vrf = yang_dnode_get_string(dnode, "../../vrf");
	*asn = yang_dnode_get_uint16(dnode, "../../asn");
	return *name && *vrf && *asn != 0;
}

static void eigrpd_named_metric_values_get(const struct lyd_node *dnode,
					   const char *prefix,
					   eigrp_metric_values_t *metric)
{
	memset(metric, 0, sizeof(*metric));

	if (prefix && prefix[0]) {
		metric->bandwidth =
			yang_dnode_get_uint32(dnode, "%s/bandwidth", prefix);
		metric->delay =
			yang_dnode_get_uint32(dnode, "%s/delay", prefix);
		metric->reliability =
			yang_dnode_get_uint8(dnode, "%s/reliability", prefix);
		metric->load =
			yang_dnode_get_uint8(dnode, "%s/load", prefix);
		metric->mtu =
			yang_dnode_get_uint16(dnode, "%s/mtu", prefix);
		return;
	}

	metric->bandwidth = yang_dnode_get_uint32(dnode, "bandwidth");
	metric->delay = yang_dnode_get_uint32(dnode, "delay");
	metric->reliability = yang_dnode_get_uint8(dnode, "reliability");
	metric->load = yang_dnode_get_uint8(dnode, "load");
	metric->mtu = yang_dnode_get_uint16(dnode, "mtu");
}

static void eigrpd_named_prefix_limit_get(const struct lyd_node *dnode,
                                           eigrp_prefix_limit_t *limit)
{
    memset(limit, 0, sizeof(*limit));
    limit->maximum = yang_dnode_get_uint32(dnode, "maximum");
    if (yang_dnode_exists(dnode, "threshold"))
        limit->threshold = yang_dnode_get_uint8(dnode, "threshold");
    limit->warning_only = yang_dnode_exists(dnode, "warning-only");
    limit->dampened = yang_dnode_exists(dnode, "dampened");
    if (yang_dnode_exists(dnode, "reset-time"))
        limit->reset_time_minutes = yang_dnode_get_uint16(dnode, "reset-time");
    if (yang_dnode_exists(dnode, "restart"))
        limit->restart_minutes = yang_dnode_get_uint16(dnode, "restart");
    if (yang_dnode_exists(dnode, "restart-count"))
        limit->restart_count = yang_dnode_get_uint16(dnode, "restart-count");
}

static bool eigrpd_named_summary_prefix_get(const struct lyd_node *dnode,
                                             eigrp_prefix_t *prefix)
{
    return dnode && prefix
           && eigrpd_named_prefix_parse(yang_dnode_get_string(dnode, "prefix"),
                                        prefix);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_topology_create()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_topology_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_topology_create(&context), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_topology_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_topology_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_topology_delete(&context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/auto-summary
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_summary_auto_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_auto_summary_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_summary_auto_update(&context, true), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/auto-summary
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_summary_auto_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_auto_summary_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_summary_auto_update(&context, false), true);
}

static int eigrpd_named_default_information_apply(const struct lyd_node *dnode,
						   bool inbound, bool enabled)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_topology_default_information_update(
			&context,
			inbound ? EIGRP_DEFAULT_INFORMATION_IN
				: EIGRP_DEFAULT_INFORMATION_OUT,
			enabled,
			yang_dnode_exists(dnode, "access-list")
				? yang_dnode_get_string(dnode, "access-list")
				: NULL),
		!enabled);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-in
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_default_information_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_information_in_create(
	struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_information_apply(args->dnode, true, true)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-in
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_default_information_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_information_in_destroy(
	struct nb_cb_destroy_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_information_apply(args->dnode, true, false)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-out
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_default_information_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_information_out_create(
	struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_information_apply(args->dnode, false, true)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-out
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_default_information_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_information_out_destroy(
	struct nb_cb_destroy_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_information_apply(args->dnode, false, false)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-in/access-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_default_information_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_information_in_modify(struct nb_cb_modify_args *args)
{
    return args->event == NB_EV_APPLY
               ? eigrpd_named_default_information_apply(lyd_parent(args->dnode), true, true)
               : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-out/access-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_default_information_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_information_out_modify(struct nb_cb_modify_args *args)
{
    return args->event == NB_EV_APPLY
               ? eigrpd_named_default_information_apply(lyd_parent(args->dnode), false, true)
               : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-in/access-list
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-out/access-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_information_access_list_destroy(
	struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-in
 * Target: eigrpd_named_default_information_apply() -> eigrp_topology_default_information_update()
 * Description:
 * This is the `apply_finish` northbound callback for the `default information in` configuration
 * node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_default_information_apply() ->
 * eigrp_topology_default_information_update()` rather than duplicating EIGRP behavior in the
 * FRR northbound layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_default_information_in_apply_finish(
	struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_default_information_apply(args->dnode, true, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-information-out
 * Target: eigrpd_named_default_information_apply() -> eigrp_topology_default_information_update()
 * Description:
 * This is the `apply_finish` northbound callback for the `default information out`
 * configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_default_information_apply() ->
 * eigrp_topology_default_information_update()` rather than duplicating EIGRP behavior in the
 * FRR northbound layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_default_information_out_apply_finish(
	struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_default_information_apply(args->dnode, false, true);
}

static int eigrpd_named_default_metric_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_metric_values_t metric;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	eigrpd_named_metric_values_get(dnode, "", &metric);
	return eigrpd_named_config_result(
		eigrp_metric_default_update(&context, &metric), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_default_metric_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_metric_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_metric_apply(args->dnode)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric/bandwidth
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric/delay
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric/reliability
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric/load
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric/mtu
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_default_metric_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_metric_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_metric_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/default-metric
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_default_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_metric_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_default_delete(&context), true);
}

static int eigrpd_named_distance_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_address_family_config_t *af;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	return eigrpd_named_config_result(
		eigrp_instance_distance_update(
			af, yang_dnode_get_uint8(dnode, "internal"),
			yang_dnode_get_uint8(dnode, "external")),
		false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distance
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_distance_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distance_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_distance_apply(args->dnode)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distance/internal
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distance/external
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_distance_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distance_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_distance_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distance
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_distance_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distance_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_address_family_config_t *af;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	return eigrpd_named_config_result(eigrp_instance_distance_delete(af), true);
}

static int eigrpd_named_maximum_prefix_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit;
    uint16_t asn;

    if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    eigrpd_named_prefix_limit_get(dnode, &limit);
    return eigrpd_named_config_result(
        eigrp_topology_maximum_prefix_update(&context, &limit), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_maximum_prefix_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_prefix_create(struct nb_cb_create_args *args)
{
    return args->event == NB_EV_APPLY
               ? eigrpd_named_maximum_prefix_apply(args->dnode) : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/maximum
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/threshold
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/reset-time
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/restart
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/restart-count
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_maximum_prefix_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_prefix_modify(struct nb_cb_modify_args *args)
{
    return args->event == NB_EV_APPLY
               ? eigrpd_named_maximum_prefix_apply(lyd_parent(args->dnode)) : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/warning-only
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/dampened
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_prefix_empty_create(struct nb_cb_create_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/threshold
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/warning-only
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/dampened
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/reset-time
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/restart
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/restart-count
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_prefix_detail_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix
 * Target: eigrpd_named_maximum_prefix_apply() -> eigrp_topology_maximum_prefix_update()
 * Description:
 * This is the `apply_finish` northbound callback for the `maximum prefix` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_maximum_prefix_apply() ->
 * eigrp_topology_maximum_prefix_update()` rather than duplicating EIGRP behavior in the FRR
 * northbound layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_maximum_prefix_apply_finish(struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_maximum_prefix_apply(args->dnode);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_topology_maximum_prefix_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_prefix_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;

    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(
        eigrp_topology_maximum_prefix_delete(&context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-paths
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_topology_maximum_paths_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_paths_modify(struct nb_cb_modify_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_topology_maximum_paths_update(
        &context, yang_dnode_get_uint8(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-paths
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_topology_maximum_paths_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_paths_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_topology_maximum_paths_delete(&context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-maximum-hops
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_maximum_hops_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_maximum_hops_modify(struct nb_cb_modify_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_maximum_hops_update(
        &context, yang_dnode_get_uint8(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-maximum-hops
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_maximum_hops_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_maximum_hops_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_maximum_hops_delete(&context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-holddown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_holddown_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_holddown_create(struct nb_cb_create_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_holddown_update(&context, true), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-holddown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_holddown_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_holddown_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_holddown_delete(&context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/event-log-size
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_eventlog_size_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_event_log_size_modify(struct nb_cb_modify_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_eventlog_size_update(
        &context, yang_dnode_get_uint32(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/event-log-size
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_eventlog_size_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_event_log_size_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_eventlog_size_delete(&context), true);
}

static int eigrpd_named_metric_weights_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_metric_weights_t weights;
	uint16_t asn;

	if (!eigrpd_named_child_context(dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	weights.tos = yang_dnode_get_uint8(dnode, "tos");
	weights.k1 = yang_dnode_get_uint8(dnode, "K1");
	weights.k2 = yang_dnode_get_uint8(dnode, "K2");
	weights.k3 = yang_dnode_get_uint8(dnode, "K3");
	weights.k4 = yang_dnode_get_uint8(dnode, "K4");
	weights.k5 = yang_dnode_get_uint8(dnode, "K5");
	weights.k6 = yang_dnode_exists(dnode, "K6")
			     ? yang_dnode_get_uint8(dnode, "K6")
			     : EIGRP_K6_DEFAULT;
	return eigrpd_named_config_result(
		eigrp_metric_weights_update(&context, &weights), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_metric_weights_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_weights_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_metric_weights_apply(args->dnode)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights/tos
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights/K1
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights/K2
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights/K3
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights/K4
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights/K5
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights/K6
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_metric_weights_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_weights_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_metric_weights_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights/K6
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_weights_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_weights_K6_destroy(struct nb_cb_destroy_args *args)
{
	const struct lyd_node *parent;
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_metric_weights_t weights;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	parent = lyd_parent(args->dnode);
	if (!parent
	    || !eigrpd_named_child_context(parent, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
					      &context))
		return NB_ERR_INCONSISTENCY;

	weights.tos = yang_dnode_get_uint8(parent, "tos");
	weights.k1 = yang_dnode_get_uint8(parent, "K1");
	weights.k2 = yang_dnode_get_uint8(parent, "K2");
	weights.k3 = yang_dnode_get_uint8(parent, "K3");
	weights.k4 = yang_dnode_get_uint8(parent, "K4");
	weights.k5 = yang_dnode_get_uint8(parent, "K5");
	weights.k6 = EIGRP_K6_DEFAULT;
	return eigrpd_named_config_result(
		eigrp_metric_weights_update(&context, &weights), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_weights_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_weights_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_weights_delete(&context), true);
}

static int eigrpd_named_offset_list_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf, *direction, *interface_name, *access_list;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_offset_direction_t offset_direction;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	access_list = yang_dnode_get_string(dnode, "access-list");
	direction = yang_dnode_get_string(dnode, "direction");
	interface_name = yang_dnode_get_string(dnode, "interface");
	offset_direction = direction && strcmp(direction, "out") == 0
				   ? EIGRP_OFFSET_OUT
				   : EIGRP_OFFSET_IN;
	return eigrpd_named_config_result(
		eigrp_offset_update(
			&context, access_list, offset_direction,
			yang_dnode_get_uint32(dnode, "offset"),
			interface_name && interface_name[0] ? interface_name : NULL),
		false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/offset-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_offset_list_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_offset_list_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_offset_list_apply(args->dnode)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/offset-list/offset
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_offset_list_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_offset_list_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_offset_list_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/offset-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_offset_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_offset_list_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *direction, *interface_name, *access_list;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_offset_direction_t offset_direction;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	access_list = yang_dnode_get_string(args->dnode, "access-list");
	direction = yang_dnode_get_string(args->dnode, "direction");
	interface_name = yang_dnode_get_string(args->dnode, "interface");
	offset_direction = direction && strcmp(direction, "out") == 0
				   ? EIGRP_OFFSET_OUT
				   : EIGRP_OFFSET_IN;
	return eigrpd_named_config_result(
		eigrp_offset_delete(
			&context, access_list, offset_direction,
			yang_dnode_exists(args->dnode, "offset")
				? yang_dnode_get_uint32(args->dnode, "offset")
				: 0,
			interface_name && interface_name[0] ? interface_name : NULL),
		true);
}

static int eigrpd_named_redistribute_apply_options(const struct lyd_node *dnode,
					     bool include_metrics,
					     bool omit_route_map)
{
	const char *name, *vrf, *protocol;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_metric_values_t metric;
	eigrp_metric_values_t *metric_ptr = NULL;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_runtime_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	protocol = yang_dnode_get_string(dnode, "protocol");
	if (include_metrics && yang_dnode_exists(dnode, "metrics")) {
		eigrpd_named_metric_values_get(dnode, "metrics", &metric);
		metric_ptr = &metric;
	}
	return eigrpd_named_config_result(
		eigrp_redistribute_update(
			&context, protocol, metric_ptr,
			!omit_route_map && yang_dnode_exists(dnode, "route-map")
				? yang_dnode_get_string(dnode, "route-map")
				: NULL),
		false);
}

static int eigrpd_named_redistribute_apply(const struct lyd_node *dnode,
					     bool include_metrics)
{
	return eigrpd_named_redistribute_apply_options(dnode, include_metrics, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_redistribute_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply(args->dnode, true)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_redistribute_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_metrics_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply(lyd_parent(args->dnode), true)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/bandwidth
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/delay
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/reliability
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/load
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/mtu
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_redistribute_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_metrics_modify(struct nb_cb_modify_args *args)
{
	const struct lyd_node *redistribute = lyd_parent(lyd_parent(args->dnode));

	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply(redistribute, true)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_redistribute_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_metrics_destroy(
	struct nb_cb_destroy_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply(lyd_parent(args->dnode), false)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/route-map
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_redistribute_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_route_map_modify(struct nb_cb_modify_args *args)
{
    return args->event == NB_EV_APPLY
               ? eigrpd_named_redistribute_apply(lyd_parent(args->dnode), true)
               : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute/route-map
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_redistribute_apply_options()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_route_map_destroy(
	struct nb_cb_destroy_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply_options(
			       lyd_parent(args->dnode), true, true)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute
 * Target: eigrpd_named_redistribute_apply() -> eigrpd_named_redistribute_apply_options() -> eigrp_redistribute_update()
 * Description:
 * This is the `apply_finish` northbound callback for the `redistribute` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_redistribute_apply() ->
 * eigrpd_named_redistribute_apply_options() -> eigrp_redistribute_update()` rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_redistribute_apply_finish(
	struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_redistribute_apply(args->dnode, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_redistribute_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn)
	    || !eigrpd_named_runtime_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_redistribute_delete(
			&context, yang_dnode_get_string(args->dnode, "protocol")),
		true);
}

static int eigrpd_named_redistribute_maximum_prefix_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf; eigrp_address_family_t afi; eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit; uint16_t asn;
    if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    eigrpd_named_prefix_limit_get(dnode, &limit);
    return eigrpd_named_config_result(eigrp_redistribute_maximum_prefix_update(&context, &limit), false);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_redistribute_maximum_prefix_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_maximum_prefix_create(struct nb_cb_create_args *args)
{ return args->event == NB_EV_APPLY ? eigrpd_named_redistribute_maximum_prefix_apply(args->dnode) : NB_OK; }
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/maximum
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/threshold
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/reset-time
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/restart
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/restart-count
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_redistribute_maximum_prefix_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_maximum_prefix_modify(struct nb_cb_modify_args *args)
{ return args->event == NB_EV_APPLY ? eigrpd_named_redistribute_maximum_prefix_apply(lyd_parent(args->dnode)) : NB_OK; }
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/warning-only
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/dampened
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_maximum_prefix_empty_create(struct nb_cb_create_args *args)
{
	(void)args;
	return NB_OK;
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/threshold
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/warning-only
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/dampened
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/reset-time
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/restart
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/restart-count
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_maximum_prefix_detail_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix
 * Target: eigrpd_named_redistribute_maximum_prefix_apply() -> eigrp_redistribute_maximum_prefix_update()
 * Description:
 * This is the `apply_finish` northbound callback for the `redistribute maximum prefix`
 * configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_redistribute_maximum_prefix_apply() ->
 * eigrp_redistribute_maximum_prefix_update()` rather than duplicating EIGRP behavior in the FRR
 * northbound layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_redistribute_maximum_prefix_apply_finish(
	struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_redistribute_maximum_prefix_apply(args->dnode);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_redistribute_maximum_prefix_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_maximum_prefix_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf; eigrp_address_family_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_redistribute_maximum_prefix_delete(&context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distribute_list_entry_create(struct nb_cb_create_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distribute_list_entry_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/in/access-list
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/in/prefix-list
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/out/access-list
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/out/prefix-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_distribute_list_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distribute_list_modify(struct nb_cb_modify_args *args)
{
    const struct lyd_node *direction_node = lyd_parent(args->dnode);
    const struct lyd_node *list_node = lyd_parent(direction_node);
    const char *name, *vrf, *ifname, *direction;
    eigrp_address_family_t afi; eigrp_instance_context_t context; uint16_t asn;
    eigrp_distribute_list_type_t type;
    eigrp_offset_direction_t dir;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_topology_child_context(list_node, &name, &afi, &vrf, &asn)
        || !eigrpd_named_runtime_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    type = strcmp(args->dnode->schema->name, "prefix-list") == 0
               ? EIGRP_DISTRIBUTE_PREFIX_LIST : EIGRP_DISTRIBUTE_ACCESS_LIST;
    direction = direction_node->schema->name;
    dir = strcmp(direction, "out") == 0 ? EIGRP_OFFSET_OUT : EIGRP_OFFSET_IN;
    ifname = yang_dnode_get_string(list_node, "interface");
    return eigrpd_named_config_result(eigrp_distribute_list_update(
        &context, type, yang_dnode_get_string(args->dnode, NULL), dir,
        ifname && ifname[0] ? ifname : NULL), false);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/in/access-list
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/in/prefix-list
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/out/access-list
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/out/prefix-list
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_distribute_list_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distribute_list_destroy(struct nb_cb_destroy_args *args)
{
    const struct lyd_node *direction_node = lyd_parent(args->dnode);
    const struct lyd_node *list_node = lyd_parent(direction_node);
    const char *name, *vrf, *ifname, *direction;
    eigrp_address_family_t afi; eigrp_instance_context_t context; uint16_t asn;
    eigrp_distribute_list_type_t type; eigrp_offset_direction_t dir;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_topology_child_context(list_node, &name, &afi, &vrf, &asn)
        || !eigrpd_named_runtime_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    type = strcmp(args->dnode->schema->name, "prefix-list") == 0
               ? EIGRP_DISTRIBUTE_PREFIX_LIST : EIGRP_DISTRIBUTE_ACCESS_LIST;
    direction = direction_node->schema->name;
    dir = strcmp(direction, "out") == 0 ? EIGRP_OFFSET_OUT : EIGRP_OFFSET_IN;
    ifname = yang_dnode_get_string(list_node, "interface");
    return eigrpd_named_config_result(eigrp_distribute_list_delete(
        &context, type, yang_dnode_get_string(args->dnode, NULL), dir,
        ifname && ifname[0] ? ifname : NULL), true);
}

static int eigrpd_named_summary_metric_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf;
    eigrp_address_family_t afi;
    eigrp_instance_context_t context;
    eigrp_prefix_t prefix;
    eigrp_summary_metric_config_t config = {0};
    uint16_t asn;

    if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_summary_prefix_get(dnode, &prefix)
        || prefix.address.afi != afi)
        return NB_ERR_INCONSISTENCY;
    if (yang_dnode_exists(dnode, "bandwidth")) {
        config.metric_configured = true;
        eigrpd_named_metric_values_get(dnode, "", &config.metric);
    }
    if (yang_dnode_exists(dnode, "distance")) {
        config.distance_configured = true;
        config.distance = yang_dnode_get_uint8(dnode, "distance");
    }
    return eigrpd_named_config_result(
        eigrp_summary_metric_update(&context, &prefix, &config), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_summary_metric_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_summary_metric_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_summary_metric_apply(args->dnode)
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/bandwidth
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/delay
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/reliability
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/load
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/mtu
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/distance
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback delegates to `eigrpd_named_summary_metric_apply()`, which reaches the EIGRP-owned target for the command.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_summary_metric_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_summary_metric_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/bandwidth
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/delay
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/reliability
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/load
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/mtu
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/distance
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This node is structural, so the child callback owns the EIGRP target when protocol state has to change.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_summary_metric_detail_destroy(
	struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Target: eigrpd_named_summary_metric_apply() -> eigrp_summary_metric_update()
 * Description:
 * This is the `apply_finish` northbound callback for the `summary metric` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_summary_metric_apply() ->
 * eigrp_summary_metric_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * This is named-mode configuration, so host and YANG objects stop at this boundary and the
 * protocol work stays in EIGRP-owned code.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static void eigrpd_named_summary_metric_apply_finish(
	struct nb_cb_apply_finish_args *args)
{
	(void)eigrpd_named_summary_metric_apply(args->dnode);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/summary-metric
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_summary_metric_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_summary_metric_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	eigrp_prefix_t prefix;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context)
	    || !eigrpd_named_summary_prefix_get(args->dnode, &prefix)
	    || prefix.address.afi != afi)
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_summary_metric_delete(&context, &prefix), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/active-time
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_timer_active_time_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_active_time_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_timer_active_time_update(&context, yang_dnode_get_uint16(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/active-time
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_timer_active_time_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_active_time_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_timer_active_time_delete(&context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/traffic-share-balanced
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_traffic_share_balanced_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_traffic_share_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_traffic_share_balanced_update(&context, yang_dnode_get_bool(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/traffic-share-balanced
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_traffic_share_balanced_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_traffic_share_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_traffic_share_balanced_update(&context, true), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/variance
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_variance_update()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_variance_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_variance_update(&context, yang_dnode_get_uint8(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/variance
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_variance_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_variance_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_variance_delete(&context), true);
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
static int eigrpd_instance_create(struct nb_cb_create_args *args)
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
static int eigrpd_instance_destroy(struct nb_cb_destroy_args *args)
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
 * Target: eigrp_instance_router_id_update()
 * Description:
 * This is the `modify` northbound callback for the `router id` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_instance_router_id_update()` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_router_id_modify(struct nb_cb_modify_args *args)
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
		result = eigrp_instance_router_id_update(
			&context, ntohl(router_id.s_addr));
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/router-id
 * Target: eigrp_instance_router_id_delete()
 * Description:
 * This is the `destroy` northbound callback for the `router id` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_instance_router_id_delete()` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_router_id_destroy(struct nb_cb_destroy_args *args)
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
		result = eigrp_instance_router_id_delete(&context);
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/passive-interface
 * Target: eigrp_interface_passive_update()
 * Description:
 * This is the `create` northbound callback for the `passive interface` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_interface_passive_update()` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
eigrpd_instance_passive_interface_create(struct nb_cb_create_args *args)
{
	eigrp_interface_t *intf;
	eigrp_instance_t *eigrp;
	eigrp_interface_context_t context = {0};
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
		if (eigrp_interface_passive_update(&context, true)
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/passive-interface
 * Target: eigrp_interface_passive_update()
 * Description:
 * This is the `destroy` northbound callback for the `passive interface` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_interface_passive_update()` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
eigrpd_instance_passive_interface_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_interface_t *intf;
	eigrp_instance_t *eigrp;
	eigrp_interface_context_t context = {0};
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
		if (eigrp_interface_passive_update(&context, false)
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
static int eigrpd_instance_active_time_modify(struct nb_cb_modify_args *args)
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
 * Target: eigrp_metric_variance_update()
 * Description:
 * This is the `modify` northbound callback for the `variance` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_metric_variance_update()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_variance_modify(struct nb_cb_modify_args *args)
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
		if (eigrp_metric_variance_update(
			    &context, yang_dnode_get_uint8(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/variance
 * Target: eigrp_metric_variance_delete()
 * Description:
 * This is the `destroy` northbound callback for the `variance` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_metric_variance_delete()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_variance_destroy(struct nb_cb_destroy_args *args)
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
		if (eigrp_metric_variance_delete(&context) != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/maximum-paths
 * Target: eigrp_topology_maximum_paths_update()
 * Description:
 * This is the `modify` northbound callback for the `maximum paths` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_topology_maximum_paths_update()` rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_maximum_paths_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	memset(&context, 0, sizeof(context));
	context.runtime = eigrp;
	return eigrp_topology_maximum_paths_update(
		       &context, yang_dnode_get_uint8(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/maximum-paths
 * Target: eigrp_topology_maximum_paths_delete()
 * Description:
 * This is the `destroy` northbound callback for the `maximum paths` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_topology_maximum_paths_delete()` rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
eigrpd_instance_maximum_paths_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	memset(&context, 0, sizeof(context));
	context.runtime = eigrp;
	return eigrp_topology_maximum_paths_delete(&context)
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/event-log-size
 * Target: eigrp_eventlog_size_update()
 * Description:
 * This is the `modify` northbound callback for the `event log size` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_eventlog_size_update()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_event_log_size_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	context.runtime = eigrp;
	context.topology_id = EIGRP_TOPOLOGY_ID_BASE;
	return eigrp_eventlog_size_update(
		       &context, yang_dnode_get_uint32(args->dnode, NULL))
		       == EIGRP_RESULT_SUCCESS
	       ? NB_OK
	       : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/event-log-size
 * Target: eigrp_eventlog_size_delete()
 * Description:
 * This is the `destroy` northbound callback for the `event log size` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_eventlog_size_delete()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_event_log_size_destroy(
	struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;
	eigrp_instance_context_t context = {0};

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	context.runtime = eigrp;
	context.topology_id = EIGRP_TOPOLOGY_ID_BASE;
	return eigrp_eventlog_size_delete(&context) == EIGRP_RESULT_SUCCESS
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
	return eigrp_metric_weights_update(&context, &weights);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K1
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K1` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K1` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K2` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K2` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K3` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K3` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K4` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K4` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K5` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K5` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `modify` northbound callback for the `metric coefficient K6` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
 * Target: eigrpd_instance_metric_weight_update() -> eigrp_metric_weights_update()
 * Description:
 * This is the `destroy` northbound callback for the `metric coefficient K6` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_metric_weight_update() ->
 * eigrp_metric_weights_update()` rather than duplicating EIGRP behavior in the FRR northbound
 * layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
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
static int eigrpd_instance_network_create(struct nb_cb_create_args *args)
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
static int eigrpd_instance_network_destroy(struct nb_cb_destroy_args *args)
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
static int eigrpd_instance_neighbor_create(struct nb_cb_create_args *args)
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
static int eigrpd_instance_neighbor_destroy(struct nb_cb_destroy_args *args)
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
static int eigrp_northbound_distribute_list_create(
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
 * Target: eigrp_redistribute_set()
 * Description:
 * This is the `create` northbound callback for the `redistribute` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_redistribute_set()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_create(struct nb_cb_create_args *args)
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
		redistribute_get_metrics(args->dnode, &metrics);
		eigrp_redistribute_set(eigrp, proto, metrics);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute
 * Target: eigrp_redistribute_unset()
 * Description:
 * This is the `destroy` northbound callback for the `redistribute` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_redistribute_unset()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_destroy(struct nb_cb_destroy_args *args)
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
		eigrp_redistribute_unset(eigrp, proto);
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
static int
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
static int
eigrpd_instance_redistribute_route_map_destroy(struct nb_cb_destroy_args *args)
{
	(void)args;
	/* Permit deletion of stale classic configuration if it already exists. */
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/bandwidth
 * Target: eigrp_redistribute_set()
 * Description:
 * This is the `modify` northbound callback for the `bandwidth` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_redistribute_set()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_metrics_bandwidth_modify(
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
		redistribute_get_metrics(args->dnode, &metrics);
		eigrp_redistribute_set(eigrp, proto, metrics);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/bandwidth
 * Target: eigrp_redistribute_set()
 * Description:
 * This is the `destroy` northbound callback for the `bandwidth` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_redistribute_set()` rather than duplicating EIGRP
 * behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_metrics_bandwidth_destroy(
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
		redistribute_get_metrics(args->dnode, &metrics);
		eigrp_redistribute_set(eigrp, proto, metrics);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/delay
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_modify() -> eigrp_redistribute_set()
 * Description:
 * This is the `modify` northbound callback for the `delay` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_modify() ->
 * eigrp_redistribute_set()` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_metrics_delay_modify(
	struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/delay
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_destroy() -> eigrp_redistribute_set()
 * Description:
 * This is the `destroy` northbound callback for the `delay` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_destroy() ->
 * eigrp_redistribute_set()` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_metrics_delay_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/reliability
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_modify() -> eigrp_redistribute_set()
 * Description:
 * This is the `modify` northbound callback for the `reliability` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_modify() ->
 * eigrp_redistribute_set()` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_metrics_reliability_modify(
	struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/reliability
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_destroy() -> eigrp_redistribute_set()
 * Description:
 * This is the `destroy` northbound callback for the `reliability` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_destroy() ->
 * eigrp_redistribute_set()` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_metrics_reliability_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/load
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_modify() -> eigrp_redistribute_set()
 * Description:
 * This is the `modify` northbound callback for the `load` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_modify() ->
 * eigrp_redistribute_set()` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
eigrpd_instance_redistribute_metrics_load_modify(struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/load
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_destroy() -> eigrp_redistribute_set()
 * Description:
 * This is the `destroy` northbound callback for the `load` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_destroy() ->
 * eigrp_redistribute_set()` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_metrics_load_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/mtu
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_modify() -> eigrp_redistribute_set()
 * Description:
 * This is the `modify` northbound callback for the `mtu` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_modify() ->
 * eigrp_redistribute_set()` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
eigrpd_instance_redistribute_metrics_mtu_modify(struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/mtu
 * Target: eigrpd_instance_redistribute_metrics_bandwidth_destroy() -> eigrp_redistribute_set()
 * Description:
 * This is the `destroy` northbound callback for the `mtu` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrpd_instance_redistribute_metrics_bandwidth_destroy() ->
 * eigrp_redistribute_set()` rather than duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int eigrpd_instance_redistribute_metrics_mtu_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/delay
 * Target: eigrp_interface_delay_set()
 * Description:
 * This is the `modify` northbound callback for the `delay` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_interface_delay_set()` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int lib_interface_eigrp_delay_modify(struct nb_cb_modify_args *args)
{
	eigrp_interface_context_t context = {0};
	eigrp_interface_t *ei;
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

		ei = eigrp_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = eigrp_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;

		context.runtime = ei;
		if (eigrp_interface_delay_set(
			    &context, yang_dnode_get_uint32(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/bandwidth
 * Target: eigrp_interface_bandwidth_set()
 * Description:
 * This is the `modify` northbound callback for the `bandwidth` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_interface_bandwidth_set()` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int lib_interface_eigrp_bandwidth_modify(struct nb_cb_modify_args *args)
{
	eigrp_interface_context_t context = {0};
	struct interface *ifp;
	eigrp_interface_t *ei;

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

		ei = eigrp_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = eigrp_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;

		context.runtime = ei;
		if (eigrp_interface_bandwidth_set(
			    &context, yang_dnode_get_uint32(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hello-interval
 * Target: eigrp_interface_hello_interval_update()
 * Description:
 * This is the `modify` northbound callback for the `hello interval` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_interface_hello_interval_update()` rather than
 * duplicating EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int
lib_interface_eigrp_hello_interval_modify(struct nb_cb_modify_args *args)
{
	struct interface *ifp;
	eigrp_interface_t *ei;
	eigrp_interface_context_t context = {0};

	switch (args->event) {
	case NB_EV_VALIDATE:
		ifp = nb_running_get_entry(args->dnode, NULL, false);
		if (ifp == NULL)
			break;
		ei = eigrp_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = eigrp_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		context.runtime = ei;
		if (eigrp_interface_hello_interval_update(
			    &context, yang_dnode_get_uint16(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hold-time
 * Target: eigrp_interface_hold_time_update()
 * Description:
 * This is the `modify` northbound callback for the `hold time` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * The callback follows northbound event ordering so validation and prepare do not perform
 * protocol work that belongs in APPLY.
 * The runtime path terminates at `eigrp_interface_hold_time_update()` rather than duplicating
 * EIGRP behavior in the FRR northbound layer.
 * If named mode exposes the same feature, it uses the real EIGRP-owned target below this
 * boundary rather than calling this classic FRR callback.
 * Failures are returned through northbound status so inconsistent, incomplete, or unsupported
 * runtime state is not silently accepted.
 */
static int lib_interface_eigrp_hold_time_modify(struct nb_cb_modify_args *args)
{
	struct interface *ifp;
	eigrp_interface_t *ei;
	eigrp_interface_context_t context = {0};

	switch (args->event) {
	case NB_EV_VALIDATE:
		ifp = nb_running_get_entry(args->dnode, NULL, false);
		if (ifp == NULL)
			break;
		ei = eigrp_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = eigrp_interface_lookup_host(ifp);
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		context.runtime = ei;
		if (eigrp_interface_hold_time_update(
			    &context, yang_dnode_get_uint16(args->dnode, NULL))
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
static int
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
static int lib_interface_eigrp_instance_create(struct nb_cb_create_args *args)
{
	eigrp_interface_t *intf;
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
static int lib_interface_eigrp_instance_destroy(struct nb_cb_destroy_args *args)
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
static int lib_interface_eigrp_instance_summarize_addresses_create(
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
static int lib_interface_eigrp_instance_summarize_addresses_destroy(
	struct nb_cb_destroy_args *args)
{
	(void)args;
	/* Permit deletion of stale classic configuration if it already exists. */
	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/authentication
 * Target: eigrp_auth_mode_update()/eigrp_auth_mode_delete()
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
static int lib_interface_eigrp_instance_authentication_modify(
	struct nb_cb_modify_args *args)
{
	eigrp_interface_context_t context = {0};
	eigrp_authentication_mode_t mode;
	eigrp_interface_t *intf;
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
			result = eigrp_auth_mode_delete(&context);
		else {
			mode = strcmp(mode_text, "md5") == 0
				       ? EIGRP_AUTHENTICATION_MD5
				       : EIGRP_AUTHENTICATION_HMAC_SHA256;
			result = eigrp_auth_mode_update(&context, mode, NULL);
		}
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain
 * Target: eigrp_auth_keychain_update()/eigrp_auth_keychain_delete()
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
static int
lib_interface_eigrp_instance_keychain_modify(struct nb_cb_modify_args *args)
{
	eigrp_interface_context_t context = {0};
	eigrp_interface_t *intf;
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
		if (eigrp_auth_keychain_update(
			    &context, yang_dnode_get_string(args->dnode, NULL))
		    != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain
 * Target: eigrp_auth_keychain_update()/eigrp_auth_keychain_delete()
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
static int
lib_interface_eigrp_instance_keychain_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_interface_context_t context = {0};
	eigrp_interface_t *intf;

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
		if (eigrp_auth_keychain_delete(&context) != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

/* clang-format off */
const struct frr_yang_module_info frr_eigrpd_info = {
	.name = "frr-eigrpd",
	.nodes = {
		{
			.xpath = "/frr-eigrpd:eigrpd/named",
			.cbs = {
				.create = eigrpd_named_create,
				.destroy = eigrpd_named_destroy,
				.cli_show = eigrp_cli_named_show_header,
				.cli_show_end = eigrp_cli_named_show_end,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family",
			.cbs = {
				.create = eigrpd_named_address_family_create,
				.destroy = eigrpd_named_address_family_destroy,
				.cli_show = eigrp_cli_named_show_address_family,
				.cli_show_end = eigrp_cli_named_show_address_family_end,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/router-id",
			.cbs = {
				.modify = eigrpd_named_router_id_modify,
				.destroy = eigrpd_named_router_id_destroy,
				.cli_show = eigrp_cli_named_show_router_id,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/network",
			.cbs = {
				.create = eigrpd_named_network_create,
				.destroy = eigrpd_named_network_destroy,
				.cli_show = eigrp_cli_named_show_network,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor",
			.cbs = {
				.create = eigrpd_named_neighbor_create,
				.destroy = eigrpd_named_neighbor_destroy,
				.cli_show = eigrp_cli_named_show_neighbor,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-policy",
			.cbs = {
				.create = eigrpd_named_neighbor_policy_create,
				.destroy = eigrpd_named_neighbor_policy_destroy,
				.flags = F_NB_CB_DESTROY_RECURSE,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-policy/description",
			.cbs = {
				.modify = eigrpd_named_neighbor_description_modify,
				.destroy = eigrpd_named_neighbor_description_destroy,
				.cli_show = eigrp_cli_named_show_neighbor_description,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix",
			.cbs = {
				.create = eigrpd_named_neighbor_prefix_limit_create,
				.destroy = eigrpd_named_neighbor_prefix_limit_destroy,
				.apply_finish = eigrpd_named_neighbor_prefix_limit_apply_finish,
				.cli_show = eigrp_cli_named_show_neighbor_maximum_prefix,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix/maximum",
			.cbs = { .modify = eigrpd_named_neighbor_prefix_limit_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix/threshold",
			.cbs = {
				.modify = eigrpd_named_neighbor_prefix_limit_modify,
				.destroy = eigrpd_named_neighbor_prefix_limit_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-policy/maximum-prefix/warning-only",
			.cbs = {
				.create = eigrpd_named_neighbor_prefix_limit_empty_create,
				.destroy = eigrpd_named_neighbor_prefix_limit_detail_destroy,
			}
		},




		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix",
			.cbs = {
				.create = eigrpd_named_neighbor_prefix_limit_all_create,
				.destroy = eigrpd_named_neighbor_prefix_limit_all_destroy,
				.apply_finish = eigrpd_named_neighbor_prefix_limit_all_apply_finish,
				.cli_show = eigrp_cli_named_show_neighbor_maximum_prefix_all,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/maximum",
			.cbs = { .modify = eigrpd_named_neighbor_prefix_limit_all_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/threshold",
			.cbs = {
				.modify = eigrpd_named_neighbor_prefix_limit_all_modify,
				.destroy = eigrpd_named_neighbor_prefix_limit_all_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/warning-only",
			.cbs = {
				.create = eigrpd_named_neighbor_prefix_limit_all_empty_create,
				.destroy = eigrpd_named_neighbor_prefix_limit_all_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/dampened",
			.cbs = {
				.create = eigrpd_named_neighbor_prefix_limit_all_empty_create,
				.destroy = eigrpd_named_neighbor_prefix_limit_all_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/reset-time",
			.cbs = {
				.modify = eigrpd_named_neighbor_prefix_limit_all_modify,
				.destroy = eigrpd_named_neighbor_prefix_limit_all_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/restart",
			.cbs = {
				.modify = eigrpd_named_neighbor_prefix_limit_all_modify,
				.destroy = eigrpd_named_neighbor_prefix_limit_all_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor-maximum-prefix/restart-count",
			.cbs = {
				.modify = eigrpd_named_neighbor_prefix_limit_all_modify,
				.destroy = eigrpd_named_neighbor_prefix_limit_all_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/log-neighbor-changes",
			.cbs = {
				.modify = eigrpd_named_log_neighbor_changes_modify,
				.destroy = eigrpd_named_log_neighbor_changes_destroy,
				.cli_show = eigrp_cli_named_show_log_neighbor_changes,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings",
			.cbs = {
				.create = eigrpd_named_log_neighbor_warnings_create,
				.destroy = eigrpd_named_log_neighbor_warnings_destroy,
				.apply_finish = eigrpd_named_log_neighbor_warnings_apply_finish,
				.cli_show = eigrp_cli_named_show_log_neighbor_warnings,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings/enabled",
			.cbs = { .modify = eigrpd_named_log_neighbor_warnings_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/log-neighbor-warnings/interval",
			.cbs = {
				.modify = eigrpd_named_log_neighbor_warnings_modify,
				.destroy = eigrpd_named_log_neighbor_warnings_interval_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/shutdown",
			.cbs = {
				.create = eigrpd_named_shutdown_create,
				.destroy = eigrpd_named_shutdown_destroy,
				.cli_show = eigrp_cli_named_show_shutdown,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface",
			.cbs = {
				.create = eigrpd_named_af_interface_create,
				.destroy = eigrpd_named_af_interface_destroy,
				.cli_show = eigrp_cli_named_show_af_interface,
				.cli_show_end = eigrp_cli_named_show_af_interface_end,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth-percent",
			.cbs = {
				.modify = eigrpd_named_af_interface_bandwidth_percent_modify,
				.destroy = eigrpd_named_af_interface_bandwidth_percent_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_bandwidth_percent,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth",
			.cbs = {
				.modify = eigrpd_named_af_interface_bandwidth_modify,
				.destroy = eigrpd_named_af_interface_bandwidth_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_bandwidth,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/delay",
			.cbs = {
				.modify = eigrpd_named_af_interface_delay_modify,
				.destroy = eigrpd_named_af_interface_delay_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_delay,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/hello-interval",
			.cbs = {
				.modify = eigrpd_named_af_interface_hello_modify,
				.destroy = eigrpd_named_af_interface_hello_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_hello,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/hold-time",
			.cbs = {
				.modify = eigrpd_named_af_interface_hold_modify,
				.destroy = eigrpd_named_af_interface_hold_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_hold,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/passive-interface",
			.cbs = {
				.create = eigrpd_named_af_interface_passive_create,
				.destroy = eigrpd_named_af_interface_passive_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_passive,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-mode",
			.cbs = {
				.modify = eigrpd_named_af_interface_authentication_modify,
				.destroy = eigrpd_named_af_interface_authentication_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_authentication,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-encryption-type",
			.cbs = {
				.modify = eigrpd_named_af_interface_authentication_detail_modify,
				.destroy = eigrpd_named_af_interface_authentication_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-password",
			.cbs = {
				.modify = eigrpd_named_af_interface_authentication_detail_modify,
				.destroy = eigrpd_named_af_interface_authentication_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-key-chain",
			.cbs = {
				.modify = eigrpd_named_af_interface_keychain_modify,
				.destroy = eigrpd_named_af_interface_keychain_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_keychain,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/next-hop-self",
			.cbs = {
				.modify = eigrpd_named_af_interface_next_hop_modify,
				.cli_show = eigrp_cli_named_show_af_interface_next_hop_self,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/split-horizon",
			.cbs = {
				.modify = eigrpd_named_af_interface_split_horizon_modify,
				.cli_show = eigrp_cli_named_show_af_interface_split_horizon,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address",
			.cbs = {
				.create = eigrpd_named_af_interface_summary_create,
				.destroy = eigrpd_named_af_interface_summary_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_summary,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address/administrative-distance",
			.cbs = {
				.modify = eigrpd_named_af_interface_summary_modify,
				.destroy = eigrpd_named_af_interface_summary_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address/leak-map",
			.cbs = {
				.modify = eigrpd_named_af_interface_summary_modify,
				.destroy = eigrpd_named_af_interface_summary_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/shutdown",
			.cbs = {
				.create = eigrpd_named_af_interface_shutdown_create,
				.destroy = eigrpd_named_af_interface_shutdown_destroy,
				.cli_show = eigrp_cli_named_show_af_interface_shutdown,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology",
			.cbs = {
				.create = eigrpd_named_topology_create,
				.destroy = eigrpd_named_topology_destroy,
				.cli_show = eigrp_cli_named_show_topology,
				.cli_show_end = eigrp_cli_named_show_topology_end,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/auto-summary",
			.cbs = {
				.create = eigrpd_named_auto_summary_create,
				.destroy = eigrpd_named_auto_summary_destroy,
				.cli_show = eigrp_cli_named_show_auto_summary,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-information-in",
			.cbs = {
				.create = eigrpd_named_default_information_in_create,
				.destroy = eigrpd_named_default_information_in_destroy,
				.apply_finish = eigrpd_named_default_information_in_apply_finish,
				.cli_show = eigrp_cli_named_show_default_information_in,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-information-in/access-list",
			.cbs = {
				.modify = eigrpd_named_default_information_in_modify,
				.destroy = eigrpd_named_default_information_access_list_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-information-out",
			.cbs = {
				.create = eigrpd_named_default_information_out_create,
				.destroy = eigrpd_named_default_information_out_destroy,
				.apply_finish = eigrpd_named_default_information_out_apply_finish,
				.cli_show = eigrp_cli_named_show_default_information_out,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-information-out/access-list",
			.cbs = {
				.modify = eigrpd_named_default_information_out_modify,
				.destroy = eigrpd_named_default_information_access_list_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-metric",
			.cbs = {
				.create = eigrpd_named_default_metric_create,
				.destroy = eigrpd_named_default_metric_destroy,
				.cli_show = eigrp_cli_named_show_default_metric,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-metric/bandwidth",
			.cbs = { .modify = eigrpd_named_default_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-metric/delay",
			.cbs = { .modify = eigrpd_named_default_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-metric/reliability",
			.cbs = { .modify = eigrpd_named_default_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-metric/load",
			.cbs = { .modify = eigrpd_named_default_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-metric/mtu",
			.cbs = { .modify = eigrpd_named_default_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distance",
			.cbs = {
				.create = eigrpd_named_distance_create,
				.destroy = eigrpd_named_distance_destroy,
				.cli_show = eigrp_cli_named_show_distance,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distance/internal",
			.cbs = { .modify = eigrpd_named_distance_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distance/external",
			.cbs = { .modify = eigrpd_named_distance_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix",
			.cbs = {
				.create = eigrpd_named_maximum_prefix_create,
				.destroy = eigrpd_named_maximum_prefix_destroy,
				.apply_finish = eigrpd_named_maximum_prefix_apply_finish,
				.cli_show = eigrp_cli_named_show_maximum_prefix,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/maximum",
			.cbs = { .modify = eigrpd_named_maximum_prefix_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/threshold",
			.cbs = {
				.modify = eigrpd_named_maximum_prefix_modify,
				.destroy = eigrpd_named_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/warning-only",
			.cbs = {
				.create = eigrpd_named_maximum_prefix_empty_create,
				.destroy = eigrpd_named_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/dampened",
			.cbs = {
				.create = eigrpd_named_maximum_prefix_empty_create,
				.destroy = eigrpd_named_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/reset-time",
			.cbs = {
				.modify = eigrpd_named_maximum_prefix_modify,
				.destroy = eigrpd_named_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/restart",
			.cbs = {
				.modify = eigrpd_named_maximum_prefix_modify,
				.destroy = eigrpd_named_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-prefix/restart-count",
			.cbs = {
				.modify = eigrpd_named_maximum_prefix_modify,
				.destroy = eigrpd_named_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/maximum-paths",
			.cbs = {
				.modify = eigrpd_named_maximum_paths_modify,
				.destroy = eigrpd_named_maximum_paths_destroy,
				.cli_show = eigrp_cli_named_show_maximum_paths,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-maximum-hops",
			.cbs = {
				.modify = eigrpd_named_metric_maximum_hops_modify,
				.destroy = eigrpd_named_metric_maximum_hops_destroy,
				.cli_show = eigrp_cli_named_show_metric_maximum_hops,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-holddown",
			.cbs = {
				.create = eigrpd_named_metric_holddown_create,
				.destroy = eigrpd_named_metric_holddown_destroy,
				.cli_show = eigrp_cli_named_show_metric_holddown,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/event-log-size",
			.cbs = {
				.modify = eigrpd_named_event_log_size_modify,
				.destroy = eigrpd_named_event_log_size_destroy,
				.cli_show = eigrp_cli_named_show_event_log_size,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/metric-weights",
			.cbs = {
				.create = eigrpd_named_metric_weights_create,
				.destroy = eigrpd_named_metric_weights_destroy,
				.cli_show = eigrp_cli_named_show_metric_weights,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/metric-weights/tos",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/metric-weights/K1",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/metric-weights/K2",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/metric-weights/K3",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/metric-weights/K4",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/metric-weights/K5",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/metric-weights/K6",
			.cbs = {
				.modify = eigrpd_named_metric_weights_modify,
				.destroy = eigrpd_named_metric_weights_K6_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distribute-list",
			.cbs = {
				.create = eigrpd_named_distribute_list_entry_create,
				.destroy = eigrpd_named_distribute_list_entry_destroy,
				.flags = F_NB_CB_DESTROY_RECURSE,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/in/access-list",
			.cbs = {
				.modify = eigrpd_named_distribute_list_modify,
				.destroy = eigrpd_named_distribute_list_destroy,
				.cli_show = eigrp_cli_named_show_distribute_list,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/in/prefix-list",
			.cbs = {
				.modify = eigrpd_named_distribute_list_modify,
				.destroy = eigrpd_named_distribute_list_destroy,
				.cli_show = eigrp_cli_named_show_distribute_list,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/out/access-list",
			.cbs = {
				.modify = eigrpd_named_distribute_list_modify,
				.destroy = eigrpd_named_distribute_list_destroy,
				.cli_show = eigrp_cli_named_show_distribute_list,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/distribute-list/out/prefix-list",
			.cbs = {
				.modify = eigrpd_named_distribute_list_modify,
				.destroy = eigrpd_named_distribute_list_destroy,
				.cli_show = eigrp_cli_named_show_distribute_list,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/offset-list",
			.cbs = {
				.create = eigrpd_named_offset_list_create,
				.destroy = eigrpd_named_offset_list_destroy,
				.cli_show = eigrp_cli_named_show_offset_list,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/offset-list/offset",
			.cbs = { .modify = eigrpd_named_offset_list_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute",
			.cbs = {
				.create = eigrpd_named_redistribute_create,
				.destroy = eigrpd_named_redistribute_destroy,
				.apply_finish = eigrpd_named_redistribute_apply_finish,
				.cli_show = eigrp_cli_named_show_redistribute,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics",
			.cbs = {
				.create = eigrpd_named_redistribute_metrics_create,
				.destroy = eigrpd_named_redistribute_metrics_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/bandwidth",
			.cbs = { .modify = eigrpd_named_redistribute_metrics_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/delay",
			.cbs = { .modify = eigrpd_named_redistribute_metrics_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/reliability",
			.cbs = { .modify = eigrpd_named_redistribute_metrics_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/load",
			.cbs = { .modify = eigrpd_named_redistribute_metrics_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute/metrics/mtu",
			.cbs = { .modify = eigrpd_named_redistribute_metrics_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute/route-map",
			.cbs = {
				.modify = eigrpd_named_redistribute_route_map_modify,
				.destroy = eigrpd_named_redistribute_route_map_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix",
			.cbs = {
				.create = eigrpd_named_redistribute_maximum_prefix_create,
				.destroy = eigrpd_named_redistribute_maximum_prefix_destroy,
				.apply_finish = eigrpd_named_redistribute_maximum_prefix_apply_finish,
				.cli_show = eigrp_cli_named_show_redistribute_maximum_prefix,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/maximum",
			.cbs = { .modify = eigrpd_named_redistribute_maximum_prefix_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/threshold",
			.cbs = {
				.modify = eigrpd_named_redistribute_maximum_prefix_modify,
				.destroy = eigrpd_named_redistribute_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/warning-only",
			.cbs = {
				.create = eigrpd_named_redistribute_maximum_prefix_empty_create,
				.destroy = eigrpd_named_redistribute_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/dampened",
			.cbs = {
				.create = eigrpd_named_redistribute_maximum_prefix_empty_create,
				.destroy = eigrpd_named_redistribute_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/reset-time",
			.cbs = {
				.modify = eigrpd_named_redistribute_maximum_prefix_modify,
				.destroy = eigrpd_named_redistribute_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/restart",
			.cbs = {
				.modify = eigrpd_named_redistribute_maximum_prefix_modify,
				.destroy = eigrpd_named_redistribute_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/redistribute-maximum-prefix/restart-count",
			.cbs = {
				.modify = eigrpd_named_redistribute_maximum_prefix_modify,
				.destroy = eigrpd_named_redistribute_maximum_prefix_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric",
			.cbs = {
				.create = eigrpd_named_summary_metric_create,
				.destroy = eigrpd_named_summary_metric_destroy,
				.apply_finish = eigrpd_named_summary_metric_apply_finish,
				.cli_show = eigrp_cli_named_show_summary_metric,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/bandwidth",
			.cbs = {
				.modify = eigrpd_named_summary_metric_modify,
				.destroy = eigrpd_named_summary_metric_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/delay",
			.cbs = {
				.modify = eigrpd_named_summary_metric_modify,
				.destroy = eigrpd_named_summary_metric_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/reliability",
			.cbs = {
				.modify = eigrpd_named_summary_metric_modify,
				.destroy = eigrpd_named_summary_metric_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/load",
			.cbs = {
				.modify = eigrpd_named_summary_metric_modify,
				.destroy = eigrpd_named_summary_metric_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/mtu",
			.cbs = {
				.modify = eigrpd_named_summary_metric_modify,
				.destroy = eigrpd_named_summary_metric_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/distance",
			.cbs = {
				.modify = eigrpd_named_summary_metric_modify,
				.destroy = eigrpd_named_summary_metric_detail_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/active-time",
			.cbs = {
				.modify = eigrpd_named_active_time_modify,
				.destroy = eigrpd_named_active_time_destroy,
				.cli_show = eigrp_cli_named_show_active_time,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/traffic-share-balanced",
			.cbs = {
				.modify = eigrpd_named_traffic_share_modify,
				.destroy = eigrpd_named_traffic_share_destroy,
				.cli_show = eigrp_cli_named_show_traffic_share_balanced,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/variance",
			.cbs = {
				.modify = eigrpd_named_variance_modify,
				.destroy = eigrpd_named_variance_destroy,
				.cli_show = eigrp_cli_named_show_variance,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance",
			.cbs = {
				.create = eigrpd_instance_create,
				.destroy = eigrpd_instance_destroy,
				.cli_show = eigrp_cli_classic_show_header,
				.cli_show_end = eigrp_cli_classic_show_end_header,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/router-id",
			.cbs = {
				.modify = eigrpd_instance_router_id_modify,
				.destroy = eigrpd_instance_router_id_destroy,
				.cli_show = eigrp_cli_classic_show_router_id,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/passive-interface",
			.cbs = {
				.create = eigrpd_instance_passive_interface_create,
				.destroy = eigrpd_instance_passive_interface_destroy,
				.cli_show = eigrp_cli_classic_show_passive_interface,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/active-time",
			.cbs = {
				.modify = eigrpd_instance_active_time_modify,
				.cli_show = eigrp_cli_classic_show_active_time,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/variance",
			.cbs = {
				.modify = eigrpd_instance_variance_modify,
				.destroy = eigrpd_instance_variance_destroy,
				.cli_show = eigrp_cli_classic_show_variance,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/maximum-paths",
			.cbs = {
				.modify = eigrpd_instance_maximum_paths_modify,
				.destroy = eigrpd_instance_maximum_paths_destroy,
				.cli_show = eigrp_cli_classic_show_maximum_paths,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/event-log-size",
			.cbs = {
				.modify = eigrpd_instance_event_log_size_modify,
				.destroy = eigrpd_instance_event_log_size_destroy,
				.cli_show = eigrp_cli_classic_show_event_log_size,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/metric-weights",
			.cbs = {
				.cli_show = eigrp_cli_classic_show_metrics,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/metric-weights/K1",
			.cbs = {
				.modify = eigrpd_instance_metric_weights_K1_modify,
				.destroy = eigrpd_instance_metric_weights_K1_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/metric-weights/K2",
			.cbs = {
				.modify = eigrpd_instance_metric_weights_K2_modify,
				.destroy = eigrpd_instance_metric_weights_K2_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/metric-weights/K3",
			.cbs = {
				.modify = eigrpd_instance_metric_weights_K3_modify,
				.destroy = eigrpd_instance_metric_weights_K3_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/metric-weights/K4",
			.cbs = {
				.modify = eigrpd_instance_metric_weights_K4_modify,
				.destroy = eigrpd_instance_metric_weights_K4_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/metric-weights/K5",
			.cbs = {
				.modify = eigrpd_instance_metric_weights_K5_modify,
				.destroy = eigrpd_instance_metric_weights_K5_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/metric-weights/K6",
			.cbs = {
				.modify = eigrpd_instance_metric_weights_K6_modify,
				.destroy = eigrpd_instance_metric_weights_K6_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/network",
			.cbs = {
				.create = eigrpd_instance_network_create,
				.destroy = eigrpd_instance_network_destroy,
				.cli_show = eigrp_cli_classic_show_network,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/neighbor",
			.cbs = {
				.create = eigrpd_instance_neighbor_create,
				.destroy = eigrpd_instance_neighbor_destroy,
				.cli_show = eigrp_cli_classic_show_neighbor,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/distribute-list",
			.cbs = {
				.create = eigrp_northbound_distribute_list_create,
				.destroy = group_distribute_list_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/distribute-list/in/access-list",
			.cbs = {
				.modify = group_distribute_list_ipv4_modify,
				.destroy = group_distribute_list_ipv4_destroy,
				.cli_show = group_distribute_list_ipv4_cli_show,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/distribute-list/out/access-list",
			.cbs = {
				.modify = group_distribute_list_ipv4_modify,
				.destroy = group_distribute_list_ipv4_destroy,
				.cli_show = group_distribute_list_ipv4_cli_show,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/distribute-list/in/prefix-list",
			.cbs = {
				.modify = group_distribute_list_ipv4_modify,
				.destroy = group_distribute_list_ipv4_destroy,
				.cli_show = group_distribute_list_ipv4_cli_show,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/distribute-list/out/prefix-list",
			.cbs = {
				.modify = group_distribute_list_ipv4_modify,
				.destroy = group_distribute_list_ipv4_destroy,
				.cli_show = group_distribute_list_ipv4_cli_show,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/redistribute",
			.cbs = {
				.create = eigrpd_instance_redistribute_create,
				.destroy = eigrpd_instance_redistribute_destroy,
				.cli_show = eigrp_cli_classic_show_redistribute,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/redistribute/route-map",
			.cbs = {
				.modify = eigrpd_instance_redistribute_route_map_modify,
				.destroy = eigrpd_instance_redistribute_route_map_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/redistribute/metrics/bandwidth",
			.cbs = {
				.modify = eigrpd_instance_redistribute_metrics_bandwidth_modify,
				.destroy = eigrpd_instance_redistribute_metrics_bandwidth_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/redistribute/metrics/delay",
			.cbs = {
				.modify = eigrpd_instance_redistribute_metrics_delay_modify,
				.destroy = eigrpd_instance_redistribute_metrics_delay_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/redistribute/metrics/reliability",
			.cbs = {
				.modify = eigrpd_instance_redistribute_metrics_reliability_modify,
				.destroy = eigrpd_instance_redistribute_metrics_reliability_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/redistribute/metrics/load",
			.cbs = {
				.modify = eigrpd_instance_redistribute_metrics_load_modify,
				.destroy = eigrpd_instance_redistribute_metrics_load_destroy,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/redistribute/metrics/mtu",
			.cbs = {
				.modify = eigrpd_instance_redistribute_metrics_mtu_modify,
				.destroy = eigrpd_instance_redistribute_metrics_mtu_destroy,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/delay",
			.cbs = {
				.modify = lib_interface_eigrp_delay_modify,
				.cli_show = eigrp_cli_classic_show_delay,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/bandwidth",
			.cbs = {
				.modify = lib_interface_eigrp_bandwidth_modify,
				.cli_show = eigrp_cli_classic_show_bandwidth,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/hello-interval",
			.cbs = {
				.modify = lib_interface_eigrp_hello_interval_modify,
				.cli_show = eigrp_cli_classic_show_hello_interval,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/hold-time",
			.cbs = {
				.modify = lib_interface_eigrp_hold_time_modify,
				.cli_show = eigrp_cli_classic_show_hold_time,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/split-horizon",
			.cbs = {
				.modify = lib_interface_eigrp_split_horizon_modify,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/instance",
			.cbs = {
				.create = lib_interface_eigrp_instance_create,
				.destroy = lib_interface_eigrp_instance_destroy,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/instance/summarize-addresses",
			.cbs = {
				.create = lib_interface_eigrp_instance_summarize_addresses_create,
				.destroy = lib_interface_eigrp_instance_summarize_addresses_destroy,
				.cli_show = eigrp_cli_classic_show_summarize_address,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/instance/authentication",
			.cbs = {
				.modify = lib_interface_eigrp_instance_authentication_modify,
				.cli_show = eigrp_cli_classic_show_authentication,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain",
			.cbs = {
				.modify = lib_interface_eigrp_instance_keychain_modify,
				.destroy = lib_interface_eigrp_instance_keychain_destroy,
				.cli_show = eigrp_cli_classic_show_keychain,
			}
		},
		{
			.xpath = NULL,
		},
	}
};
