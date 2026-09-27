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
#include "eigrpd/eigrp_cli.h"
#include "eigrpd/eigrp_filter.h"
#include "eigrpd/eigrp_eventlog.h"
#include "eigrpd/eigrp_instance.h"
#include "eigrpd/eigrp_metric.h"
#include "eigrpd/eigrp_summary.h"
#include "eigrpd/eigrp_sys.h"
#include "eigrpd/eigrp_rib.h"
#include "eigrpd/eigrp_timer.h"
#include "eigrpd/eigrp_topology.h"
#include "eigrp_zebra.h"
#include "eigrp_cli_classic.h"
#include "eigrp_cli_named.h"
#include "eigrp_northbound.h"
#include "eigrp_northbound_internal.h"
#include "eigrp_frr.h"
#include "eigrp_policy.h"

#include "lib/keychain.h"
#include "lib/distribute.h"
#include "lib/northbound.h"
#include "lib/zclient.h"

/* Helper functions. */
static int eigrpd_named_config_result(eigrp_result_t result, bool removing);


eigrp_result_t eigrp_northbound_neighbor_clear_address(
	eigrp_instance_t *runtime, eigrp_afi_t afi,
	const struct in_addr *ipv4_address,
	const struct in6_addr *ipv6_address, bool soft,
	eigrp_nbr_clear_cb callback, void *arg, size_t *affected_count)
{
	eigrp_address_t address;
	eigrp_nbr_clear_request_t request = {
		.address = &address,
		.soft = soft,
	};

	if (afi == EIGRP_AFI_IPV4) {
		if (!eigrp_northbound_ipv4_neighbor_address_copy(&address, ipv4_address))
			return EIGRP_RESULT_INVALID_ARGUMENT;
	} else if (afi == EIGRP_AFI_IPV6) {
		if (!eigrp_northbound_ipv6_neighbor_address_copy(&address, ipv6_address))
			return EIGRP_RESULT_INVALID_ARGUMENT;
	} else {
		return EIGRP_RESULT_INVALID_ARGUMENT;
	}

	return eigrp_nbr_clear(runtime, &request, callback, arg,
				    affected_count);
}
static void eigrpd_named_prefix_limit_get(const struct lyd_node *dnode,
                                          eigrp_prefix_limit_t *limit);

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

