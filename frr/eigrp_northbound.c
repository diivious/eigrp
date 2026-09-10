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
#include "eigrp_named.h"
#include "eigrp_zebra.h"
#include "eigrp_cli.h"

#include "lib/keychain.h"
#include "lib/distribute.h"
#include "lib/northbound.h"
#include "lib/zclient.h"

/* Helper functions. */
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

static eigrp_interface_t *eigrp_interface_lookup(const eigrp_instance_t *eigrp,
						      const char *ifname)
{
	eigrp_interface_t *intf;
	struct listnode *ln;

	for (ALL_LIST_ELEMENTS_RO(eigrp->eiflist, ln, intf)) {
		if (strcmp(ifname, intf->ifp->name))
			continue;

		return intf;
	}

	return NULL;
}

/*
 * Named-mode configuration is retained in FRR YANG, then normalized here
 * before it reaches the portable EIGRP named configuration model.
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
		result = eigrp_named_process_create(
			yang_dnode_get_string(args->dnode, "name"));
		if (result != EIGRP_RESULT_SUCCESS)
			return NB_ERR_INCONSISTENCY;
		break;
	}
	return NB_OK;
}

static int eigrpd_named_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_result_t result;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		break;
	case NB_EV_APPLY:
		result = eigrp_named_process_delete(
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
		result = eigrp_named_address_family_create(
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
		result = eigrp_named_address_family_delete(
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

static bool eigrpd_named_address_parse(const char *text,
				       eigrp_address_family_t afi,
				       eigrp_named_address_t *address)
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
				      eigrp_named_prefix_t *prefix)
{
	char address[INET_ADDRSTRLEN];
	const char *slash;
	char *end = NULL;
	unsigned long prefix_length;
	size_t address_length;

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

	prefix_length = strtoul(slash + 1, &end, 10);
	if (!end || *end != '\0' || prefix_length > 32)
		return false;

	memset(prefix, 0, sizeof(*prefix));
	prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	prefix->prefix_length = (uint8_t)prefix_length;
	return inet_pton(AF_INET, address, prefix->address.bytes) == 1;
}

static int eigrpd_named_router_id_modify(struct nb_cb_modify_args *args)
{
	const char *name;
	const char *vrf;
	const char *router_id;
	eigrp_address_family_t afi;
	eigrp_named_address_t address;
	eigrp_result_t result;
	uint16_t asn;
	uint32_t value;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;

	router_id = yang_dnode_get_string(args->dnode, NULL);
	if (!eigrpd_named_address_parse(router_id, EIGRP_ADDRESS_FAMILY_IPV4,
					&address))
		return NB_ERR_INCONSISTENCY;
	memcpy(&value, address.bytes, sizeof(value));
	value = ntohl(value);
	result = eigrp_named_router_id_set(name, afi, vrf, asn, value);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_router_id_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_router_id_clear(name, afi, vrf, asn);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_network_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_named_prefix_t prefix;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || !eigrpd_named_prefix_parse(yang_dnode_get_string(args->dnode, NULL),
					 &prefix))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_network_add(name, afi, vrf, asn, &prefix);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_network_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_named_prefix_t prefix;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn)
	    || !eigrpd_named_prefix_parse(yang_dnode_get_string(args->dnode, NULL),
					 &prefix))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_network_remove(name, afi, vrf, asn, &prefix);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_neighbor_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_address_family_t afi;
	eigrp_named_address_t address;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	if (!eigrpd_named_address_parse(
		    yang_dnode_get_string(args->dnode, "address"), afi, &address))
		return NB_ERR_INCONSISTENCY;
	interface_name = yang_dnode_get_string(args->dnode, "interface");
	result = eigrp_named_neighbor_add(name, afi, vrf, asn, &address,
					  interface_name);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_neighbor_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_address_family_t afi;
	eigrp_named_address_t address;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	if (!eigrpd_named_address_parse(
		    yang_dnode_get_string(args->dnode, "address"), afi, &address))
		return NB_ERR_INCONSISTENCY;
	interface_name = yang_dnode_get_string(args->dnode, "interface");
	result = eigrp_named_neighbor_remove(name, afi, vrf, asn, &address,
					     interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_shutdown_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_address_family_shutdown_set(name, afi, vrf, asn, true);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_shutdown_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_address_family_shutdown_set(name, afi, vrf, asn, false);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
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

static int eigrpd_named_af_interface_create(struct nb_cb_create_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, false, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_create(name, afi, vrf, asn,
						 interface_name);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_destroy(struct nb_cb_destroy_args *args)
{
	const char *name;
	const char *vrf;
	const char *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, false, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_delete(name, afi, vrf, asn,
						 interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_bandwidth_modify(
	struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_bandwidth_percent_set(
		name, afi, vrf, asn, interface_name,
		yang_dnode_get_uint32(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_bandwidth_destroy(
	struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_bandwidth_percent_clear(
		name, afi, vrf, asn, interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_hello_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_hello_interval_set(
		name, afi, vrf, asn, interface_name,
		yang_dnode_get_uint16(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_hello_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_hello_interval_clear(
		name, afi, vrf, asn, interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_hold_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_hold_time_set(
		name, afi, vrf, asn, interface_name,
		yang_dnode_get_uint16(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_hold_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_hold_time_clear(
		name, afi, vrf, asn, interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_passive_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_passive_set(name, afi, vrf, asn,
						      interface_name, true);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_passive_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_passive_set(name, afi, vrf, asn,
						      interface_name, false);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_authentication_modify(
	struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	const char *mode_text;
	eigrp_address_family_t afi;
	eigrp_named_authentication_mode_t mode;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	mode_text = yang_dnode_get_string(args->dnode, NULL);
	if (strcmp(mode_text, "md5") == 0)
		mode = EIGRP_NAMED_AUTHENTICATION_MD5;
	else if (strcmp(mode_text, "hmac-sha-256") == 0)
		mode = EIGRP_NAMED_AUTHENTICATION_HMAC_SHA256;
	else
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_authentication_mode_set(
		name, afi, vrf, asn, interface_name, mode);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_authentication_destroy(
	struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_authentication_mode_clear(
		name, afi, vrf, asn, interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_keychain_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_keychain_set(
		name, afi, vrf, asn, interface_name,
		yang_dnode_get_string(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_keychain_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_keychain_clear(name, afi, vrf, asn,
							 interface_name);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_next_hop_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_next_hop_self_set(
		name, afi, vrf, asn, interface_name,
		yang_dnode_get_bool(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}


static int eigrpd_named_af_interface_split_horizon_modify(
	struct nb_cb_modify_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_split_horizon_set(
		name, afi, vrf, asn, interface_name,
		yang_dnode_get_bool(args->dnode, NULL));
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}


static int eigrpd_named_af_interface_summary_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_named_address_t address;
	eigrp_named_address_t mask;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || !eigrpd_named_address_parse(
		    yang_dnode_get_string(args->dnode, "address"), afi, &address)
	    || !eigrpd_named_address_parse(
		    yang_dnode_get_string(args->dnode, "mask"), afi, &mask))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_summary_add(name, afi, vrf, asn,
						      interface_name, &address, &mask);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_summary_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_named_address_t address;
	eigrp_named_address_t mask;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name)
	    || !eigrpd_named_address_parse(
		    yang_dnode_get_string(args->dnode, "address"),
		    EIGRP_ADDRESS_FAMILY_IPV4, &address)
	    || !eigrpd_named_address_parse(
		    yang_dnode_get_string(args->dnode, "mask"),
		    EIGRP_ADDRESS_FAMILY_IPV4, &mask))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_summary_remove(name, afi, vrf, asn,
							 interface_name, &address, &mask);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_shutdown_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_shutdown_set(name, afi, vrf, asn,
						       interface_name, true);
	return result == EIGRP_RESULT_SUCCESS ? NB_OK : NB_ERR_INCONSISTENCY;
}

static int eigrpd_named_af_interface_shutdown_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *interface_name;
	eigrp_address_family_t afi;
	eigrp_result_t result;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_af_interface_context(args->dnode, true, &name, &afi,
						 &vrf, &asn,
						 &interface_name))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_af_interface_shutdown_set(name, afi, vrf, asn,
						       interface_name, false);
	return result == EIGRP_RESULT_SUCCESS || result == EIGRP_RESULT_NOT_FOUND
		       ? NB_OK
		       : NB_ERR_INCONSISTENCY;
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
					   eigrp_named_metric_values_t *metric)
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

static bool eigrpd_named_summary_prefix_get(const struct lyd_node *dnode,
					     eigrp_named_prefix_t *prefix)
{
	struct in_addr address;
	struct in_addr mask;
	uint32_t host_mask;
	uint32_t inverse;
	uint8_t prefix_length = 0;

	if (!dnode || !prefix
	    || inet_pton(AF_INET, yang_dnode_get_string(dnode, "address"),
			 &address) != 1
	    || inet_pton(AF_INET, yang_dnode_get_string(dnode, "mask"), &mask) != 1)
		return false;

	host_mask = ntohl(mask.s_addr);
	inverse = ~host_mask;
	if ((inverse & (inverse + 1U)) != 0)
		return false;
	while (host_mask & 0x80000000U) {
		prefix_length++;
		host_mask <<= 1;
	}

	memset(prefix, 0, sizeof(*prefix));
	prefix->address.afi = EIGRP_ADDRESS_FAMILY_IPV4;
	memcpy(prefix->address.bytes, &address, sizeof(address));
	prefix->prefix_length = prefix_length;
	return true;
}

static int eigrpd_named_topology_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;
	eigrp_result_t result;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_topology_base_create(name, afi, vrf, asn);
	return eigrpd_named_config_result(result, false);
}

static int eigrpd_named_topology_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;
	eigrp_result_t result;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_child_context(args->dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	result = eigrp_named_topology_base_delete(name, afi, vrf, asn);
	return eigrpd_named_config_result(result, true);
}

static int eigrpd_named_auto_summary_create(struct nb_cb_create_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_auto_summary_set(name, afi, vrf, asn, true), false);
}

static int eigrpd_named_auto_summary_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_auto_summary_set(name, afi, vrf, asn, false), true);
}

static int eigrpd_named_default_information_apply(const struct lyd_node *dnode,
						   bool inbound, bool enabled)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_default_information_set(
			name, afi, vrf, asn,
			inbound ? EIGRP_NAMED_DEFAULT_INFORMATION_IN
				: EIGRP_NAMED_DEFAULT_INFORMATION_OUT,
			enabled),
		!enabled);
}

static int eigrpd_named_default_information_in_create(
	struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_information_apply(args->dnode, true, true)
		       : NB_OK;
}

static int eigrpd_named_default_information_in_destroy(
	struct nb_cb_destroy_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_information_apply(args->dnode, true, false)
		       : NB_OK;
}

static int eigrpd_named_default_information_out_create(
	struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_information_apply(args->dnode, false, true)
		       : NB_OK;
}

static int eigrpd_named_default_information_out_destroy(
	struct nb_cb_destroy_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_information_apply(args->dnode, false, false)
		       : NB_OK;
}

static int eigrpd_named_default_metric_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_named_metric_values_t metric;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	eigrpd_named_metric_values_get(dnode, "", &metric);
	return eigrpd_named_config_result(
		eigrp_named_default_metric_set(name, afi, vrf, asn, &metric), false);
}

static int eigrpd_named_default_metric_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_metric_apply(args->dnode)
		       : NB_OK;
}

static int eigrpd_named_default_metric_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_default_metric_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

static int eigrpd_named_default_metric_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_default_metric_clear(name, afi, vrf, asn), true);
}

static int eigrpd_named_distance_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_distance_set(name, afi, vrf, asn,
			yang_dnode_get_uint8(dnode, "internal"),
			yang_dnode_get_uint8(dnode, "external")),
		false);
}

static int eigrpd_named_distance_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_distance_apply(args->dnode)
		       : NB_OK;
}

static int eigrpd_named_distance_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_distance_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

static int eigrpd_named_distance_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_distance_clear(name, afi, vrf, asn), true);
}

static int eigrpd_named_maximum_prefix_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_maximum_prefix_set(
			name, afi, vrf, asn, yang_dnode_get_uint32(args->dnode, NULL)),
		false);
}

static int eigrpd_named_maximum_prefix_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_maximum_prefix_clear(name, afi, vrf, asn), true);
}

static int eigrpd_named_metric_weights_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_named_metric_weights_t weights;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	weights.tos = yang_dnode_get_uint8(dnode, "tos");
	weights.k1 = yang_dnode_get_uint8(dnode, "K1");
	weights.k2 = yang_dnode_get_uint8(dnode, "K2");
	weights.k3 = yang_dnode_get_uint8(dnode, "K3");
	weights.k4 = yang_dnode_get_uint8(dnode, "K4");
	weights.k5 = yang_dnode_get_uint8(dnode, "K5");
	return eigrpd_named_config_result(
		eigrp_named_metric_weights_set(name, afi, vrf, asn, &weights), false);
}

static int eigrpd_named_metric_weights_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_metric_weights_apply(args->dnode)
		       : NB_OK;
}

static int eigrpd_named_metric_weights_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_metric_weights_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

static int eigrpd_named_metric_weights_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_metric_weights_clear(name, afi, vrf, asn), true);
}

static int eigrpd_named_offset_list_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf, *direction, *interface_name, *access_list;
	eigrp_address_family_t afi;
	eigrp_named_offset_direction_t offset_direction;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	access_list = yang_dnode_get_string(dnode, "access-list");
	direction = yang_dnode_get_string(dnode, "direction");
	interface_name = yang_dnode_get_string(dnode, "interface");
	offset_direction = direction && strcmp(direction, "out") == 0
				   ? EIGRP_NAMED_OFFSET_OUT
				   : EIGRP_NAMED_OFFSET_IN;
	return eigrpd_named_config_result(
		eigrp_named_offset_list_set(
			name, afi, vrf, asn, access_list, offset_direction,
			yang_dnode_get_uint32(dnode, "offset"),
			interface_name && interface_name[0] ? interface_name : NULL),
		false);
}

static int eigrpd_named_offset_list_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_offset_list_apply(args->dnode)
		       : NB_OK;
}

static int eigrpd_named_offset_list_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_offset_list_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

static int eigrpd_named_offset_list_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf, *direction, *interface_name, *access_list;
	eigrp_address_family_t afi;
	eigrp_named_offset_direction_t offset_direction;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	access_list = yang_dnode_get_string(args->dnode, "access-list");
	direction = yang_dnode_get_string(args->dnode, "direction");
	interface_name = yang_dnode_get_string(args->dnode, "interface");
	offset_direction = direction && strcmp(direction, "out") == 0
				   ? EIGRP_NAMED_OFFSET_OUT
				   : EIGRP_NAMED_OFFSET_IN;
	return eigrpd_named_config_result(
		eigrp_named_offset_list_remove(
			name, afi, vrf, asn, access_list, offset_direction,
			yang_dnode_exists(args->dnode, "offset")
				? yang_dnode_get_uint32(args->dnode, "offset")
				: 0,
			interface_name && interface_name[0] ? interface_name : NULL),
		true);
}

static int eigrpd_named_redistribute_apply(const struct lyd_node *dnode,
					     bool include_metrics)
{
	const char *name, *vrf, *protocol;
	eigrp_address_family_t afi;
	eigrp_named_metric_values_t metric;
	eigrp_named_metric_values_t *metric_ptr = NULL;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn))
		return NB_ERR_INCONSISTENCY;
	protocol = yang_dnode_get_string(dnode, "protocol");
	if (include_metrics && yang_dnode_exists(dnode, "metrics")) {
		eigrpd_named_metric_values_get(dnode, "metrics", &metric);
		metric_ptr = &metric;
	}
	return eigrpd_named_config_result(
		eigrp_named_redistribute_set(name, afi, vrf, asn, protocol,
					     metric_ptr),
		false);
}

static int eigrpd_named_redistribute_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply(args->dnode, true)
		       : NB_OK;
}

static int eigrpd_named_redistribute_metrics_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply(lyd_parent(args->dnode), true)
		       : NB_OK;
}

static int eigrpd_named_redistribute_metrics_modify(struct nb_cb_modify_args *args)
{
	const struct lyd_node *redistribute = lyd_parent(lyd_parent(args->dnode));

	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply(redistribute, true)
		       : NB_OK;
}

static int eigrpd_named_redistribute_metrics_destroy(
	struct nb_cb_destroy_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_redistribute_apply(lyd_parent(args->dnode), false)
		       : NB_OK;
}

static int eigrpd_named_redistribute_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_redistribute_remove(
			name, afi, vrf, asn,
			yang_dnode_get_string(args->dnode, "protocol")),
		true);
}

static int eigrpd_named_summary_metric_apply(const struct lyd_node *dnode)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_named_prefix_t prefix;
	eigrp_named_metric_values_t metric;
	uint16_t asn;

	if (!eigrpd_named_topology_child_context(dnode, &name, &afi, &vrf, &asn)
	    || afi != EIGRP_ADDRESS_FAMILY_IPV4
	    || !eigrpd_named_summary_prefix_get(dnode, &prefix))
		return NB_ERR_INCONSISTENCY;
	eigrpd_named_metric_values_get(dnode, "", &metric);
	return eigrpd_named_config_result(
		eigrp_named_summary_metric_set(name, afi, vrf, asn, &prefix, &metric),
		false);
}

static int eigrpd_named_summary_metric_create(struct nb_cb_create_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_summary_metric_apply(args->dnode)
		       : NB_OK;
}

static int eigrpd_named_summary_metric_modify(struct nb_cb_modify_args *args)
{
	return args->event == NB_EV_APPLY
		       ? eigrpd_named_summary_metric_apply(lyd_parent(args->dnode))
		       : NB_OK;
}

static int eigrpd_named_summary_metric_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	eigrp_named_prefix_t prefix;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn)
	    || !eigrpd_named_summary_prefix_get(args->dnode, &prefix))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_summary_metric_remove(name, afi, vrf, asn, &prefix), true);
}

static int eigrpd_named_active_time_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_active_time_set(name, afi, vrf, asn,
					    yang_dnode_get_uint16(args->dnode, NULL)),
		false);
}

static int eigrpd_named_active_time_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_active_time_clear(name, afi, vrf, asn), true);
}

static int eigrpd_named_traffic_share_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_traffic_share_balanced_set(
			name, afi, vrf, asn, yang_dnode_get_bool(args->dnode, NULL)),
		false);
}

static int eigrpd_named_traffic_share_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_traffic_share_balanced_set(name, afi, vrf, asn, true), true);
}

static int eigrpd_named_variance_modify(struct nb_cb_modify_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_variance_set(name, afi, vrf, asn,
					 yang_dnode_get_uint8(args->dnode, NULL)),
		false);
}

static int eigrpd_named_variance_destroy(struct nb_cb_destroy_args *args)
{
	const char *name, *vrf;
	eigrp_address_family_t afi;
	uint16_t asn;

	if (args->event != NB_EV_APPLY)
		return NB_OK;
	if (!eigrpd_named_topology_child_context(args->dnode, &name, &afi, &vrf,
						  &asn))
		return NB_ERR_INCONSISTENCY;
	return eigrpd_named_config_result(
		eigrp_named_variance_clear(name, afi, vrf, asn), true);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance
 */