static eigrp_afi_t
eigrpd_named_address_family_afi(const struct lyd_node *dnode)
{
	const char *afi = yang_dnode_get_string(dnode, "afi");

	if (afi && strcmp(afi, "ipv6") == 0)
		return EIGRP_AFI_IPV6;
	return EIGRP_AFI_IPV4;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_af_instance_create()` instead of carrying protocol behavior in the FRR layer.
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
		result = eigrp_af_instance_create(
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
 * This callback calls `eigrp_af_instance_delete()` instead of carrying protocol behavior in the FRR layer.
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
		result = eigrp_af_instance_delete(
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


static eigrp_afi_t
eigrpd_named_child_afi(const struct lyd_node *dnode)
{
	const char *afi = yang_dnode_get_string(dnode, "../afi");

	if (afi && strcmp(afi, "ipv6") == 0)
		return EIGRP_AFI_IPV6;
	return EIGRP_AFI_IPV4;
}

static bool eigrpd_named_child_context(const struct lyd_node *dnode,
				       const char **name,
				       eigrp_afi_t *afi,
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

static eigrp_af_instance_t *eigrpd_named_address_family_config_read(
	const char *name, eigrp_afi_t afi, const char *vrf, uint16_t asn)
{
	return eigrp_af_instance_read(name, afi, vrf, asn);
}

static bool eigrpd_named_instance_context_resolve(
	const char *name, eigrp_afi_t afi, const char *vrf, uint16_t asn,
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
	const char *name, eigrp_afi_t afi, const char *vrf, uint16_t asn,
	eigrp_instance_context_t *context)
{
	return eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						    context);
}

static bool eigrpd_named_interface_context_resolve(
	const char *name, eigrp_afi_t afi, const char *vrf, uint16_t asn,
	const char *interface_name, eigrp_intf_context_t *context)
{
	eigrp_af_instance_t *af;
	eigrp_instance_t *runtime;

	if (!context)
		return false;
	memset(context, 0, sizeof(*context));
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	if (!af)
		return false;
	context->address_family = af;
	context->config = eigrp_intf_config_read(af, interface_name);
	if (!context->config)
		return false;

	/* The address-family lifecycle owns the runtime binding.  Commands only
	 * consume that binding; they never create or rediscover a process.  The
	 * special af-interface default configuration has no single runtime
	 * interface.
	 */
	if (strcmp(interface_name, "default") != 0) {
		runtime = af->runtime;
		if (runtime)
			context->runtime =
				eigrp_intf_lookup_by_name(runtime, interface_name);
	}

	return true;
}

static bool eigrpd_named_network_context_resolve(
	const char *name, eigrp_afi_t afi, const char *vrf, uint16_t asn,
	eigrp_instance_context_t *context)
{
	return eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						    context);
}

static bool eigrpd_named_address_parse(const char *text,
				       eigrp_afi_t afi,
				       eigrp_address_t *address)
{
	int family;

	if (!text || !address)
		return false;

	memset(address, 0, sizeof(*address));
	address->afi = afi;
	family = afi == EIGRP_AFI_IPV6 ? AF_INET6 : AF_INET;
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
    eigrp_afi_t afi;
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

    afi = strchr(address, ':') ? EIGRP_AFI_IPV6
                               : EIGRP_AFI_IPV4;
    family = afi == EIGRP_AFI_IPV6 ? AF_INET6 : AF_INET;
    prefix_length = strtoul(slash + 1, &end, 10);
    if (!end || *end != '\0'
        || prefix_length > (afi == EIGRP_AFI_IPV6 ? 128 : 32))
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
 * This callback calls `eigrp_instance_router_id_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_router_id_modify(struct nb_cb_modify_args *args)
{
	const char *name;
	const char *vrf;
	const char *router_id;
	eigrp_afi_t afi;
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
	if (!eigrpd_named_address_parse(router_id, EIGRP_AFI_IPV4,
					&address))
		return NB_ERR_INCONSISTENCY;
	memcpy(&value, address.bytes, sizeof(value));
	value = ntohl(value);
	result = eigrp_instance_router_id_update(EIGRP_SET, &context, value);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/router-id
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_instance_router_id_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_router_id_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_instance_router_id_update(EIGRP_RESET, &context, 0);
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
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	eigrp_prefix_t prefix;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || afi != EIGRP_AFI_IPV4
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
	eigrp_afi_t afi;
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
 * This callback calls `eigrp_nbr_static_create()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_afi_t afi;
	eigrp_af_instance_t *af;
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
	result = eigrp_nbr_static_create(af, &address, interface_name);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_nbr_static_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_afi_t afi;
	eigrp_af_instance_t *af;
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
	result = eigrp_nbr_static_delete(af, &address, interface_name);
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
 * This callback calls `eigrp_af_instance_shutdown_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_shutdown_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_afi_t afi;
	eigrp_af_instance_t *af;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	result = eigrp_af_instance_shutdown_update(EIGRP_SET, af);
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/shutdown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_af_instance_shutdown_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_shutdown_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_afi_t afi;
	eigrp_af_instance_t *af;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	result = eigrp_af_instance_shutdown_update(EIGRP_RESET, af);
	return eigrpd_named_config_result(result, true);
}

static bool eigrpd_named_af_interface_context(
	const struct lyd_node *dnode, bool child, const char **name,
	eigrp_afi_t *afi, const char **vrf, uint16_t *asn,
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
		       ? EIGRP_AFI_IPV6
		       : EIGRP_AFI_IPV4;
	return *name && *vrf && *asn != 0 && *interface_name
	       && (*interface_name)[0] != '\0';
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_config_create()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_afi_t afi;
	eigrp_af_instance_t *af;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, false, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	result = eigrp_intf_config_create(af, interface_name);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_config_delete()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_afi_t afi;
	eigrp_af_instance_t *af;
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
	result = eigrp_intf_config_delete(af, interface_name);
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
 * This callback calls `eigrp_intf_bandwidth_percent_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_bandwidth_percent_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_bandwidth_percent_update(EIGRP_SET, &context, yang_dnode_get_uint32(args->dnode, NULL));
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth-percent
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_bandwidth_percent_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_bandwidth_percent_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_bandwidth_percent_update(EIGRP_RESET, &context, 0);
	return eigrpd_named_config_result(result, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_bandwidth_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_bandwidth_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_bandwidth_update(EIGRP_SET, &context, yang_dnode_get_uint32(args->dnode, NULL));
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_bandwidth_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_bandwidth_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_bandwidth_update(EIGRP_RESET, &context, 0);
	return eigrpd_named_config_result(result, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/delay
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_delay_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_delay_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_delay_update(EIGRP_SET, &context, yang_dnode_get_uint32(args->dnode, NULL));
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/delay
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_delay_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_delay_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_delay_update(EIGRP_RESET, &context, 0);
	return eigrpd_named_config_result(result, true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/hello-interval
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_hello_interval_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_hello_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_hello_interval_update(EIGRP_SET, &context, yang_dnode_get_uint16(args->dnode, NULL));
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
 * This callback calls `eigrp_intf_hello_interval_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_hello_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_hello_interval_update(EIGRP_RESET, &context, 0);
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
 * This callback calls `eigrp_intf_hold_time_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_hold_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_hold_time_update(EIGRP_SET, &context, yang_dnode_get_uint16(args->dnode, NULL));
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
 * This callback calls `eigrp_intf_hold_time_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_hold_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_hold_time_update(EIGRP_RESET, &context, 0);
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
 * This callback calls `eigrp_intf_passive_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_passive_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_passive_update(EIGRP_SET, &context);
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
 * This callback calls `eigrp_intf_passive_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_passive_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_passive_update(EIGRP_RESET, &context);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_authentication_apply(
    const struct lyd_node *interface_dnode)
{
    const char *name, *vrf, *interface_name, *mode_text;
    eigrp_afi_t afi;
    eigrp_authentication_mode_t mode;
    eigrp_auth_hmac_config_t hmac = {0};
    const eigrp_auth_hmac_config_t *hmac_ptr = NULL;
    eigrp_intf_context_t context;
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
        eigrp_auth_mode_update(EIGRP_SET, &context, mode, hmac_ptr), false);
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
 * This callback calls `eigrp_auth_mode_update(EIGRP_RESET, 0, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_authentication_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_auth_mode_update(EIGRP_RESET, &context, 0, 0);
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
 * This callback calls `eigrp_auth_keychain_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_keychain_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_auth_keychain_update(EIGRP_SET, &context, yang_dnode_get_string(args->dnode, NULL));
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
 * This callback calls `eigrp_auth_keychain_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_keychain_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_auth_keychain_update(EIGRP_RESET, &context, 0);
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
 * This callback calls `eigrp_intf_nexthop_self_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_next_hop_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = yang_dnode_get_bool(args->dnode, NULL)
		 ? eigrp_intf_nexthop_self_update(EIGRP_SET, &context)
		 : eigrp_intf_nexthop_self_update(EIGRP_RESET, &context);
	return eigrpd_named_config_result(result, false);
}


/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/split-horizon
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_split_horizon_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_split_horizon_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = yang_dnode_get_bool(args->dnode, NULL)
		 ? eigrp_intf_split_horizon_update(EIGRP_SET, &context)
		 : eigrp_intf_split_horizon_update(EIGRP_RESET, &context);
	return eigrpd_named_config_result(result, false);
}


static int eigrpd_named_af_interface_summary_apply_options(
	const struct lyd_node *dnode, bool omit_distance, bool omit_leak_map)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_prefix_t prefix;
	eigrp_intf_context_t context;
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
	eigrp_afi_t afi;
	eigrp_prefix_t prefix;
	eigrp_intf_context_t context;
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
 * This callback calls `eigrp_intf_shutdown_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_shutdown_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_shutdown_update(EIGRP_SET, &context);
	return eigrpd_named_config_result(result, false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/af-interface/shutdown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_intf_shutdown_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_af_interface_shutdown_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_afi_t afi;
	eigrp_intf_context_t context;
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
	result = eigrp_intf_shutdown_update(EIGRP_RESET, &context);
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
 * This callback calls `eigrp_nbr_description_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_description_modify(struct nb_cb_modify_args *args)
{
    const struct lyd_node *policy = lyd_parent(args->dnode);
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    eigrp_address_t address;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_child_context(policy, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_address_parse(yang_dnode_get_string(policy, "address"), afi, &address))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_nbr_description_update(EIGRP_SET, &context, &address, yang_dnode_get_string(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/neighbor-policy/description
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_nbr_description_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_description_destroy(struct nb_cb_destroy_args *args)
{
    const struct lyd_node *policy = lyd_parent(args->dnode);
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    eigrp_address_t address;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_child_context(policy, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_address_parse(yang_dnode_get_string(policy, "address"), afi, &address))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_nbr_description_update(EIGRP_RESET, &context, &address, 0), true);
}

static int eigrpd_named_neighbor_prefix_limit_apply(const struct lyd_node *dnode)
{
    const struct lyd_node *policy = lyd_parent(dnode);
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    eigrp_address_t address;
    eigrp_prefix_limit_t limit;
    uint16_t asn;
    if (!eigrpd_named_child_context(policy, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_address_parse(yang_dnode_get_string(policy, "address"), afi, &address))
        return NB_ERR_INCONSISTENCY;
    eigrpd_named_prefix_limit_get(dnode, &limit);
    return eigrpd_named_config_result(eigrp_nbr_max_prefix_update(EIGRP_SET, &context, &address, &limit), false);
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
 * Target: eigrpd_named_neighbor_prefix_limit_apply() -> eigrp_nbr_max_prefix_update(EIGRP_SET)
 * Description:
 * This is the `apply_finish` northbound callback for the `maximum prefix` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_neighbor_prefix_limit_apply() ->
 * eigrp_nbr_max_prefix_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR
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
 * This callback calls `eigrp_nbr_max_prefix_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_destroy(struct nb_cb_destroy_args *args)
{
    const struct lyd_node *policy = lyd_parent(args->dnode);
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    eigrp_address_t address;
    uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(policy, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)
        || !eigrpd_named_address_parse(yang_dnode_get_string(policy, "address"), afi, &address))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_nbr_max_prefix_update(EIGRP_RESET, &context, &address, 0), true);
}

static int eigrpd_named_neighbor_prefix_limit_all_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit;
    uint16_t asn;
    if (!eigrpd_named_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    eigrpd_named_prefix_limit_get(dnode, &limit);
    return eigrpd_named_config_result(eigrp_nbr_max_prefix_all_update(EIGRP_SET, &context, &limit), false);
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
 * Target: eigrpd_named_neighbor_prefix_limit_all_apply() -> eigrp_nbr_max_prefix_all_update(EIGRP_SET)
 * Description:
 * This is the `apply_finish` northbound callback for the `neighbor maximum prefix`
 * configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_neighbor_prefix_limit_all_apply() ->
 * eigrp_nbr_max_prefix_all_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR
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
 * This callback calls `eigrp_nbr_max_prefix_all_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_neighbor_prefix_limit_all_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf; eigrp_afi_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_nbr_max_prefix_all_update(EIGRP_RESET, &context, 0), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-changes
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_nbr_log_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_changes_modify(struct nb_cb_modify_args *args)
{
    const char *name, *vrf; eigrp_afi_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_nbr_log_update(EIGRP_SET, &context, EIGRP_NEIGHBOR_LOG_CHANGES,
        yang_dnode_get_bool(args->dnode, NULL), 0), false);
}
/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/log-neighbor-changes
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_nbr_log_update(EIGRP_RESET, 0, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_changes_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf; eigrp_afi_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_nbr_log_update(EIGRP_RESET, &context, EIGRP_NEIGHBOR_LOG_CHANGES, 0, 0), true);
}

static int eigrpd_named_log_neighbor_warnings_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf; eigrp_afi_t afi; eigrp_instance_context_t context; uint16_t asn;
    bool enabled; uint16_t seconds = 10;
    if (!eigrpd_named_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    enabled = yang_dnode_get_bool(dnode, "enabled");
    if (yang_dnode_exists(dnode, "interval")) seconds = yang_dnode_get_uint16(dnode, "interval");
    return eigrpd_named_config_result(eigrp_nbr_log_update(EIGRP_SET, &context, EIGRP_NEIGHBOR_LOG_WARNINGS, enabled, seconds), false);
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
 * Target: eigrpd_named_log_neighbor_warnings_apply() -> eigrp_nbr_log_update(EIGRP_SET)
 * Description:
 * This is the `apply_finish` northbound callback for the `log neighbor warnings` configuration
 * node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_log_neighbor_warnings_apply() ->
 * eigrp_nbr_log_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR
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
 * This callback calls `eigrp_nbr_log_update(EIGRP_RESET, 0, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_log_neighbor_warnings_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf; eigrp_afi_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_nbr_log_update(EIGRP_RESET, &context, EIGRP_NEIGHBOR_LOG_WARNINGS, 0, 0), true);
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
	eigrp_afi_t *afi, const char **vrf, uint16_t *asn)
{
	const char *afi_text;

	if (!dnode || !name || !afi || !vrf || !asn)
		return false;
	*name = yang_dnode_get_string(dnode, "../../../name");
	afi_text = yang_dnode_get_string(dnode, "../../afi");
	*afi = afi_text && strcmp(afi_text, "ipv6") == 0
		       ? EIGRP_AFI_IPV6
		       : EIGRP_AFI_IPV4;
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
	eigrp_afi_t afi;
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
	eigrp_afi_t afi;
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
 * This callback calls `eigrp_summary_auto_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_auto_summary_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_summary_auto_update(EIGRP_SET, &context), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/auto-summary
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_summary_auto_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_auto_summary_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_summary_auto_update(EIGRP_RESET, &context), true);
}

static int eigrpd_named_default_information_apply(const struct lyd_node *dnode,
						   bool inbound, bool enabled)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		enabled
			? eigrp_topology_default_information_update(EIGRP_SET, &context,
				inbound ? EIGRP_DEFAULT_INFORMATION_IN
					: EIGRP_DEFAULT_INFORMATION_OUT,
				yang_dnode_exists(dnode, "access-list")
					? yang_dnode_get_string(dnode, "access-list")
					: NULL)
			: eigrp_topology_default_information_update(EIGRP_RESET, &context,
				inbound ? EIGRP_DEFAULT_INFORMATION_IN
					: EIGRP_DEFAULT_INFORMATION_OUT,
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
 * Target: eigrpd_named_default_information_apply() -> eigrp_topology_default_information_update(EIGRP_SET)
 * Description:
 * This is the `apply_finish` northbound callback for the `default information in` configuration
 * node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_default_information_apply() ->
 * eigrp_topology_default_information_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the
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
 * Target: eigrpd_named_default_information_apply() -> eigrp_topology_default_information_update(EIGRP_SET)
 * Description:
 * This is the `apply_finish` northbound callback for the `default information out`
 * configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_default_information_apply() ->
 * eigrp_topology_default_information_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the
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
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	eigrp_metric_values_t metric;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	eigrpd_named_metric_values_get(dnode, "", &metric);
	return eigrpd_named_config_result(
		eigrp_metric_default_update(EIGRP_SET, &context, &metric), false);
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
 * This callback calls `eigrp_metric_default_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_default_metric_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_default_update(EIGRP_RESET, &context, 0), true);
}

static int eigrpd_named_distance_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_af_instance_t *af;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	return eigrpd_named_config_result(
		eigrp_instance_distance_update(EIGRP_SET, af, yang_dnode_get_uint8(dnode, "internal"),
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
 * This callback calls `eigrp_instance_distance_update(EIGRP_RESET, 0, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distance_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_af_instance_t *af;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	af = eigrpd_named_address_family_config_read(name, afi, vrf, asn);
	return eigrpd_named_config_result(eigrp_instance_distance_update(EIGRP_RESET, af, 0, 0), true);
}

static int eigrpd_named_maximum_prefix_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit;
    uint16_t asn;

    if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    eigrpd_named_prefix_limit_get(dnode, &limit);
    return eigrpd_named_config_result(
        eigrp_topology_max_prefix_update(EIGRP_SET, &context, &limit), false);
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
 * Target: eigrpd_named_maximum_prefix_apply() -> eigrp_topology_max_prefix_update(EIGRP_SET)
 * Description:
 * This is the `apply_finish` northbound callback for the `maximum prefix` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_maximum_prefix_apply() ->
 * eigrp_topology_max_prefix_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR
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
 * This callback calls `eigrp_topology_max_prefix_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_prefix_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;

    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(
        eigrp_topology_max_prefix_update(EIGRP_RESET, &context, 0), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-paths
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_topology_maximum_paths_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_paths_modify(struct nb_cb_modify_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_topology_maximum_paths_update(EIGRP_SET, &context, yang_dnode_get_uint8(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/maximum-paths
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_topology_maximum_paths_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_maximum_paths_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_topology_maximum_paths_update(EIGRP_RESET, &context, 0), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-maximum-hops
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_maximum_hops_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_maximum_hops_modify(struct nb_cb_modify_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_maximum_hops_update(EIGRP_SET, &context, yang_dnode_get_uint8(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-maximum-hops
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_maximum_hops_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_maximum_hops_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_maximum_hops_update(EIGRP_RESET, &context, 0), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-holddown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_holddown_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_holddown_create(struct nb_cb_create_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_holddown_update(EIGRP_SET, &context, true), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/metric-holddown
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_holddown_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_holddown_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_holddown_update(EIGRP_RESET, &context, 0), true);
}

static int eigrpd_named_metric_version_32bit_create(struct nb_cb_create_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_version_update(EIGRP_SET, &context), false);
}

static int eigrpd_named_metric_version_32bit_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_metric_version_update(EIGRP_RESET, &context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/event-log-size
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_eventlog_size_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_event_log_size_modify(struct nb_cb_modify_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_eventlog_size_update(EIGRP_SET, &context, yang_dnode_get_uint32(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/event-log-size
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_eventlog_size_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_event_log_size_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
    eigrp_instance_context_t context;
    uint16_t asn;
    if (args->event != NB_EV_APPLY)
        return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context))
        return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_eventlog_size_update(EIGRP_RESET, &context, 0), true);
}

static int eigrpd_named_metric_weights_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
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
		eigrp_metric_weights_update(EIGRP_SET, &context, &weights), false);
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
 * This callback calls `eigrp_metric_weights_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_weights_K6_destroy(struct nb_cb_destroy_args *args)
{
	const struct lyd_node *parent;
	const char *name, *vrf;
	eigrp_afi_t afi;
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
		eigrp_metric_weights_update(EIGRP_SET, &context, &weights), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/metric-weights
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_weights_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_metric_weights_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_weights_update(EIGRP_RESET, &context, 0), true);
}

static int eigrpd_named_offset_list_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf, *direction, *interface_name, *access_list;
	eigrp_afi_t afi;
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
		eigrp_offset_add(
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
 * This callback calls `eigrp_offset_remove()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_offset_list_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *direction, *interface_name, *access_list;
	eigrp_afi_t afi;
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
		eigrp_offset_remove(
			&context, access_list, offset_direction,
			yang_dnode_exists(args->dnode, "offset")
				? yang_dnode_get_uint32(args->dnode, "offset")
				: 0,
			interface_name && interface_name[0] ? interface_name : NULL),
		true);
}

static bool eigrpd_named_redistribute_source_get(
	const struct lyd_node *dnode, eigrp_redist_source_t *source)
{
	const char *protocol;
	eigrp_route_instance_t route_instance;

	if (!dnode || !source)
		return false;
	protocol = yang_dnode_get_string(dnode, "protocol");
	/*
	 * The CLI omits route-instance for sources whose FRR identity is
	 * unqualified (connected, static, RIP, IS-IS and BGP).  Do not call
	 * the typed getter for an absent child: FRR's YANG accessor expects
	 * the node to exist.  Zero is the EIGRP-owned normalized identity for
	 * those sources and for unqualified OSPF.
	 */
	route_instance = yang_dnode_exists(dnode, "route-instance")
			 ? yang_dnode_get_uint16(dnode, "route-instance")
			 : 0;
	if (!protocol)
		return false;

	if (strcmp(protocol, "eigrp") == 0) {
		if (route_instance == 0 || route_instance > UINT16_MAX)
			return false;
		source->protocol = EIGRP_REDISTRIBUTE_PROTOCOL_EIGRP;
	} else if (strcmp(protocol, "ospf") == 0) {
		if (route_instance > UINT16_MAX)
			return false;
		source->protocol = EIGRP_REDISTRIBUTE_PROTOCOL_OSPF;
	} else {
		if (route_instance != 0)
			return false;
		if (strcmp(protocol, "connected") == 0)
			source->protocol = EIGRP_REDISTRIBUTE_PROTOCOL_CONNECTED;
		else if (strcmp(protocol, "static") == 0)
			source->protocol = EIGRP_REDISTRIBUTE_PROTOCOL_STATIC;
		else if (strcmp(protocol, "rip") == 0)
			source->protocol = EIGRP_REDISTRIBUTE_PROTOCOL_RIP;
		else if (strcmp(protocol, "isis") == 0)
			source->protocol = EIGRP_REDISTRIBUTE_PROTOCOL_ISIS;
		else if (strcmp(protocol, "bgp") == 0)
			source->protocol = EIGRP_REDISTRIBUTE_PROTOCOL_BGP;
		else
			return false;
	}

	source->route_instance = route_instance;
	return true;
}

static int eigrpd_named_redistribute_apply_options(const struct lyd_node *dnode,
					     bool include_metrics)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	eigrp_metric_values_t metric;
	eigrp_redist_source_t source = {0};
	eigrp_metric_values_t *metric_ptr = NULL;
	uint16_t asn;
	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)) {
		return NB_ERR_INCONSISTENCY;
	}

	if (!eigrpd_named_runtime_context_resolve(name, afi, vrf, asn, &context)) {
		return NB_ERR_INCONSISTENCY;
	}

	if (!eigrpd_named_redistribute_source_get(dnode, &source)) {
		return NB_ERR_INCONSISTENCY;
	}

	if (include_metrics && yang_dnode_exists(dnode, "metrics")) {
		eigrpd_named_metric_values_get(dnode, "metrics", &metric);
		metric_ptr = &metric;
	}
	return eigrpd_named_config_result(
		eigrp_redist_add(
			&context, &source, metric_ptr,
			yang_dnode_exists(dnode, "route-map")
				? yang_dnode_get_string(dnode, "route-map")
				: NULL),
		false);
}

static int eigrpd_named_redistribute_apply(const struct lyd_node *dnode,
					     bool include_metrics)
{
	return eigrpd_named_redistribute_apply_options(dnode, include_metrics);
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
	/*
	 * One CLI command can create this list, its metrics container and five
	 * metric leaves, and its route-map in one transaction.  Do not subscribe
	 * or mutate EIGRP from an intermediate APPLY callback.  FRR guarantees
	 * the list apply_finish callback runs once after all descendant changes.
	 */
	(void)args;
	return NB_OK;
}

static int eigrpd_named_redistribute_metrics_create(struct nb_cb_create_args *args)
{
	(void)args;
	return NB_OK;
}

static int eigrpd_named_redistribute_metrics_modify(struct nb_cb_modify_args *args)
{
	(void)args;
	return NB_OK;
}

static int eigrpd_named_redistribute_metrics_destroy(
	struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}

static int eigrpd_named_redistribute_route_map_modify(struct nb_cb_modify_args *args)
{
	(void)args;
	return NB_OK;
}

static int eigrpd_named_redistribute_route_map_destroy(
	struct nb_cb_destroy_args *args)
{
	(void)args;
	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/redistribute
 * Target: eigrpd_named_redistribute_apply() -> eigrpd_named_redistribute_apply_options() -> eigrp_redist_add()
 * Description:
 * This is the `apply_finish` northbound callback for the `redistribute` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_redistribute_apply() ->
 * eigrpd_named_redistribute_apply_options() -> eigrp_redist_add()` rather than
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
 * This callback calls `eigrp_redist_remove()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	eigrp_redist_source_t source = {0};
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn)
	    || !eigrpd_named_runtime_context_resolve(name, afi, vrf, asn,
						      &context)
	    || !eigrpd_named_redistribute_source_get(args->dnode, &source))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_redist_remove(&context, &source),
		true);
}

static int eigrpd_named_redistribute_maximum_prefix_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf; eigrp_afi_t afi; eigrp_instance_context_t context;
    eigrp_prefix_limit_t limit; uint16_t asn;
    if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    eigrpd_named_prefix_limit_get(dnode, &limit);
    return eigrpd_named_config_result(eigrp_redist_max_prefix_update(EIGRP_SET, &context, &limit), false);
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
 * Target: eigrpd_named_redistribute_maximum_prefix_apply() -> eigrp_redist_max_prefix_update(EIGRP_SET)
 * Description:
 * This is the `apply_finish` northbound callback for the `redistribute maximum prefix`
 * configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_redistribute_maximum_prefix_apply() ->
 * eigrp_redist_max_prefix_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR
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
 * This callback calls `eigrp_redist_max_prefix_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_redistribute_maximum_prefix_destroy(struct nb_cb_destroy_args *args)
{
    const char *name, *vrf; eigrp_afi_t afi; eigrp_instance_context_t context; uint16_t asn;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
        || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    return eigrpd_named_config_result(eigrp_redist_max_prefix_update(EIGRP_RESET, &context, 0), true);
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
 * This callback calls `eigrp_distribute_add()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distribute_list_modify(struct nb_cb_modify_args *args)
{
    const struct lyd_node *direction_node = lyd_parent(args->dnode);
    const struct lyd_node *list_node = lyd_parent(direction_node);
    const char *name, *vrf, *ifname, *direction;
    eigrp_afi_t afi; eigrp_instance_context_t context; uint16_t asn;
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
    return eigrpd_named_config_result(eigrp_distribute_add(
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
 * This callback calls `eigrp_distribute_remove()` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_distribute_list_destroy(struct nb_cb_destroy_args *args)
{
    const struct lyd_node *direction_node = lyd_parent(args->dnode);
    const struct lyd_node *list_node = lyd_parent(direction_node);
    const char *name, *vrf, *ifname, *direction;
    eigrp_afi_t afi; eigrp_instance_context_t context; uint16_t asn;
    eigrp_distribute_list_type_t type; eigrp_offset_direction_t dir;
    if (args->event != NB_EV_APPLY) return NB_OK;
    if (!eigrpd_named_topology_child_context(list_node, &name, &afi, &vrf, &asn)
        || !eigrpd_named_runtime_context_resolve(name, afi, vrf, asn, &context)) return NB_ERR_INCONSISTENCY;
    type = strcmp(args->dnode->schema->name, "prefix-list") == 0
               ? EIGRP_DISTRIBUTE_PREFIX_LIST : EIGRP_DISTRIBUTE_ACCESS_LIST;
    direction = direction_node->schema->name;
    dir = strcmp(direction, "out") == 0 ? EIGRP_OFFSET_OUT : EIGRP_OFFSET_IN;
    ifname = yang_dnode_get_string(list_node, "interface");
    return eigrpd_named_config_result(eigrp_distribute_remove(
        &context, type, yang_dnode_get_string(args->dnode, NULL), dir,
        ifname && ifname[0] ? ifname : NULL), true);
}

static int eigrpd_named_summary_metric_apply(const struct lyd_node *dnode)
{
    const char *name, *vrf;
    eigrp_afi_t afi;
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
        eigrp_summary_metric_update(EIGRP_SET, &context, &prefix, &config), false);
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
 * Target: eigrpd_named_summary_metric_apply() -> eigrp_summary_metric_update(EIGRP_SET)
 * Description:
 * This is the `apply_finish` northbound callback for the `summary metric` configuration node.
 * FRR owns the YANG transaction here and resolves or normalizes host values before common EIGRP
 * state is touched.
 * This callback runs at APPLY_FINISH so multi-leaf configuration is presented to the target as
 * one settled command state.
 * The runtime path terminates at `eigrpd_named_summary_metric_apply() ->
 * eigrp_summary_metric_update(EIGRP_SET)` rather than duplicating EIGRP behavior in the FRR northbound
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
 * This callback calls `eigrp_summary_metric_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_summary_metric_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
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
		eigrp_summary_metric_update(EIGRP_RESET, &context, &prefix, 0), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/active-time
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_timer_active_time_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_active_time_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_timer_active_time_update(EIGRP_SET, &context, yang_dnode_get_uint16(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/active-time
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_timer_active_time_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_active_time_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_timer_active_time_update(EIGRP_RESET, &context, 0), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/traffic-share-balanced
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_traffic_share_balanced_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_traffic_share_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(yang_dnode_get_bool(args->dnode, NULL)
		 ? eigrp_traffic_share_balanced_update(EIGRP_SET, &context)
		 : eigrp_traffic_share_balanced_update(EIGRP_RESET, &context), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/traffic-share-balanced
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_traffic_share_balanced_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_traffic_share_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_traffic_share_balanced_update(EIGRP_RESET, &context), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/variance
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_variance_update(EIGRP_SET)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_variance_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_variance_update(EIGRP_SET, &context, yang_dnode_get_uint8(args->dnode, NULL)), false);
}

/*
 * XPath: /frr-eigrpd:eigrpd/named/address-family/topology/variance
 * Description:
 * This is the FRR northbound edge for the named-mode node above.
 * It reads YANG here only long enough to normalize the command into EIGRP-owned values.
 * It resolves the named address-family, topology, or interface context before changing EIGRP state.
 * This callback calls `eigrp_metric_variance_update(EIGRP_RESET, 0)` instead of carrying protocol behavior in the FRR layer.
 * Retained configuration and runtime side effects stay with the common target so named mode does not grow a second protocol implementation.
 * Structured EIGRP results are translated back to northbound status, including NOT_IMPLEMENTED when the real runtime path is still incomplete.
 */
static int eigrpd_named_variance_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_afi_t afi;
	eigrp_instance_context_t context;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_instance_context_resolve(name, afi, vrf, asn,
						      &context))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(eigrp_metric_variance_update(EIGRP_RESET, &context, 0), true);
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
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-version-32bit",
			.cbs = {
				.create = eigrpd_named_metric_version_32bit_create,
				.destroy = eigrpd_named_metric_version_32bit_destroy,
				.cli_show = eigrp_cli_named_show_metric_version_32bit,
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