static int eigrpd_instance_create(struct nb_cb_create_args *args)
{
	eigrp_instance_t *eigrp;
	const char *vrf;
	struct vrf *pVrf;
	vrf_id_t vrfid;

	switch (args->event) {
	case NB_EV_VALIDATE:
		/* NOTHING */
		break;
	case NB_EV_PREPARE:
		vrf = yang_dnode_get_string(args->dnode, "./vrf");

		pVrf = vrf_lookup_by_name(vrf);
		if (pVrf)
			vrfid = pVrf->vrf_id;
		else
			vrfid = VRF_DEFAULT;

		eigrp = eigrp_get(yang_dnode_get_uint16(args->dnode, "./asn"),
				  vrfid);
		args->resource->ptr = eigrp;
		break;
	case NB_EV_ABORT:
		eigrp_finish_final(args->resource->ptr);
		break;
	case NB_EV_APPLY:
		nb_running_set_entry(args->dnode, args->resource->ptr);
		break;
	}

	return NB_OK;
}

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
		eigrp_finish_final(eigrp);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/router-id
 */
static int eigrpd_instance_router_id_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		yang_dnode_get_ipv4(&eigrp->router_id_static, args->dnode,
				    NULL);
		break;
	}

	return NB_OK;
}

static int eigrpd_instance_router_id_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->router_id_static.s_addr = INADDR_ANY;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/passive-interface
 */
static int
eigrpd_instance_passive_interface_create(struct nb_cb_create_args *args)
{
	eigrp_interface_t *intf;
	eigrp_instance_t *eigrp;
	const char *ifname;

	switch (args->event) {
	case NB_EV_VALIDATE:
		eigrp = nb_running_get_entry(args->dnode, NULL, false);
		if (eigrp == NULL) {
			/*
			 * XXX: we can't verify if the interface exists
			 * and is active until EIGRP is up.
			 */
			break;
		}

		ifname = yang_dnode_get_string(args->dnode, NULL);
		intf = eigrp_interface_lookup(eigrp, ifname);
		if (intf == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		ifname = yang_dnode_get_string(args->dnode, NULL);
		intf = eigrp_interface_lookup(eigrp, ifname);
		if (intf == NULL)
			return NB_ERR_INCONSISTENCY;

		intf->params.passive_interface = EIGRP_INTF_PASSIVE;
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_passive_interface_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_interface_t *intf;
	eigrp_instance_t *eigrp;
	const char *ifname;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		ifname = yang_dnode_get_string(args->dnode, NULL);
		intf = eigrp_interface_lookup(eigrp, ifname);
		if (intf == NULL)
			break;

		intf->params.passive_interface = EIGRP_INTF_ACTIVE;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/active-time
 */
static int eigrpd_instance_active_time_modify(struct nb_cb_modify_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
		/* TODO: Not implemented. */
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		return NB_OK;
	case NB_EV_APPLY:
		snprintf(args->errmsg, args->errmsg_len,
			 "active time not implemented yet");
		/* NOTHING */
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/variance
 */
static int eigrpd_instance_variance_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->variance = yang_dnode_get_uint8(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

static int eigrpd_instance_variance_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->variance = EIGRP_VARIANCE_DEFAULT;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/maximum-paths
 */
static int eigrpd_instance_maximum_paths_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->max_paths = yang_dnode_get_uint8(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_maximum_paths_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->max_paths = EIGRP_MAX_PATHS_DEFAULT;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K1
 */
static int
eigrpd_instance_metric_weights_K1_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[0] = yang_dnode_get_uint8(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_metric_weights_K1_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[0] = EIGRP_K1_DEFAULT;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K2
 */
static int
eigrpd_instance_metric_weights_K2_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[1] = yang_dnode_get_uint8(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_metric_weights_K2_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[1] = EIGRP_K2_DEFAULT;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K3
 */
static int
eigrpd_instance_metric_weights_K3_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[2] = yang_dnode_get_uint8(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_metric_weights_K3_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[2] = EIGRP_K3_DEFAULT;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K4
 */
static int
eigrpd_instance_metric_weights_K4_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[3] = yang_dnode_get_uint8(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_metric_weights_K4_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[3] = EIGRP_K4_DEFAULT;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K5
 */
static int
eigrpd_instance_metric_weights_K5_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[4] = yang_dnode_get_uint8(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_metric_weights_K5_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[4] = EIGRP_K5_DEFAULT;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/metric-weights/K6
 */
static int
eigrpd_instance_metric_weights_K6_modify(struct nb_cb_modify_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[5] = yang_dnode_get_uint8(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_metric_weights_K6_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_instance_t *eigrp;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp->k_values[5] = EIGRP_K6_DEFAULT;
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/network
 */
static int eigrpd_instance_network_create(struct nb_cb_create_args *args)
{
	struct route_node *rnode;
	struct prefix prefix;
	eigrp_instance_t *eigrp;
	int exists;

	yang_dnode_get_ipv4p(&prefix, args->dnode, NULL);

	switch (args->event) {
	case NB_EV_VALIDATE:
		eigrp = nb_running_get_entry(args->dnode, NULL, false);
		/* If entry doesn't exist it means the list is empty. */
		if (eigrp == NULL)
			break;

		rnode = route_node_get(eigrp->networks, &prefix);
		exists = (rnode->info != NULL);
		route_unlock_node(rnode);
		if (exists)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		if (eigrp_network_set(eigrp, &prefix) == 0)
			return NB_ERR_INCONSISTENCY;
		break;
	}

	return NB_OK;
}

static int eigrpd_instance_network_destroy(struct nb_cb_destroy_args *args)
{
	struct route_node *rnode;
	struct prefix prefix;
	eigrp_instance_t *eigrp;
	int exists = 0;

	yang_dnode_get_ipv4p(&prefix, args->dnode, NULL);

	switch (args->event) {
	case NB_EV_VALIDATE:
		eigrp = nb_running_get_entry(args->dnode, NULL, false);
		/* If entry doesn't exist it means the list is empty. */
		if (eigrp == NULL)
			break;

		rnode = route_node_get(eigrp->networks, &prefix);
		exists = (rnode->info != NULL);
		route_unlock_node(rnode);
		if (exists == 0)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		eigrp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp_network_unset(eigrp, &prefix);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/neighbor
 */
static int eigrpd_instance_neighbor_create(struct nb_cb_create_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
		/* TODO: Not implemented. */
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		return NB_OK;
	case NB_EV_APPLY:
		snprintf(args->errmsg, args->errmsg_len,
			 "neighbor Command is not implemented yet");
		break;
	}

	return NB_OK;
}

static int eigrpd_instance_neighbor_destroy(struct nb_cb_destroy_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
		/* TODO: Not implemented. */
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		return NB_OK;
	case NB_EV_APPLY:
		snprintf(args->errmsg, args->errmsg_len,
			 "no neighbor Command is not implemented yet");
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/distribute-list
 */
static int eigrp_northbound_distribute_list_create(
	struct nb_cb_create_args *args)
{
	eigrp_instance_t *eigrp;

	if (args->event != NB_EV_APPLY)
		return NB_OK;

	eigrp = nb_running_get_entry(args->dnode, NULL, true);
	group_distribute_list_create_helper(args, eigrp->distribute_ctx);

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute
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
 */
static int
eigrpd_instance_redistribute_route_map_modify(struct nb_cb_modify_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
		/* TODO: Not implemented. */
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		return NB_OK;
	case NB_EV_APPLY:
		snprintf(
			args->errmsg, args->errmsg_len,
			"'redistribute X route-map FOO' command not implemented yet");
		break;
	}

	return NB_OK;
}

static int
eigrpd_instance_redistribute_route_map_destroy(struct nb_cb_destroy_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
		/* TODO: Not implemented. */
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		return NB_OK;
	case NB_EV_APPLY:
		snprintf(
			args->errmsg, args->errmsg_len,
			"'no redistribute X route-map FOO' command not implemented yet");
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/bandwidth
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
 */
static int eigrpd_instance_redistribute_metrics_delay_modify(
	struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

static int eigrpd_instance_redistribute_metrics_delay_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/reliability
 */
static int eigrpd_instance_redistribute_metrics_reliability_modify(
	struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

static int eigrpd_instance_redistribute_metrics_reliability_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/load
 */
static int
eigrpd_instance_redistribute_metrics_load_modify(struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

static int eigrpd_instance_redistribute_metrics_load_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-eigrpd:eigrpd/instance/redistribute/metrics/mtu
 */
static int
eigrpd_instance_redistribute_metrics_mtu_modify(struct nb_cb_modify_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_modify(args);
}

static int eigrpd_instance_redistribute_metrics_mtu_destroy(
	struct nb_cb_destroy_args *args)
{
	return eigrpd_instance_redistribute_metrics_bandwidth_destroy(args);
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/delay
 */
static int lib_interface_eigrp_delay_modify(struct nb_cb_modify_args *args)
{
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

		ei = ifp->info;
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = ifp->info;
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;

		ei->params.delay = yang_dnode_get_uint32(args->dnode, NULL);
		eigrp_intf_reset(ifp);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/bandwidth
 */
static int lib_interface_eigrp_bandwidth_modify(struct nb_cb_modify_args *args)
{
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

		ei = ifp->info;
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = ifp->info;
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;

		ei->params.bandwidth = yang_dnode_get_uint32(args->dnode, NULL);
		eigrp_intf_reset(ifp);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hello-interval
 */
static int
lib_interface_eigrp_hello_interval_modify(struct nb_cb_modify_args *args)
{
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

		ei = ifp->info;
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = ifp->info;
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;

		ei->params.v_hello = yang_dnode_get_uint16(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/hold-time
 */
static int lib_interface_eigrp_hold_time_modify(struct nb_cb_modify_args *args)
{
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

		ei = ifp->info;
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		ei = ifp->info;
		if (ei == NULL)
			return NB_ERR_INCONSISTENCY;

		ei->params.v_wait = yang_dnode_get_uint16(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/split-horizon
 */
static int
lib_interface_eigrp_split_horizon_modify(struct nb_cb_modify_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
		/* TODO: Not implemented. */
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		return NB_OK;
	case NB_EV_APPLY:
		snprintf(args->errmsg, args->errmsg_len,
			 "split-horizon command not implemented yet");
		/* NOTHING */
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance
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

		eigrp = eigrp_get(yang_dnode_get_uint16(args->dnode, "./asn"),
				  ifp->vrf->vrf_id);
		intf = eigrp_interface_lookup(eigrp, ifp->name);
		if (intf == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		ifp = nb_running_get_entry(args->dnode, NULL, true);
		eigrp = eigrp_get(yang_dnode_get_uint16(args->dnode, "./asn"),
				  ifp->vrf->vrf_id);
		intf = eigrp_interface_lookup(eigrp, ifp->name);
		if (intf == NULL)
			return NB_ERR_INCONSISTENCY;

		nb_running_set_entry(args->dnode, intf);
		break;
	}

	return NB_OK;
}

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
 * XPath:
 * /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/summarize-addresses
 */
static int lib_interface_eigrp_instance_summarize_addresses_create(
	struct nb_cb_create_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
		/* TODO: Not implemented. */
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		return NB_OK;
	case NB_EV_APPLY:
		snprintf(args->errmsg, args->errmsg_len,
			 "summary command not implemented yet");
		break;
	}

	return NB_OK;
}

static int lib_interface_eigrp_instance_summarize_addresses_destroy(
	struct nb_cb_destroy_args *args)
{
	switch (args->event) {
	case NB_EV_VALIDATE:
		/* TODO: Not implemented. */
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		return NB_OK;
	case NB_EV_APPLY:
		snprintf(args->errmsg, args->errmsg_len,
			 "no summary command not implemented yet");
		/* NOTHING */
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/authentication
 */
static int lib_interface_eigrp_instance_authentication_modify(
	struct nb_cb_modify_args *args)
{
	eigrp_interface_t *intf;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		intf = nb_running_get_entry(args->dnode, NULL, true);
		intf->params.auth_type = yang_dnode_get_enum(args->dnode, NULL);
		break;
	}

	return NB_OK;
}

/*
 * XPath: /frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain
 */
static int
lib_interface_eigrp_instance_keychain_modify(struct nb_cb_modify_args *args)
{
	eigrp_interface_t *intf;
	struct keychain *keychain;

	switch (args->event) {
	case NB_EV_VALIDATE:
		keychain = keychain_lookup(
			yang_dnode_get_string(args->dnode, NULL));
		if (keychain == NULL)
			return NB_ERR_INCONSISTENCY;
		break;
	case NB_EV_PREPARE:
		args->resource->ptr =
			strdup(yang_dnode_get_string(args->dnode, NULL));
		if (args->resource->ptr == NULL)
			return NB_ERR_RESOURCE;
		break;
	case NB_EV_ABORT:
		free(args->resource->ptr);
		args->resource->ptr = NULL;
		break;
	case NB_EV_APPLY:
		intf = nb_running_get_entry(args->dnode, NULL, true);
		if (intf->params.auth_keychain)
			free(intf->params.auth_keychain);

		intf->params.auth_keychain = args->resource->ptr;
		break;
	}

	return NB_OK;
}

static int
lib_interface_eigrp_instance_keychain_destroy(struct nb_cb_destroy_args *args)
{
	eigrp_interface_t *intf;

	switch (args->event) {
	case NB_EV_VALIDATE:
	case NB_EV_PREPARE:
	case NB_EV_ABORT:
		/* NOTHING */
		break;
	case NB_EV_APPLY:
		intf = nb_running_get_entry(args->dnode, NULL, true);
		if (intf->params.auth_keychain)
			free(intf->params.auth_keychain);

		intf->params.auth_keychain = NULL;
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
				.cli_show = eigrp_cli_show_named_header,
				.cli_show_end = eigrp_cli_show_named_end,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family",
			.cbs = {
				.create = eigrpd_named_address_family_create,
				.destroy = eigrpd_named_address_family_destroy,
				.cli_show = eigrp_cli_show_named_address_family,
				.cli_show_end = eigrp_cli_show_named_address_family_end,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/router-id",
			.cbs = {
				.modify = eigrpd_named_router_id_modify,
				.destroy = eigrpd_named_router_id_destroy,
				.cli_show = eigrp_cli_show_router_id,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/network",
			.cbs = {
				.create = eigrpd_named_network_create,
				.destroy = eigrpd_named_network_destroy,
				.cli_show = eigrp_cli_show_network,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/neighbor",
			.cbs = {
				.create = eigrpd_named_neighbor_create,
				.destroy = eigrpd_named_neighbor_destroy,
				.cli_show = eigrp_cli_show_named_neighbor,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/shutdown",
			.cbs = {
				.create = eigrpd_named_shutdown_create,
				.destroy = eigrpd_named_shutdown_destroy,
				.cli_show = eigrp_cli_show_named_shutdown,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface",
			.cbs = {
				.create = eigrpd_named_af_interface_create,
				.destroy = eigrpd_named_af_interface_destroy,
				.cli_show = eigrp_cli_show_named_af_interface,
				.cli_show_end = eigrp_cli_show_named_af_interface_end,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/bandwidth-percent",
			.cbs = {
				.modify = eigrpd_named_af_interface_bandwidth_modify,
				.destroy = eigrpd_named_af_interface_bandwidth_destroy,
				.cli_show = eigrp_cli_show_named_af_interface_bandwidth,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/hello-interval",
			.cbs = {
				.modify = eigrpd_named_af_interface_hello_modify,
				.destroy = eigrpd_named_af_interface_hello_destroy,
				.cli_show = eigrp_cli_show_named_af_interface_hello,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/hold-time",
			.cbs = {
				.modify = eigrpd_named_af_interface_hold_modify,
				.destroy = eigrpd_named_af_interface_hold_destroy,
				.cli_show = eigrp_cli_show_named_af_interface_hold,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/passive-interface",
			.cbs = {
				.create = eigrpd_named_af_interface_passive_create,
				.destroy = eigrpd_named_af_interface_passive_destroy,
				.cli_show = eigrp_cli_show_named_af_interface_passive,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-mode",
			.cbs = {
				.modify = eigrpd_named_af_interface_authentication_modify,
				.destroy = eigrpd_named_af_interface_authentication_destroy,
				.cli_show = eigrp_cli_show_named_af_interface_authentication,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/authentication-key-chain",
			.cbs = {
				.modify = eigrpd_named_af_interface_keychain_modify,
				.destroy = eigrpd_named_af_interface_keychain_destroy,
				.cli_show = eigrp_cli_show_named_af_interface_keychain,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/next-hop-self",
			.cbs = {
				.modify = eigrpd_named_af_interface_next_hop_modify,
				.cli_show = eigrp_cli_show_named_af_interface_next_hop_self,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/split-horizon",
			.cbs = {
				.modify = eigrpd_named_af_interface_split_horizon_modify,
				.cli_show = eigrp_cli_show_named_af_interface_split_horizon,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/summary-address",
			.cbs = {
				.create = eigrpd_named_af_interface_summary_create,
				.destroy = eigrpd_named_af_interface_summary_destroy,
				.cli_show = eigrp_cli_show_named_af_interface_summary,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/af-interface/shutdown",
			.cbs = {
				.create = eigrpd_named_af_interface_shutdown_create,
				.destroy = eigrpd_named_af_interface_shutdown_destroy,
				.cli_show = eigrp_cli_show_named_af_interface_shutdown,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology",
			.cbs = {
				.create = eigrpd_named_topology_create,
				.destroy = eigrpd_named_topology_destroy,
				.cli_show = eigrp_cli_show_named_topology,
				.cli_show_end = eigrp_cli_show_named_topology_end,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/auto-summary",
			.cbs = {
				.create = eigrpd_named_auto_summary_create,
				.destroy = eigrpd_named_auto_summary_destroy,
				.cli_show = eigrp_cli_show_named_auto_summary,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-information-in",
			.cbs = {
				.create = eigrpd_named_default_information_in_create,
				.destroy = eigrpd_named_default_information_in_destroy,
				.cli_show = eigrp_cli_show_named_default_information_in,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-information-out",
			.cbs = {
				.create = eigrpd_named_default_information_out_create,
				.destroy = eigrpd_named_default_information_out_destroy,
				.cli_show = eigrp_cli_show_named_default_information_out,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/default-metric",
			.cbs = {
				.create = eigrpd_named_default_metric_create,
				.destroy = eigrpd_named_default_metric_destroy,
				.cli_show = eigrp_cli_show_named_default_metric,
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
				.cli_show = eigrp_cli_show_named_distance,
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
				.modify = eigrpd_named_maximum_prefix_modify,
				.destroy = eigrpd_named_maximum_prefix_destroy,
				.cli_show = eigrp_cli_show_named_maximum_prefix,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-weights",
			.cbs = {
				.create = eigrpd_named_metric_weights_create,
				.destroy = eigrpd_named_metric_weights_destroy,
				.cli_show = eigrp_cli_show_named_metric_weights,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-weights/tos",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-weights/K1",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-weights/K2",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-weights/K3",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-weights/K4",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/metric-weights/K5",
			.cbs = { .modify = eigrpd_named_metric_weights_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/offset-list",
			.cbs = {
				.create = eigrpd_named_offset_list_create,
				.destroy = eigrpd_named_offset_list_destroy,
				.cli_show = eigrp_cli_show_named_offset_list,
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
				.cli_show = eigrp_cli_show_named_redistribute,
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
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric",
			.cbs = {
				.create = eigrpd_named_summary_metric_create,
				.destroy = eigrpd_named_summary_metric_destroy,
				.cli_show = eigrp_cli_show_named_summary_metric,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/bandwidth",
			.cbs = { .modify = eigrpd_named_summary_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/delay",
			.cbs = { .modify = eigrpd_named_summary_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/reliability",
			.cbs = { .modify = eigrpd_named_summary_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/load",
			.cbs = { .modify = eigrpd_named_summary_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/summary-metric/mtu",
			.cbs = { .modify = eigrpd_named_summary_metric_modify }
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/active-time",
			.cbs = {
				.modify = eigrpd_named_active_time_modify,
				.destroy = eigrpd_named_active_time_destroy,
				.cli_show = eigrp_cli_show_named_active_time,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/traffic-share-balanced",
			.cbs = {
				.modify = eigrpd_named_traffic_share_modify,
				.destroy = eigrpd_named_traffic_share_destroy,
				.cli_show = eigrp_cli_show_named_traffic_share_balanced,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/named/address-family/topology/variance",
			.cbs = {
				.modify = eigrpd_named_variance_modify,
				.destroy = eigrpd_named_variance_destroy,
				.cli_show = eigrp_cli_show_named_variance,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance",
			.cbs = {
				.create = eigrpd_instance_create,
				.destroy = eigrpd_instance_destroy,
				.cli_show = eigrp_cli_show_header,
				.cli_show_end = eigrp_cli_show_end_header,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/router-id",
			.cbs = {
				.modify = eigrpd_instance_router_id_modify,
				.destroy = eigrpd_instance_router_id_destroy,
				.cli_show = eigrp_cli_show_router_id,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/passive-interface",
			.cbs = {
				.create = eigrpd_instance_passive_interface_create,
				.destroy = eigrpd_instance_passive_interface_destroy,
				.cli_show = eigrp_cli_show_passive_interface,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/active-time",
			.cbs = {
				.modify = eigrpd_instance_active_time_modify,
				.cli_show = eigrp_cli_show_active_time,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/variance",
			.cbs = {
				.modify = eigrpd_instance_variance_modify,
				.destroy = eigrpd_instance_variance_destroy,
				.cli_show = eigrp_cli_show_variance,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/maximum-paths",
			.cbs = {
				.modify = eigrpd_instance_maximum_paths_modify,
				.destroy = eigrpd_instance_maximum_paths_destroy,
				.cli_show = eigrp_cli_show_maximum_paths,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/metric-weights",
			.cbs = {
				.cli_show = eigrp_cli_show_metrics,
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
				.cli_show = eigrp_cli_show_network,
			}
		},
		{
			.xpath = "/frr-eigrpd:eigrpd/instance/neighbor",
			.cbs = {
				.create = eigrpd_instance_neighbor_create,
				.destroy = eigrpd_instance_neighbor_destroy,
				.cli_show = eigrp_cli_show_neighbor,
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
				.cli_show = eigrp_cli_show_redistribute,
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
				.cli_show = eigrp_cli_show_delay,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/bandwidth",
			.cbs = {
				.modify = lib_interface_eigrp_bandwidth_modify,
				.cli_show = eigrp_cli_show_bandwidth,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/hello-interval",
			.cbs = {
				.modify = lib_interface_eigrp_hello_interval_modify,
				.cli_show = eigrp_cli_show_hello_interval,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/hold-time",
			.cbs = {
				.modify = lib_interface_eigrp_hold_time_modify,
				.cli_show = eigrp_cli_show_hold_time,
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
				.cli_show = eigrp_cli_show_summarize_address,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/instance/authentication",
			.cbs = {
				.modify = lib_interface_eigrp_instance_authentication_modify,
				.cli_show = eigrp_cli_show_authentication,
			}
		},
		{
			.xpath = "/frr-interface:lib/interface/frr-eigrpd:eigrp/instance/keychain",
			.cbs = {
				.modify = lib_interface_eigrp_instance_keychain_modify,
				.destroy = lib_interface_eigrp_instance_keychain_destroy,
				.cli_show = eigrp_cli_show_keychain,
			}
		},
		{
			.xpath = NULL,
		},
	}
};
